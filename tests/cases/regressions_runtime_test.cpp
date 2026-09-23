/// \file
/// Bugs in the runtime - the platform, isolates, jobs, interrupts, termination,
/// engine faults and the inspector: the parts that run on more than one thread
/// or at moments nobody asked the engine anything - each pinned by the case
/// that showed it. Every case here failed, or crashed, on the code before its
/// fix, and passes on every backend after it. The comment on each says what it
/// caught.

#include <atomic>
#include <string>
#include <vector>

#include "support/harness.h"

namespace {

/// A posted job that says which one it was, into a log both backends can be
/// compared on.
struct Tagged {
    std::vector<std::string>* log = nullptr;
    std::string tag;
};

void LogTag(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    if (auto* job = data.As<Tagged>()) {
        job->log->push_back(job->tag);
    }
}

void LogTagAndStop(ub::Isolate& isolate, ub::CallbackData data) {
    LogTag(isolate, data);
    isolate.TerminateExecution();
}

/// A job that settles the promise script is waiting on, as an answer arriving
/// from elsewhere does, and logs that it ran.
struct Settler {
    const ub::Context* context = nullptr;
    std::vector<std::string>* log = nullptr;
};

void SettleFromJob(ub::Isolate& isolate, ub::CallbackData data) {
    auto* settler = data.As<Settler>();
    if (settler == nullptr) {
        return;
    }
    settler->log->emplace_back("settled");
    const ub::HandleScope scope(isolate);
    const ub::ContextScope entered(*settler->context);
    (void)ub::Evaluate(*settler->context, "settle()");
}

/// Script's way to record into the same log the jobs write to.
void LogFromScript(const ub::CallbackInfo& info) {
    auto* log = info.Data<std::vector<std::string>>();
    if (log == nullptr || info.Length() < 1) {
        return;
    }
    const auto text = info[0].To<ub::String>();
    if (text) {
        log->push_back(text->Utf8Value());
    }
}

void StopFromScript(const ub::CallbackInfo& info) {
    info.GetIsolate().TerminateExecution();
}

void ExposeLogAndStop(const ub::Context& context, std::vector<std::string>& log) {
    const auto logFunction = ub::Function::New(context, &LogFromScript, ub::CallbackData::For(log));
    REQUIRE(logFunction.has_value());
    ub_test::Expose(context, "log", *logFunction);
    const auto stopFunction = ub::Function::New(context, &StopFromScript);
    REQUIRE(stopFunction.has_value());
    ub_test::Expose(context, "stop", *stopFunction);
}

}  // namespace

UNIBIND_TEST_CASE2(JOBS, TERMINATION,
                   "regressions: work queued behind a job that stops the isolate waits for the cancel") {
    // "The queues survive" a stop. On SpiderMonkey the pump took every posted
    // job off the queue before running the first, and a job that stopped the
    // isolate took the rest of that batch with it: gone, not waiting.
    ub_test::Fixture fixture;
    std::vector<std::string> log;
    Tagged first{.log = &log, .tag = "first stops"};
    Tagged second{.log = &log, .tag = "second"};
    fixture.iso().PostJob(&LogTagAndStop, ub::CallbackData::For(first));
    fixture.iso().PostJob(&LogTag, ub::CallbackData::For(second));

    fixture.iso().PumpJobs();
    CHECK(log == std::vector<std::string>{"first stops"});

    fixture.iso().CancelTerminateExecution();
    fixture.iso().PumpJobs();
    CHECK(log == std::vector<std::string>{"first stops", "second"});
}

UNIBIND_TEST_CASE2(JOBS, PROMISES, "regressions: the continuations a job queues run before the next job does") {
    // One drain, and one order on both backends: engine jobs, then a piece of
    // posted work, then engine jobs again. SpiderMonkey ran every posted job
    // before any continuation they queued, V8 each job's continuations before
    // the next job - and which one a script saw depended on the engine.
    ub_test::Fixture fixture;
    std::vector<std::string> log;
    ExposeLogAndStop(fixture.context, log);
    (void)ub_test::Eval(fixture.context, R"(
        globalThis.settle = null;
        new Promise(resolve => { settle = resolve; }).then(() => log('continuation'));
    )");

    Settler settler{.context = &fixture.context, .log = &log};
    Tagged next{.log = &log, .tag = "next job"};
    fixture.iso().PostJob(&SettleFromJob, ub::CallbackData::For(settler));
    fixture.iso().PostJob(&LogTag, ub::CallbackData::For(next));
    fixture.iso().PumpJobs();

    CHECK(log == std::vector<std::string>{"settled", "continuation", "next job"});
}

UNIBIND_TEST_CASE2(JOBS, TERMINATION,
                   "regressions: work queued behind a continuation that stops is not run in the stop") {
    // A stop that lands in a continuation stops the pump there. Both backends
    // went on to take the next posted job and run it, stop or no stop - its
    // scripts refused, its native side effects not.
    ub_test::Fixture fixture;
    std::vector<std::string> log;
    ExposeLogAndStop(fixture.context, log);
    (void)ub_test::Eval(fixture.context, "Promise.resolve().then(() => { log('continuation stops'); stop(); })");
    Tagged job{.log = &log, .tag = "job"};
    fixture.iso().PostJob(&LogTag, ub::CallbackData::For(job));

    fixture.iso().PumpJobs();
    CHECK(fixture.iso().IsExecutionTerminating());
    CHECK(log == std::vector<std::string>{"continuation stops"});

    fixture.iso().CancelTerminateExecution();
    fixture.iso().PumpJobs();
    CHECK(log == std::vector<std::string>{"continuation stops", "job"});
}

UNIBIND_TEST_CASE2(PROMISES, TERMINATION, "regressions: the continuations queued behind one that stops never run") {
    // A continuation that stops the isolate stops the continuations queued
    // behind it too, on both engines. SpiderMonkey's drain went on past the
    // stopped one and ran the rest there and then, stop or no stop; V8 empties
    // its queue when a stop lands in it, which cannot be kept, so that is what
    // both backends do - the continuations behind it do not wait for the
    // cancel the way posted work does.
    ub_test::Fixture fixture;
    std::vector<std::string> log;
    ExposeLogAndStop(fixture.context, log);
    (void)ub_test::Eval(fixture.context, R"(
        const settled = Promise.resolve();
        settled.then(() => { log('first stops'); stop(); });
        settled.then(() => log('second'));
        settled.then(() => log('third'));
    )");

    fixture.iso().PumpJobs();
    CHECK(fixture.iso().IsExecutionTerminating());
    CHECK(log == std::vector<std::string>{"first stops"});

    fixture.iso().CancelTerminateExecution();
    fixture.iso().PumpJobs();
    CHECK(log == std::vector<std::string>{"first stops"});

    // And the queue is an ordinary queue again afterwards.
    (void)ub_test::Eval(fixture.context, "Promise.resolve().then(() => log('later'))");
    fixture.iso().PumpJobs();
    CHECK(log == std::vector<std::string>{"first stops", "later"});
}

namespace {

/// An interrupt that asks for itself again from inside its own callback, a set
/// number of times, noting how far the script had got each time it ran.
struct Chain {
    std::atomic<int> ticks{0};
    std::vector<int> ranAtTick;
    int remaining = 0;
    std::atomic<bool> done{false};
};

void ChainLink(ub::Isolate& isolate, ub::CallbackData data) {
    auto* chain = data.As<Chain>();
    chain->ranAtTick.push_back(chain->ticks.load());
    if (--chain->remaining > 0) {
        isolate.RequestInterrupt(&ChainLink, data);
    } else {
        chain->done = true;
    }
}

void TickUntilDone(const ub::CallbackInfo& info) {
    auto* chain = info.Data<Chain>();
    chain->ticks.fetch_add(1);
    info.GetReturnValue().Set(chain->done.load());
}

}  // namespace

UNIBIND_TEST_CASE(INTERRUPTS, "regressions: an interrupt asked for inside one runs in the same pass") {
    // When an interrupt a callback asks for runs was the engine's choice: V8's
    // dispatch runs its queue until it is empty, so the new request ran before
    // the script moved on, while SpiderMonkey's waited for the next check - and
    // a callback that re-armed itself was a sampler on one backend and a hang
    // on the other. V8's cannot be changed, so both run it in the same pass.
    ub_test::Fixture fixture;
    Chain chain;
    chain.remaining = 5;
    const auto tick = ub::Function::New(fixture.context, &TickUntilDone, ub::CallbackData::For(chain));
    REQUIRE(tick.has_value());
    ub_test::Expose(fixture.context, "tick", *tick);

    fixture.iso().RequestInterrupt(&ChainLink, ub::CallbackData::For(chain));
    CHECK(ub_test::EvalInt(fixture.context, "let n = 0; while (!tick() && n < 1e7) { ++n; } 1") == 1);

    REQUIRE(chain.ranAtTick.size() == 5);
    CHECK(chain.ranAtTick == std::vector<int>(5, chain.ranAtTick.front()));
}
