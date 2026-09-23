#define DOCTEST_CONFIG_IMPLEMENT
#include <crtdbg.h>
#include <doctest/doctest.h>
// for `_set_abort_behavior`, a CRT extension that the C header declares and <cstdlib> does not promise.
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdlib.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>

#include "support/harness.h"
#include "unibind/unibind.h"

namespace {

/// Rule 2 of docs/lifetimes.md, deliberately broken: a handle outliving its
/// frame. A checked build diagnoses this (section 9), which means the process
/// dies - so it cannot be a test case among others, and it runs in a process of
/// its own. CTest registers it for checked configurations only and expects it
/// to fail; see tests/CMakeLists.txt.
int UseHandleAfterItsScopeClosed() {
    // Die quietly rather than into a dialog box no one is there to click.
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

    auto isolate = ub::Isolate::New();
    if (isolate == nullptr) {
        return 2;
    }

    ub::Local<ub::Integer> stale;
    {
        ub::HandleScope scope(*isolate);
        stale = ub::Integer::New(*isolate, 7);
    }

    // A fresh frame lands on the same stack storage the dead one had, so
    // nothing but the epoch distinguishes them.
    ub::HandleScope reused(*isolate);
    (void)ub::Integer::New(*isolate, 11);

    std::cout << "read a stale handle and lived: " << stale.Int32Value() << "\n";
    return 0;
}

/// Decision 27, deliberately broken: a `Global<T>` still holding one of an
/// isolate's roots when that isolate is destroyed. A checked build counts what
/// an isolate handed out and diagnoses this in `~Isolate`, which means the
/// process dies - so like the stale handle above it runs in a process of its
/// own, registered for checked configurations only and expected to fail.
///
/// The root is leaked on purpose. Giving it back after the isolate has gone is
/// the *second* half of the fault - a reset against a disposed isolate - and
/// this is a test of the first half, which is the one an isolate can see. A
/// build with the check compiled out therefore prints and exits cleanly rather
/// than walking into undefined behaviour on the way out.
int GlobalOutlivingItsIsolate() {
    // Die quietly rather than into a dialog box no one is there to click.
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

    {
        auto isolate = ub::Isolate::New();
        if (isolate == nullptr) {
            return 2;
        }
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        if (!context) {
            return 3;
        }
        ub::ContextScope entered(*context);
        auto object = ub::Object::New(*context);
        if (!object) {
            return 4;
        }
        // Never destroyed, so nothing gives the root back - which is exactly
        // the shape of the mistake: an embedder who kept one somewhere the
        // isolate's own teardown does not reach.
        (void)new ub::Global<ub::Object>(*isolate, *object);
    }  // ~Isolate, with that root outstanding

    std::cout << "an isolate went while a Global still held one of its roots\n";
    return 0;
}

/// Decision 16's load-bearing clause: comparing two `Global`s requires **no
/// open `HandleScope`**, because the backend roots what it needs for the
/// duration of the comparison. A backend that gets this wrong does not answer
/// wrongly - it makes a handle with no frame to make it in, and the engine
/// takes the process down - so this cannot be a case among others, and it runs
/// in a process of its own like the stale-handle check above.
///
/// Unlike that one it is expected to **succeed**: it is a promise the headers
/// make, not a rule being deliberately broken.
int CompareGlobalsWithNoScopeOpen() {
    auto isolate = ub::Isolate::New();
    if (isolate == nullptr) {
        return 2;
    }

    ub::Global<ub::Object> first;
    ub::Global<ub::Object> second;
    ub::Global<ub::Object> elsewhere;
    {
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        if (!context) {
            return 3;
        }
        ub::ContextScope entered(*context);

        auto object = ub::Object::New(*context);
        auto other = ub::Object::New(*context);
        if (!object || !other) {
            return 4;
        }
        first = ub::Global<ub::Object>(*isolate, *object);
        second = ub::Global<ub::Object>(*isolate, *object);
        elsewhere = ub::Global<ub::Object>(*isolate, *other);
    }

    // From here on nothing is open. Every line below is the assertion.
    isolate->RequestGarbageCollection();

    if (!first.StrictEquals(second) || !second.StrictEquals(first)) {
        return 5;
    }
    if (!first.SameValue(second)) {
        return 6;
    }
    if (first.StrictEquals(elsewhere) || first.SameValue(elsewhere)) {
        return 7;
    }

    const ub::Global<ub::Object> empty;
    const ub::Global<ub::Object> alsoEmpty;
    if (empty.StrictEquals(alsoEmpty) || empty.StrictEquals(first) || first.StrictEquals(empty)) {
        return 8;
    }

    // A duplicate is the canonical second root over one value, and making one
    // is not making a handle either.
    const ub::Global<ub::Object> copy = first.Duplicate();
    if (!copy.StrictEquals(first) || !first.StrictEquals(copy)) {
        return 9;
    }

    std::cout << "compared roots with no handle scope open\n";
    return 0;
}

/// A worker-thread count, or the fact that there was not one. `empty` is not
/// `0` (`unibind/isolate.h`), so the two are printed differently.
[[nodiscard]] std::string Describe(const std::optional<std::uint32_t>& count) {
    return count ? std::to_string(*count) : std::string("empty");
}

/// `Platform::IsInitialized()` is the bring-up failure channel and
/// `Isolate::New` is documented to consult it - so a platform that is not up
/// hands out no isolates. Neither half can be asked from inside the suite,
/// because the suite runs under a platform that is already up and a *second*
/// `Platform` is a precondition violation rather than a failure. So the whole
/// lifecycle runs in a process of its own: before any platform exists, while
/// one does, and after it has gone.
///
/// The last of those three is also the only reachable test of a figure
/// outliving the bring-up that produced it: `Platform::WorkerThreads()` is
/// cached, and a cache that survives its own platform is a figure about
/// something that no longer exists.
int PlatformIsTheGateOnAnIsolate() {
    // Nothing is up, so nothing may be handed out and nothing may be reported.
    if (ub::Platform::IsInitialized()) {
        return 2;
    }
    if (ub::Isolate::New() != nullptr) {
        return 3;
    }
    if (ub::Platform::WorkerThreads().has_value()) {
        return 4;
    }

    std::optional<std::uint32_t> whileUp;
    {
        const ub::Platform platform;
        // Not the assertion - it is the precondition for the rest of them. A
        // build whose engine genuinely will not start has nothing to prove
        // here and should say so rather than pass.
        if (!ub::Platform::IsInitialized()) {
            return 5;
        }

        auto isolate = ub::Isolate::New();
        if (isolate == nullptr) {
            return 6;
        }
        {
            ub::HandleScope scope(*isolate);
            auto context = ub::Context::New(*isolate);
            if (!context) {
                return 7;
            }
            ub::ContextScope entered(*context);
            const auto answer = ub::Evaluate(*context, "6 * 7");
            if (!answer) {
                return 8;
            }
            const auto number = answer->To<ub::Integer>();
            if (!number || number->Int32Value() != 42) {
                return 8;
            }
        }
        whileUp = ub::Platform::WorkerThreads();
    }

    // The platform is gone, so every answer goes back to what it was before
    // there was one.
    if (ub::Platform::IsInitialized()) {
        return 9;
    }
    if (ub::Isolate::New() != nullptr) {
        return 10;
    }
    if (ub::Platform::WorkerThreads().has_value()) {
        return 11;
    }

    std::cout << "isolates only while the platform is up; worker threads reported " << Describe(whileUp)
              << " while up and empty either side\n";
    return 0;
}

/// The same lifecycle with a worker-thread count actually asked for, which is
/// what makes the cache observable: a backend that answers from a figure it
/// kept will answer it after the platform that produced it has gone.
///
/// What is *asserted* is only what both engines promise - no figure before
/// there is a platform, and the same answer twice while there is one. What
/// happens to an explicitly requested figure afterwards is **reported**,
/// because the backends do not agree; see `docs/testing.md`.
int WorkerThreadsBelongToOnePlatform() {
    if (ub::Platform::WorkerThreads().has_value()) {
        return 2;
    }

    std::optional<std::uint32_t> whileUp;
    {
        const ub::Platform platform({.workerThreads = 1});
        if (!ub::Platform::IsInitialized()) {
            return 3;
        }
        whileUp = ub::Platform::WorkerThreads();
        // A count that moved between two identical questions would mean it was
        // measuring something else.
        if (ub::Platform::WorkerThreads() != whileUp) {
            return 4;
        }
    }

    std::cout << "asked for 1 worker thread: reported " << Describe(whileUp) << " while up, "
              << Describe(ub::Platform::WorkerThreads()) << " once the platform had gone\n";
    return 0;
}

}  // namespace

/// The engine is process-wide on both backends, so it is set up once around the
/// whole run rather than per test case.
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--unibind-use-after-scope") == 0) {
            const ub::Platform platform;
            return UseHandleAfterItsScopeClosed();
        }
        if (std::strcmp(argv[i], "--unibind-global-outlives-its-isolate") == 0) {
            const ub::Platform platform;
            return GlobalOutlivingItsIsolate();
        }
        if (std::strcmp(argv[i], "--unibind-compare-globals-without-a-scope") == 0) {
            const ub::Platform platform;
            return CompareGlobalsWithNoScopeOpen();
        }
        // Both of these make their own platform, and look at the process
        // before there is one, so they run before the one this function would
        // otherwise construct.
        if (std::strcmp(argv[i], "--unibind-platform-is-the-gate") == 0) {
            return PlatformIsTheGateOnAnIsolate();
        }
        if (std::strcmp(argv[i], "--unibind-worker-threads-belong-to-one-platform") == 0) {
            return WorkerThreadsBelongToOnePlatform();
        }
    }

    // The one place the fault handler can be installed: it is fixed for the
    // life of a `Platform` (decision 28), and the suite has one `Platform`
    // around the whole run. A case reads `ub_test::EngineFaults()` and clears
    // it, which is what an embedder with a changing policy does behind its own
    // fixed handler.
    const ub::Platform platform(ub_test::PlatformOptionsWithFaultHandler());

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
