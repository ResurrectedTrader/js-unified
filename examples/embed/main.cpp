/// \file
/// The smallest useful embedding: a platform, an isolate, a realm, a native
/// class bound into it, a script, a JavaScript function called from C++, an
/// exception caught, and the drain that promise continuations wait for.
///
/// It is built against an **installed** unibind, the way a stranger would build it,
/// rather than against the source tree - see examples/README.md. Nothing here
/// names an engine, so the same source builds and behaves the same whichever
/// backend the prefix holds.

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "unibind/unibind.h"

namespace {

// --- the native a script gets to hold -------------------------------------

/// A counter, interesting only because script owns a *share* of it: the wrapper
/// holds one, this program could hold another, and whoever lets go last
/// destroys it.
struct Counter {
    explicit Counter(std::int32_t start) : value(start) {}
    std::int32_t value = 0;
};

std::unique_ptr<Counter> MakeCounter(const ub::CallbackInfo& info) {
    std::int32_t start = 0;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return nullptr;  // the coercion ran script and it threw
        }
        start = *asInt;
    }
    if (start < 0) {
        info.ThrowTypeError("a counter starts at zero or above");
        return nullptr;  // null after a throw is how a constructor declines
    }
    return std::make_unique<Counter>(start);
}

void Increment(Counter& self, const ub::CallbackInfo& info) {
    std::int32_t by = 1;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return;
        }
        by = *asInt;
    }
    self.value += by;
    info.GetReturnValue().Set(self.value);
}

void ReadValue(Counter& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.value);
}

// --- a free function script can call --------------------------------------

void Print(const ub::CallbackInfo& info) {
    for (std::uint32_t i = 0; i < info.Length(); ++i) {
        const auto text = info[i].ToString(info.GetContext());
        if (!text) {
            return;  // the coercion threw; return promptly and let it propagate
        }
        std::printf("%s%s", i == 0 ? "" : " ", text->Utf8Value().c_str());
    }
    std::printf("\n");
}

// --- small conveniences ----------------------------------------------------

/// Installs `value` on the realm's global object under `name`.
template <class T>
bool Expose(const ub::Context& context, const char* name, const ub::Local<T>& value) {
    return context.GlobalObject().Set(context, name, value).value_or(false);
}

std::string TextOf(const ub::Context& context, const ub::Local<ub::Value>& value) {
    const auto text = value.ToString(context);
    return text ? text->Utf8Value() : std::string("<threw while converting>");
}

/// Evaluates `source` and describes the result as text, for printing.
std::string EvaluateToText(const ub::Context& context, const char* source) {
    const auto result = ub::Evaluate(context, source);
    return result ? TextOf(context, *result) : std::string("<threw>");
}

int Run() {
    // One per process, before the first isolate and outliving the last.
    const ub::Platform platform;
    // This object file is the same object file under either engine - which one
    // is underneath is a question with a runtime answer, and this is it.
    std::printf("backend: %s %s\n", std::string(ub::Platform::BackendName()).c_str(),
                std::string(ub::Platform::BackendVersion()).c_str());

    // One heap, one thread; at most one alive per thread.
    const auto isolate = ub::Isolate::New();
    if (isolate == nullptr) {
        std::fprintf(stderr, "could not make an isolate\n");
        return 1;
    }

    // Every handle below lives in this frame and dies with it.
    const ub::HandleScope scope(*isolate);

    const auto context = ub::Context::New(*isolate);
    if (!context) {
        std::fprintf(stderr, "could not make a realm\n");
        return 1;
    }
    const ub::ContextScope entered(*context);

    // --- bind the native class --------------------------------------------

    const auto counterClass = ub::Class<Counter>::New(*isolate, "Counter");
    counterClass.Construct<&MakeCounter>();
    counterClass.Method<&Increment>("increment");
    counterClass.Accessor<&ReadValue>("value");

    const auto constructor = counterClass.GetConstructor(*context);
    const auto print = ub::Function::New(*context, &Print);
    if (!constructor || !print || !Expose(*context, "Counter", *constructor) || !Expose(*context, "print", *print)) {
        std::fprintf(stderr, "could not install the API\n");
        return 1;
    }

    // --- run some script --------------------------------------------------

    {
        const ub::TryCatch caught(*isolate);
        const auto result = ub::Evaluate(*context, R"(
            const c = new Counter(40);
            c.increment(2);
            print('the counter says', c.value);
            globalThis.describe = (what) => `${what} is ${c.value}`;
            c.value
        )",
                                         {.resourceName = "example.js"});
        if (!result) {
            std::fprintf(stderr, "script failed: %s\n", caught.Message(*context).value_or("<no message>").c_str());
            return 1;
        }
        const auto asInteger = result->To<ub::Integer>();
        if (!asInteger) {
            std::fprintf(stderr, "expected the script to evaluate to an integer\n");
            return 1;
        }
        std::printf("the script evaluated to %d\n", asInteger->Int32Value());
    }

    // --- call a script function from native -------------------------------

    {
        const auto described = context->GlobalObject().Get(*context, "describe");
        const auto asFunction = described ? described->To<ub::Function>() : std::nullopt;
        if (!asFunction) {
            std::fprintf(stderr, "describe is not a function\n");
            return 1;
        }
        const auto argument = ub::String::New(*isolate, "the answer");
        if (!argument) {
            std::fprintf(stderr, "could not make a string\n");
            return 1;
        }
        const std::array<ub::Local<ub::Value>, 1> arguments{*argument};
        const auto answer = asFunction->Call(*context, context->GlobalObject(), arguments);
        if (!answer) {
            std::fprintf(stderr, "describe threw\n");
            return 1;
        }
        std::printf("describe() said: %s\n", TextOf(*context, *answer).c_str());
    }

    // --- catch what script throws -----------------------------------------

    {
        const ub::TryCatch caught(*isolate);
        const auto failed = ub::Evaluate(*context, "new Counter(-1)", {.resourceName = "bad.js"});
        if (failed || !caught.HasCaught()) {
            std::fprintf(stderr, "a negative counter should have thrown\n");
            return 1;
        }
        std::printf("as expected: %s\n", caught.Message(*context).value_or("<no message>").c_str());
    }

    // --- nothing runs a promise continuation but PumpJobs ------------------

    {
        if (!ub::Evaluate(*context, "globalThis.ran = false; Promise.resolve().then(() => { ran = true });")) {
            std::fprintf(stderr, "could not make a promise\n");
            return 1;
        }
        std::printf("before the pump: ran = %s\n", EvaluateToText(*context, "String(ran)").c_str());
        isolate->PumpJobs();
        std::printf("after the pump:  ran = %s\n", EvaluateToText(*context, "String(ran)").c_str());
    }

    return 0;
}

}  // namespace

int main() {
    return Run();
}
