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
/// `InspectorDispatcher::RequestDispatch`, which is how a message arriving on a
/// socket thread gets to the isolate. It runs the embedder's callback at the
/// next safe point on the isolate's thread - in the middle of running script,
/// which is how DevTools gets an answer from a busy isolate, or at the next
/// `Isolate::PumpJobs` if the isolate is idle - and unlike
/// `Isolate::RequestInterrupt`'s callback, this one may dispatch protocol
/// messages, which run script. The inspector is designed to be driven from
/// exactly that point.
///
/// The dispatcher is a separate object, shared, because the thread that reads
/// the socket is exactly the one that cannot know when the inspector goes: it
/// is V8's `TaskRunner` shape - a `std::shared_ptr` a foreign thread holds for
/// as long as it likes, which keeps answering after its owner is gone, by
/// declining.
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
/// isolate's thread. A session may be destroyed anywhere on that thread -
/// inside `RunMessageLoopOnPause` and inside a callback its own dispatch made
/// included (see `~InspectorSession`) - but the `Inspector` may not be
/// destroyed while a pause or a dispatch is on the stack. An
/// `InspectorDispatcher` may outlive everything, on any thread.

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
///
/// `Resume`, `Stop` and destruction, in any order and any number of times, do
/// what is written below and nothing else; none of them is an error.
class InspectorSession {
   public:
    /// The connection is gone: everything `Stop` does - a pause ends, as it
    /// says - and the client is sent nothing more on this session's behalf.
    /// Anywhere on the isolate's thread, including inside
    /// `RunMessageLoopOnPause` and inside a callback this session's own
    /// dispatch made, several frames below the engine's session: a connection
    /// that closes during a pause is destroyed right there, not remembered
    /// for later.
    ///
    /// **Inside `SendProtocolMessage` with a notification** - a console
    /// message, a parsed script, the notice of a pause: the send a socket
    /// write fails in - the engine is still inside the part of the session
    /// that raised it. The session is gone to the client at once, and the
    /// engine's half of it goes as soon as that send has unwound: at the next
    /// call into the inspector, before a pause is run, or at the isolate's
    /// next safe point, whichever comes first. A pause it was holding ends
    /// then, rather than before this returns.
    ~InspectorSession();

    InspectorSession(const InspectorSession&) = delete;
    InspectorSession& operator=(const InspectorSession&) = delete;
    InspectorSession(InspectorSession&&) = delete;
    InspectorSession& operator=(InspectorSession&&) = delete;

    /// A protocol message from DevTools, JSON in UTF-8. Answers arrive through
    /// `InspectorClient::SendProtocolMessage`, usually before this returns -
    /// and it may run script to produce them, so a `Runtime.evaluate` can
    /// throw, loop or pause like any other script. Still answered after
    /// `Stop`, but nothing a stopped session asks for pauses script:
    /// `Debugger.enable` is refused. Still answered on a stopped isolate too -
    /// one `Isolate::TerminateExecution` has stopped and nothing has cancelled
    /// - but what it would run is stopped at once, as any other script there
    /// is, and the answer is the error saying so. **Except during a pause**:
    /// the engine holds every interrupt off while script is paused, a stop's
    /// included, so a stop asked for then takes the paused script when it
    /// resumes, and what DevTools evaluates before that still runs.
    void DispatchProtocolMessage(std::string_view message);

    /// Leave a pause from outside the protocol - on the embedder's own
    /// decision rather than a `Debugger.resume` from DevTools. The inspector
    /// calls `InspectorClient::QuitMessageLoopOnPause` before this returns, and
    /// the script goes on once `RunMessageLoopOnPause` does. Outside a pause it
    /// does nothing, and after `Stop` it does nothing: a stopped session holds
    /// no pause to leave.
    void Resume();

    /// The connection is going: the session stops pausing script - its
    /// breakpoints and `debugger` statements no longer stop anything - so that
    /// nothing waits on a DevTools that is not there. **During a pause it ends
    /// the pause**: the inspector calls `InspectorClient::QuitMessageLoopOnPause`
    /// before this returns, unless another session still has the debugger on.
    /// Final - there is no restart - and a second call does nothing. Inside a
    /// notification it is put off exactly as destruction is, for the same
    /// reason, and stops pausing script from then on.
    void Stop();

    /// Implementation detail: the backend's per-session state.
    struct Impl;

   private:
    friend class Inspector;
    explicit InspectorSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

/// How another thread reaches an inspector's isolate. Every member may be called
/// from any thread, at any time - including while the `Inspector` is being
/// destroyed on the isolate's thread, and after it has gone - so a socket
/// thread holds one of these, never the `Inspector`. Made by
/// `Inspector::Dispatcher`.
class InspectorDispatcher {
   public:
    ~InspectorDispatcher();

    InspectorDispatcher(const InspectorDispatcher&) = delete;
    InspectorDispatcher& operator=(const InspectorDispatcher&) = delete;
    InspectorDispatcher(InspectorDispatcher&&) = delete;
    InspectorDispatcher& operator=(InspectorDispatcher&&) = delete;

    /// Run `callback` on the isolate's thread at the next safe point - inside
    /// running script, as soon as the engine next checks, or at the next
    /// `Isolate::PumpJobs` if no script is running. Unlike
    /// `Isolate::RequestInterrupt`'s, this callback may call
    /// `InspectorSession::DispatchProtocolMessage`, and is how a message read
    /// on a socket thread reaches a session.
    ///
    /// **A pause is not a safe point.** Script paused in
    /// `InspectorClient::RunMessageLoopOnPause` checks for nothing, so a
    /// request made then waits until script runs again - after the pause, or
    /// inside an evaluation made in it. A pause loop therefore takes its
    /// messages from wherever the socket thread puts them, not from here: a
    /// `Debugger.resume` that could only arrive through a request would never
    /// arrive.
    ///
    /// **Each request runs exactly once**, in the order requests were made -
    /// the same callback and data asked for twice run twice. What is coalesced
    /// is the wake-up, not the work: requests made before the isolate gets to
    /// the first of them share one engine interrupt and one posted job, and
    /// all of them run from whichever of the two reaches the thread first. A
    /// request made while callbacks are running - by one of them, or from
    /// another thread - runs in that same pass or a later one, never lost. A
    /// pass runs until nothing is waiting, as `Isolate::PumpJobs` does, so a
    /// thread that never stops requesting never lets it end.
    ///
    /// True when the request was taken. False when the inspector has gone, or
    /// the request could not be stored (out of memory, or a null `callback`),
    /// and then the callback never runs. A request taken and still waiting
    /// when the `Inspector` is destroyed is dropped, not run - so true means
    /// "will run unless the inspector goes first".
    bool RequestDispatch(JobCallback callback, CallbackData data = {}) noexcept;

    /// Implementation detail: the backend's shared dispatch state.
    struct Impl;
    [[nodiscard]] Impl& impl() const noexcept { return *impl_; }

   private:
    friend class Inspector;
    explicit InspectorDispatcher(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

/// The inspector for one isolate: the realms DevTools can see, and the sessions
/// it connects. At most one per isolate. Isolate thread only; another thread
/// reaches it through its `Dispatcher()`.
class Inspector {
   public:
    /// Whether this backend has an inspector at all. Fixed for the program;
    /// ask once.
    [[nodiscard]] static bool Supported() noexcept;

    /// An inspector for `isolate`, calling `client` - which must outlive it.
    /// Null when `Supported()` is false, null if the isolate already has one,
    /// and null when there is not the memory to make one.
    [[nodiscard]] static std::unique_ptr<Inspector> New(Isolate& isolate, InspectorClient& client);

    /// Drops every dispatch still waiting; from here on this inspector's
    /// `InspectorDispatcher::RequestDispatch` declines. Not inside a pause or
    /// a dispatch: a session may go there, the inspector may not.
    ~Inspector();

    Inspector(const Inspector&) = delete;
    Inspector& operator=(const Inspector&) = delete;
    Inspector(Inspector&&) = delete;
    Inspector& operator=(Inspector&&) = delete;

    /// Show DevTools a realm, under `name`. Every realm of the isolate is in one
    /// group, and the one most recently announced and not yet withdrawn is the
    /// default - where a `Runtime.evaluate` that names no context runs.
    /// Announcing a realm does not keep it alive, and announcing one again
    /// replaces its entry - under the new name, as the newest - rather than
    /// adding a second.
    void ContextCreated(const Context& context, std::string_view name);
    /// The realm is going; DevTools stops offering it, and the default falls
    /// back to the one announced before it. Call it before the `Context` is let
    /// go.
    void ContextDestroyed(const Context& context);

    /// A new DevTools connection. Fully trusted, and not waiting for a debugger:
    /// script keeps running while DevTools attaches, and nothing is withheld
    /// from it.
    ///
    /// Null in one case only: there was not the memory to make the session.
    /// The engine refuses no connection - any number may be open at once, and
    /// one may be made during a pause - so null is handled as running out of
    /// memory is handled anywhere else, not as a condition to wait out.
    [[nodiscard]] std::unique_ptr<InspectorSession> Connect();

    /// What another thread uses to reach this inspector: the same object on
    /// every call, never null. Take it here, on the isolate's thread, and hand
    /// it to the thread that reads the socket.
    [[nodiscard]] std::shared_ptr<InspectorDispatcher> Dispatcher() const noexcept;

    /// Implementation detail: the backend's per-inspector state.
    struct Impl;

   private:
    explicit Inspector(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ub
