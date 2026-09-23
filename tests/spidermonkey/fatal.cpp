// EngineFault::Fatal on SpiderMonkey, which ends the process - so it is a
// process of its own, and CTest reads what the handler printed rather than an
// exit code.
//
// The failure is MOZ_CRASH itself, which is what every MOZ_RELEASE_ASSERT in the
// engine expands to: the reason stored in gMozCrashReason, a breakpoint, a
// null write. There is no public API that fails that way on purpose, so this
// runs the engine's own crash macro from the engine's own header rather than
// imitating what it does - the thing under test is that the backend recognises
// that sequence, and this is that sequence.
//
// MOZ_HAS_MOZGLUE is what makes the macro store its reason, and the engine was
// compiled with it: its objects reference gMozCrashReason at every crash site.
// A consumer is not, so without this the test's MOZ_CRASH would skip the store
// and be a crash the engine itself never produces. It changes nothing about
// linkage - mozilla/Types.h decides that from STATIC_JS_API alone.
#define MOZ_HAS_MOZGLUE

#include <jsapi.h>
#include <mozilla/Assertions.h>

#include <cstdio>

#include "unibind/unibind.h"

namespace {

void Report(const ub::EngineFaultReport& report, ub::CallbackData /*data*/) {
    std::printf(
        "unibind-fault: %s isolate=%s message=%.*s\n", report.fault == ub::EngineFault::Fatal ? "Fatal" : "OutOfMemory",
        report.isolate != nullptr ? "yes" : "no", static_cast<int>(report.message.size()), report.message.data());
    std::fflush(stdout);
}

}  // namespace

int main() {
    const ub::Platform platform(ub::PlatformOptions{.onEngineFault = &Report});
    const auto isolate = ub::Isolate::New();
    if (isolate == nullptr) {
        std::fprintf(stderr, "fatal_spidermonkey: could not make an isolate\n");
        return 1;
    }
    MOZ_CRASH("unibind test: a deliberate engine crash");
}
