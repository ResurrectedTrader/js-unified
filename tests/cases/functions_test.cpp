/// \file
/// Native functions: how a call arrives, what it can read, and how it answers.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "support/harness.h"

namespace {

struct CallLog {
    int calls = 0;
    std::uint32_t lastLength = 0;
    std::int32_t lastArgument = 0;
    bool lastWasConstruct = false;
};

struct Unrelated {
    int unused = 0;
};

void ReportsNoData(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(info.Data<CallLog>() == nullptr);
}

void Doubler(const ub::CallbackInfo& info) {
    auto* log = info.Data<CallLog>();
    if (log == nullptr) {
        info.ThrowTypeError("callback data missing");
        return;
    }
    ++log->calls;
    log->lastLength = info.Length();
    log->lastWasConstruct = info.IsConstructCall();

    if (info.Length() < 1) {
        info.ThrowTypeError("expected one argument");
        return;
    }
    const auto argument = info[0].ToInt32(info.GetContext());
    if (!argument) {
        return;  // the coercion threw; the exception is already pending
    }
    log->lastArgument = *argument;
    info.GetReturnValue().Set(*argument * 2);
}

/// Answers with whatever kind of value the first argument names, so one case
/// can walk every `ReturnValue` setter.
void ReturnAKindOf(const ub::CallbackInfo& info) {
    const auto which = info[0].ToString(info.GetContext());
    if (!which) {
        return;
    }
    const std::string kind = which->Utf8Value();

    if (kind == "undefined") {
        info.GetReturnValue().SetUndefined();
    } else if (kind == "null") {
        info.GetReturnValue().SetNull();
    } else if (kind == "bool") {
        info.GetReturnValue().Set(true);
    } else if (kind == "double") {
        info.GetReturnValue().Set(2.5);
    } else if (kind == "int") {
        info.GetReturnValue().Set(std::int32_t{-7});
    } else if (kind == "int16") {
        info.GetReturnValue().Set(std::int16_t{-300});
    } else if (kind == "uint16") {
        info.GetReturnValue().Set(std::uint16_t{65535});
    } else if (kind == "uint32") {
        info.GetReturnValue().Set(std::uint32_t{4294967295U});
    } else if (kind == "int64") {
        info.GetReturnValue().Set(std::int64_t{-9007199254740991LL});
    } else if (kind == "uint64") {
        info.GetReturnValue().Set(std::uint64_t{9007199254740991ULL});
    } else if (kind == "false") {
        info.GetReturnValue().SetFalse();
    } else if (kind == "empty") {
        if (!info.GetReturnValue().SetEmptyString()) {
            info.ThrowTypeError("could not make the string");
        }
    } else if (kind == "string") {
        if (!info.GetReturnValue().Set("from native")) {
            info.ThrowTypeError("could not make the string");
        }
    } else if (kind == "handle") {
        auto object = ub::Object::New(info.GetContext());
        if (!object) {
            return;
        }
        if (!object->Set(info.GetContext(), "made", ub::Integer::New(info.GetIsolate(), 1)).value_or(false)) {
            return;
        }
        info.GetReturnValue().Set(*object);
    } else {
        // Nothing set at all: the call must see undefined.
    }
}

/// Hands back what a `Global` holds, as a callback would return a value it
/// keeps between calls.
void ReturnHeld(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(*info.Data<ub::Global<ub::Value>>());
}

/// Records the receiver it was called with, so a test can ask what `this` was.
void RememberReceiver(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(info.This());
}

/// Reads every argument it was given, plus two past the end.
void CollectArguments(const ub::CallbackInfo& info) {
    auto array = ub::Array::New(info.GetContext(), 0);
    if (!array) {
        return;
    }
    for (std::uint32_t i = 0; i < info.Length() + 2; ++i) {
        // Making values between reads forces the frame to grow underneath the
        // borrowed argument region (docs/lifetimes.md section 8).
        (void)ub::Integer::New(info.GetIsolate(), static_cast<std::int32_t>(i));
        if (!array->Set(info.GetContext(), i, info[i]).value_or(false)) {
            return;
        }
    }
    info.GetReturnValue().Set(*array);
}

}  // namespace

TEST_CASE("functions: script calls native and native calls it straight back") {
    ub_test::Fixture fixture;
    CallLog log;

    const auto function = ub::Function::New(fixture.context, &Doubler, ub::CallbackData::For(log));
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "twice", *function);

    CHECK(ub_test::EvalInt(fixture.context, "twice(21)") == 42);
    CHECK(log.calls == 1);
    CHECK(log.lastArgument == 21);
    CHECK(log.lastLength == 1);
    CHECK_FALSE(log.lastWasConstruct);

    const std::array<ub::Local<ub::Value>, 1> arguments{ub::Integer::New(fixture.iso(), 4)};
    const auto direct = function->Call(fixture.context, fixture.context.GlobalObject(), arguments);
    REQUIRE(direct.has_value());
    CHECK(direct->To<ub::Integer>()->Int32Value() == 8);
    CHECK(log.calls == 2);
}

TEST_CASE("functions: a callback reads only the arguments it was given") {
    ub_test::Fixture fixture;

    const auto function = ub::Function::New(fixture.context, &CollectArguments);
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "collect", *function);

    const auto value = ub_test::Eval(fixture.context, "collect(10, 20, 30)");
    const auto array = value.To<ub::Array>();
    REQUIRE(array.has_value());
    REQUIRE(array->Length() == 5);

    CHECK(array->Get(fixture.context, 0U)->To<ub::Integer>()->Int32Value() == 10);
    CHECK(array->Get(fixture.context, 2U)->To<ub::Integer>()->Int32Value() == 30);
    // Past the end is undefined, which is what script would have seen.
    CHECK(array->Get(fixture.context, 3U)->Kind() == ub::ValueKind::Undefined);
    CHECK(array->Get(fixture.context, 4U)->Kind() == ub::ValueKind::Undefined);

    CHECK(ub_test::EvalInt(fixture.context, "collect().length") == 2);
    CHECK(ub_test::EvalInt(fixture.context, "collect(1,2,3,4,5,6,7,8,9,10).length") == 12);
}

TEST_CASE("functions: a callback sees the receiver it was called on") {
    ub_test::Fixture fixture;

    const auto function = ub::Function::New(fixture.context, &RememberReceiver);
    REQUIRE(function.has_value());

    auto host = ub::Object::New(fixture.context);
    REQUIRE(host.has_value());
    REQUIRE(host->Set(fixture.context, "method", *function).value_or(false));
    ub_test::Expose(fixture.context, "host", *host);

    const auto asMethod = ub_test::Eval(fixture.context, "host.method()");
    CHECK(asMethod.StrictEquals(*host));

    // Calling from native with an explicit receiver says the same thing.
    auto other = ub::Object::New(fixture.context);
    REQUIRE(other.has_value());
    const auto called = function->Call(fixture.context, *other);
    REQUIRE(called.has_value());
    CHECK(called->StrictEquals(*other));
}

TEST_CASE("functions: every way of answering a call works") {
    ub_test::Fixture fixture;

    const auto function = ub::Function::New(fixture.context, &ReturnAKindOf);
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "answer", *function);

    CHECK(ub_test::Eval(fixture.context, "answer('undefined')").Kind() == ub::ValueKind::Undefined);
    CHECK(ub_test::Eval(fixture.context, "answer('null')").Kind() == ub::ValueKind::Null);
    CHECK(ub_test::EvalTruth(fixture.context, "answer('bool')"));
    CHECK(ub_test::EvalNumber(fixture.context, "answer('double')") == doctest::Approx(2.5));
    CHECK(ub_test::EvalInt(fixture.context, "answer('int')") == -7);
    CHECK(ub_test::EvalText(fixture.context, "answer('string')") == "from native");
    CHECK(ub_test::EvalInt(fixture.context, "answer('handle').made") == 1);
    CHECK(ub_test::Eval(fixture.context, "answer('nothing')").Kind() == ub::ValueKind::Undefined);

    // V8's other widths, each as the integer it names.
    CHECK(ub_test::EvalInt(fixture.context, "answer('int16')") == -300);
    CHECK(ub_test::EvalInt(fixture.context, "answer('uint16')") == 65535);
    CHECK(ub_test::EvalNumber(fixture.context, "answer('uint32')") == 4294967295.0);
    CHECK(ub_test::EvalTruth(fixture.context, "answer('uint32') === 4294967295"));
    CHECK(ub_test::EvalTruth(fixture.context, "answer('int64') === -Number.MAX_SAFE_INTEGER"));
    CHECK(ub_test::EvalTruth(fixture.context, "answer('uint64') === Number.MAX_SAFE_INTEGER"));
    CHECK(ub_test::EvalTruth(fixture.context, "answer('false') === false"));
    CHECK(ub_test::EvalTruth(fixture.context, "answer('empty') === ''"));
}

TEST_CASE("functions: a callback can answer with a value it holds between calls") {
    ub_test::Fixture fixture;

    const auto held = ub::String::New(fixture.iso(), "kept");
    REQUIRE(held.has_value());
    ub::Global<ub::Value> kept(fixture.iso(), *held);
    ub::Global<ub::Value> nothing;

    const auto answer = ub::Function::New(fixture.context, &ReturnHeld, ub::CallbackData::For(kept));
    const auto none = ub::Function::New(fixture.context, &ReturnHeld, ub::CallbackData::For(nothing));
    REQUIRE(answer.has_value());
    REQUIRE(none.has_value());
    ub_test::Expose(fixture.context, "held", *answer);
    ub_test::Expose(fixture.context, "none", *none);

    CHECK(ub_test::EvalText(fixture.context, "held()") == "kept");
    CHECK(ub_test::EvalTruth(fixture.context, "none() === undefined"));
}

TEST_CASE("functions: embedder data is typed and cannot be mistaken for another type") {
    ub_test::Fixture fixture;
    CallLog log;

    const auto withData = ub::Function::New(fixture.context, &Doubler, ub::CallbackData::For(log));
    REQUIRE(withData.has_value());
    ub_test::Expose(fixture.context, "twice", *withData);
    CHECK(ub_test::EvalInt(fixture.context, "twice(1)") == 2);

    // Declared with no data at all: the same callback must find none rather
    // than something of the wrong type, and it says so by throwing.
    const auto withoutData = ub::Function::New(fixture.context, &Doubler);
    REQUIRE(withoutData.has_value());
    ub_test::Expose(fixture.context, "orphan", *withoutData);

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "orphan(1)").has_value());
    CHECK(tryCatch.HasCaught());
    CHECK(tryCatch.Message(fixture.context).value_or("").find("callback data missing") != std::string::npos);
}

TEST_CASE("functions: embedder data of the wrong type reads as nothing") {
    ub_test::Fixture fixture;

    Unrelated unrelated;
    const auto function = ub::Function::New(fixture.context, &ReportsNoData, ub::CallbackData::For(unrelated));
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "probe", *function);

    CHECK(ub_test::EvalTruth(fixture.context, "probe()"));
}

TEST_CASE("functions: a plain native function is callable and not constructable") {
    ub_test::Fixture fixture;
    CallLog log;

    const auto function = ub::Function::New(fixture.context, &Doubler, ub::CallbackData::For(log));
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "Twice", *function);

    // A function that can be `new`-ed without anyone asking hands script a
    // fresh empty object instead of the callback's result, and the callback
    // cannot tell unless it thought to check. `FunctionTemplate` and `Class<T>`
    // are how an embedder asks for a constructor. See docs/status.md decision 9.
    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            try { new Twice(3); return false; }
            catch (e) { return e instanceof TypeError; }
        })()
    )"));
    CHECK(log.calls == 0);

    ub::TryCatch tryCatch(fixture.iso());
    const std::array<ub::Local<ub::Value>, 1> arguments{ub::Integer::New(fixture.iso(), 5)};
    CHECK_FALSE(function->NewInstance(fixture.context, arguments).has_value());
    CHECK(tryCatch.HasCaught());
    tryCatch.Reset();
    CHECK(log.calls == 0);

    // Calling it is exactly as it was.
    CHECK(ub_test::EvalInt(fixture.context, "Twice(3)") == 6);
    CHECK(log.calls == 1);
    CHECK_FALSE(log.lastWasConstruct);
}

TEST_CASE("functions: native can call a function script wrote") {
    ub_test::Fixture fixture;

    const auto value = ub_test::Eval(fixture.context, "(function (a, b) { return a + b + this.base; })");
    const auto function = value.To<ub::Function>();
    REQUIRE(function.has_value());

    auto receiver = ub::Object::New(fixture.context);
    REQUIRE(receiver.has_value());
    REQUIRE(receiver->Set(fixture.context, "base", ub::Integer::New(fixture.iso(), 100)).value_or(false));

    const std::array<ub::Local<ub::Value>, 2> arguments{ub::Integer::New(fixture.iso(), 1),
                                                        ub::Integer::New(fixture.iso(), 2)};
    const auto result = function->Call(fixture.context, *receiver, arguments);
    REQUIRE(result.has_value());
    CHECK(result->To<ub::Integer>()->Int32Value() == 103);
}

TEST_CASE("functions: native can construct with a function script wrote") {
    ub_test::Fixture fixture;

    const auto value = ub_test::Eval(fixture.context, "(function Point(x) { this.x = x; })");
    const auto constructor = value.To<ub::Function>();
    REQUIRE(constructor.has_value());

    const std::array<ub::Local<ub::Value>, 1> arguments{ub::Integer::New(fixture.iso(), 9)};
    const auto instance = constructor->NewInstance(fixture.context, arguments);
    REQUIRE(instance.has_value());
    CHECK(instance->Get(fixture.context, "x")->To<ub::Integer>()->Int32Value() == 9);
}

TEST_CASE("functions: a native callback that throws surfaces as a script exception") {
    ub_test::Fixture fixture;
    CallLog log;

    const auto function = ub::Function::New(fixture.context, &Doubler, ub::CallbackData::For(log));
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "twice", *function);

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "twice()").has_value());
    REQUIRE(tryCatch.HasCaught());

    // The engines decorate their errors differently; what has to survive is the
    // message the callback wrote. See docs/testing.md.
    const auto message = tryCatch.Message(fixture.context);
    REQUIRE(message.has_value());
    CHECK(message->find("expected one argument") != std::string::npos);
}

TEST_CASE("functions: a throw from a native callback is catchable in script") {
    ub_test::Fixture fixture;
    CallLog log;

    const auto function = ub::Function::New(fixture.context, &Doubler, ub::CallbackData::For(log));
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "twice", *function);

    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            try { twice(); return false; }
            catch (e) { return e instanceof TypeError; }
        })()
    )"));
}

TEST_CASE("functions: native into script into native") {
    ub_test::Fixture fixture;
    CallLog log;

    const auto doubler = ub::Function::New(fixture.context, &Doubler, ub::CallbackData::For(log));
    REQUIRE(doubler.has_value());
    ub_test::Expose(fixture.context, "twice", *doubler);

    const auto value = ub_test::Eval(fixture.context, "(function (n) { return twice(n) + twice(n); })");
    const auto scripted = value.To<ub::Function>();
    REQUIRE(scripted.has_value());

    const std::array<ub::Local<ub::Value>, 1> arguments{ub::Integer::New(fixture.iso(), 6)};
    const auto result = scripted->Call(fixture.context, fixture.context.GlobalObject(), arguments);
    REQUIRE(result.has_value());
    CHECK(result->To<ub::Integer>()->Int32Value() == 24);
    CHECK(log.calls == 2);
}
