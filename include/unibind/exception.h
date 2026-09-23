#pragma once
/// \file
/// Throwing from native code, catching what script throws, and reading the
/// stack behind either.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "unibind/context.h"
#include "unibind/detail/backend.h"
#include "unibind/fwd.h"
#include "unibind/handle.h"
#include "unibind/isolate.h"
#include "unibind/types.h"
#include "unibind/value.h"

namespace ub {

/// Throw a fresh error. Takes effect when control returns to the engine; the
/// caller should return promptly and not call further into the engine.
///
/// `message` is decoded as `String::NewFromUtf8` decodes: bytes that are not
/// UTF-8 become U+FFFD rather than costing the error.
inline void Throw(Isolate& isolate, ErrorKind kind, std::string_view message) {
    detail::ThrowErrorLossy(isolate, kind, message);
}

/// Throw an arbitrary value, as `throw x` does.
template <class T>
inline void Throw(Isolate& isolate, const Local<T>& value) {
    detail::ThrowValue(isolate, value.slot());
}

/// Make an Error object without throwing it. `message` is decoded as `Throw`
/// decodes it.
[[nodiscard]] inline std::optional<Local<Object>> MakeError(const Context& context, ErrorKind kind,
                                                            std::string_view message) {
    const std::size_t firstInvalid = detail::FirstInvalidUtf8(message);
    if (firstInvalid == std::string_view::npos) {
        return detail::WrapSlot<Object>(detail::MakeError(context, kind, message));
    }
    return detail::WrapSlot<Object>(
        detail::MakeError(context, kind, detail::ReplaceInvalidUtf8(message, firstInvalid)));
}

/// Where the code running right now came from: the JavaScript frames below
/// this native call, innermost first.
///
/// This is the answer to "who called me", which a native callback wants for a
/// diagnostic and cannot get any other way. `limit` caps the frames collected,
/// and zero collects none; engines cap it again at their own configured depth,
/// so a short stack is not evidence that there were no more frames.
///
/// Empty if no script is running, or if the engine kept no stack. It needs no
/// open `HandleScope`: frames come out as plain strings and numbers, not
/// handles.
[[nodiscard]] inline std::vector<StackFrame> CaptureStackFrames(Isolate& isolate, std::uint32_t limit = 16) {
    return detail::CaptureStack(isolate, limit);
}

/// Catches exceptions thrown while it is in scope.
///
/// Stack only and LIFO, like every scope here.
///
/// **Closing a `TryCatch` consumes what it caught.** A handler is a `catch`
/// block, not an observer: if it caught something and was not asked to let it
/// continue, the exception stops there and nothing is pending afterwards. Call
/// `ReThrow` to send it on to the enclosing handler.
///
/// The other rule - propagate unless explicitly consumed - was considered and
/// is worse: forgetting one call would leak a pending exception into code that
/// never went near the throw, and the leak surfaces somewhere else entirely.
/// Forgetting `ReThrow` swallows an exception where you can see the handler
/// that swallowed it. This is also V8's rule.
///
/// **A termination is the one thing a handler does not consume.** When
/// execution was stopped from another thread (`Isolate::TerminateExecution`),
/// `HasCaught()` is true, `HasTerminated()` is true, and closing the handler
/// lets the unwind continue whether or not `ReThrow` was called. A `TryCatch`
/// that swallowed a termination would let the script it was told to stop keep
/// running, which is the whole point of the facility; so the rule above is
/// about exceptions, and this is the exception to it.
class TryCatch {
   public:
    explicit TryCatch(Isolate& isolate) noexcept : isolate_(&isolate) { detail::TryCatchOpen(isolate, State()); }
    ~TryCatch() { detail::TryCatchClose(State()); }

    TryCatch(const TryCatch&) = delete;
    TryCatch& operator=(const TryCatch&) = delete;
    TryCatch(TryCatch&&) = delete;
    TryCatch& operator=(TryCatch&&) = delete;

    void* operator new(std::size_t) = delete;
    void* operator new[](std::size_t) = delete;

    [[nodiscard]] bool HasCaught() const noexcept { return detail::TryCatchHasCaught(State()); }

    /// True when what stopped the code was a termination rather than a throw.
    ///
    /// This is how a caller tells "script failed" from "I was told to stop":
    /// when it is true there is no exception value, no message and no stack,
    /// `Reset` and `ReThrow` mean nothing, and the right response is to return
    /// out of every frame promptly. Only `Isolate::CancelTerminateExecution`,
    /// on the isolate's own thread, clears the state.
    [[nodiscard]] bool HasTerminated() const noexcept { return detail::TryCatchHasTerminated(State()); }

    /// The thrown value. Requires an open HandleScope; empty if nothing was
    /// caught.
    [[nodiscard]] Local<Value> Exception() const noexcept {
        return Local<Value>::FromSlot(detail::TryCatchException(State(), *isolate_));
    }

    /// The message text, without the engine's decoration. Engines word their
    /// built-in errors differently, so a test asserts the shape of this, not
    /// its exact contents.
    [[nodiscard]] std::optional<std::string> Message(const Context& context) const {
        return detail::TryCatchMessage(State(), context);
    }

    /// The stack as the engine's own text - what you show a human, and nothing
    /// else. The format differs between engines and is not parseable across
    /// them; to *read* a stack, ask `StackFrames`.
    ///
    /// Empty if there was no stack to render.
    ///
    /// **Rendering may run script.** The text comes from `error.stack`, which
    /// a script is free to have made an accessor; if that accessor throws,
    /// this answers empty and *what this handler caught is untouched* - still
    /// caught, still the same value and message. Nothing an error's own
    /// `stack` does can change which exception you are holding.
    [[nodiscard]] std::optional<std::string> StackTrace(const Context& context) const {
        return detail::TryCatchStackTrace(State(), context);
    }

    /// The stack of the caught exception, frame by frame, innermost first.
    ///
    /// This is the portable half of the pair above: function name, script name
    /// and line are what both engines will name, and they are what an embedder
    /// showing an error to a user actually needs. Empty if nothing was caught,
    /// or if the thrown value carried no stack - throwing a string carries
    /// none on either engine, and a termination carries none by definition.
    ///
    /// An engine caps the frames it records (V8 at `Error.stackTraceLimit`,
    /// SpiderMonkey at its own depth), so a short trace is not evidence that
    /// there were no more frames.
    [[nodiscard]] std::optional<std::vector<StackFrame>> StackFrames(const Context& context) const {
        return detail::TryCatchStackFrames(State(), context);
    }

    /// Where the caught exception was raised, and the text of that line.
    ///
    /// Where it was thrown, whatever was thrown - an `Error` made on one line
    /// and thrown on another is placed at the `throw`, and one a native
    /// callback threw is placed at the call into it. For a syntax error, the
    /// offending position in the source being compiled - which is the case
    /// this exists for beside `StackFrames`, because a script that never
    /// compiled has no frame to report. It is what an embedder prints as
    /// `file:line: message` followed by the line itself.
    ///
    /// Empty when nothing was caught, on a termination, and when the engine
    /// has no position - a value thrown by native code with no script running.
    ///
    /// **`sourceLine` needs the source.** V8 keeps every script's text and
    /// reads the line back from it. SpiderMonkey keeps its own copy but offers
    /// no way to read a line of it back, and quotes the line only for a
    /// compile error - so its backend keeps a second copy of the text of every
    /// script compiled through `Script`, for as long as the engine keeps that
    /// script's code, and quotes from that. Code that was not compiled through
    /// `Script` (`eval`, `new Function`) has a line on V8 and none on
    /// SpiderMonkey. Nor does a script that shares its resource name with
    /// another live script of different text - every script compiled with no
    /// origin shares the default one - because SpiderMonkey names the script
    /// in an error only by that name, and quoting the wrong one of the two
    /// would be worse than quoting neither. Name your scripts.
    [[nodiscard]] std::optional<MessageLocation> Location(const Context& context) const {
        std::optional<MessageLocation> location = detail::TryCatchLocation(State(), context);
        // Both engines answer a throw with no script under it with a location
        // that names no line - and each fills the rest differently, V8 with an
        // empty line of text and SpiderMonkey with a script called "<native>".
        // No line is no position, so it is settled here, once, as empty.
        if (location && location->lineNumber <= 0) {
            return std::nullopt;
        }
        return location;
    }

    /// Let the exception continue outwards when this handler closes, instead
    /// of stopping here.
    void ReThrow() noexcept { detail::TryCatchReThrow(State()); }
    /// Consume the exception now rather than at the close, so that the rest of
    /// this scope can call into the engine again. After this, nothing is
    /// pending and there is nothing left for `ReThrow` to send on.
    void Reset() noexcept { detail::TryCatchReset(State()); }

   private:
    [[nodiscard]] detail::TryCatchState& State() const noexcept {
        return *reinterpret_cast<detail::TryCatchState*>(const_cast<unsigned char*>(storage_));
    }

    Isolate* isolate_;
    alignas(UNIBIND_TRY_CATCH_STORAGE_ALIGN) unsigned char storage_[UNIBIND_TRY_CATCH_STORAGE_SIZE]{};
};

}  // namespace ub
