/// \file
/// Throwing from native code, catching what script throws, and reading what
/// came out.
///
/// The engines word their built-in errors differently and format their stacks
/// differently, so these cases assert the shape: the kind of error, the text
/// the embedder supplied, and that a stack exists. See docs/testing.md.

#include <array>
#include <optional>
#include <string>

#include "support/harness.h"

namespace {

void ThrowsRangeError(const ub::CallbackInfo& info) {
    info.Throw(ub::ErrorKind::RangeError, "out of range");
}

void ThrowsAValue(const ub::CallbackInfo& info) {
    ub::Throw(info.GetIsolate(), ub::Integer::New(info.GetIsolate(), 4242));
}

void Boom(const ub::CallbackInfo& info) {
    info.Throw(ub::ErrorKind::Error, "boom");
}

}  // namespace

TEST_CASE("exceptions: script throwing is caught and the value comes back") {
    ub_test::Fixture fixture;

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "throw 42").has_value());
    REQUIRE(tryCatch.HasCaught());

    const auto thrown = tryCatch.Exception();
    REQUIRE(thrown.Is<ub::Integer>());
    CHECK(thrown.To<ub::Integer>()->Int32Value() == 42);
}

TEST_CASE("exceptions: a native throw arrives in script as the kind that was asked for") {
    ub_test::Fixture fixture;

    const auto function = ub::Function::New(fixture.context, &ThrowsRangeError);
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "misbehave", *function);

    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            try { misbehave(); return false; }
            catch (e) { return e instanceof RangeError && e.message === 'out of range'; }
        })()
    )"));
}

TEST_CASE("exceptions: native can throw a value that is not an error") {
    ub_test::Fixture fixture;

    const auto function = ub::Function::New(fixture.context, &ThrowsAValue);
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "misbehave", *function);

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "misbehave()").has_value());
    REQUIRE(tryCatch.HasCaught());

    const auto thrown = tryCatch.Exception().To<ub::Integer>();
    REQUIRE(thrown.has_value());
    CHECK(thrown->Int32Value() == 4242);
}

TEST_CASE("exceptions: an error object can be made without being thrown") {
    ub_test::Fixture fixture;

    struct Case {
        ub::ErrorKind kind;
        const char* constructorName;
    };
    const std::array<Case, 5> cases{{
        {ub::ErrorKind::Error, "Error"},
        {ub::ErrorKind::TypeError, "TypeError"},
        {ub::ErrorKind::RangeError, "RangeError"},
        {ub::ErrorKind::ReferenceError, "ReferenceError"},
        {ub::ErrorKind::SyntaxError, "SyntaxError"},
    }};

    for (const Case& one : cases) {
        const std::string expected = one.constructorName;
        CAPTURE(expected);
        const auto error = ub::MakeError(fixture.context, one.kind, "made, not thrown");
        REQUIRE(error.has_value());
        ub_test::Expose(fixture.context, "err", *error);

        CHECK(ub_test::EvalText(fixture.context, "err.constructor.name") == expected);
        CHECK(ub_test::EvalText(fixture.context, "err.message") == "made, not thrown");
        CHECK(ub_test::EvalTruth(fixture.context, "err instanceof Error"));
    }
}

TEST_CASE("exceptions: the message keeps the text the embedder wrote") {
    ub_test::Fixture fixture;

    const auto function = ub::Function::New(fixture.context, &Boom);
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "boom", *function);

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "boom()").has_value());
    REQUIRE(tryCatch.HasCaught());

    // Whatever the engine wraps around it, the embedder's own text survives.
    const auto message = tryCatch.Message(fixture.context);
    REQUIRE(message.has_value());
    CHECK_FALSE(message->empty());
    CHECK(message->find("boom") != std::string::npos);
}

UNIBIND_TEST_CASE(STACK_TRACE, "exceptions: a stack trace exists and names the frame that threw") {
    ub_test::Fixture fixture;

    ub::TryCatch tryCatch(fixture.iso());
    const auto result = ub::Evaluate(fixture.context,
                                     "function innerMost() { throw new Error('deep'); }\n"
                                     "function outerMost() { innerMost(); }\n"
                                     "outerMost();",
                                     {.resourceName = "trace-test.js"});
    CHECK_FALSE(result.has_value());
    REQUIRE(tryCatch.HasCaught());

    const auto stack = tryCatch.StackTrace(fixture.context);
    REQUIRE(stack.has_value());
    CHECK_FALSE(stack->empty());

    // The format is the engine's and is not parseable across backends. What is
    // portable is that the frames that threw are named in it.
    CHECK(stack->find("innerMost") != std::string::npos);
    CHECK(stack->find("outerMost") != std::string::npos);
    CHECK(stack->find("trace-test.js") != std::string::npos);
}

TEST_CASE("exceptions: a handler that does nothing still consumes what it caught") {
    ub_test::Fixture fixture;

    // The default, and the one no other case here exercises because they all
    // call Reset or ReThrow explicitly. A handler is a `catch` block, not an
    // observer: closing one without re-throwing ends the exception, so an outer
    // handler sees nothing and the isolate has nothing pending.
    {
        ub::TryCatch outer(fixture.iso());
        {
            ub::TryCatch inner(fixture.iso());
            CHECK_FALSE(ub::Evaluate(fixture.context, "throw new Error('handled and dropped')").has_value());
            REQUIRE(inner.HasCaught());
        }
        CHECK_FALSE(outer.HasCaught());
        CHECK_FALSE(fixture.iso().HasPendingException());
    }

    CHECK_FALSE(fixture.iso().HasPendingException());
    CHECK(ub_test::EvalInt(fixture.context, "9 + 9") == 18);
}

TEST_CASE("exceptions: a handler that caught nothing has no exception to hand back") {
    // `unibind/exception.h`: "empty if nothing was caught". An *empty handle*,
    // not a handle naming `undefined` - the two are the same thing to anyone
    // who writes `if (handler.Exception().IsEmpty())`, which is the documented
    // way to ask, and only one of them is distinguishable from someone having
    // thrown `undefined` on purpose.
    ub_test::Fixture fixture;

    const ub::TryCatch quiet(fixture.iso());
    CHECK_FALSE(quiet.HasCaught());
    CHECK(quiet.Exception().IsEmpty());

    // And the other half: a handler that caught a thrown `undefined` has a
    // value, and it is not empty.
    {
        ub::TryCatch caught(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, "throw undefined").has_value());
        REQUIRE(caught.HasCaught());
        const auto thrown = caught.Exception();
        CHECK_FALSE(thrown.IsEmpty());
        CHECK(thrown.Kind() == ub::ValueKind::Undefined);
    }
}

TEST_CASE("exceptions: a TryCatch that is reset consumes what it caught") {
    ub_test::Fixture fixture;

    {
        ub::TryCatch outer(fixture.iso());
        {
            ub::TryCatch inner(fixture.iso());
            CHECK_FALSE(ub::Evaluate(fixture.context, "throw new Error('swallowed')").has_value());
            REQUIRE(inner.HasCaught());
            inner.Reset();
            CHECK_FALSE(inner.HasCaught());
        }
        CHECK_FALSE(outer.HasCaught());
    }

    // The isolate is usable again straight away.
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

TEST_CASE("exceptions: a re-thrown exception reaches the enclosing TryCatch") {
    ub_test::Fixture fixture;

    ub::TryCatch outer(fixture.iso());
    {
        ub::TryCatch inner(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, "throw new Error('passed along')").has_value());
        REQUIRE(inner.HasCaught());
        inner.ReThrow();
    }

    REQUIRE(outer.HasCaught());
    CHECK(outer.Message(fixture.context).value_or("").find("passed along") != std::string::npos);
    outer.Reset();
}

TEST_CASE("exceptions: nothing is pending before a throw or after one is consumed") {
    ub_test::Fixture fixture;

    {
        ub::TryCatch tryCatch(fixture.iso());
        CHECK_FALSE(fixture.iso().HasPendingException());

        CHECK_FALSE(ub::Evaluate(fixture.context, "undefinedFunctionName()").has_value());
        CHECK(tryCatch.HasCaught());

        // Whether an exception a TryCatch is holding still counts as "pending"
        // is not something the two engines have to agree on, so it is not
        // asserted here; what has to hold is that consuming it clears it.
        tryCatch.Reset();
        CHECK_FALSE(fixture.iso().HasPendingException());
    }

    CHECK(ub_test::EvalInt(fixture.context, "2 + 2") == 4);
}

TEST_CASE("exceptions: a syntax error is a failed compile, not a crash") {
    ub_test::Fixture fixture;

    ub::TryCatch tryCatch(fixture.iso());
    const auto script = ub::Script::Compile(fixture.context, "this is ( not javascript", {.resourceName = "bad.js"});
    CHECK_FALSE(script.has_value());
    REQUIRE(tryCatch.HasCaught());

    const auto message = tryCatch.Message(fixture.context);
    REQUIRE(message.has_value());
    CHECK_FALSE(message->empty());
    tryCatch.Reset();

    CHECK(ub_test::EvalInt(fixture.context, "3 + 3") == 6);
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "exceptions: a runtime error says where it was raised and quotes the line") {
    ub_test::Fixture fixture;

    const auto script = ub::Script::Compile(fixture.context, "const fine = 1;\n  null.property;\nconst after = 2;",
                                            {.resourceName = "where.js"});
    REQUIRE(script.has_value());

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(script->Run(fixture.context).has_value());
    REQUIRE(tryCatch.HasCaught());

    const auto location = tryCatch.Location(fixture.context);
    REQUIRE(location.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(location->scriptName == "where.js");
    CHECK(location->lineNumber == 2);
    CHECK(location->columnNumber > 0);
    CHECK(location->sourceLine == std::optional<std::string>("  null.property;"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "exceptions: a syntax error has a location though it has no stack") {
    ub_test::Fixture fixture;

    ub::TryCatch tryCatch(fixture.iso());
    // Compiled as though it started on line 11, as a script with a preamble
    // would be: the location counts from there, and the quoted line is still
    // the right one.
    const auto script = ub::Script::Compile(fixture.context, "let ok = 1;\nlet = = 2;",
                                            {.resourceName = "broken.js", .lineOffset = 10});
    CHECK_FALSE(script.has_value());
    REQUIRE(tryCatch.HasCaught());

    const auto location = tryCatch.Location(fixture.context);
    REQUIRE(location.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(location->scriptName == "broken.js");
    CHECK(location->lineNumber == 12);
    CHECK(location->sourceLine == std::optional<std::string>("let = = 2;"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "exceptions: a thrown value that is not an error is placed where it was thrown") {
    ub_test::Fixture fixture;

    const auto script =
        ub::Script::Compile(fixture.context, "function f() {\n  throw 42;\n}\nf();", {.resourceName = "value.js"});
    REQUIRE(script.has_value());

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(script->Run(fixture.context).has_value());
    REQUIRE(tryCatch.HasCaught());

    const auto location = tryCatch.Location(fixture.context);
    REQUIRE(location.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    CHECK(location->scriptName == "value.js");
    CHECK(location->lineNumber == 2);
    CHECK(location->sourceLine == std::optional<std::string>("  throw 42;"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

UNIBIND_TEST_CASE(MESSAGE_LOCATION, "exceptions: a handler that caught nothing has no location") {
    ub_test::Fixture fixture;

    const ub::TryCatch tryCatch(fixture.iso());
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
    CHECK_FALSE(tryCatch.Location(fixture.context).has_value());
}

TEST_CASE("exceptions: catching one does not disturb the next") {
    ub_test::Fixture fixture;

    for (int round = 0; round < 50; ++round) {
        ub::TryCatch tryCatch(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, "throw new Error('again')").has_value());
        CHECK(tryCatch.HasCaught());
        tryCatch.Reset();
    }
    CHECK(ub_test::EvalInt(fixture.context, "4 + 4") == 8);
}

UNIBIND_TEST_CASE(STACK_TRACE, "exceptions: rendering a stack cannot replace the exception the handler caught") {
    // `TryCatch::StackTrace` reads `error.stack`, and script is free to have
    // made that an accessor. If it throws while the handler being asked is the
    // innermost one, the engine reports the getter's error *to that handler* -
    // and the caller is left holding an exception it never caught, with
    // `HasCaught()` still true and the message describing something else
    // entirely. Whatever the text answers, what was caught must not move.
    ub_test::Fixture fixture;

    ub::TryCatch handler(fixture.iso());
    const auto ran = ub::Evaluate(fixture.context,
                                  "(() => { const e = new Error('the real one');"
                                  " Object.defineProperty(e, 'stack', { get() { throw new TypeError('gotcha'); } });"
                                  " throw e; })()");
    REQUIRE_FALSE(ran.has_value());
    REQUIRE(handler.HasCaught());

    // The text is the engine's business - one renders it from a stack it kept
    // for itself, the other cannot render it without the getter - so nothing
    // is asserted about whether there is any.
    const auto text = handler.StackTrace(fixture.context);
    MESSAGE("stack text after a throwing `stack` getter: ", text.has_value() ? "rendered" : "empty");

    CHECK(handler.HasCaught());
    CHECK(handler.Message(fixture.context).value_or("").find("the real one") != std::string::npos);
    const auto thrown = handler.Exception();
    REQUIRE_FALSE(thrown.IsEmpty());
    const auto asObject = thrown.To<ub::Object>();
    REQUIRE(asObject.has_value());
    const auto message = asObject->Get(fixture.context, "message");
    REQUIRE(message.has_value());
    CHECK(ub_test::TextOf(*message) == "the real one");

    handler.Reset();
    CHECK(ub_test::EvalInt(fixture.context, "3 + 4") == 7);
}
