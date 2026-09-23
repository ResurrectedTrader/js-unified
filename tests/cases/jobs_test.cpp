/// \file
/// Work that is not a call: promise continuations, posted jobs, and the
/// interrupt that lets a foreign thread say "look now".
///
/// The sentence these cases exist to hold the backends to is in
/// `unibind/isolate.h`: **nothing else runs any of it**. Script containing `await`
/// or `.then` compiles and runs, and its continuations do not execute until
/// `PumpJobs` is called - no error, no exception, the work simply does not
/// happen. One engine would otherwise drain its own queue when a call returns
/// and the other would not, and when a continuation runs is something *script
/// can see*, so it is made uniform rather than left as a hint. That makes these
/// the cases that keep two backends observably equal where the engines'
/// defaults are not.
///
/// The other half is the pair `PostJob` + `RequestInterrupt`, which are only
/// useful together and are easy to get subtly wrong: the interrupt reaches the
/// isolate's thread while script is running, and the *job* runs at the drain
/// point, never inside the interrupt.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "support/harness.h"

namespace {

struct JobState {
    const ub::Context* context = nullptr;
    std::thread::id ranOn;
    std::atomic<int> jobRuns{0};
    std::atomic<int> interruptRuns{0};
    std::atomic<bool> flag{false};
    std::vector<int> order;
    int nextTag = 0;
    std::atomic<int> handleValue{0};
    const ub::Global<ub::Promise>* promise = nullptr;
};

JobState* From(ub::CallbackData data) {
    return data.As<JobState>();
}

/// What `PostJob` documents a job as arriving in: the isolate's thread, no
/// realm current, and nothing but an `Isolate&` to work with. Everything here
/// takes exactly that and nothing more.
void MakeValuesWithNoRealm(ub::Isolate& isolate, ub::CallbackData data) {
    auto* state = From(data);
    if (state == nullptr) {
        return;
    }
    const ub::HandleScope scope(isolate);
    const auto text = ub::String::New(isolate, "made in a job");
    state->jobRuns.fetch_add(text.has_value() ? 1 : 0, std::memory_order_relaxed);
    state->handleValue.store(static_cast<int>(ub::CaptureStackFrames(isolate).size()), std::memory_order_relaxed);
    state->flag.store(true, std::memory_order_release);
}

void RecordJob(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    auto* state = From(data);
    if (state == nullptr) {
        return;
    }
    state->ranOn = std::this_thread::get_id();
    state->jobRuns.fetch_add(1, std::memory_order_relaxed);
    state->order.push_back(state->nextTag++);
}

/// The only thing an interrupt callback is allowed to do: notice, and set a
/// flag. It runs inside a runtime function called from arbitrary bytecode, so
/// it runs no script and throws nothing.
void NoticeAndReturn(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    auto* state = From(data);
    if (state == nullptr) {
        return;
    }
    state->interruptRuns.fetch_add(1, std::memory_order_relaxed);
    state->flag.store(true, std::memory_order_release);
}

/// Settles the promise script is waiting on, from a job - which is the shape an
/// embedder actually uses: the answer arrives on another thread, and the
/// continuation has to run on this one.
void SettleThePromise(ub::Isolate& isolate, ub::CallbackData data) {
    auto* state = From(data);
    if (state == nullptr || state->context == nullptr) {
        return;
    }
    state->jobRuns.fetch_add(1, std::memory_order_relaxed);

    ub::HandleScope scope(isolate);
    ub::ContextScope entered(*state->context);
    ub::TryCatch handler(isolate);

    const auto settle = state->context->GlobalObject().Get(*state->context, "settle");
    if (!settle) {
        return;
    }
    const auto asFunction = settle->To<ub::Function>();
    if (!asFunction) {
        return;
    }
    const std::array<ub::Local<ub::Value>, 1> arguments{ub::Integer::New(isolate, 42)};
    (void)asFunction->Call(*state->context, state->context->GlobalObject(), arguments);
}

/// Script's window onto the flag an interrupt sets, so a loop can wait for one.
void ReadFlag(const ub::CallbackInfo& info) {
    auto* state = info.Data<JobState>();
    if (state == nullptr) {
        info.ThrowTypeError("callback data missing");
        return;
    }
    info.GetReturnValue().Set(state->flag.load(std::memory_order_acquire));
}

}  // namespace

UNIBIND_TEST_CASE(JOBS, "jobs: a promise continuation waits for the pump and nothing else") {
    ub_test::Fixture fixture;

    (void)ub_test::Eval(fixture.context, R"(
        globalThis.log = [];
        Promise.resolve('first').then(v => { log.push(v); });
        log.length;
    )");

    // The script has finished. On an engine left to its own policy this would
    // already have run.
    CHECK(ub_test::EvalInt(fixture.context, "log.length") == 0);

    fixture.iso().PumpJobs();
    CHECK(ub_test::EvalInt(fixture.context, "log.length") == 1);
    CHECK(ub_test::EvalText(fixture.context, "log[0]") == "first");

    // And a pump with nothing pending is not an error.
    fixture.iso().PumpJobs();
    CHECK(ub_test::EvalInt(fixture.context, "log.length") == 1);
}

UNIBIND_TEST_CASE(JOBS, "jobs: an async function resumes at the pump") {
    ub_test::Fixture fixture;

    (void)ub_test::Eval(fixture.context, R"(
        globalThis.log = [];
        async function work() {
            log.push('before');
            await null;
            log.push('after');
            return 'done';
        }
        globalThis.result = work();
        log.push('sync');
    )");

    // Everything up to the first await ran; nothing past it did.
    CHECK(ub_test::EvalText(fixture.context, "log.join(',')") == "before,sync");

    fixture.iso().PumpJobs();
    CHECK(ub_test::EvalText(fixture.context, "log.join(',')") == "before,sync,after");

    (void)ub_test::Eval(fixture.context, "result.then(v => log.push(v));");
    fixture.iso().PumpJobs();
    CHECK(ub_test::EvalText(fixture.context, "log.join(',')") == "before,sync,after,done");
}

UNIBIND_TEST_CASE(JOBS, "jobs: a chain of continuations drains in one pump") {
    ub_test::Fixture fixture;

    (void)ub_test::Eval(fixture.context, R"(
        globalThis.log = [];
        Promise.resolve(1)
            .then(v => { log.push(v); return v + 1; })
            .then(v => { log.push(v); return v + 1; })
            .then(v => { log.push(v); });
    )");

    CHECK(ub_test::EvalInt(fixture.context, "log.length") == 0);
    fixture.iso().PumpJobs();
    // Until there is nothing left, not one round of it.
    CHECK(ub_test::EvalText(fixture.context, "log.join(',')") == "1,2,3");
}

UNIBIND_TEST_CASE(JOBS, "jobs: posted work runs on the isolate's thread, in order, once each") {
    ub_test::Fixture fixture;

    JobState state;
    state.context = &fixture.context;
    const std::thread::id here = std::this_thread::get_id();

    std::thread elsewhere([&fixture, &state] {
        for (int i = 0; i < 3; ++i) {
            fixture.iso().PostJob(&RecordJob, ub::CallbackData::For(state));
        }
    });
    elsewhere.join();

    // Posting does not run anything.
    CHECK(state.jobRuns.load() == 0);

    fixture.iso().PumpJobs();

    CHECK(state.jobRuns.load() == 3);
    CHECK(state.order == std::vector<int>{0, 1, 2});
    // The same callback posted three times ran three times - never coalesced -
    // and on the thread that owns the isolate, not the one that posted.
    CHECK(state.ranOn == here);
}

UNIBIND_TEST_CASE(JOBS, "jobs: work posted from a job is work, and is drained too") {
    ub_test::Fixture fixture;

    JobState state;
    state.context = &fixture.context;
    fixture.iso().PostJob(
        [](ub::Isolate& isolate, ub::CallbackData data) {
            RecordJob(isolate, data);
            isolate.PostJob(&RecordJob, data);
        },
        ub::CallbackData::For(state));

    fixture.iso().PumpJobs();
    CHECK(state.jobRuns.load() == 2);
}

UNIBIND_TEST_CASE(JOBS, "jobs: a job that settles a promise sees the continuation in the same pump") {
    // The reason one drain is better than two: the answer arrives from
    // somewhere else, and the script that was waiting for it carries on without
    // the embedder having to pump again.
    ub_test::Fixture fixture;

    JobState state;
    state.context = &fixture.context;

    (void)ub_test::Eval(fixture.context, R"(
        globalThis.log = [];
        globalThis.settle = null;
        globalThis.waiting = new Promise(resolve => { settle = resolve; });
        waiting.then(v => { log.push('answered:' + v); });
    )");
    CHECK(ub_test::EvalInt(fixture.context, "log.length") == 0);

    std::thread elsewhere(
        [&fixture, &state] { fixture.iso().PostJob(&SettleThePromise, ub::CallbackData::For(state)); });
    elsewhere.join();

    fixture.iso().PumpJobs();

    CHECK(state.jobRuns.load() == 1);
    CHECK(ub_test::EvalText(fixture.context, "log.join(',')") == "answered:42");
}

UNIBIND_TEST_CASE2(JOBS, INTERRUPTS, "jobs: an interrupt reaches a running script, and the job it announced does not") {
    // The sentence that is the difference between this working and failing
    // under load: a posted job runs at the drain point, *never* inside the
    // interrupt. So the loop below ends because the interrupt set a flag, and
    // the job it was announcing has still not run when it does.
    ub_test::Fixture fixture;

    JobState state;
    state.context = &fixture.context;

    const auto flagged = ub::Function::New(fixture.context, &ReadFlag, ub::CallbackData::For(state));
    REQUIRE(flagged.has_value());
    ub_test::Expose(fixture.context, "flagged", *flagged);

    std::thread elsewhere([&fixture, &state] {
        // Give the script a moment to be running, then do what a foreign thread
        // is supposed to do: post the work, then ask the thread to notice.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        fixture.iso().PostJob(&RecordJob, ub::CallbackData::For(state));
        fixture.iso().RequestInterrupt(&NoticeAndReturn, ub::CallbackData::For(state));
    });

    const auto ran = ub::Evaluate(fixture.context, "let spins = 0; while (!flagged()) { spins += 1; } spins");
    elsewhere.join();
    REQUIRE(ran.has_value());

    CHECK(state.interruptRuns.load() == 1);
    CHECK(state.jobRuns.load() == 0);

    fixture.iso().PumpJobs();
    CHECK(state.jobRuns.load() == 1);
}

UNIBIND_TEST_CASE(JOBS, "jobs: what is still queued when the isolate goes is dropped, not run") {
    // Said out loud in the header because "it will run eventually" is what a
    // caller would otherwise assume.
    JobState state;

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        isolate->PostJob(&RecordJob, ub::CallbackData::For(state));
        isolate->PostJob(&RecordJob, ub::CallbackData::For(state));
        CHECK(state.jobRuns.load() == 0);
    }

    CHECK(state.jobRuns.load() == 0);
}

UNIBIND_TEST_CASE(JOBS, "jobs: what a job throws stops at the pump") {
    // A pump is not a call and has nowhere to put an exception, so it swallows
    // one - and the isolate is fine afterwards, which is the part worth
    // asserting.
    ub_test::Fixture fixture;

    JobState state;
    state.context = &fixture.context;

    fixture.iso().PostJob(
        [](ub::Isolate& isolate, ub::CallbackData data) {
            RecordJob(isolate, data);
            auto* self = From(data);
            if (self == nullptr || self->context == nullptr) {
                return;
            }
            ub::HandleScope scope(isolate);
            ub::ContextScope entered(*self->context);
            (void)ub::Evaluate(*self->context, "throw new Error('from a job')");
        },
        ub::CallbackData::For(state));
    fixture.iso().PostJob(&RecordJob, ub::CallbackData::For(state));

    fixture.iso().PumpJobs();

    // Both ran: one throwing job does not cancel the queue.
    CHECK(state.jobRuns.load() == 2);
    CHECK_FALSE(fixture.iso().HasPendingException());
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE2(JOBS, TERMINATION, "jobs: nothing is pumped while a stop is in force, and the queue survives it") {
    ub_test::Fixture fixture;

    JobState state;
    state.context = &fixture.context;
    fixture.iso().PostJob(&RecordJob, ub::CallbackData::For(state));

    fixture.iso().TerminateExecution();
    fixture.iso().PumpJobs();
    CHECK(state.jobRuns.load() == 0);

    fixture.iso().CancelTerminateExecution();
    fixture.iso().PumpJobs();
    CHECK(state.jobRuns.load() == 1);
}

UNIBIND_TEST_CASE(INTERRUPTS, "interrupts: a callback may make a handle, which is the half that is allowed") {
    // The contract is a split, not a ban (decision 24): handles are legal
    // inside an interrupt because the engine sets a scope up for exactly this,
    // and running script is not. The first half needs asserting, because a
    // backend that took the cautious reading and sealed handles would break
    // every honest use - a watchdog that samples, a profiler tick - and nothing
    // else would notice.
    ub_test::Fixture fixture;

    JobState state;
    state.context = &fixture.context;

    const auto flagged = ub::Function::New(fixture.context, &ReadFlag, ub::CallbackData::For(state));
    REQUIRE(flagged.has_value());
    ub_test::Expose(fixture.context, "flagged", *flagged);

    std::thread elsewhere([&fixture, &state] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        fixture.iso().RequestInterrupt(
            [](ub::Isolate& isolate, ub::CallbackData data) {
                auto* self = From(data);
                if (self == nullptr) {
                    return;
                }
                // A handle, made and read, from inside the interrupt.
                ub::HandleScope scope(isolate);
                const auto made = ub::Integer::New(isolate, 4242);
                self->handleValue.store(made.IsEmpty() ? 0 : made.Int32Value(), std::memory_order_relaxed);
                self->interruptRuns.fetch_add(1, std::memory_order_relaxed);
                self->flag.store(true, std::memory_order_release);
            },
            ub::CallbackData::For(state));
    });

    const auto ran = ub::Evaluate(fixture.context, "let spins = 0; while (!flagged()) { spins += 1; } spins");
    elsewhere.join();
    REQUIRE(ran.has_value());

    CHECK(state.interruptRuns.load() == 1);
    CHECK(state.handleValue.load() == 4242);

    // And the isolate is untroubled by having been interrupted.
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

// ---------------------------------------------------------------------------
// Promises the embedder settles
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(PROMISES, "promises: native makes one, script waits on it, native settles it") {
    // The whole point of the surface: an asynchronous native result. The
    // embedder keeps its own root to settle from, because handing the promise
    // to script hands script the power to settle it too.
    ub_test::Fixture fixture;

    const auto promise = ub::Promise::New(fixture.context);
    REQUIRE(promise.has_value());
    CHECK(ub::GetState(*promise) == ub::PromiseState::Pending);

    ub_test::Expose(fixture.context, "waiting", *promise);
    (void)ub_test::Eval(fixture.context, "globalThis.log = []; waiting.then(v => { log.push('got ' + v); });");

    // Pending means pending: pumping now runs nothing, because nothing has
    // settled.
    fixture.iso().PumpJobs();
    CHECK(ub_test::EvalInt(fixture.context, "log.length") == 0);

    REQUIRE(ub::Resolve(fixture.context, *promise, ub::Integer::New(fixture.iso(), 7)).value_or(false));
    CHECK(ub::GetState(*promise) == ub::PromiseState::Fulfilled);

    // Settled, but the continuation still waits for the drain - nothing else
    // runs it (decision 23).
    CHECK(ub_test::EvalInt(fixture.context, "log.length") == 0);
    fixture.iso().PumpJobs();
    CHECK(ub_test::EvalText(fixture.context, "log.join(',')") == "got 7");
}

UNIBIND_TEST_CASE(PROMISES, "promises: a rejection reaches the handler that was waiting for one") {
    ub_test::Fixture fixture;

    const auto promise = ub::Promise::New(fixture.context);
    REQUIRE(promise.has_value());
    ub_test::Expose(fixture.context, "waiting", *promise);
    (void)ub_test::Eval(fixture.context, R"(
        globalThis.log = [];
        waiting.then(v => { log.push('resolved'); }, e => { log.push('rejected: ' + e.message); });
    )");

    const auto reason = ub::MakeError(fixture.context, ub::ErrorKind::TypeError, "no answer");
    REQUIRE(reason.has_value());
    REQUIRE(ub::Reject(fixture.context, *promise, *reason).value_or(false));
    CHECK(ub::GetState(*promise) == ub::PromiseState::Rejected);

    fixture.iso().PumpJobs();
    const std::string logged = ub_test::EvalText(fixture.context, "log.join(',')");
    CHECK(logged.rfind("rejected: ", 0) == 0);
    CHECK(logged.find("no answer") != std::string::npos);
}

UNIBIND_TEST_CASE(PROMISES, "promises: the first settlement is the only one") {
    ub_test::Fixture fixture;

    const auto promise = ub::Promise::New(fixture.context);
    REQUIRE(promise.has_value());
    ub_test::Expose(fixture.context, "waiting", *promise);
    (void)ub_test::Eval(
        fixture.context,
        "globalThis.log = []; waiting.then(v => { log.push('first ' + v); }, e => { log.push('no'); });");

    REQUIRE(ub::Resolve(fixture.context, *promise, ub::Integer::New(fixture.iso(), 1)).value_or(false));
    // A second settlement of either kind changes nothing; what it *reports* is
    // the engine's business, so only the state and the outcome are asserted.
    (void)ub::Resolve(fixture.context, *promise, ub::Integer::New(fixture.iso(), 2));
    (void)ub::Reject(fixture.context, *promise, ub::Integer::New(fixture.iso(), 3));

    CHECK(ub::GetState(*promise) == ub::PromiseState::Fulfilled);
    fixture.iso().PumpJobs();
    CHECK(ub_test::EvalText(fixture.context, "log.join(',')") == "first 1");
}

UNIBIND_TEST_CASE(PROMISES, "promises: a promise script made is the same kind of thing") {
    // One made by script narrows to `Promise` and answers the same question, so
    // the state accessor is about promises rather than about the ones this API
    // happened to make.
    ub_test::Fixture fixture;

    const auto pending = ub_test::Eval(fixture.context, "new Promise(() => {})");
    const auto asPromise = pending.To<ub::Promise>();
    REQUIRE(asPromise.has_value());
    CHECK(ub::GetState(*asPromise) == ub::PromiseState::Pending);

    const auto settled = ub_test::Eval(fixture.context, "Promise.resolve(1)");
    const auto asSettled = settled.To<ub::Promise>();
    REQUIRE(asSettled.has_value());
    CHECK(ub::GetState(*asSettled) == ub::PromiseState::Fulfilled);

    // And a thenable that is not a promise is not one.
    CHECK_FALSE(ub_test::Eval(fixture.context, "({ then() {} })").To<ub::Promise>().has_value());
}

UNIBIND_TEST_CASE2(PROMISES, JOBS, "promises: a job settles it and the continuation runs in the same pump") {
    // The shape an embedder actually has - the answer arrives on another
    // thread - with the promise made natively rather than fished out of script.
    ub_test::Fixture fixture;

    const auto promise = ub::Promise::New(fixture.context);
    REQUIRE(promise.has_value());
    ub::Global<ub::Promise> kept(fixture.iso(), *promise);
    ub_test::Expose(fixture.context, "waiting", *promise);
    (void)ub_test::Eval(fixture.context, "globalThis.log = []; waiting.then(v => { log.push('answered ' + v); });");

    JobState state;
    state.context = &fixture.context;
    state.promise = &kept;

    std::thread elsewhere([&fixture, &state] {
        fixture.iso().PostJob(
            [](ub::Isolate& isolate, ub::CallbackData data) {
                auto* self = From(data);
                if (self == nullptr || self->context == nullptr || self->promise == nullptr) {
                    return;
                }
                self->jobRuns.fetch_add(1, std::memory_order_relaxed);
                ub::HandleScope scope(isolate);
                ub::ContextScope entered(*self->context);
                (void)ub::Resolve(*self->context, self->promise->Get(isolate), ub::Integer::New(isolate, 99));
            },
            ub::CallbackData::For(state));
    });
    elsewhere.join();

    CHECK(ub_test::EvalInt(fixture.context, "log.length") == 0);
    fixture.iso().PumpJobs();

    CHECK(state.jobRuns.load() == 1);
    CHECK(ub_test::EvalText(fixture.context, "log.join(',')") == "answered 99");
}

UNIBIND_TEST_CASE(JOBS, "jobs: a job can make a value and read the stack with no realm entered") {
    // `Isolate::PostJob` says a job "is not inside a call and no realm is
    // current", and every operation used here takes an `Isolate&` and nothing
    // else - `String::New` because a job that reports anything needs a string,
    // `CaptureStackFrames` because `unibind/exception.h` says it needs not even
    // a `HandleScope`. Whatever the answer is, it must be an answer: an empty
    // one is a contract, a crash is not.
    //
    // Deliberately *not* `Fixture`, which keeps a `ContextScope` open for the
    // whole case and so would never reach the state this is about: a realm is
    // current there, and the job would be running in the fixture's.
    JobState state;
    auto isolate = ub_test::Fixture::MakeIsolate();
    REQUIRE(isolate != nullptr);
    {
        const ub::HandleScope scope(*isolate);
        auto context = ub_test::Fixture::MakeContext(*isolate);
        isolate->PostJob(&MakeValuesWithNoRealm, ub::CallbackData::For(state));
        isolate->PumpJobs();
    }

    REQUIRE(state.flag.load(std::memory_order_acquire));
    CHECK(state.jobRuns.load() == 1);
    // No script is running under a pump, so the stack is empty rather than
    // wrong - which is also what makes this readable at all.
    CHECK(state.handleValue.load() == 0);
}
