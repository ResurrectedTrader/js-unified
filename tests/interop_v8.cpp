// unibind/interop/v8.h, which only the V8 backend defines - so it is its own
// executable, built only in a V8 tree, and the suite's case list stays the same
// under both engines (which is what the parity comparison reads).
//
// The claim under test is that what comes back is the live engine object and
// not merely a non-null pointer: the isolate is the one V8 considers current,
// and script compiled straight against the V8 context sees a global that was
// set through unibind.

// Before v8.h, which only forward-declares the CFunction that v8-template.h
// spans over; see unibind/interop/v8.h.
#include <v8-fast-api-calls.h>
#include <v8.h>

#include <cstdio>

#include "unibind/interop/v8.h"
#include "unibind/unibind.h"

namespace {

int Fail(const char* what) {
    std::fprintf(stderr, "interop_v8: %s\n", what);
    return 1;
}

int Run() {
    const ub::Platform platform;
    const auto isolate = ub::Isolate::New();
    if (isolate == nullptr) {
        return Fail("could not make an isolate");
    }
    const ub::HandleScope scope(*isolate);
    const auto context = ub::Context::New(*isolate);
    if (!context) {
        return Fail("could not make a realm");
    }
    const ub::ContextScope entered(*context);

    v8::Isolate* native = ub::interop::V8Isolate(*isolate);
    if (native == nullptr) {
        return Fail("V8Isolate returned null");
    }
    if (native != v8::Isolate::GetCurrent()) {
        return Fail("V8Isolate is not the isolate V8 has entered");
    }

    if (!context->GlobalObject().Set(*context, "fromUnibind", ub::Integer::New(*isolate, 41)).value_or(false)) {
        return Fail("could not set a global through unibind");
    }

    const v8::HandleScope nativeScope(native);
    const v8::Local<v8::Context> nativeContext = ub::interop::V8Context(*context);
    if (nativeContext.IsEmpty()) {
        return Fail("V8Context returned an empty handle");
    }
    const v8::Context::Scope nativeEntered(nativeContext);
    v8::Local<v8::String> source = v8::String::NewFromUtf8Literal(native, "fromUnibind + 1");
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;
    if (!v8::Script::Compile(nativeContext, source).ToLocal(&script) || !script->Run(nativeContext).ToLocal(&result)) {
        return Fail("script against the native context did not run");
    }
    if (!result->IsInt32() || result.As<v8::Int32>()->Value() != 42) {
        return Fail("the native context does not see the unibind global");
    }

    if (!ub::interop::V8Context(ub::Context{}).IsEmpty()) {
        return Fail("an empty ub::Context did not give an empty v8::Context");
    }

    std::printf("interop_v8: ok\n");
    return 0;
}

}  // namespace

int main() {
    return Run();
}
