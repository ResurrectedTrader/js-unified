#pragma once
/// \file
/// What every test file in this suite is allowed to know.
///
/// Two rules shape this header:
///
///   * No engine type, no engine header, ever. A test that has to ask which
///     engine it is running on has failed at its job. The one
///     exception is reporting - `ub::Platform::BackendName()` may be printed,
///     never branched on.
///   * A backend that has not implemented an area yet must make the cases in
///     that area *skipped and visible*, never absent and never red. That is
///     what `UNIBIND_TEST_CASE` below is for.

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
// doctest only forward-declares std::ostream, and its message macros stream
// whatever they are handed - a std::string_view among other things, whose
// operator<< the compiler cannot find without the real header.
#include <ostream>
#include <string>
#include <string_view>

#include "unibind/unibind.h"
#include "unibind_test_capabilities.h"

namespace ub_test {

// ---------------------------------------------------------------------------
// Skipping
// ---------------------------------------------------------------------------

/// Record that the case running now did nothing, and why. Prints a line CTest
/// recognises (`SKIP_REGULAR_EXPRESSION`) and tells the parity reporter to score
/// the case SKIPPED rather than PASSED.
void ReportSkip(std::string_view reason);

/// The same, for the capability gate below: this backend does not implement the
/// area the case belongs to.
void ReportUnimplementedArea(std::string_view area);

/// Taken by the reporter at the end of each case; empty when the case ran.
[[nodiscard]] std::string TakeSkipReason();

// ---------------------------------------------------------------------------
// Capability gating
//
// An operation a backend declares but does not define is a *link error at the
// call site*, so a case that used one could not be built, let alone skipped.
// The gate therefore has to remove the body from the translation unit - but
// removing the case as well would make an unimplemented area silently absent,
// and the parity matrix lines up only if both backends list the same cases
// under the same names.
//
// So `UNIBIND_TEST_CASE(AREA, "name")` compiles to one of two things:
//
//   capability present - an ordinary TEST_CASE.
//   capability absent  - a TEST_CASE of the same name that reports a skip, plus
//                        an uninstantiated function template that swallows the
//                        body. The body is still parsed and type-checked; no
//                        code is emitted for it, so it references nothing the
//                        backend does not define.
//
// `AREA` is a row in tests/cmake/Capabilities.cmake.
// ---------------------------------------------------------------------------

#define UNIBIND_TEST_CAT2(a, b) a##b
#define UNIBIND_TEST_CAT(a, b) UNIBIND_TEST_CAT2(a, b)

#define UNIBIND_TEST_CASE_IMPL_1(name, area) TEST_CASE(name)

#define UNIBIND_TEST_CASE_IMPL_0(name, area)      \
    TEST_CASE(name) {                             \
        ::ub_test::ReportUnimplementedArea(area); \
    }                                             \
    template <class>                              \
    static void UNIBIND_TEST_CAT(UnibindUnsupportedBody_, __LINE__)()

/// A test case that exists on every backend and runs only where the backend
/// can answer it.
#define UNIBIND_TEST_CASE(area, name) \
    UNIBIND_TEST_CAT(UNIBIND_TEST_CASE_IMPL_, UNIBIND_TEST_CAT(UNIBIND_TEST_HAS_, area))(name, #area)

#define UNIBIND_TEST_BOTH_00 0
#define UNIBIND_TEST_BOTH_01 0
#define UNIBIND_TEST_BOTH_10 0
#define UNIBIND_TEST_BOTH_11 1
#define UNIBIND_TEST_BOTH(a, b) UNIBIND_TEST_CAT(UNIBIND_TEST_BOTH_, UNIBIND_TEST_CAT(a, b))

/// Same, for a case that needs two areas at once.
#define UNIBIND_TEST_CASE2(areaA, areaB, name)                                                               \
    UNIBIND_TEST_CAT(UNIBIND_TEST_CASE_IMPL_, UNIBIND_TEST_BOTH(UNIBIND_TEST_CAT(UNIBIND_TEST_HAS_, areaA),  \
                                                                UNIBIND_TEST_CAT(UNIBIND_TEST_HAS_, areaB))) \
    (name, #areaA " + " #areaB)

// ---------------------------------------------------------------------------
// Engine faults
//
// `PlatformOptions::onEngineFault` is installed once, for the life of the
// process, and cannot be changed afterwards (decision 28) - so the suite does
// what an embedder with a changing policy does: one fixed handler, and the
// policy behind it. `tests/main.cpp` installs `RecordEngineFault` on the one
// `Platform` the run has, and a case reads and clears this log.
//
// The recorder obeys the rule it is testing: it allocates nothing, because the
// fault it hears about most is that allocation is failing. The message is
// copied into a fixed buffer and the counter is atomic because a fault may
// arrive on any thread.
// ---------------------------------------------------------------------------

struct EngineFaultLog {
    static constexpr std::size_t TEXT = 192;

    std::atomic<int> count{0};
    ub::EngineFault fault = ub::EngineFault::OutOfMemory;
    ub::Isolate* isolate = nullptr;
    std::array<char, TEXT> message{};
    bool sawData = false;

    void Clear() noexcept;
    [[nodiscard]] int Count() const noexcept { return count.load(std::memory_order_acquire); }
    [[nodiscard]] std::string Message() const { return {message.data()}; }
};

/// The one log, filled by the handler `tests/main.cpp` installs.
[[nodiscard]] EngineFaultLog& EngineFaults() noexcept;

/// The handler itself. Passed `CallbackData::For(EngineFaults())`, so that
/// recovering the embedder pointer is exercised rather than assumed.
void RecordEngineFault(const ub::EngineFaultReport& report, ub::CallbackData data);

/// What `tests/main.cpp` builds its `Platform` with.
[[nodiscard]] ub::PlatformOptions PlatformOptionsWithFaultHandler() noexcept;

// ---------------------------------------------------------------------------
// The standing arrangement of a test
// ---------------------------------------------------------------------------

/// An isolate, a scope, a context, and the context entered - in that order, and
/// unwound in reverse. `HandleScope` and `ContextScope` delete `operator new`
/// and cannot be moved, so they are members of a stack object rather than
/// anything owning.
struct Fixture {
    Fixture();

    static std::unique_ptr<ub::Isolate> MakeIsolate();
    static ub::Context MakeContext(ub::Isolate& isolate);

    [[nodiscard]] ub::Isolate& iso() const noexcept { return *isolate; }

    std::unique_ptr<ub::Isolate> isolate;
    ub::HandleScope scope;
    ub::Context context;
    ub::ContextScope entered;
};

// ---------------------------------------------------------------------------
// Small helpers, so a test reads as what it is testing
// ---------------------------------------------------------------------------

/// A string handle, or a failed REQUIRE.
[[nodiscard]] ub::Local<ub::String> Str(ub::Isolate& isolate, std::string_view utf8);

/// Evaluate, requiring that it produced a value.
[[nodiscard]] ub::Local<ub::Value> Eval(const ub::Context& context, std::string_view source);

[[nodiscard]] std::int32_t EvalInt(const ub::Context& context, std::string_view source);
[[nodiscard]] double EvalNumber(const ub::Context& context, std::string_view source);
[[nodiscard]] std::string EvalText(const ub::Context& context, std::string_view source);
[[nodiscard]] bool EvalTruth(const ub::Context& context, std::string_view source);

/// The UTF-8 of a value that must already be a string.
[[nodiscard]] std::string TextOf(const ub::Local<ub::Value>& value);

/// Install `value` on the entered realm's global object under `name`.
template <class T>
void Expose(const ub::Context& context, std::string_view name, const ub::Local<T>& value) {
    REQUIRE(context.GlobalObject().Set(context, name, value).value_or(false));
}

// ---------------------------------------------------------------------------
// Allocation accounting
//
// The suite replaces global operator new/delete so a leak test can ask a
// question neither engine exposes: did this loop give back everything it took?
// A frame that leaked its overflow storage, or a Global that leaked its node,
// shows up here as one outstanding block per iteration and nowhere else.
// ---------------------------------------------------------------------------

/// Blocks allocated and not yet freed, process-wide.
[[nodiscard]] long long OutstandingAllocations() noexcept;

/// Allocations asked for since the process started, granted or not. Like the
/// count above it is process-wide, so the difference across a call is what the
/// call asked for plus whatever another thread asked for meanwhile - never less.
[[nodiscard]] long long AllocationsRequested() noexcept;

/// Make `count` allocations fail - throwing `std::bad_alloc` from the throwing
/// forms, returning null from the nothrow ones - which is what running out of
/// memory looks like to the code under test, after letting `skip` of them
/// through first.
///
/// `skip` is what makes a *later* allocation inside one call the one that
/// fails, and that is not a refinement: an operation that allocates more than
/// once has a different failure per site, and the interesting ones are the
/// sites after the first. Sweeping `skip` walks the failure through the call.
///
/// Arm it for exactly the call being tested and disarm immediately: while it is
/// armed, *every* allocation in the process is a candidate, the engine's
/// included.
void FailNextAllocations(long long count, long long skip = 0) noexcept;

/// Disarm, and answer how many failures actually fired. Zero means the code
/// under test never reached the C++ allocator, so the test proved nothing and
/// should say so rather than assert.
long long StopFailingAllocations() noexcept;

/// Arms on construction and disarms on destruction, so an assertion that fails
/// mid-test cannot leave the process unable to allocate.
class AllocationFailure {
   public:
    explicit AllocationFailure(long long count, long long skip = 0) noexcept { FailNextAllocations(count, skip); }
    ~AllocationFailure() { (void)StopFailingAllocations(); }

    AllocationFailure(const AllocationFailure&) = delete;
    AllocationFailure& operator=(const AllocationFailure&) = delete;
    AllocationFailure(AllocationFailure&&) = delete;
    AllocationFailure& operator=(AllocationFailure&&) = delete;

    /// Disarm now and report how many fired.
    // disarming is what this guard does; that the underlying counters are process-wide is not a reason to call it off
    // the class. NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    long long Stop() noexcept { return StopFailingAllocations(); }
};

}  // namespace ub_test
