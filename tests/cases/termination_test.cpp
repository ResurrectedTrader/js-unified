/// \file
/// Stopping a script that will not stop by itself.
///
/// The hard part of testing this is not the stopping, it is having something to
/// assert afterwards. A case has to start something that does not return, stop
/// it from another thread, and then say what happened - and "the run produced
/// no value" is true of an ordinary `throw` as well, so on its own it proves
/// nothing. Three things make these cases assertions rather than hopes, and
/// each is exercised below:
///
///   * `Isolate::TerminateExecution` is callable **from another thread** - the
///     one operation in the API that is. The thread stuck in `for(;;)` was
///     never going to call it.
///   * `TryCatch::HasTerminated` tells a stop from a throw. Without it the
///     embedder cannot write a correct `catch`, and a test cannot assert that
///     the stop was *its* stop.
///   * `Isolate::CancelTerminateExecution` is the way back, so "usable again"
///     is a question with an answer.
///
/// **Why this file is longer than the feature looks.** Only one of the two
/// engines has a sticky terminating state of its own; on the other, "everything
/// that would run script fails until the termination is cancelled" is the
/// backend's own bookkeeping over an engine whose context is live again the
/// moment the unwind finishes. Nothing in that engine will object if the
/// bookkeeping drifts, so these cases are the only thing holding it to the
/// promise in `unibind/isolate.h`.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "support/harness.h"

namespace {

struct RunState {
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> sawCatch{false};
    std::atomic<bool> sawFinally{false};
    std::atomic<bool> nativeCompleted{false};
    std::atomic<int> ticks{0};
    std::atomic<int> ticksSeeingTermination{0};
};

RunState* StateOf(const ub::CallbackInfo& info) {
    auto* state = info.Data<RunState>();
    if (state == nullptr) {
        info.ThrowTypeError("callback data missing");
    }
    return state;
}

void NoteEntered(const ub::CallbackInfo& info) {
    if (auto* state = StateOf(info)) {
        state->entered.store(true, std::memory_order_release);
    }
}

void NoteFinished(const ub::CallbackInfo& info) {
    if (auto* state = StateOf(info)) {
        state->finished.store(true, std::memory_order_release);
    }
}

void NoteCatch(const ub::CallbackInfo& info) {
    if (auto* state = StateOf(info)) {
        state->sawCatch.store(true, std::memory_order_release);
    }
}

void NoteFinally(const ub::CallbackInfo& info) {
    if (auto* state = StateOf(info)) {
        state->sawFinally.store(true, std::memory_order_release);
    }
}

/// Called from inside a loop, so a native frame gets to ask whether the isolate
/// it runs in has been told to stop.
void Tick(const ub::CallbackInfo& info) {
    auto* state = StateOf(info);
    if (state == nullptr) {
        return;
    }
    state->entered.store(true, std::memory_order_release);
    state->ticks.fetch_add(1, std::memory_order_relaxed);
    if (info.GetIsolate().IsExecutionTerminating()) {
        state->ticksSeeingTermination.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Spins without re-entering the engine, which is the one thing neither engine
/// can interrupt. It announces itself on the way in, so the stop is certain to
/// land while it is running.
void SpinWithoutAsking(const ub::CallbackInfo& info) {
    auto* state = StateOf(info);
    if (state == nullptr) {
        return;
    }
    state->entered.store(true, std::memory_order_release);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
    state->nativeCompleted.store(true, std::memory_order_release);
}

void ExposeMarkers(const ub::Context& context, RunState& state) {
    const auto install = [&](std::string_view name, ub::FunctionCallback callback) {
        const auto made = ub::Function::New(context, callback, ub::CallbackData::For(state));
        REQUIRE(made.has_value());
        ub_test::Expose(context, name, *made);
    };
    install("entered", &NoteEntered);
    install("finished", &NoteFinished);
    install("sawCatch", &NoteCatch);
    install("sawFinally", &NoteFinally);
    install("tick", &Tick);
    install("spin", &SpinWithoutAsking);
}

/// Waits until script is actually running and then stops it. Starting the
/// stopper first and having it wait is the only ordering a test can rely on; a
/// stop requested before the run is remembered, which is a different case.
class Stopper {
   public:
    Stopper(ub::Isolate& isolate, RunState& state)
        : thread_([&isolate, &state] {
              while (!state.entered.load(std::memory_order_acquire)) {
                  std::this_thread::yield();
              }
              isolate.TerminateExecution();
          }) {}
    ~Stopper() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    Stopper(const Stopper&) = delete;
    Stopper& operator=(const Stopper&) = delete;
    Stopper(Stopper&&) = delete;
    Stopper& operator=(Stopper&&) = delete;

    void Join() { thread_.join(); }

   private:
    std::thread thread_;
};

}  // namespace

UNIBIND_TEST_CASE(TERMINATION, "termination: a script that will not return is stopped from another thread") {
    ub_test::Fixture fixture;

    RunState state;
    ExposeMarkers(fixture.context, state);

    Stopper stopper(fixture.iso(), state);

    bool produced = true;
    bool caught = false;
    bool terminated = false;
    {
        ub::TryCatch handler(fixture.iso());
        produced = ub::Evaluate(fixture.context, "entered(); for (;;) {} finished();").has_value();
        caught = handler.HasCaught();
        terminated = handler.HasTerminated();
    }
    stopper.Join();

    CHECK_FALSE(produced);
    CHECK(state.entered.load());
    // It stopped where it was rather than falling out of the loop.
    CHECK_FALSE(state.finished.load());

    // And the embedder can say why it stopped. This is the pair the whole area
    // rests on: an ordinary throw answers the second one the other way.
    CHECK(caught);
    CHECK(terminated);

    fixture.iso().CancelTerminateExecution();
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE(TERMINATION, "termination: a stop is not a throw, and a throw is not a stop") {
    // The contrast that gives the previous case's assertion its meaning: the
    // same two questions, over an ordinary exception, answer the other way.
    ub_test::Fixture fixture;

    ub::TryCatch handler(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "throw new Error('ordinary')").has_value());
    CHECK(handler.HasCaught());
    CHECK_FALSE(handler.HasTerminated());
    CHECK(handler.Message(fixture.context).value_or("").find("ordinary") != std::string::npos);
    CHECK_FALSE(fixture.iso().IsExecutionTerminating());
    handler.Reset();

    CHECK(ub_test::EvalInt(fixture.context, "2 + 2") == 4);
}

UNIBIND_TEST_CASE(TERMINATION, "termination: script cannot catch a stop, and its finally does not run") {
    // The property that makes a stop a stop. A `catch` that could swallow it,
    // or a `finally` that ran, would let the script that was told to stop keep
    // executing - so neither happens, on either engine.
    ub_test::Fixture fixture;

    RunState state;
    ExposeMarkers(fixture.context, state);

    Stopper stopper(fixture.iso(), state);
    {
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, R"(
            try {
                entered();
                for (;;) {}
            } catch (e) {
                sawCatch();
            } finally {
                sawFinally();
            }
            finished();
        )")
                        .has_value());
        CHECK(handler.HasTerminated());
    }
    stopper.Join();

    CHECK(state.entered.load());
    CHECK_FALSE(state.sawCatch.load());
    CHECK_FALSE(state.sawFinally.load());
    CHECK_FALSE(state.finished.load());

    fixture.iso().CancelTerminateExecution();
}

UNIBIND_TEST_CASE(TERMINATION, "termination: a handler does not swallow a stop the way it swallows an exception") {
    // `~TryCatch` consumes what it caught (docs/status.md decision 3) - except
    // this. A handler that swallowed a termination would let the script it was
    // told to stop carry on.
    ub_test::Fixture fixture;

    RunState state;
    ExposeMarkers(fixture.context, state);

    Stopper stopper(fixture.iso(), state);
    {
        ub::TryCatch swallower(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, "entered(); for (;;) {}").has_value());
        CHECK(swallower.HasTerminated());

        // Everything a handler would normally do to consume what it caught -
        // and while it is open, the stop is still in force: the isolate runs
        // nothing.
        swallower.Reset();
        CHECK(fixture.iso().IsExecutionTerminating());
        CHECK_FALSE(ub::Evaluate(fixture.context, "1 + 1").has_value());
    }
    stopper.Join();

    // And closing it does not end the stop either (decision 15). The engines
    // answer this differently on their own - one keeps a pending termination,
    // the other is usable the instant the unwind finishes - so the sticky bit
    // is the *library's*, on both. Nothing in either engine objects if a
    // backend drops it, which makes this assertion the only thing holding the
    // promise up: without it a stop requested from another thread could race
    // the very next `Run` and sometimes lose, silently, on one backend only.
    CHECK(fixture.iso().IsExecutionTerminating());
    CHECK_FALSE(ub::Evaluate(fixture.context, "1 + 1").has_value());

    fixture.iso().CancelTerminateExecution();
    CHECK_FALSE(fixture.iso().IsExecutionTerminating());
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE(TERMINATION, "termination: a stop requested while nothing runs stops the next thing that does") {
    // Documented in unibind/isolate.h, and the half of the contract that needs no
    // second thread: there is no observable race between "already running" and
    // "about to run", so the request is remembered.
    ub_test::Fixture fixture;

    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);

    fixture.iso().TerminateExecution();
    {
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, "1 + 1").has_value());
        CHECK(handler.HasTerminated());
    }

    fixture.iso().CancelTerminateExecution();
    // And the thing after that runs normally.
    CHECK(ub_test::EvalInt(fixture.context, "40 + 2") == 42);
}

UNIBIND_TEST_CASE(TERMINATION, "termination: stopping twice is stopping twice, not a broken isolate") {
    // Repeatability, on one context: an embedder that runs untrusted scripts in
    // a loop stops more than one of them.
    ub_test::Fixture fixture;

    for (int round = 0; round < 2; ++round) {
        CAPTURE(round);
        RunState state;
        ExposeMarkers(fixture.context, state);

        Stopper stopper(fixture.iso(), state);
        {
            ub::TryCatch handler(fixture.iso());
            CHECK_FALSE(ub::Evaluate(fixture.context, "entered(); for (;;) {} finished();").has_value());
            CHECK(handler.HasTerminated());
        }
        stopper.Join();
        CHECK_FALSE(state.finished.load());

        fixture.iso().CancelTerminateExecution();
        CHECK(ub_test::EvalInt(fixture.context, "7 * 6") == 42);
    }
}

UNIBIND_TEST_CASE(TERMINATION, "termination: a native that does not re-enter the engine runs to completion") {
    // The honest shape of the guarantee. A stop takes effect where the *engine*
    // checks for one, so a native spinning on its own is never interrupted: it
    // finishes. That half is portable and is what is asserted.
    //
    // *Where* the next check is, is not. One engine unwinds at the next
    // statement; the other has no check between two top-level calls and lets
    // the script finish, leaving the stop armed for the next thing that runs.
    // So what follows the native is reported, not asserted - see
    // docs/testing.md.
    ub_test::Fixture fixture;

    RunState state;
    ExposeMarkers(fixture.context, state);

    Stopper stopper(fixture.iso(), state);
    bool ranOn = false;
    {
        ub::TryCatch handler(fixture.iso());
        ranOn = ub::Evaluate(fixture.context, "spin(); finished();").has_value();
    }
    stopper.Join();

    CHECK(state.nativeCompleted.load());
    MESSAGE("after the blocking native returned, the statement following it ",
            (state.finished.load() ? "ran" : "did not run"), " and the script ", (ranOn ? "completed" : "was unwound"));

    // Either way the isolate is left usable, which is the part an embedder
    // depends on. A cancel is a no-op when nothing is armed.
    fixture.iso().CancelTerminateExecution();
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE(TERMINATION, "termination: a native callback can see that it is terminating") {
    // What a long-running native is supposed to do: ask, and return promptly.
    // Without this a native that loops has no way to find out.
    ub_test::Fixture fixture;

    RunState state;
    ExposeMarkers(fixture.context, state);

    Stopper stopper(fixture.iso(), state);
    {
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, "for (;;) { tick(); }").has_value());
        CHECK(handler.HasTerminated());
    }
    stopper.Join();

    CHECK(state.ticks.load() > 0);
    // How many calls saw it is the engine's business - it unwinds at its next
    // check, which may come before the next call.
    MESSAGE("of ", state.ticks.load(), " native calls, ", state.ticksSeeingTermination.load(),
            " saw the isolate terminating");

    fixture.iso().CancelTerminateExecution();
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE(TERMINATION, "termination: a stopped isolate is still safe to tear down") {
    // The shutdown shape an embedder actually has: stop the script, join the
    // thread, drop the isolate - without cancelling anything first.
    ub_test::Fixture fixture;

    RunState state;
    ExposeMarkers(fixture.context, state);

    Stopper stopper(fixture.iso(), state);
    {
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, "entered(); for (;;) {}").has_value());
        CHECK(handler.HasTerminated());
        // Torn down from *inside* the stop, which is the shape that matters:
        // nothing is cancelled, nothing is pumped, the isolate simply goes.
        CHECK(fixture.iso().IsExecutionTerminating());
    }
    stopper.Join();

    // The fixture's destructor is the rest of the assertion.
}
