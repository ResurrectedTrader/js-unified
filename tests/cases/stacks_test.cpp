/// \file
/// Stacks: who threw, and who called.
///
/// Two shapes, and they answer different questions. `TryCatch::StackFrames` is
/// the stack of what was *caught*; `CaptureStackFrames` is the stack the code
/// running *now* sits on, which is what a native callback wants for a
/// diagnostic and cannot get any other way.
///
/// Neither engine will agree with the other about how many frames there were,
/// how an anonymous function is named, or what the rendered text looks like -
/// so every case here asserts the frames it *caused* and reports the rest.
/// `TryCatch::StackTrace` is the engine's own text and is only ever searched,
/// never parsed.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/harness.h"

namespace {

/// Source with two named functions and a throw at the bottom, so the innermost
/// frames are ones the test named itself.
constexpr std::string_view THROWING_SOURCE = R"(
function innerMost() {
    throw new Error('from the bottom');
}
function outerMost() {
    return innerMost();
}
outerMost();
)";

[[nodiscard]] bool NamesAFrame(const std::vector<ub::StackFrame>& frames, std::string_view name) {
    for (const auto& frame : frames) {
        if (frame.functionName == name) {
            return true;
        }
    }
    return false;
}

/// Captures the stack it was called on, so a test can read it afterwards.
struct Capture {
    std::vector<ub::StackFrame> frames;
    std::uint32_t limit = 16;
};

void CaptureHere(const ub::CallbackInfo& info) {
    auto* capture = info.Data<Capture>();
    if (capture == nullptr) {
        info.ThrowTypeError("callback data missing");
        return;
    }
    capture->frames = ub::CaptureStackFrames(info.GetIsolate(), capture->limit);
}

}  // namespace

UNIBIND_TEST_CASE(STACK_FRAMES, "stacks: a caught error names the functions it was thrown through") {
    ub_test::Fixture fixture;

    ub::TryCatch handler(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, THROWING_SOURCE, {.resourceName = "trace-test.js"}).has_value());
    REQUIRE(handler.HasCaught());

    const auto frames = handler.StackFrames(fixture.context);
    REQUIRE(frames.has_value());
    REQUIRE_FALSE(frames->empty());

    // Innermost first: the function that actually threw.
    CHECK(frames->front().functionName == "innerMost");
    CHECK(NamesAFrame(*frames, "outerMost"));
    CHECK(frames->front().scriptName == "trace-test.js");
    CHECK(frames->front().lineNumber > 0);

    // How many frames there are beyond those two, and whether top-level code is
    // one of them, is the engine's business.
    MESSAGE("the caught error had ", frames->size(), " frames; the outermost names '", frames->back().functionName,
            "'");

    // The rendered form is for humans, so it is searched and never parsed.
    const auto text = handler.StackTrace(fixture.context);
    REQUIRE(text.has_value());
    CHECK(text->find("innerMost") != std::string::npos);
    CHECK(text->find("trace-test.js") != std::string::npos);

    handler.Reset();
}

UNIBIND_TEST_CASE(STACK_FRAMES, "stacks: the frames and the error's own stack agree about who threw") {
    // `err.stack` is what script sees, and it is the engine's text too. Nothing
    // here parses either - the assertion is that the two do not disagree about
    // the one name the test put there.
    ub_test::Fixture fixture;

    ub::TryCatch handler(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, THROWING_SOURCE, {.resourceName = "trace-test.js"}).has_value());
    REQUIRE(handler.HasCaught());

    const auto frames = handler.StackFrames(fixture.context);
    REQUIRE(frames.has_value());
    CHECK(NamesAFrame(*frames, "innerMost"));

    const auto thrown = handler.Exception().To<ub::Object>();
    REQUIRE(thrown.has_value());
    const auto stack = thrown->Get(fixture.context, "stack");
    REQUIRE(stack.has_value());
    const auto asText = stack->ToString(fixture.context);
    REQUIRE(asText.has_value());
    CHECK(asText->Utf8Value().find("innerMost") != std::string::npos);

    handler.Reset();
}

UNIBIND_TEST_CASE(STACK_FRAMES, "stacks: a native callback can ask who called it") {
    ub_test::Fixture fixture;

    Capture capture;
    const auto probe = ub::Function::New(fixture.context, &CaptureHere, ub::CallbackData::For(capture));
    REQUIRE(probe.has_value());
    ub_test::Expose(fixture.context, "whoCalledMe", *probe);

    (void)ub_test::Eval(fixture.context, R"(
        function middle() { return whoCalledMe(); }
        function top() { return middle(); }
        top();
    )");

    REQUIRE_FALSE(capture.frames.empty());
    // The innermost JavaScript frame is the function that made the call. The
    // native frame itself is not one: what is a frame differs between engines,
    // and only the script ones are portable.
    CHECK(capture.frames.front().functionName == "middle");
    CHECK(NamesAFrame(capture.frames, "top"));
    CHECK(capture.frames.front().lineNumber > 0);
    MESSAGE("a native called two frames deep saw ", capture.frames.size(), " of them");
}

UNIBIND_TEST_CASE(STACK_FRAMES, "stacks: a capture is capped by the limit it was given") {
    ub_test::Fixture fixture;

    Capture capture;
    capture.limit = 2;
    const auto probe = ub::Function::New(fixture.context, &CaptureHere, ub::CallbackData::For(capture));
    REQUIRE(probe.has_value());
    ub_test::Expose(fixture.context, "whoCalledMe", *probe);

    (void)ub_test::Eval(fixture.context, R"(
        function down(n) { return n === 0 ? whoCalledMe() : down(n - 1); }
        down(20);
    )");

    CHECK_FALSE(capture.frames.empty());
    CHECK(capture.frames.size() <= 2);
    // A short stack is never evidence that there were no more frames - the
    // engine caps it again at its own depth - so the only assertion available
    // is the one above.
}

UNIBIND_TEST_CASE(STACK_FRAMES, "stacks: nothing running is no frames, not a made-up one") {
    ub_test::Fixture fixture;

    const auto frames = ub::CaptureStackFrames(fixture.iso());
    CHECK(frames.empty());
}

UNIBIND_TEST_CASE(STACK_FRAMES,
                  "stacks: a thrown value carrying no stack of its own answers nothing or the throw site") {
    // A thrown string has no `stack` property for anyone to read, and the two
    // engines answer differently: one has nothing to say, the other reports
    // where the throw happened because it records that for the *message*
    // rather than for the value. Both are defensible and neither is a lie, so
    // what is asserted is that the answer is not a *different* stack: either
    // there are no frames, or the innermost is the function that threw.
    ub_test::Fixture fixture;

    ub::TryCatch handler(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "function throwsAString() { throw 'just a string'; } throwsAString();",
                             {.resourceName = "string-throw.js"})
                    .has_value());
    REQUIRE(handler.HasCaught());

    const auto frames = handler.StackFrames(fixture.context);
    if (!frames.has_value() || frames->empty()) {
        MESSAGE("a thrown string carries no stack on this backend");
    } else {
        MESSAGE("a thrown string reports the throw site on this backend: ", frames->size(), " frames, innermost '",
                frames->front().functionName, "'");
        CHECK(NamesAFrame(*frames, "throwsAString"));
        CHECK(frames->front().scriptName == "string-throw.js");
    }

    handler.Reset();
}

UNIBIND_TEST_CASE(STACK_FRAMES, "stacks: a handler that caught nothing has no stack to give") {
    ub_test::Fixture fixture;

    ub::TryCatch handler(fixture.iso());
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
    CHECK_FALSE(handler.HasCaught());

    const auto frames = handler.StackFrames(fixture.context);
    CHECK((!frames.has_value() || frames->empty()));
}

UNIBIND_TEST_CASE(STACK_FRAMES, "stacks: an origin's line offset moves what a diagnostic reports") {
    // What `ScriptOrigin` is *for*: an embedder that wraps or transforms source
    // before compiling needs the positions to point at the original file. An
    // origin nothing reads would be a struct, not a feature.
    ub_test::Fixture fixture;

    constexpr std::string_view SOURCE = R"(
function thrower() {
    throw new Error('from a wrapped file');
}
thrower();
)";

    const auto lineOfTheThrow = [&fixture, SOURCE](int lineOffset) {
        ub::TryCatch handler(fixture.iso());
        const auto ran =
            ub::Evaluate(fixture.context, SOURCE, {.resourceName = "wrapped.js", .lineOffset = lineOffset});
        REQUIRE_FALSE(ran.has_value());
        REQUIRE(handler.HasCaught());
        const auto frames = handler.StackFrames(fixture.context);
        REQUIRE(frames.has_value());
        REQUIRE_FALSE(frames->empty());
        CHECK(frames->front().functionName == "thrower");
        CHECK(frames->front().scriptName == "wrapped.js");
        const int line = frames->front().lineNumber;
        handler.Reset();
        return line;
    };

    const int plain = lineOfTheThrow(0);
    const int shifted = lineOfTheThrow(10);

    CHECK(plain > 0);
    // Not a particular line - which line a given engine calls the first one is
    // its business - but that the offset is added to it.
    CHECK(shifted == plain + 10);
}
