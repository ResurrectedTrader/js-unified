/// \file
/// The edges of the operations added since the first release, which the cases
/// next to each feature leave out: the boundaries of a conversion, the failure
/// and empty answers a header documents, the second name a callback serves, the
/// other thread a call is allowed from, and what is dropped rather than run.
///
/// Each section names the header whose promise it holds. Like every other file
/// here it never asks which backend it is on; where an engine may genuinely
/// answer either way, the case asserts what both promise and reports the rest.

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "support/harness.h"

namespace {

// ---------------------------------------------------------------------------
// Callbacks the cases below share
// ---------------------------------------------------------------------------

/// Answers with the value its argument names, one per `ReturnValue::Set`
/// overload a boundary is worth checking on.
void ReturnWidth(const ub::CallbackInfo& info) {
    const auto kind = info[0].ToString(info.GetContext());
    if (!kind) {
        return;
    }
    const std::string which = kind->Utf8Value();
    const auto rv = info.GetReturnValue();
    if (which == "int16-min") {
        rv.Set(std::numeric_limits<std::int16_t>::min());
    } else if (which == "uint16-zero") {
        rv.Set(std::uint16_t{0});
    } else if (which == "uint32-int32-max") {
        rv.Set(std::uint32_t{2147483647U});
    } else if (which == "uint32-past-int32") {
        rv.Set(std::uint32_t{2147483648U});
    } else if (which == "int64-int32-min") {
        rv.Set(std::int64_t{std::numeric_limits<std::int32_t>::min()});
    } else if (which == "int64-below-int32") {
        rv.Set(std::int64_t{std::numeric_limits<std::int32_t>::min()} - 1);
    } else if (which == "int64-past-double") {
        rv.Set(std::int64_t{9007199254740993LL});  // 2^53 + 1: no double is exactly this
    } else if (which == "uint64-max") {
        rv.Set(std::numeric_limits<std::uint64_t>::max());
    } else if (which == "uint64-int32-max") {
        rv.Set(std::uint64_t{2147483647U});
    } else if (which == "literal") {
        // The `const char*` overload, with bytes that are not UTF-8.
        if (!rv.Set("x\xC0y")) {
            info.ThrowTypeError("could not make the string");
        }
    }
}

/// Throws through the isolate rather than through the call, with bytes that
/// are not UTF-8.
void ThrowThroughIsolate(const ub::CallbackInfo& info) {
    info.GetIsolate().ThrowError(ub::ErrorKind::RangeError, std::string_view("bad \xFF byte"));
}

/// The native state behind accessors that one pair of callbacks serves under
/// several names.
struct Registers {
    std::int32_t first = 1;
    std::int32_t second = 2;
    std::vector<std::string> readNames;
    std::vector<std::string> writeNames;
    int throwsOnRead = 0;
};

[[nodiscard]] std::string NameOf(const ub::Local<ub::Name>& property) {
    const auto asString = property.To<ub::String>();
    return asString ? asString->Utf8Value() : std::string();
}

void ReadRegister(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    auto* registers = info.Data<Registers>();
    const std::string name = NameOf(property);
    registers->readNames.push_back(name);
    if (name == "broken") {
        ++registers->throwsOnRead;
        info.ThrowTypeError("this register cannot be read");
        return;
    }
    info.GetReturnValue().Set(name == "first" ? registers->first : registers->second);
}

void WriteRegister(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& value,
                   const ub::PropertyCallbackInfo& info) {
    auto* registers = info.Data<Registers>();
    const std::string name = NameOf(property);
    registers->writeNames.push_back(name);
    const auto asInt = value.ToInt32(info.GetContext());
    if (!asInt) {
        return;
    }
    (name == "first" ? registers->first : registers->second) = *asInt;
}

/// Answers with the receiver the read was made through.
void ReadReceiver(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(info.This());
}

/// A native a class constructor hands over a share of.
struct Widget {
    explicit Widget(std::int32_t size) : size(size) {}
    std::int32_t size = 0;
};

/// The share the embedder holds, which the constructor below hands script too.
std::shared_ptr<Widget> g_heldWidget;

std::shared_ptr<Widget> ShareHeldWidget(const ub::CallbackInfo& info) {
    if (info.Length() > 0 && info[0].IsString()) {
        info.ThrowTypeError("a widget is not made from a string");
        return nullptr;
    }
    return g_heldWidget;
}

void WidgetSize(Widget& self, const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(self.size);
}

/// A job that records it ran, and on which thread.
struct Ran {
    std::atomic<int> count{0};
    std::thread::id on;
};

void RecordRan(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    auto* ran = data.As<Ran>();
    if (ran != nullptr) {
        ran->on = std::this_thread::get_id();
        ++ran->count;
    }
}

/// A job that says which one it was.
struct Tagged {
    std::vector<int>* log = nullptr;
    int tag = 0;
};

void RecordTag(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    auto* tagged = data.As<Tagged>();
    if (tagged != nullptr && tagged->log != nullptr) {
        tagged->log->push_back(tagged->tag);
    }
}

/// Mostly function bodies, none of which the top level calls: source whose
/// eager blob is larger than its lazy one on every engine. See the eager
/// compilation section of `codecache_test.cpp` for what is compared, and why not
/// by a fixed factor.
[[nodiscard]] std::string ManyFunctions() {
    std::string source;
    for (int i = 0; i < 40; ++i) {
        const std::string n = std::to_string(i);
        source.append("function g").append(n).append("(a) { var total = a * ").append(n);
        source.append("; for (var k = 0; k < a; ++k) { total += k % ").append(std::to_string(i + 3));
        source.append("; } return function () { return total - ").append(n).append("; }; }\n");
    }
    source += "41;\n";
    return source;
}

/// The blob an ordinary compile of `ManyFunctions` gives once everything in it,
/// the functions they return included, has run - in an isolate of its own.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> BlobAfterRunningEverything(std::string_view source) {
    std::string everything = "var sum = 0;";
    for (int i = 0; i < 40; ++i) {
        everything.append(" sum += g").append(std::to_string(i)).append("(2)();");
    }
    everything += " 1";

    ub_test::Fixture fixture;
    const auto script = ub::Script::Compile(fixture.context, source, {.resourceName = "none.js"});
    REQUIRE(script.has_value());
    REQUIRE(script->Run(fixture.context).has_value());
    CHECK(ub_test::EvalInt(fixture.context, everything) == 1);
    return script->CreateCodeCache();
}

/// A client that keeps what it is sent and resumes any pause at once.
struct QuietClient final : ub::InspectorClient {
    void SendProtocolMessage(std::string_view message) override { messages.emplace_back(message); }
    void RunMessageLoopOnPause() override {
        ++pauses;
        if (session != nullptr) {
            session->Resume();
        }
    }
    void QuitMessageLoopOnPause() override {}

    [[nodiscard]] bool Saw(std::string_view needle) const {
        for (const std::string& message : messages) {
            if (message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> messages;
    ub::InspectorSession* session = nullptr;
    int pauses = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Local's type predicates (unibind/handle.h)
// ---------------------------------------------------------------------------

TEST_CASE("coverage: the type predicates answer about the value, not about a wrapper around it") {
    ub_test::Fixture fixture;
    const auto& context = fixture.context;

    // A boxed primitive is an object, and nothing more specific.
    const auto boxedTrue = ub_test::Eval(context, "new Boolean(true)");
    CHECK(boxedTrue.IsObject());
    CHECK_FALSE(boxedTrue.IsBoolean());
    CHECK_FALSE(boxedTrue.IsTrue());
    CHECK_FALSE(ub_test::Eval(context, "new Number(1)").IsNumber());
    CHECK_FALSE(ub_test::Eval(context, "new String('s')").IsString());
    CHECK_FALSE(ub_test::Eval(context, "new String('s')").IsName());

    // Falsy is not false, and nothing but undefined and null is either of them.
    CHECK_FALSE(ub_test::Eval(context, "0").IsFalse());
    CHECK_FALSE(ub_test::Eval(context, "''").IsFalse());
    CHECK_FALSE(ub_test::Eval(context, "0").IsNullOrUndefined());
    CHECK_FALSE(ub_test::Eval(context, "false").IsNullOrUndefined());
    CHECK_FALSE(ub_test::Eval(context, "1").IsName());

    // A thenable is not a promise, and a class is a function.
    CHECK_FALSE(ub_test::Eval(context, "({ then() {} })").IsPromise());
    CHECK(ub_test::Eval(context, "(class Thing {})").IsFunction());
    CHECK(ub_test::Eval(context, "(async function () {})").IsFunction());
    CHECK_FALSE(ub_test::Eval(context, "({ length: 0 })").IsArray());

    // The view predicates, on everything that is not a view.
    CHECK_FALSE(ub_test::Eval(context, "new ArrayBuffer(4)").IsArrayBufferView());
    CHECK_FALSE(ub_test::Eval(context, "new ArrayBuffer(4)").IsTypedArray());
    CHECK_FALSE(ub_test::Eval(context, "[1, 2]").IsArrayBufferView());
    CHECK(ub_test::Eval(context, "new Float64Array(2)").IsArrayBufferView());
    CHECK_FALSE(ub_test::Eval(context, "new Float64Array(2)").IsArrayBuffer());

    // An external is an external, and not an object to either question - which
    // V8 15.6 on its own would answer the other way.
    int payload = 5;
    const auto external = ub::External::New(fixture.iso(), payload);
    REQUIRE(external.has_value());
    const ub::Local<ub::Value> asValue = *external;
    CHECK(asValue.IsExternal());
    CHECK_FALSE(asValue.IsObject());
    CHECK_FALSE(asValue.Is<ub::Object>());
    CHECK_FALSE(asValue.To<ub::Object>().has_value());
    CHECK_FALSE(ub_test::Eval(context, "({})").IsExternal());
}

TEST_CASE("coverage: IsInt32 and IsUint32 at the edges of their ranges") {
    ub_test::Fixture fixture;
    const auto& context = fixture.context;

    // The ends of each range, and one past them.
    CHECK(ub_test::Eval(context, "2147483647").IsInt32());
    CHECK_FALSE(ub_test::Eval(context, "2147483648").IsInt32());
    CHECK(ub_test::Eval(context, "-2147483648").IsInt32());
    CHECK_FALSE(ub_test::Eval(context, "-2147483649").IsInt32());
    CHECK(ub_test::Eval(context, "2147483648").IsUint32());
    CHECK_FALSE(ub_test::Eval(context, "-1").IsUint32());

    // -0 is neither, as in V8: it is a number that no integer type holds.
    CHECK_FALSE(ub_test::Eval(context, "-0").IsInt32());
    CHECK_FALSE(ub_test::Eval(context, "-0").IsUint32());
    CHECK(ub_test::Eval(context, "-0").IsNumber());

    // What is not a finite integer is neither, however large.
    for (const char* source : {"NaN", "Infinity", "-Infinity", "0.5", "1e300"}) {
        CAPTURE(source);
        CHECK_FALSE(ub_test::Eval(context, source).IsInt32());
        CHECK_FALSE(ub_test::Eval(context, source).IsUint32());
    }

    // And what is not a number is neither, even when it would convert to one.
    CHECK_FALSE(ub_test::Eval(context, "'7'").IsInt32());
    CHECK_FALSE(ub_test::Eval(context, "true").IsUint32());
    CHECK_FALSE(ub_test::Eval(context, "7n").IsInt32());
}

// ---------------------------------------------------------------------------
// ReturnValue's integer widths (unibind/function.h)
// ---------------------------------------------------------------------------

TEST_CASE("coverage: each integer width returns the integer it names on both sides of int32") {
    ub_test::Fixture fixture;
    const auto answer = ub::Function::New(fixture.context, &ReturnWidth);
    REQUIRE(answer.has_value());
    ub_test::Expose(fixture.context, "width", *answer);

    CHECK(ub_test::EvalTruth(fixture.context, "width('int16-min') === -32768"));
    CHECK(ub_test::EvalTruth(fixture.context, "Object.is(width('uint16-zero'), 0)"));
    CHECK(ub_test::EvalTruth(fixture.context, "width('uint32-int32-max') === 2147483647"));
    // Past an int32 the answer is a Number, and still the same number - not a
    // negative one from a reinterpreted bit pattern.
    CHECK(ub_test::EvalTruth(fixture.context, "width('uint32-past-int32') === 2147483648"));
    CHECK(ub_test::EvalTruth(fixture.context, "width('int64-int32-min') === -2147483648"));
    CHECK(ub_test::EvalTruth(fixture.context, "width('int64-below-int32') === -2147483649"));
    CHECK(ub_test::EvalTruth(fixture.context, "width('uint64-int32-max') === 2147483647"));
    // Beyond 2^53 the nearest double, as V8 answers.
    CHECK(ub_test::EvalTruth(fixture.context, "width('int64-past-double') === 9007199254740992"));
    CHECK(ub_test::EvalTruth(fixture.context, "width('uint64-max') === 18446744073709551616"));
    CHECK(ub_test::EvalTruth(fixture.context, "width('uint64-max') > 0"));
}

TEST_CASE("coverage: a literal returned and an error thrown through the isolate are decoded lossily") {
    ub_test::Fixture fixture;
    const auto answer = ub::Function::New(fixture.context, &ReturnWidth);
    const auto fail = ub::Function::New(fixture.context, &ThrowThroughIsolate);
    REQUIRE(answer.has_value());
    REQUIRE(fail.has_value());
    ub_test::Expose(fixture.context, "width", *answer);
    ub_test::Expose(fixture.context, "fail", *fail);

    // C0 is never a byte of UTF-8, so it is one U+FFFD and the rest survives.
    CHECK(ub_test::EvalTruth(fixture.context, "width('literal') === 'x\\uFFFDy'"));
    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            try { fail(); return false; }
            catch (e) { return e instanceof RangeError && e.message === 'bad � byte'; }
        })()
    )"));
}

// ---------------------------------------------------------------------------
// A class constructor that hands back a share (unibind/class.h)
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE2(CLASSES, CALLABLE_CLASS,
                   "coverage: a constructor that shares a native the embedder holds, called with or without new") {
    g_heldWidget = std::make_shared<Widget>(11);
    const std::weak_ptr<Widget> watch = g_heldWidget;
    {
        ub_test::Fixture fixture;
        const auto cls = ub::Class<Widget>::New(fixture.iso(), "Widget");
        cls.ConstructOrCall<&ShareHeldWidget>();
        cls.Method<&WidgetSize>("size");
        ub_test::Expose(fixture.context, "Widget", *cls.GetConstructor(fixture.context));

        // Both ways of making one give an instance over the embedder's native.
        const auto made = ub_test::Eval(fixture.context, "globalThis.a = new Widget(); globalThis.b = Widget(); a");
        CHECK(ub_test::EvalTruth(fixture.context, "a instanceof Widget && b instanceof Widget && a !== b"));
        CHECK(ub_test::EvalInt(fixture.context, "a.size() + b.size()") == 22);
        CHECK(ub::Class<Widget>::Unwrap(made) == g_heldWidget.get());
        CHECK(ub::Class<Widget>::UnwrapShared(made) == g_heldWidget);

        // A share, so the engine's wrappers and the embedder hold one native.
        CHECK(g_heldWidget.use_count() > 1);

        // Null after a throw declines the construction, and the throw is what
        // script sees - on either path in.
        CHECK(ub_test::EvalTruth(fixture.context, R"(
            (function () {
                let thrown = 0;
                try { new Widget('no'); } catch (e) { if (e instanceof TypeError) ++thrown; }
                try { Widget('no'); } catch (e) { if (e instanceof TypeError) ++thrown; }
                return thrown === 2;
            })()
        )"));
    }
    // The isolate gave its shares back and the embedder's still holds the
    // native, whole.
    REQUIRE(g_heldWidget != nullptr);
    CHECK(g_heldWidget.use_count() == 1);
    CHECK(g_heldWidget->size == 11);
    g_heldWidget.reset();
    CHECK(watch.expired());
}

// ---------------------------------------------------------------------------
// Object::SetAccessor (unibind/handle.h)
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(OBJECT_ACCESSORS, "coverage: one pair of accessor callbacks serves several names") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    Registers registers;
    for (const char* name : {"first", "second"}) {
        CHECK(object->SetAccessor(fixture.context, name, &ReadRegister, &WriteRegister,
                                  ub::CallbackData::For(registers)) == std::optional<bool>(true));
    }
    ub_test::Expose(fixture.context, "regs", *object);

    CHECK(ub_test::EvalInt(fixture.context, "regs.first * 10 + regs.second") == 12);
    CHECK(ub_test::EvalInt(fixture.context, "regs.second = 7; regs.second") == 7);
    CHECK(registers.first == 1);
    CHECK(registers.second == 7);
    CHECK(registers.readNames == std::vector<std::string>{"first", "second", "second"});
    CHECK(registers.writeNames == std::vector<std::string>{"second"});
}

UNIBIND_TEST_CASE(OBJECT_ACCESSORS, "coverage: an accessor's attributes are the property's, and ReadOnly is not one") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    Registers registers;
    const auto data = ub::CallbackData::For(registers);
    CHECK(object->SetAccessor(fixture.context, "first", &ReadRegister, &WriteRegister, data,
                              ub::PropertyAttribute::DontEnum) == std::optional<bool>(true));
    CHECK(object->SetAccessor(fixture.context, "second", &ReadRegister, &WriteRegister, data,
                              ub::PropertyAttribute::DontDelete | ub::PropertyAttribute::ReadOnly) ==
          std::optional<bool>(true));
    ub_test::Expose(fixture.context, "regs", *object);

    // DontEnum hides it from enumeration and nothing else.
    CHECK(ub_test::EvalTruth(fixture.context, "!Object.keys(regs).includes('first')"));
    CHECK(ub_test::EvalTruth(fixture.context, "Object.keys(regs).includes('second')"));
    CHECK(ub_test::EvalInt(fixture.context, "regs.first") == 1);

    // DontDelete makes it stay, and ReadOnly is dropped: a setter was given, so
    // the property is writable through it (decision 6).
    CHECK(ub_test::EvalTruth(fixture.context,
                             "(function () { 'use strict'; try { delete regs.second; return false; } "
                             "catch (e) { return e instanceof TypeError; } })()"));
    CHECK(ub_test::EvalInt(fixture.context, "regs.second = 9; regs.second") == 9);
    CHECK(registers.second == 9);
    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            const d = Object.getOwnPropertyDescriptor(regs, 'second');
            return typeof d.set === 'function' && !d.configurable && d.enumerable;
        })()
    )"));
}

UNIBIND_TEST_CASE(OBJECT_ACCESSORS, "coverage: an accessor has the reader as its receiver, and its throw is script's") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    Registers registers;
    CHECK(object->SetAccessor(fixture.context, "self", &ReadReceiver) == std::optional<bool>(true));
    CHECK(object->SetAccessor(fixture.context, "broken", &ReadRegister, nullptr, ub::CallbackData::For(registers)) ==
          std::optional<bool>(true));
    ub_test::Expose(fixture.context, "base", *object);

    // Read through an object that inherits it, the receiver is that object and
    // not the one the accessor is on - it is a real accessor property.
    CHECK(ub_test::EvalTruth(fixture.context, "base.self === base"));
    CHECK(
        ub_test::EvalTruth(fixture.context, "(function () { const d = Object.create(base); return d.self === d; })()"));

    // A getter that throws hands script the error it threw.
    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            try { base.broken; return false; }
            catch (e) { return e instanceof TypeError && e.message.includes('cannot be read'); }
        })()
    )"));
    CHECK(registers.throwsOnRead == 1);

    // An accessor put over an existing data property replaces it.
    CHECK(ub_test::EvalInt(fixture.context, "globalThis.plain = { self: 1 }; 0") == 0);
    const auto plain = ub_test::Eval(fixture.context, "plain").To<ub::Object>();
    REQUIRE(plain.has_value());
    CHECK(plain->SetAccessor(fixture.context, "self", &ReadReceiver) == std::optional<bool>(true));
    CHECK(ub_test::EvalTruth(fixture.context, "plain.self === plain"));
}

// ---------------------------------------------------------------------------
// TryCatch::Location (unibind/exception.h)
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "coverage: a runtime error's line counts from the origin's offset") {
    ub_test::Fixture fixture;

    const auto script = ub::Script::Compile(fixture.context, "const a = 1;\nundefinedFunction();\n",
                                            {.resourceName = "offset.js", .lineOffset = 20});
    REQUIRE(script.has_value());
    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(script->Run(fixture.context).has_value());
    REQUIRE(tryCatch.HasCaught());

    const auto location = tryCatch.Location(fixture.context);
    REQUIRE(location.has_value());
    CHECK(location->scriptName == "offset.js");
    CHECK(location->lineNumber == 22);
    CHECK(location->sourceLine == std::optional<std::string>("undefinedFunction();"));
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "coverage: the quoted line has no line terminator, whichever one the source used") {
    ub_test::Fixture fixture;

    SUBCASE("a runtime error") {
        const auto script = ub::Script::Compile(fixture.context, "var x = 1;\r\nnull.y;\r\nvar z = 2;\r\n",
                                                {.resourceName = "crlf.js"});
        REQUIRE(script.has_value());
        ub::TryCatch tryCatch(fixture.iso());
        CHECK_FALSE(script->Run(fixture.context).has_value());
        const auto location = tryCatch.Location(fixture.context);
        REQUIRE(location.has_value());
        CHECK(location->lineNumber == 2);
        CHECK(location->sourceLine == std::optional<std::string>("null.y;"));
    }

    SUBCASE("a syntax error") {
        ub::TryCatch tryCatch(fixture.iso());
        CHECK_FALSE(ub::Script::Compile(fixture.context, "var x = 1;\r\nvar = 2;\r\nvar z = 3;\r\n",
                                        {.resourceName = "crlf.js"})
                        .has_value());
        const auto location = tryCatch.Location(fixture.context);
        REQUIRE(location.has_value());
        CHECK(location->lineNumber == 2);
        CHECK(location->sourceLine == std::optional<std::string>("var = 2;"));
    }
}

UNIBIND_TEST_CASE2(MESSAGE_LOCATION, TERMINATION, "coverage: a stop and a reset handler have no location") {
    ub_test::Fixture fixture;

    {
        ub::TryCatch tryCatch(fixture.iso());
        (void)ub::Evaluate(fixture.context, "null.z");
        REQUIRE(tryCatch.HasCaught());
        CHECK(tryCatch.Location(fixture.context).has_value());
        tryCatch.Reset();
        CHECK_FALSE(tryCatch.Location(fixture.context).has_value());
    }

    fixture.iso().TerminateExecution();
    {
        ub::TryCatch tryCatch(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, "for (;;) {}").has_value());
        CHECK(tryCatch.HasTerminated());
        CHECK_FALSE(tryCatch.Location(fixture.context).has_value());
    }
    fixture.iso().CancelTerminateExecution();
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "coverage: an error native code throws with no script running has no location") {
    // No script under the throw, so nothing to place it in - and "nothing" is
    // one answer, not one per engine.
    ub_test::Fixture fixture;

    {
        ub::TryCatch tryCatch(fixture.iso());
        ub::Throw(fixture.iso(), ub::ErrorKind::Error, "from native");
        REQUIRE(tryCatch.HasCaught());
        CHECK_FALSE(tryCatch.Location(fixture.context).has_value());
    }
    {
        ub::TryCatch tryCatch(fixture.iso());
        ub::Throw(fixture.iso(), ub::Integer::New(fixture.iso(), 3));
        REQUIRE(tryCatch.HasCaught());
        CHECK_FALSE(tryCatch.Location(fixture.context).has_value());
    }
}

UNIBIND_TEST_CASE(DELAYED_JOBS, "coverage: delayed work may be posted from another thread") {
    ub_test::Fixture fixture;

    Ran ran;
    ub::Isolate* isolate = &fixture.iso();
    std::thread poster([isolate, &ran] { isolate->PostDelayedJob(&RecordRan, ub::CallbackData::For(ran), 0.05); });
    poster.join();

    fixture.iso().PumpJobs();
    CHECK(ran.count.load() == 0);

    // Pump until it falls due, with a ceiling so a lost job fails rather than
    // hangs.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ran.count.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        fixture.iso().PumpJobs();
    }
    CHECK(ran.count.load() == 1);
    CHECK(ran.on == std::this_thread::get_id());
}

UNIBIND_TEST_CASE(DELAYED_JOBS, "coverage: delayed work still waiting when the isolate goes is dropped") {
    Ran due;
    Ran waiting;
    {
        ub_test::Fixture fixture;
        fixture.iso().PostDelayedJob(&RecordRan, ub::CallbackData::For(due), 0.0);
        fixture.iso().PostDelayedJob(&RecordRan, ub::CallbackData::For(waiting), 3600.0);
        // Due, but never pumped: dropped as well.
        fixture.iso().PostDelayedJob(&RecordRan, ub::CallbackData::For(due), 0.01);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    CHECK(due.count.load() == 0);
    CHECK(waiting.count.load() == 0);
}

UNIBIND_TEST_CASE(DELAYED_JOBS, "coverage: delayed work posted by a job is delayed from when the job posted it") {
    ub_test::Fixture fixture;

    std::vector<int> log;
    Tagged inner{.log = &log, .tag = 2};
    struct Poster {
        Tagged* inner = nullptr;
        std::vector<int>* log = nullptr;
    } poster{.inner = &inner, .log = &log};

    fixture.iso().PostJob(
        +[](ub::Isolate& isolate, ub::CallbackData data) {
            auto* self = data.As<Poster>();
            self->log->push_back(1);
            isolate.PostDelayedJob(&RecordTag, ub::CallbackData::For(*self->inner), 0.1);
        },
        ub::CallbackData::For(poster));

    // The pump that runs the poster does not run what it posted: that is not
    // due for a tenth of a second.
    fixture.iso().PumpJobs();
    CHECK(log == std::vector<int>{1});

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    fixture.iso().PumpJobs();
    CHECK(log == std::vector<int>{1, 2});
}

// ---------------------------------------------------------------------------
// Views (unibind/value.h)
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(ARRAY_BUFFER_VIEWS,
                  "coverage: a view over shared memory has no buffer to hand back, and still copies") {
    ub_test::Fixture fixture;

    if (!ub_test::EvalTruth(fixture.context, "typeof SharedArrayBuffer === 'function'")) {
        ub_test::ReportSkip("this realm has no SharedArrayBuffer, so script cannot make a view over one");
        return;
    }
    const auto made = ub_test::Eval(fixture.context, R"(
        (function () {
            const view = new Uint8Array(new SharedArrayBuffer(6), 2, 3);
            view.set([7, 8, 9]);
            return view;
        })()
    )");
    const auto view = made.To<ub::ArrayBufferView>();
    REQUIRE(view.has_value());
    CHECK(made.IsTypedArray());
    CHECK(ub::ByteLength(*view) == 3);
    CHECK(ub::ByteOffset(*view) == 2);

    // No type for it in this API, so no handle typed as though there were.
    CHECK_FALSE(ub::GetBuffer(fixture.context, *view).has_value());
    CHECK_FALSE(fixture.iso().HasPendingException());

    std::array<std::byte, 3> out{};
    CHECK(ub::CopyBytes(*view, out) == 3);
    CHECK(std::to_integer<int>(out[0]) == 7);
    CHECK(std::to_integer<int>(out[2]) == 9);
}

UNIBIND_TEST_CASE(ARRAY_BUFFER_VIEWS, "coverage: every way of asking for a view's buffer gives the same buffer") {
    ub_test::Fixture fixture;

    const auto buffer = ub::ArrayBuffer::New(fixture.context, 16);
    REQUIRE(buffer.has_value());
    const auto typed = ub::TypedArray::New(fixture.context, ub::ElementType::Int32, *buffer, 4, 2);
    const auto dataView = ub::DataView::New(fixture.context, *buffer, 0, 16);
    REQUIRE(typed.has_value());
    REQUIRE(dataView.has_value());

    const ub::Local<ub::ArrayBufferView> typedAsView = *typed;
    const auto viaView = ub::GetBuffer(fixture.context, typedAsView);
    const auto viaTyped = ub::GetBuffer(fixture.context, *typed);
    const auto viaDataView = ub::GetBuffer(fixture.context, *dataView);
    REQUIRE(viaView.has_value());
    REQUIRE(viaTyped.has_value());
    REQUIRE(viaDataView.has_value());
    CHECK(viaView->StrictEquals(*buffer));
    CHECK(viaTyped->StrictEquals(*buffer));
    CHECK(viaDataView->StrictEquals(*buffer));

    // The byte questions agree with the element ones, times the width.
    CHECK(ub::ByteLength(typedAsView) == 2 * sizeof(std::int32_t));
    CHECK(ub::ByteOffset(typedAsView) == 4);

    // A write through one view is read through the other: one buffer.
    ub_test::Expose(fixture.context, "ints", *typed);
    ub_test::Expose(fixture.context, "bytes", *dataView);
    CHECK(ub_test::EvalInt(fixture.context, "ints[1] = 0x01020304; bytes.getInt32(8, true)") == 0x01020304);
}

UNIBIND_TEST_CASE(ARRAY_BUFFER_VIEWS, "coverage: a DataView over an empty buffer is an empty view") {
    ub_test::Fixture fixture;

    const auto buffer = ub::ArrayBuffer::New(fixture.context, 0);
    REQUIRE(buffer.has_value());
    const auto view = ub::DataView::New(fixture.context, *buffer, 0, 0);
    REQUIRE(view.has_value());
    CHECK(ub::ByteLength(*view) == 0);
    CHECK(ub::ByteOffset(*view) == 0);
    CHECK_FALSE(ub::DataView::New(fixture.context, *buffer, 0, 1).has_value());
    CHECK_FALSE(ub::DataView::New(fixture.context, *buffer, 1, 0).has_value());
    CHECK_FALSE(fixture.iso().HasPendingException());
}

// ---------------------------------------------------------------------------
// CompileOptions::EagerCompile (unibind/script.h)
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(EAGER_COMPILE, "coverage: with a good blob, asking for eager uses the blob") {
    const std::string source = ManyFunctions();

    std::optional<std::vector<std::uint8_t>> lazyBlob;
    {
        ub_test::Fixture writer;
        const auto script = ub::Script::Compile(writer.context, source, {.resourceName = "good.js"});
        REQUIRE(script.has_value());
        lazyBlob = script->CreateCodeCache();
    }
    if (!lazyBlob) {
        ub_test::ReportSkip("this engine declined to produce a code cache for the test source");
        return;
    }

    // The blob decides when it is used: a lazy blob offered with eager asked
    // for is consumed, not refused and recompiled.
    ub_test::Fixture reader;
    const auto script = ub::Script::CompileWithCache(reader.context, source, *lazyBlob, {.resourceName = "good.js"},
                                                     ub::CompileOptions::EagerCompile);
    REQUIRE(script.has_value());
    CHECK(script->UsedCodeCache());
    const auto result = script->Run(reader.context);
    REQUIRE(result.has_value());
    CHECK(result->ToInt32(reader.context) == std::optional<std::int32_t>(41));
    CHECK(ub_test::EvalInt(reader.context, "g2(3)()") == (2 * 3) + (0 + 1 + 2) - 2);
}

UNIBIND_TEST_CASE(EAGER_COMPILE, "coverage: with no blob at all, asking for eager compiles eagerly") {
    const std::string source = ManyFunctions();
    const auto ran = BlobAfterRunningEverything(source);
    ub_test::Fixture fixture;

    const auto lazy = ub::Script::Compile(fixture.context, source, {.resourceName = "none.js"});
    REQUIRE(lazy.has_value());
    const auto lazyBlob = lazy->CreateCodeCache();

    const auto eager = ub::Script::CompileWithCache(fixture.context, source, {}, {.resourceName = "none.js"},
                                                    ub::CompileOptions::EagerCompile);
    REQUIRE(eager.has_value());
    CHECK_FALSE(eager->UsedCodeCache());
    const auto eagerBlob = eager->CreateCodeCache();
    if (!lazyBlob || !eagerBlob || !ran) {
        ub_test::ReportSkip("this engine declined to produce a code cache for the test source");
        return;
    }
    MESSAGE("lazy blob ", lazyBlob->size(), " bytes, after running everything ", ran->size(),
            " bytes, eager blob from an empty cache ", eagerBlob->size(), " bytes");
    CHECK(eagerBlob->size() > lazyBlob->size());
    CHECK(eagerBlob->size() >= ran->size());
}

UNIBIND_TEST_CASE(EAGER_COMPILE,
                  "coverage: an eager compile of source that does not compile fails as any compile does") {
    ub_test::Fixture fixture;

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Script::Compile(fixture.context, "function (", {}, ub::CompileOptions::EagerCompile).has_value());
    REQUIRE(tryCatch.HasCaught());
    CHECK(ub_test::EvalTruth(fixture.context, "true"));
    tryCatch.Reset();

    // Eager compiles bodies up front, so an early error in a body that never
    // runs is still an error - as it is lazily, where the body is checked too.
    CHECK_FALSE(ub::Script::Compile(fixture.context, "function neverCalled() { return 1 +; }", {},
                                    ub::CompileOptions::EagerCompile)
                    .has_value());
    CHECK(tryCatch.HasCaught());
}

// ---------------------------------------------------------------------------
// The inspector (unibind/inspector.h)
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(INSPECTOR, "coverage: dispatches run in the order they were requested") {
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return;
    }
    ub_test::Fixture fixture;
    QuietClient client;
    auto inspector = ub::Inspector::New(fixture.iso(), client);
    REQUIRE(inspector != nullptr);

    std::vector<int> log;
    std::array<Tagged, 4> requests{};
    for (int i = 0; i < 4; ++i) {
        requests.at(i) = Tagged{.log = &log, .tag = i};
    }
    const std::shared_ptr<ub::InspectorDispatcher> dispatcher = inspector->Dispatcher();
    std::atomic<int> taken{0};
    std::thread requester([dispatcher, &requests, &taken] {
        for (Tagged& request : requests) {
            taken += dispatcher->RequestDispatch(&RecordTag, ub::CallbackData::For(request)) ? 1 : 0;
        }
    });
    requester.join();
    CHECK(taken == 4);

    fixture.iso().PumpJobs();
    CHECK(log == std::vector<int>{0, 1, 2, 3});
    // Each ran once, whichever of the interrupt and the pump reached it.
    CHECK(ub_test::EvalInt(fixture.context, "1") == 1);
    fixture.iso().PumpJobs();
    CHECK(log.size() == 4);
}

UNIBIND_TEST_CASE(INSPECTOR, "coverage: dispatches still waiting when the inspector goes are dropped") {
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return;
    }
    ub_test::Fixture fixture;
    QuietClient client;
    Ran ran;
    {
        auto inspector = ub::Inspector::New(fixture.iso(), client);
        REQUIRE(inspector != nullptr);
        CHECK(inspector->Dispatcher()->RequestDispatch(&RecordRan, ub::CallbackData::For(ran)));
    }
    // Neither the pump nor a script's interrupt check runs it now.
    fixture.iso().PumpJobs();
    CHECK(ub_test::EvalInt(fixture.context, "2") == 2);
    fixture.iso().PumpJobs();
    CHECK(ran.count.load() == 0);

    // And the isolate takes a new inspector afterwards.
    auto again = ub::Inspector::New(fixture.iso(), client);
    CHECK(again != nullptr);
}

UNIBIND_TEST_CASE(INSPECTOR, "coverage: a stopped session no longer pauses script") {
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return;
    }
    ub_test::Fixture fixture;
    QuietClient client;
    auto inspector = ub::Inspector::New(fixture.iso(), client);
    REQUIRE(inspector != nullptr);
    inspector->ContextCreated(fixture.context, "main");
    auto session = inspector->Connect();
    REQUIRE(session != nullptr);
    client.session = session.get();

    session->DispatchProtocolMessage(R"({"id":1,"method":"Debugger.enable"})");
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 1") == 1);
    CHECK(client.pauses == 1);

    session->Stop();
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 2") == 2);
    CHECK(client.pauses == 1);

    session.reset();
    inspector->ContextDestroyed(fixture.context);
}

UNIBIND_TEST_CASE(INSPECTOR, "coverage: with no answer from the client, a script's URL is its resource name") {
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return;
    }
    ub_test::Fixture fixture;
    QuietClient client;
    auto inspector = ub::Inspector::New(fixture.iso(), client);
    REQUIRE(inspector != nullptr);
    inspector->ContextCreated(fixture.context, "main");
    auto session = inspector->Connect();
    REQUIRE(session != nullptr);
    client.session = session.get();

    session->DispatchProtocolMessage(R"({"id":1,"method":"Debugger.enable"})");
    const auto ran = ub::Evaluate(fixture.context, "5", {.resourceName = "plain-name.js"});
    REQUIRE(ran.has_value());
    CHECK(client.Saw("\"url\":\"plain-name.js\""));

    session.reset();
    inspector->ContextDestroyed(fixture.context);
}
