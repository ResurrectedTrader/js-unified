/// \file
/// Bugs in the runtime - the platform, isolates, jobs, interrupts, termination,
/// engine faults and the inspector: the parts that run on more than one thread
/// or at moments nobody asked the engine anything - each pinned by the case
/// that showed it. Every case here failed, or crashed, on the code before its
/// fix, and passes on every backend after it. The comment on each says what it
/// caught.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "support/harness.h"

// CreateThread, for a thread with a stack of a known size.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

namespace {

/// What happened to a runaway recursion on a thread of its own.
struct Recursion {
    std::size_t stackLimitBytes = 0;
    bool isolateMade = false;
    bool caught = false;
};

DWORD WINAPI RecurseOnThisThread(LPVOID parameter) {
    auto* result = static_cast<Recursion*>(parameter);
    const auto isolate = ub::Isolate::New({.stackLimitBytes = result->stackLimitBytes});
    if (isolate == nullptr) {
        return 0;
    }
    result->isolateMade = true;
    const ub::HandleScope scope(*isolate);
    const auto context = ub::Context::New(*isolate);
    if (!context) {
        return 0;
    }
    const ub::ContextScope entered(*context);
    const ub::TryCatch caught(*isolate);
    const auto value = ub::Evaluate(*context, "function deeper(n) { return deeper(n + 1) + 1; } deeper(0)");
    result->caught = !value.has_value() && caught.HasCaught();
    return 0;
}

}  // namespace

UNIBIND_TEST_CASE(STACK_LIMIT, "regressions: a stack limit larger than the thread's stack is still a limit") {
    // The limit is measured from where the isolate is made, and an embedder
    // setting one does not always know how much stack the thread it lands on
    // has. SpiderMonkey's backend clamped it to the stack that exists; V8's set
    // it wherever it pointed, below the bottom of the real stack, and runaway
    // recursion walked off the end and took the process with it.
    Recursion result{.stackLimitBytes = std::size_t{16} * 1024 * 1024};
    HANDLE thread = CreateThread(nullptr, std::size_t{512} * 1024, &RecurseOnThisThread, &result,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    REQUIRE(thread != nullptr);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    REQUIRE(result.isolateMade);
    CHECK(result.caught);
}

namespace {

/// A DevTools connection that keeps what it is told.
struct DevTools final : ub::InspectorClient {
    void SendProtocolMessage(std::string_view message) override { messages.emplace_back(message); }
    void RunMessageLoopOnPause() override {}
    void QuitMessageLoopOnPause() override {}

    /// The response to request `id`, or empty if there was none.
    [[nodiscard]] std::string ResponseTo(int id) const {
        const std::string prefix = "{\"id\":" + std::to_string(id) + ",";
        for (const std::string& message : messages) {
            if (message.starts_with(prefix)) {
                return message;
            }
        }
        return {};
    }

    std::vector<std::string> messages;
};

}  // namespace

UNIBIND_TEST_CASE2(INSPECTOR, TERMINATION, "regressions: a stopped isolate runs no script for DevTools either") {
    // A stop holds until it is cancelled, whatever the engine would allow -
    // and a Runtime.evaluate goes to the engine directly, past the gates that
    // hold it everywhere else. V8 forgets a stop once its unwind is done, so
    // DevTools could run script in an isolate its embedder had stopped.
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return;
    }
    ub_test::Fixture fixture;
    DevTools client;
    auto inspector = ub::Inspector::New(fixture.iso(), client);
    REQUIRE(inspector != nullptr);
    inspector->ContextCreated(fixture.context, "main");
    auto session = inspector->Connect();
    REQUIRE(session != nullptr);

    fixture.iso().TerminateExecution();
    CHECK_FALSE(ub::Evaluate(fixture.context, "for (;;) {}").has_value());
    REQUIRE(fixture.iso().IsExecutionTerminating());

    session->DispatchProtocolMessage(
        R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"globalThis.ranWhileStopped = true"}})");
    INFO("answered: ", client.ResponseTo(1));
    CHECK(client.ResponseTo(1).find("\"error\"") != std::string::npos);
    session->DispatchProtocolMessage(R"({"id":2,"method":"Runtime.enable"})");
    CHECK_FALSE(client.ResponseTo(2).empty());

    fixture.iso().CancelTerminateExecution();
    CHECK(ub_test::EvalText(fixture.context, "typeof globalThis.ranWhileStopped") == "undefined");
    session->DispatchProtocolMessage(R"({"id":3,"method":"Runtime.evaluate","params":{"expression":"6 * 7"}})");
    CHECK(client.ResponseTo(3).find("\"value\":42") != std::string::npos);

    session.reset();
    inspector->ContextDestroyed(fixture.context);
}

namespace {

void Count(ub::Isolate& /*isolate*/, ub::CallbackData data) {
    if (auto* count = data.As<int>()) {
        ++*count;
    }
}

/// Call `post` with the next allocation failing, over and over, until one of
/// the calls actually needed memory. Answers how many were taken and whether
/// the one refused said so.
struct Starved {
    int taken = 0;
    bool refusedOne = false;
    bool refusalReported = false;
};

template <class Post>
Starved PostUntilStarved(Post post) {
    Starved result;
    for (int attempt = 0; attempt < 4096 && !result.refusedOne; ++attempt) {
        bool accepted = false;
        long long fired = 0;
        {
            ub_test::AllocationFailure failing(1);
            accepted = post();
            fired = failing.Stop();
        }
        if (fired > 0) {
            result.refusedOne = true;
            result.refusalReported = !accepted;
        }
        result.taken += accepted ? 1 : 0;
    }
    return result;
}

}  // namespace

UNIBIND_TEST_CASE2(JOBS, INTERRUPTS, "regressions: posting when there is no memory for it is refused, not fatal") {
    // PostJob, PostDelayedJob and RequestInterrupt are noexcept and grow a
    // queue, and a queue that cannot grow throws: std::terminate, from the one
    // operation an embedder calls from a thread that knows nothing of the
    // isolate. Now a request that cannot be kept is refused and says so.
    ub_test::Fixture fixture;
    ub::Isolate& isolate = fixture.iso();

    int jobRuns = 0;
    const Starved jobs = PostUntilStarved([&] { return isolate.PostJob(&Count, ub::CallbackData::For(jobRuns)); });
    int delayedRuns = 0;
    const Starved delayed =
        PostUntilStarved([&] { return isolate.PostDelayedJob(&Count, ub::CallbackData::For(delayedRuns), 1e-9); });
    int interruptRuns = 0;
    const Starved interrupts =
        PostUntilStarved([&] { return isolate.RequestInterrupt(&Count, ub::CallbackData::For(interruptRuns)); });
    if (!jobs.refusedOne || !delayed.refusedOne || !interrupts.refusedOne) {
        ub_test::ReportSkip("an allocation never failed, so nothing was refused");
        return;
    }
    CHECK(jobs.refusalReported);
    CHECK(delayed.refusalReported);
    CHECK(interrupts.refusalReported);

    // What was taken runs, once each; what was refused does not.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    isolate.PumpJobs();
    CHECK(jobRuns == jobs.taken);
    CHECK(delayedRuns == delayed.taken);
    CHECK(ub_test::EvalInt(fixture.context,
                           "(function () { let n = 0; for (let i = 0; i < 100; ++i) ++n; return n; })()") == 100);
    CHECK(interruptRuns == interrupts.taken);

    CHECK_FALSE(isolate.PostJob(nullptr, {}));
    CHECK_FALSE(isolate.PostDelayedJob(nullptr, {}, 1.0));
    CHECK_FALSE(isolate.RequestInterrupt(nullptr, {}));
}

namespace {

/// A connection that goes away - or stops - from inside the first message it
/// is sent that `trigger` names, which is where a socket write fails.
struct Dropping final : ub::InspectorClient {
    void SendProtocolMessage(std::string_view message) override {
        ++sent;
        if (!acted && !trigger.empty() && message.find(trigger) != std::string_view::npos) {
            acted = true;
            sentAtAct = sent;
            if (stopOnly) {
                (*session)->Stop();
            } else {
                session->reset();
            }
        }
        if (message.starts_with("{\"id\":")) {
            responses.emplace_back(message);
        }
    }
    void RunMessageLoopOnPause() override {
        ++pauses;
        if (*session != nullptr) {
            (*session)->Resume();
        }
    }
    void QuitMessageLoopOnPause() override {}

    std::unique_ptr<ub::InspectorSession>* session = nullptr;
    std::string trigger;
    bool stopOnly = false;
    bool acted = false;
    int sent = 0;
    int sentAtAct = 0;
    int pauses = 0;
    std::vector<std::string> responses;
};

}  // namespace

UNIBIND_TEST_CASE(INSPECTOR, "regressions: a session may go, or stop, inside a notification it is sent") {
    // "A session may be destroyed anywhere on the isolate's thread", and the
    // commonest place is the one an embedder does not choose: a socket write
    // that fails inside SendProtocolMessage. For a notification the engine is
    // still inside the session's own agent when the client is called - a
    // console message, a script being parsed - and V8 went on to use the
    // agent it had just freed. Stop in a scriptParsed notification did the
    // same to the debugger agent.
    if (!ub::Inspector::Supported()) {
        ub_test::ReportSkip("this backend has no inspector");
        return;
    }
    ub_test::Fixture fixture;
    std::unique_ptr<ub::InspectorSession> session;
    Dropping client;
    client.session = &session;
    auto inspector = ub::Inspector::New(fixture.iso(), client);
    REQUIRE(inspector != nullptr);
    inspector->ContextCreated(fixture.context, "main");

    const auto connect = [&](std::string trigger, bool stopOnly) {
        session = inspector->Connect();
        REQUIRE(session != nullptr);
        session->DispatchProtocolMessage(R"({"id":1,"method":"Runtime.enable"})");
        session->DispatchProtocolMessage(R"({"id":2,"method":"Debugger.enable"})");
        client.trigger = std::move(trigger);
        client.stopOnly = stopOnly;
        client.acted = false;
        client.pauses = 0;
    };

    // Gone inside a console message script logged.
    connect("Runtime.consoleAPICalled", false);
    CHECK(ub_test::EvalInt(fixture.context, "console.log('one'); console.log('two'); 1") == 1);
    CHECK(client.acted);
    CHECK(session == nullptr);
    CHECK(client.sent == client.sentAtAct);
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 2") == 2);
    CHECK(client.pauses == 0);

    // Gone inside the notice that a script was parsed.
    connect("Debugger.scriptParsed", false);
    CHECK(ub::Evaluate(fixture.context, "debugger; 3", {.resourceName = "parsed.js"}).has_value());
    CHECK(client.acted);
    CHECK(session == nullptr);
    CHECK(client.pauses == 0);
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 4") == 4);
    CHECK(client.sent == client.sentAtAct);

    // Stopped there, and so pausing nothing - and still answered.
    connect("Debugger.scriptParsed", true);
    CHECK(ub::Evaluate(fixture.context, "debugger; 5", {.resourceName = "stopped.js"}).has_value());
    CHECK(client.acted);
    REQUIRE(session != nullptr);
    CHECK(client.pauses == 0);
    session->DispatchProtocolMessage(R"({"id":3,"method":"Runtime.evaluate","params":{"expression":"6 * 7"}})");
    CHECK(client.responses.back().find("\"value\":42") != std::string::npos);
    CHECK(ub_test::EvalInt(fixture.context, "debugger; 6") == 6);
    CHECK(client.pauses == 0);

    session.reset();
    inspector->ContextDestroyed(fixture.context);
}
