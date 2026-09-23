#pragma once
/// \file
/// The inspector: Chrome DevTools attached to an isolate, over the Chrome
/// DevTools Protocol. Shaped after V8's `v8_inspector` - an embedder-written
/// client the engine calls, one inspector per isolate, and a session per
/// DevTools connection that protocol messages are dispatched into.
///
/// ---------------------------------------------------------------------------
/// Where it exists, and how a program finds out
/// ---------------------------------------------------------------------------
///
/// **`Inspector::Supported()` answers, at run time, and a program links either
/// way.** V8 has an inspector; SpiderMonkey does not have one of this shape -
/// its debugging surface is the `Debugger` object, a JavaScript API installed
/// into a debuggee realm, which speaks no protocol and has no C++ session to
/// drive. So on SpiderMonkey `New` returns null and nothing else here is ever
/// reachable, and every member is still defined.
///
/// That is the opposite of how this library says "this engine cannot" anywhere
/// else - a link error at the call site (`docs/status.md`, decision 19) - and
/// the difference is the point of this header. A debugger is optional at run
/// time by nature: whether to open a DevTools port is a decision a program
/// makes when it starts, not when it is built, and it has to be able to make
/// it in one binary, compiled once against these headers, that links either
/// engine. What keeps that from being the silent answer decision 19 refuses is
/// the shape: there is no parameter here that a backend quietly ignores, only
/// an object a backend declines to make, and a null `unique_ptr` is not
/// something a caller can use by accident.
///
/// ---------------------------------------------------------------------------
/// Threads, pauses, and the one thing that may come from elsewhere
/// ---------------------------------------------------------------------------
///
/// Everything happens on the isolate's own thread except
/// `Inspector::RequestDispatch`, which is how a message arriving on a socket
/// thread gets to the isolate. It runs the embedder's callback at the next safe
/// point on the isolate's thread - in the middle of running script, which is
/// how DevTools gets an answer from a busy isolate, or at the next
/// `Isolate::PumpJobs` if the isolate is idle - and unlike
/// `Isolate::RequestInterrupt`'s callback, this one may dispatch protocol
/// messages, which run script. The inspector is designed to be driven from
/// exactly that point.
///
/// A pause - a breakpoint, a `debugger` statement, a step - happens inside
/// whatever call was running script, and it is the embedder's to run:
/// `InspectorClient::RunMessageLoopOnPause` is called, feeds messages to the
/// session until `QuitMessageLoopOnPause` says the script may go on, and
/// returns. Nothing unibind holds is disturbed by that: the frames and handles
/// of the paused call stay exactly as they were.
///
/// ---------------------------------------------------------------------------
/// Two names that are not V8's
/// ---------------------------------------------------------------------------
///
/// V8's channel has `sendResponse` and `sendNotification`, and its session
/// `dispatchProtocolMessage`. The first two are one call here,
/// `InspectorClient::SendProtocolMessage`, because an embedder forwards both to
/// the same socket and the message says which it is. None of them is spelled
/// `SendMessage` or `DispatchMessage`: this library is built for Windows, where
/// `<windows.h>` defines both as macros, and a member with either name is
/// silently renamed in whichever translation units happened to include it
/// first - a virtual that overrides nothing, or a call that links against a
/// function nobody defined.
///
/// **Lifetimes.** An `InspectorSession` does not outlive its `Inspector`, and an
/// `Inspector` does not outlive its isolate: destroy them in that order, on the
/// isolate's thread. A session is not destroyed from inside
/// `RunMessageLoopOnPause` - stop it there if you must, and destroy it once the
/// pause is over.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "unibind/fwd.h"
#include "unibind/types.h"

namespace ub {

/// What the inspector calls back into. Written by the embedder; every call
/// arrives on the isolate's thread.
class InspectorClient {
   public:
    InspectorClient() = default;
    virtual ~InspectorClient() = default;

    InspectorClient(const InspectorClient&) = delete;
    InspectorClient& operator=(const InspectorClient&) = delete;
    InspectorClient(InspectorClient&&) = delete;
    InspectorClient& operator=(InspectorClient&&) = delete;

    /// A protocol message for DevTools - a response or a notification, which
    /// V8 hands its channel separately and the message itself distinguishes.
    /// JSON, UTF-8. Called synchronously from inside a dispatch, from inside a
    /// script, and from inside a pause; forward it and return.
    virtual void SendProtocolMessage(std::string_view message) = 0;

    /// Script has paused. Run a loop that feeds `InspectorSession::
    /// DispatchProtocolMessage` until `QuitMessageLoopOnPause` is called, then
    /// return, and the script continues. It may be re-entered: an evaluation
    /// made during a pause can pause again.
    virtual void RunMessageLoopOnPause() = 0;
    /// The loop above should stop. Called from inside a dispatch it made - the
    /// `Debugger.resume` it forwarded, say - so it sets a flag the loop reads.
    virtual void QuitMessageLoopOnPause() = 0;

    /// Wall-clock milliseconds since the epoch, for the timestamps in protocol
    /// messages. The default is the system clock.
    virtual double CurrentTimeMs() {
        return std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /// The URL DevTools shows for a script compiled under `resourceName` -
    /// what `Debugger.scriptParsed` reports, and what a breakpoint by URL
    /// matches against. Empty for the name verbatim, which is the default.
    virtual std::optional<std::string> ResourceNameToUrl(std::string_view /*resourceName*/) { return std::nullopt; }
};

/// One DevTools connection. Isolate thread only.
class InspectorSession {
   public:
    ~InspectorSession();

    InspectorSession(const InspectorSession&) = delete;
    InspectorSession& operator=(const InspectorSession&) = delete;
    InspectorSession(InspectorSession&&) = delete;
    InspectorSession& operator=(InspectorSession&&) = delete;

    /// A protocol message from DevTools, JSON in UTF-8. Answers arrive through
    /// `InspectorClient::SendProtocolMessage`, usually before this returns -
    /// and it may run script to produce them, so a `Runtime.evaluate` can
    /// throw, loop or pause like any other script.
    void DispatchProtocolMessage(std::string_view message);

    /// Leave a pause from outside the protocol - on the embedder's own
    /// decision rather than a `Debugger.resume` from DevTools.
    void Resume();

    /// The connection is going: the session stops pausing script - its
    /// breakpoints and `debugger` statements no longer stop anything - so that
    /// nothing waits on a DevTools that is not there. Destroying the session
    /// does this too; `Stop` is for a connection that closed while the session
    /// cannot be destroyed yet, such as during a pause.
    void Stop();

    /// Implementation detail: the backend's per-session state.
    struct Impl;

   private:
    friend class Inspector;
    explicit InspectorSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

/// The inspector for one isolate: the realms DevTools can see, and the sessions
/// it connects. At most one per isolate. Isolate thread only, except
/// `RequestDispatch`.
class Inspector {
   public:
    /// Whether this backend has an inspector at all. Fixed for the program;
    /// ask once.
    [[nodiscard]] static bool Supported() noexcept;

    /// An inspector for `isolate`, calling `client` - which must outlive it.
    /// Null when `Supported()` is false, and null if the isolate already has
    /// one.
    [[nodiscard]] static std::unique_ptr<Inspector> New(Isolate& isolate, InspectorClient& client);

    ~Inspector();

    Inspector(const Inspector&) = delete;
    Inspector& operator=(const Inspector&) = delete;
    Inspector(Inspector&&) = delete;
    Inspector& operator=(Inspector&&) = delete;

    /// Show DevTools a realm, under `name`. Every realm of the isolate is in one
    /// group, and the one most recently announced and not yet withdrawn is the
    /// default - where a `Runtime.evaluate` that names no context runs.
    /// Announcing a realm does not keep it alive.
    void ContextCreated(const Context& context, std::string_view name);
    /// The realm is going; DevTools stops offering it, and the default falls
    /// back to the one announced before it. Call it before the `Context` is let
    /// go.
    void ContextDestroyed(const Context& context);

    /// A new DevTools connection. Fully trusted, and not waiting for a debugger:
    /// script keeps running while DevTools attaches, and nothing is withheld
    /// from it.
    [[nodiscard]] std::unique_ptr<InspectorSession> Connect();

    /// From any thread: run `callback` on the isolate's thread at the next safe
    /// point - inside running script, as soon as the engine next checks, or at
    /// the next `Isolate::PumpJobs` if no script is running. Whichever comes
    /// first runs it, once. Unlike `Isolate::RequestInterrupt`'s, this callback
    /// may call `InspectorSession::DispatchProtocolMessage`, and is how a
    /// message read on a socket thread reaches a session.
    ///
    /// Requests are run in the order they were made. Ones still waiting when
    /// the `Inspector` is destroyed are dropped, not run; make sure no other
    /// thread is still requesting by then. Does nothing when `Supported()` is
    /// false.
    void RequestDispatch(JobCallback callback, CallbackData data = {}) noexcept;

    /// Implementation detail: the backend's per-inspector state.
    struct Impl;

   private:
    explicit Inspector(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ub
