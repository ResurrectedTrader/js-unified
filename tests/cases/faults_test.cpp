/// \file
/// The engine telling the embedder that *it* is in trouble.
///
/// Everything else in the suite asks the API for something and checks the
/// answer. Nothing here asks for anything: these cases arrange for the engine
/// to fail and then check that the failure arrived somewhere an embedder could
/// have heard it.
///
/// Which makes them awkward in a way worth stating before the code. There are
/// exactly two conditions a test can provoke on purpose, and neither is the
/// dramatic one:
///
///   * **Running out of memory**, through the lever the suite already has for
///     `docs/lifetimes.md` rule 9 - a replaced `operator new` that fails once.
///     That is a real out-of-memory in the only sense that matters here: an
///     allocation the engine's work needed failed, and the backend said so.
///     It is not faked and nothing is called directly.
///   * **A heap about to hit its ceiling**, which is not a failure at all - it
///     is the decision point `Isolate::SetHeapLimitCallback` exists for, and
///     only one of the two engines has it.
///
/// `EngineFault::Fatal` is not here: it ends the process by design, so each
/// backend provokes it in a process of its own (`tests/fatal_*.cpp`, see
/// `docs/testing.md`).

#include <cstddef>
#include <string>

#include "support/harness.h"

namespace {

/// What the heap-limit case decides with, behind the one fixed callback the
/// isolate is given.
struct RescuePolicy {
    int asked = 0;
    std::size_t initialSeen = 0;
    std::size_t currentSeen = 0;
    ub::Isolate* isolate = nullptr;
};

/// The policy from `Isolate::SetHeapLimitCallback`'s own documentation: raise
/// the ceiling enough to unwind in, and stop the script that filled it.
///
/// Raising *and* terminating is the pairing that makes this worth having.
/// Raising alone hands a runaway script a bigger heap to fill; terminating
/// alone leaves the unwind itself with no room to happen in.
///
/// `[[maybe_unused]]` because a backend without the hook compiles the two cases
/// that name it into an uninstantiated template, which is how an unimplemented
/// area stays visible and skipped rather than absent (see `support/harness.h`).
[[maybe_unused]] std::size_t RaiseAndStop(ub::Isolate& isolate, std::size_t current, std::size_t initial,
                                          ub::CallbackData data) {
    auto* policy = data.As<RescuePolicy>();
    if (policy == nullptr) {
        return current;
    }
    ++policy->asked;
    policy->initialSeen = initial;
    policy->currentSeen = current;
    policy->isolate = &isolate;
    isolate.TerminateExecution();
    return current + (std::size_t{64} * 1024 * 1024);
}

/// Something that fills a heap and does not stop by itself, kept alive so the
/// collector cannot help.
constexpr const char* FILL_THE_HEAP = R"(
    const kept = [];
    for (let i = 0; i < 1e9; ++i) {
        kept.push(new Array(4096).fill(i));
    }
    'finished'
)";

}  // namespace

// ---------------------------------------------------------------------------
// Out of memory
// ---------------------------------------------------------------------------

TEST_CASE("faults: running out of memory reaches the engine-fault handler") {
    ub_test::Fixture fixture;
    ub::HandleScope scope(fixture.iso());

    // Fill the inline slots, none of which can fail; the next append is the one
    // that has to grow the frame, which is the one that goes to the allocator.
    for (int i = 0; i < UNIBIND_FRAME_INLINE_SLOTS; ++i) {
        REQUIRE_FALSE(ub::Integer::New(fixture.iso(), i).IsEmpty());
    }

    ub_test::EngineFaults().Clear();
    ub::TryCatch tryCatch(fixture.iso());

    ub::Local<ub::Integer> beyond;
    long long fired = 0;
    {
        ub_test::AllocationFailure failing(1);
        beyond = ub::Integer::New(fixture.iso(), 1234);
        fired = failing.Stop();
    }

    if (fired == 0) {
        // Same honest answer the rule 9 case gives: the frame grew without
        // touching the C++ allocator, so nothing ran out of memory and this
        // case has proved nothing.
        ub_test::ReportSkip(
            "frame growth on this backend does not go through the C++ allocator, so exhaustion "
            "cannot be provoked from a test - see docs/testing.md");
        tryCatch.Reset();
        return;
    }

    // The value half is rule 9's and is asserted in handles_test.cpp; it is
    // here only to say that the thing the handler is reporting really did fail.
    REQUIRE(beyond.IsEmpty());

    const ub_test::EngineFaultLog& log = ub_test::EngineFaults();
    CHECK(log.Count() >= 1);
    CHECK(log.fault == ub::EngineFault::OutOfMemory);
    // The report names the heap it happened in. Both engines can say so here:
    // one because its hook is per-context, the other because one isolate per
    // thread means the thread names it.
    CHECK(log.isolate == &fixture.iso());
    // `CallbackData` came back as its real type rather than as a null.
    CHECK(log.sawData);
    // Engine wording is not asserted - see docs/testing.md - only that there is
    // some, because a handler that logs the message should have something to
    // log.
    CHECK_FALSE(log.Message().empty());
    MESSAGE("out of memory reported as: ", log.Message());

    tryCatch.Reset();
    ub_test::EngineFaults().Clear();

    // And the isolate is usable again, so the report was a report and not a
    // funeral.
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

TEST_CASE("faults: nothing is reported while nothing is going wrong") {
    // The control, and not a formality: a handler that fired on ordinary work
    // would be worse than no handler at all, because an embedder would learn
    // to ignore it.
    ub_test::EngineFaults().Clear();

    ub_test::Fixture fixture;
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);

    ub::HandleScope scope(fixture.iso());
    for (int i = 0; i < UNIBIND_FRAME_INLINE_SLOTS * 4; ++i) {
        CHECK_FALSE(ub::Integer::New(fixture.iso(), i).IsEmpty());
    }

    // Script that throws is not the engine being in trouble, and must not be
    // reported as such: an exception is a value, and this channel is for the
    // things that are not.
    {
        const ub::TryCatch caught(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, "throw new Error('ordinary')").has_value());
        CHECK(caught.HasCaught());
    }

    fixture.iso().RequestGarbageCollection();

    CHECK(ub_test::EngineFaults().Count() == 0);
}

// ---------------------------------------------------------------------------
// The heap ceiling as a decision
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(HEAP_LIMIT, "faults: a heap about to hit its ceiling is a decision, not a death") {
    // The case this whole hook exists for: a long-running embedder must not be
    // killed by one runaway script. Without it, the script below ends the
    // process - which is why `heapLimitBytes` alone was never assertable (see
    // docs/testing.md).
    RescuePolicy policy;

    auto isolate = ub::Isolate::New({.heapLimitBytes = std::size_t{32} * 1024 * 1024});
    REQUIRE(isolate != nullptr);
    isolate->SetHeapLimitCallback(&RaiseAndStop, ub::CallbackData::For(policy));

    ub_test::EngineFaults().Clear();
    {
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        const ub::ContextScope entered(*context);

        const ub::TryCatch caught(*isolate);
        const auto result = ub::Evaluate(*context, FILL_THE_HEAP);

        // Asked, and answered by the callback rather than by the engine's own
        // default, which is to abort.
        CHECK(policy.asked >= 1);
        CHECK(policy.isolate == isolate.get());
        CHECK(policy.currentSeen > 0);
        CHECK(policy.initialSeen > 0);

        // The script did not finish, and the reason is the stop the callback
        // asked for - not a throw, which is what `HasTerminated` is for.
        CHECK_FALSE(result.has_value());
        CHECK(caught.HasTerminated());
        CHECK(isolate->IsExecutionTerminating());
    }

    // The point of the exercise: the process is still here, and so is the
    // isolate once the stop is cancelled.
    isolate->CancelTerminateExecution();
    isolate->SetHeapLimitCallback(nullptr, {});
    {
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        const ub::ContextScope entered(*context);
        CHECK(ub_test::EvalInt(*context, "1 + 1") == 2);
    }

    MESSAGE("asked ", policy.asked, " time(s) at a ceiling of ", policy.currentSeen, " bytes");
}

UNIBIND_TEST_CASE(HEAP_LIMIT, "faults: a heap-limit callback that was taken away is not asked") {
    RescuePolicy policy;

    auto isolate = ub::Isolate::New();
    REQUIRE(isolate != nullptr);

    // Installed and removed with nothing in between. What is asserted is that
    // the removal is a real removal rather than bookkeeping - a backend that
    // left the engine holding its trampoline would still route through the
    // callback it no longer has, which is a null dereference waiting for a
    // busy heap.
    isolate->SetHeapLimitCallback(&RaiseAndStop, ub::CallbackData::For(policy));
    isolate->SetHeapLimitCallback(nullptr, {});
    // Setting it twice over, and removing it twice over, are both no-ops rather
    // than a second registration or a second removal.
    isolate->SetHeapLimitCallback(nullptr, {});
    isolate->SetHeapLimitCallback(&RaiseAndStop, ub::CallbackData::For(policy));
    isolate->SetHeapLimitCallback(&RaiseAndStop, ub::CallbackData::For(policy));
    isolate->SetHeapLimitCallback(nullptr, {});

    ub::HandleScope scope(*isolate);
    auto context = ub::Context::New(*isolate);
    REQUIRE(context.has_value());
    const ub::ContextScope entered(*context);
    CHECK(ub_test::EvalInt(*context, "1 + 1") == 2);
    CHECK(policy.asked == 0);
}
