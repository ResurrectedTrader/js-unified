/// \file
/// Bugs that were found, each pinned by the case that showed it. Every case
/// here failed - or crashed - on the code before its fix, and passes on every
/// backend after it. The comment on each says what it caught.

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/harness.h"

namespace {

/// The location of what `source` throws when run, compiled under `origin`.
std::optional<ub::MessageLocation> LocationOfThrow(ub_test::Fixture& fixture, std::string_view source,
                                                   const ub::ScriptOrigin& origin) {
    const auto script = ub::Script::Compile(fixture.context, source, origin);
    REQUIRE(script.has_value());
    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(script->Run(fixture.context).has_value());
    REQUIRE(tryCatch.HasCaught());
    return tryCatch.Location(fixture.context);
}

}  // namespace

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "regressions: a location never quotes another script of the same name") {
    // The quoted line has to come from the script that raised the error. Two
    // scripts compiled under one name - and every script compiled without an
    // origin shares the default one - are still two scripts. A backend that
    // cannot tell which of them threw quotes neither; quoting the other one is
    // the answer that must not happen.
    ub_test::Fixture fixture;

    const auto thrower = ub::Script::Compile(fixture.context, "var a = 1;\nthrow new Error('first');");
    REQUIRE(thrower.has_value());
    const auto run = [&]() {
        ub::TryCatch tryCatch(fixture.iso());
        CHECK_FALSE(thrower->Run(fixture.context).has_value());
        REQUIRE(tryCatch.HasCaught());
        const auto location = tryCatch.Location(fixture.context);
        REQUIRE(location.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
        CHECK(location->lineNumber == 2);
        return location->sourceLine;  // NOLINT(bugprone-unchecked-optional-access) - as above
    };

    {
        const auto bystander = ub::Script::Compile(fixture.context, "var b = 2;\nvar c = 3;");
        REQUIRE(bystander.has_value());
        const auto quoted = run();
        CHECK((!quoted || *quoted == "throw new Error('first');"));
    }

    // Once the other script is gone, nothing is ambiguous and the line is
    // quoted on every backend.
    fixture.iso().RequestGarbageCollection();
    CHECK(run() == std::optional<std::string>("throw new Error('first');"));
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION,
                  "regressions: a location quotes the line under every line terminator the language has") {
    // JavaScript ends a line at LF, CR, CRLF, U+2028 and U+2029, and both
    // engines number lines that way - so the quoted line has to be cut the same
    // way, or the number and the text disagree.
    ub_test::Fixture fixture;

    for (const std::string_view terminator : {std::string_view("\n"), std::string_view("\r"), std::string_view("\r\n"),
                                              std::string_view("\xE2\x80\xA8"), std::string_view("\xE2\x80\xA9")}) {
        CAPTURE(terminator.size());
        std::string source = "var a = 1;";
        source += terminator;
        source += "var b = 2;";
        source += terminator;
        source += "null.property;";
        source += terminator;
        source += "var c = 3;";
        const std::string name = "terminators-" + std::to_string(terminator.size()) + "-" +
                                 std::to_string(static_cast<std::uint8_t>(terminator.back())) + ".js";
        const auto location = LocationOfThrow(fixture, source, {.resourceName = name});
        REQUIRE(location.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
        CHECK(location->lineNumber == 3);
        CHECK(location->sourceLine == std::optional<std::string>("null.property;"));
        // NOLINTEND(bugprone-unchecked-optional-access)
    }
}

UNIBIND_TEST_CASE2(MESSAGE_LOCATION, CODE_CACHE,
                   "regressions: a location quotes a script that came out of a code cache") {
    // A script made from a blob was never compiled from its text in this
    // process, and its line has to be quoted all the same.
    ub_test::Fixture fixture;

    constexpr std::string_view SOURCE = "var a = 1;\nnull.property;";
    const ub::ScriptOrigin origin{.resourceName = "cached.js"};
    std::vector<std::uint8_t> blob;
    {
        const auto first = ub::Script::Compile(fixture.context, SOURCE, origin);
        REQUIRE(first.has_value());
        const auto made = first->CreateCodeCache();
        REQUIRE(made.has_value());
        blob = *made;  // NOLINT(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    }
    const auto second = ub::Script::CompileWithCache(fixture.context, SOURCE, blob, origin);
    REQUIRE(second.has_value());
    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(second->Run(fixture.context).has_value());
    REQUIRE(tryCatch.HasCaught());
    const auto location = tryCatch.Location(fixture.context);
    REQUIRE(location.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(location->lineNumber == 2);
    CHECK(location->sourceLine == std::optional<std::string>("null.property;"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "regressions: a column offset moves the first line's columns and no other's") {
    // `ScriptOrigin::columnOffset` says the source began that far into its
    // first line - an inline script in a page, a template's expression. Every
    // column on that line moves by it, and no column on any later line does.
    ub_test::Fixture fixture;

    const auto firstPlain = LocationOfThrow(fixture, "null.property;", {.resourceName = "col.js"});
    const auto firstMoved = LocationOfThrow(fixture, "null.property;", {.resourceName = "col.js", .columnOffset = 10});
    const auto secondPlain = LocationOfThrow(fixture, "var a = 1;\nnull.property;", {.resourceName = "col.js"});
    const auto secondMoved =
        LocationOfThrow(fixture, "var a = 1;\nnull.property;", {.resourceName = "col.js", .columnOffset = 10});
    REQUIRE(firstPlain.has_value());
    REQUIRE(firstMoved.has_value());
    REQUIRE(secondPlain.has_value());
    REQUIRE(secondMoved.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(firstMoved->columnNumber == firstPlain->columnNumber + 10);
    CHECK(secondMoved->columnNumber == secondPlain->columnNumber);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

namespace {

std::atomic<int> g_delayedRuns{0};

void CountDelayedRun(ub::Isolate& /*isolate*/, ub::CallbackData /*data*/) {
    g_delayedRuns.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

UNIBIND_TEST_CASE(DELAYED_JOBS, "regressions: a delay too long for the clock is a long delay, not none") {
    // The delay is a double and the steady clock counts integer nanoseconds,
    // which run out a little under three hundred years from now. A delay past
    // that - infinity included - must still be a floor nothing reaches, not an
    // overflow that lands in the past and runs at the next pump.
    ub_test::Fixture fixture;
    g_delayedRuns = 0;

    auto& isolate = fixture.iso();
    isolate.PostDelayedJob(&CountDelayedRun, {}, std::numeric_limits<double>::infinity());
    isolate.PostDelayedJob(&CountDelayedRun, {}, 1e12);
    isolate.PostDelayedJob(&CountDelayedRun, {}, std::numeric_limits<double>::max());
    isolate.PostDelayedJob(&CountDelayedRun, {}, 9.3e9);
    isolate.PumpJobs();
    CHECK(g_delayedRuns.load() == 0);
}

UNIBIND_TEST_CASE(OBJECTS, "regressions: defining a property an object refuses is false, not a throw") {
    // V8's `DefineOwnProperty` answers the way `Reflect.defineProperty` does:
    // a definition the object refuses - it is frozen, or the property is not
    // configurable - is `false`, with nothing thrown. Empty would mean script
    // threw, and nothing did.
    ub_test::Fixture fixture;

    const auto frozen = ub_test::Eval(fixture.context, "Object.freeze({ kept: 1 })").To<ub::Object>();
    REQUIRE(frozen.has_value());

    ub::TryCatch tryCatch(fixture.iso());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    const auto added = frozen->DefineOwnProperty(fixture.context, ub_test::Str(fixture.iso(), "added"),
                                                 ub::Integer::New(fixture.iso(), 2));
    CHECK(added == std::optional<bool>(false));
    CHECK_FALSE(tryCatch.HasCaught());
    tryCatch.Reset();

    const auto changed = frozen->DefineOwnProperty(fixture.context, ub_test::Str(fixture.iso(), "kept"),
                                                   ub::Integer::New(fixture.iso(), 3));
    CHECK(changed == std::optional<bool>(false));
    CHECK_FALSE(tryCatch.HasCaught());
    // NOLINTEND(bugprone-unchecked-optional-access)
}

namespace {

void ReadSeven(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(std::int32_t{7});
}

}  // namespace

UNIBIND_TEST_CASE(OBJECT_ACCESSORS, "regressions: an accessor an object refuses is false, not a throw or a lie") {
    // `SetAccessor` on an object that cannot take the property must say so the
    // way `DefineOwnProperty` does, and must not have installed anything.
    ub_test::Fixture fixture;

    const auto frozen = ub_test::Eval(fixture.context, "Object.freeze({})").To<ub::Object>();
    REQUIRE(frozen.has_value());

    ub::TryCatch tryCatch(fixture.iso());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(frozen->SetAccessor(fixture.context, "seven", &ReadSeven) == std::optional<bool>(false));
    CHECK_FALSE(tryCatch.HasCaught());
    ub_test::Expose(fixture.context, "frozen", *frozen);
    // NOLINTEND(bugprone-unchecked-optional-access)
    CHECK(ub_test::EvalTruth(fixture.context, "!('seven' in frozen) && Object.isFrozen(frozen)"));
}

UNIBIND_TEST_CASE(OBJECT_ACCESSORS, "regressions: an accessor set on a proxy is defined through it") {
    // A proxy is an object like any other to this API, and defining a property
    // on one goes through its `defineProperty` trap - or, with none, onto its
    // target. Answering true and installing nothing is the one wrong answer.
    ub_test::Fixture fixture;

    const auto proxy = ub_test::Eval(fixture.context, "globalThis.target = {}; new Proxy(target, {})").To<ub::Object>();
    REQUIRE(proxy.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(proxy->SetAccessor(fixture.context, "seven", &ReadSeven) == std::optional<bool>(true));
    ub_test::Expose(fixture.context, "proxy", *proxy);
    // NOLINTEND(bugprone-unchecked-optional-access)
    CHECK(ub_test::EvalInt(fixture.context, "proxy.seven") == 7);
    CHECK(ub_test::EvalInt(fixture.context, "target.seven") == 7);
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "regressions: an error thrown away from where it was made is placed at the throw") {
    // V8 places a caught exception where it was thrown, whatever it is - an
    // Error made on one line and thrown on another is reported at the `throw`,
    // and so is its quoted line. The other backend must not answer with where
    // the Error was made.
    ub_test::Fixture fixture;

    const auto location = LocationOfThrow(fixture, "const e = new Error('made');\nvar x = 1;\nthrow e;",
                                          {.resourceName = "made-and-thrown.js"});
    REQUIRE(location.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(location->scriptName == "made-and-thrown.js");
    CHECK(location->lineNumber == 3);
    CHECK(location->sourceLine == std::optional<std::string>("throw e;"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

namespace {

void ThrowFromNative(const ub::CallbackInfo& info) {
    info.Throw(ub::ErrorKind::RangeError, "from native");
}

}  // namespace

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "regressions: an error a native throws under script is placed at the call") {
    // The same rule from the other side: an error made and thrown by native
    // code has no script position of its own, and it is placed where script
    // was when it arrived - the call into the native.
    ub_test::Fixture fixture;

    const auto function = ub::Function::New(fixture.context, &ThrowFromNative);
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "boom", *function);  // NOLINT(bugprone-unchecked-optional-access)

    const auto location = LocationOfThrow(fixture, "var a = 1;\nboom();", {.resourceName = "native-throw.js"});
    REQUIRE(location.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(location->scriptName == "native-throw.js");
    CHECK(location->lineNumber == 2);
    CHECK(location->sourceLine == std::optional<std::string>("boom();"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

UNIBIND_TEST_CASE(BINARY_DATA, "regressions: a typed array at an offset its elements cannot start at is empty") {
    // A typed array's byte offset must be a multiple of its element size -
    // `new Int32Array(buffer, 1)` is a RangeError in script. Asked from native
    // code, the answer is empty, as for any view that does not fit; it must not
    // be the engine's own check, which on one engine ends the process.
    ub_test::Fixture fixture;

    const auto buffer = ub::ArrayBuffer::New(fixture.context, 32);
    REQUIRE(buffer.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK_FALSE(ub::TypedArray::New(fixture.context, ub::ElementType::Int32, *buffer, 1, 1).has_value());
    CHECK_FALSE(ub::TypedArray::New(fixture.context, ub::ElementType::Uint16, *buffer, 3, 2).has_value());
    CHECK_FALSE(ub::TypedArray::New(fixture.context, ub::ElementType::Float64, *buffer, 4, 1).has_value());
    CHECK(ub::TypedArray::New(fixture.context, ub::ElementType::Float64, *buffer, 8, 3).has_value());
    CHECK(ub::TypedArray::New(fixture.context, ub::ElementType::Uint8, *buffer, 3, 2).has_value());
    // NOLINTEND(bugprone-unchecked-optional-access)
    CHECK_FALSE(fixture.iso().HasPendingException());
}

UNIBIND_TEST_CASE(BINARY_DATA, "regressions: a typed array over a detached buffer is refused like a DataView") {
    // `new Uint8Array(detached)` is a TypeError in script, and `DataView::New`
    // over one is already empty. A typed array must not be the one view that
    // can be made over nothing.
    ub_test::Fixture fixture;

    const auto buffer =
        ub_test::Eval(fixture.context, "const b = new ArrayBuffer(8); b.transfer(); b").To<ub::ArrayBuffer>();
    REQUIRE(buffer.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(ub::ByteLength(*buffer) == 0);
    CHECK_FALSE(ub::DataView::New(fixture.context, *buffer, 0, 0).has_value());
    CHECK_FALSE(ub::TypedArray::New(fixture.context, ub::ElementType::Uint8, *buffer, 0, 0).has_value());
    // NOLINTEND(bugprone-unchecked-optional-access)
    CHECK_FALSE(fixture.iso().HasPendingException());
}

UNIBIND_TEST_CASE(BINARY_DATA, "regressions: a typed array of a kind the API had no name for is not read as doubles") {
    // Script can make a `BigInt64Array`, a `BigUint64Array` and a
    // `Float16Array`. Reporting any of them as `Float64` turned
    // `CopyElements<double>` into a reinterpretation of their bytes - exactly
    // the conversion it promises never to make.
    ub_test::Fixture fixture;

    struct Kind {
        std::string_view source;
        ub::ElementType type;
    };
    for (const Kind kind :
         {Kind{.source = "new BigInt64Array([1n, -2n])", .type = ub::ElementType::BigInt64},
          Kind{.source = "new BigUint64Array([1n, 2n])", .type = ub::ElementType::BigUint64},
          Kind{.source = "new Float16Array([1.5, 2.5, 3.5, 4.5])", .type = ub::ElementType::Float16}}) {
        CAPTURE(kind.source);
        const auto view = ub_test::Eval(fixture.context, kind.source).To<ub::TypedArray>();
        REQUIRE(view.has_value());
        std::array<double, 4> out{};
        // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
        CHECK(ub::GetElementType(*view) == kind.type);
        CHECK(ub::CopyElements(*view, std::span<double>(out)) == 0);
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    // And each kind has a name now, so it reads at its own width and can be
    // made from native data like any other.
    const auto bigs = ub_test::Eval(fixture.context, "new BigInt64Array([1n, -2n])").To<ub::TypedArray>();
    REQUIRE(bigs.has_value());
    std::array<std::int64_t, 2> read{};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(ub::CopyElements(*bigs, std::span<std::int64_t>(read)) == 2);
    CHECK(read == std::array<std::int64_t, 2>{1, -2});

    const std::array<std::uint64_t, 2> written{7, 18446744073709551615ULL};
    const auto made = ub::TypedArray::New(fixture.context, std::span<const std::uint64_t>(written));
    REQUIRE(made.has_value());
    ub_test::Expose(fixture.context, "made", *made);  // NOLINT(bugprone-unchecked-optional-access)
    CHECK(ub_test::EvalTruth(fixture.context,
                             "made instanceof BigUint64Array && made[0] === 7n && made[1] === 2n ** 64n - 1n"));
}

namespace {

std::vector<ub::StackFrame> g_capturedFrames;

void CaptureCaller(const ub::CallbackInfo& info) {
    g_capturedFrames = ub::CaptureStackFrames(info.GetIsolate());
}

/// Whether any frame names the engine's own built-in code rather than script.
bool NamesBuiltinCode(const std::vector<ub::StackFrame>& frames) {
    for (const ub::StackFrame& frame : frames) {
        if (frame.scriptName != "frames.js") {
            return true;
        }
    }
    return false;
}

}  // namespace

UNIBIND_TEST_CASE2(STACK_FRAMES, MESSAGE_LOCATION,
                   "regressions: frames and locations are script's, not the engine's built-ins") {
    // A built-in such as `Array.prototype.reduce` is JavaScript inside one
    // engine and native code inside the other. Either way it is not the
    // embedder's script: a frame read off a stack names script, and an error a
    // built-in throws is placed at the script that called it.
    ub_test::Fixture fixture;

    {
        const auto script = ub::Script::Compile(
            fixture.context, "function f() { return [].reduce((x, y) => x); }\nf();", {.resourceName = "frames.js"});
        REQUIRE(script.has_value());
        ub::TryCatch tryCatch(fixture.iso());
        CHECK_FALSE(script->Run(fixture.context).has_value());
        REQUIRE(tryCatch.HasCaught());
        const auto frames = tryCatch.StackFrames(fixture.context);
        REQUIRE(frames.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
        CHECK_FALSE(NamesBuiltinCode(*frames));
        REQUIRE_FALSE(frames->empty());
        CHECK(frames->front().functionName == "f");
        const auto location = tryCatch.Location(fixture.context);
        REQUIRE(location.has_value());
        CHECK(location->scriptName == "frames.js");
        CHECK(location->lineNumber == 1);
        // NOLINTEND(bugprone-unchecked-optional-access)
    }

    const auto capture = ub::Function::New(fixture.context, &CaptureCaller);
    REQUIRE(capture.has_value());
    ub_test::Expose(fixture.context, "capture", *capture);  // NOLINT(bugprone-unchecked-optional-access)
    const auto script = ub::Script::Compile(fixture.context, "[1].forEach(function each() { capture(); });",
                                            {.resourceName = "frames.js"});
    REQUIRE(script.has_value());
    REQUIRE(script->Run(fixture.context).has_value());  // NOLINT(bugprone-unchecked-optional-access)
    CHECK_FALSE(NamesBuiltinCode(g_capturedFrames));
    CHECK(g_capturedFrames.size() == 2);
}

namespace {
struct ProbeClient final : ub::InspectorClient {
    void SendProtocolMessage(std::string_view message) override { messages.emplace_back(message); }
    void RunMessageLoopOnPause() override {}
    void QuitMessageLoopOnPause() override {}
    std::vector<std::string> messages;
};
}  // namespace

UNIBIND_TEST_CASE(INSPECTOR, "regressions: a realm announced twice is withdrawn by one ContextDestroyed") {
    // Announcing a realm again - to rename it, say - must not leave DevTools
    // a second, stale entry for it that nothing can take back: once the realm
    // is withdrawn, DevTools no longer offers it under any name.
    ub_test::Fixture fixture;
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return;
    }
    ProbeClient client;
    const auto inspector = ub::Inspector::New(fixture.iso(), client);
    REQUIRE(inspector != nullptr);
    auto other = ub::Context::New(fixture.iso());
    REQUIRE(other.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    inspector->ContextCreated(*other, "other");
    inspector->ContextCreated(fixture.context, "first name");
    inspector->ContextCreated(fixture.context, "second name");
    inspector->ContextDestroyed(fixture.context);
    // NOLINTEND(bugprone-unchecked-optional-access)

    const auto session = inspector->Connect();
    REQUIRE(session != nullptr);
    session->DispatchProtocolMessage(R"({"id":1,"method":"Runtime.enable"})");
    int offered = 0;
    for (const std::string& message : client.messages) {
        if (message.find("Runtime.executionContextCreated") != std::string::npos) {
            ++offered;
            CHECK(message.find("\"other\"") != std::string::npos);
        }
    }
    CHECK(offered == 1);
}

namespace {

struct Declined {
    int value = 0;
};

std::unique_ptr<Declined> DeclineQuietly(const ub::CallbackInfo& /*info*/) {
    return nullptr;
}

}  // namespace

UNIBIND_TEST_CASE(CLASSES, "regressions: a constructor that declines without throwing still makes no instance") {
    // A `Class<T>` promises that what comes out of it carries a `T`. A
    // constructor that hands back null has declined to make one, whether or
    // not it threw first - and `new` must then fail, rather than hand script
    // an object with no native behind it that every method will refuse.
    ub_test::Fixture fixture;

    const auto cls = ub::Class<Declined>::New(fixture.iso(), "Declined");
    cls.Construct<&DeclineQuietly>();
    const auto constructor = cls.GetConstructor(fixture.context);
    REQUIRE(constructor.has_value());
    ub_test::Expose(fixture.context, "Declined", *constructor);  // NOLINT(bugprone-unchecked-optional-access)

    CHECK(ub_test::EvalText(fixture.context,
                            "(() => { try { new Declined(); return 'made'; } "
                            "catch (e) { return e instanceof Error ? 'refused' : 'odd'; } })()") == "refused");

    // The same for a class that also answers to a plain call.
    const auto callable = ub::Class<Declined>::New(fixture.iso(), "DeclinedToo");
    callable.ConstructOrCall<&DeclineQuietly>();
    const auto function = callable.GetConstructor(fixture.context);
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "DeclinedToo", *function);  // NOLINT(bugprone-unchecked-optional-access)
    CHECK(ub_test::EvalText(fixture.context,
                            "(() => { try { DeclinedToo(); return 'made'; } "
                            "catch (e) { return e instanceof Error ? 'refused' : 'odd'; } })()") == "refused");
}

namespace {

std::unique_ptr<Declined> MakeThenStarve(const ub::CallbackInfo& /*info*/) {
    auto made = std::make_unique<Declined>();
    // The next allocation is the constructor machinery's own, taking the
    // native over - and it fails.
    ub_test::FailNextAllocations(1);
    return made;
}

}  // namespace

UNIBIND_TEST_CASE(CLASSES,
                  "regressions: running out of memory taking a constructed native over is a failed construction") {
    ub_test::Fixture fixture;

    const auto cls = ub::Class<Declined>::New(fixture.iso(), "Starved");
    cls.Construct<&MakeThenStarve>();
    const auto constructor = cls.GetConstructor(fixture.context);
    REQUIRE(constructor.has_value());
    ub_test::Expose(fixture.context, "Starved", *constructor);  // NOLINT(bugprone-unchecked-optional-access)

    ub::TryCatch tryCatch(fixture.iso());
    const auto result = ub::Evaluate(fixture.context, "new Starved()");
    const long long fired = ub_test::StopFailingAllocations();
    if (fired == 0) {
        ub_test::ReportSkip("the constructor machinery made no C++ allocation here");
        return;
    }
    CHECK_FALSE(result.has_value());
    CHECK(tryCatch.HasCaught());
}

namespace {

std::size_t g_zeroLimitFrames = 99;

void CaptureNoFrames(const ub::CallbackInfo& info) {
    g_zeroLimitFrames = ub::CaptureStackFrames(info.GetIsolate(), 0).size();
}

}  // namespace

UNIBIND_TEST_CASE(STACK_FRAMES, "regressions: capturing at most no frames captures none") {
    // `limit` caps the frames collected, and a cap of zero is a cap: no frames.
    // It is not a request for all of them, which one backend read it as.
    ub_test::Fixture fixture;

    const auto capture = ub::Function::New(fixture.context, &CaptureNoFrames);
    REQUIRE(capture.has_value());
    ub_test::Expose(fixture.context, "captureNone", *capture);  // NOLINT(bugprone-unchecked-optional-access)
    g_zeroLimitFrames = 99;
    CHECK(ub_test::EvalInt(fixture.context,
                           "function a() { captureNone(); return 1; } function b() { return a(); } b()") == 1);
    CHECK(g_zeroLimitFrames == 0);
}

namespace {
std::string g_probeNames;
void ProbeNames(const ub::CallbackInfo& info) {
    const auto frames = ub::CaptureStackFrames(info.GetIsolate(), 1);
    g_probeNames += "[" + (frames.empty() ? std::string("?") : frames.front().functionName) + "] ";
}
}  // namespace

UNIBIND_TEST_CASE(STACK_FRAMES, "regressions: an anonymous function's frame has no name, and a guessed one no prefix") {
    // `StackFrame::functionName` is empty for an anonymous function. One engine
    // hands out the name it guessed for one instead, in a notation of its own
    // - `g/<` for "an anonymous function inside g", `outer/obj.n` for a
    // function assigned to `obj.n` inside `outer` - where the other names what
    // was assigned and nothing for what was not.
    ub_test::Fixture fixture;

    const auto probe = ub::Function::New(fixture.context, &ProbeNames);
    REQUIRE(probe.has_value());
    ub_test::Expose(fixture.context, "p", *probe);  // NOLINT(bugprone-unchecked-optional-access)
    g_probeNames.clear();
    (void)ub_test::Eval(fixture.context, R"(
        function g() { [1].map(x => p()); [1].map(function () { p(); }); }
        g();
        var obj = {};
        function outer() { obj.n = function () { p(); }; obj.n(); return function () { p(); }; }
        outer()();
        const h = () => p(); h();
        function named() { p(); } named();
        0)");
    CHECK(g_probeNames == "[] [] [obj.n] [] [h] [named] ");
}

UNIBIND_TEST_CASE(EXCEPTIONS, "regressions: a caught value's message is read without running script, whatever it is") {
    // `TryCatch::Message` is the text of what was caught. Reading it must not
    // run script - a thrown object's own `toString` is script, and may throw or
    // do anything else - and it must answer for anything that can be thrown,
    // including values that will not convert to a string at all.
    ub_test::Fixture fixture;

    const auto messageOf = [&](std::string_view source) {
        ub::TryCatch tryCatch(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, source).has_value());
        REQUIRE(tryCatch.HasCaught());
        return tryCatch.Message(fixture.context);
    };

    const auto symbol = messageOf("throw Symbol('thrown symbol')");
    REQUIRE(symbol.has_value());
    CHECK(symbol->find("Symbol(thrown symbol)") != std::string::npos);  // NOLINT(bugprone-unchecked-optional-access)
    CHECK(messageOf("throw Object.create(null)").has_value());
    CHECK(messageOf("throw { toString() { throw new Error('from toString'); } }").has_value());

    ub_test::Eval(fixture.context, "globalThis.conversions = 0");
    CHECK(messageOf("throw { toString() { ++conversions; return 'converted'; } }").has_value());
    CHECK(ub_test::EvalInt(fixture.context, "conversions") == 0);

    // An Error still reads as its name and message.
    const auto renamed = messageOf("throw Object.assign(new Error('the text'), { name: 'Custom' })");
    REQUIRE(renamed.has_value());
    CHECK(renamed->find("Custom: the text") != std::string::npos);  // NOLINT(bugprone-unchecked-optional-access)
}

namespace {

void NamedMethod(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(std::int32_t{2});
}

void NamedGetter(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(std::int32_t{3});
}

struct Named {};

void NamedClassMethod(Named& /*self*/, const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(std::int32_t{4});
}

}  // namespace

UNIBIND_TEST_CASE2(TEMPLATES, CLASSES, "regressions: a name a template or class declares is text, whatever its bytes") {
    // Every name a template, a class or `SetAccessor` declares is UTF-8, as
    // every other string handed to this API is, and reaches script as that
    // text: `café` is four characters on both engines, not five on one. Bytes
    // that are not UTF-8 are decoded as `String::NewFromUtf8` decodes them -
    // a name is text an embedder may have read from anywhere - and are not a
    // reason to end the process.
    ub_test::Fixture fixture;
    auto& isolate = fixture.iso();

    const auto shape = ub::ObjectTemplate::New(isolate);
    shape.Set("caf\xC3\xA9", ub::Constant(std::int32_t{1}));
    shape.Set("na\xC3\xAFve", &NamedMethod);
    shape.SetAccessor(
        "\xC3\xBC"
        "ber",
        &NamedGetter);
    shape.Set(std::string_view("bad\xFF", 4), ub::Constant(std::int32_t{5}));
    shape.Set("text", ub::Constant("r\xC3\xA9sum\xC3\xA9"));
    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "shaped", *instance);  // NOLINT(bugprone-unchecked-optional-access)

    const auto cls = ub::Class<Named>::New(isolate, "Na\xC3\xAFve");
    cls.Method<&NamedClassMethod>("r\xC3\xA9sum\xC3\xA9");
    const auto constructor = cls.GetConstructor(fixture.context);
    REQUIRE(constructor.has_value());
    ub_test::Expose(fixture.context, "NamedClass", *constructor);  // NOLINT(bugprone-unchecked-optional-access)

    CHECK(ub_test::EvalInt(fixture.context, "shaped['caf\u00e9']") == 1);
    CHECK(ub_test::EvalInt(fixture.context, "shaped['na\u00efve']()") == 2);
    CHECK(ub_test::EvalText(fixture.context, "shaped['na\u00efve'].name") == "na\xC3\xAFve");
    CHECK(ub_test::EvalInt(fixture.context, "shaped['\u00fcber']") == 3);
    CHECK(ub_test::EvalInt(fixture.context, "shaped['bad\ufffd']") == 5);
    CHECK(ub_test::EvalText(fixture.context, "shaped.text") == "r\xC3\xA9sum\xC3\xA9");
    CHECK(ub_test::EvalText(fixture.context, "NamedClass.name") == "Na\xC3\xAFve");
    CHECK(ub_test::EvalTruth(fixture.context, "typeof NamedClass.prototype['r\u00e9sum\u00e9'] === 'function'"));

    const auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(object->SetAccessor(fixture.context, std::string_view("got\xFF", 4), &NamedGetter) ==
          std::optional<bool>(true));
    ub_test::Expose(fixture.context, "plain", *object);
    // NOLINTEND(bugprone-unchecked-optional-access)
    CHECK(ub_test::EvalInt(fixture.context, "plain['got\ufffd']") == 3);
}
