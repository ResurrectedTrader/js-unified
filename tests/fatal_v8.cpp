// EngineFault::Fatal on V8, which ends the process - so it is a process of its
// own, and CTest reads what the handler printed rather than an exit code.
//
// The failure is a real one V8 detects in a release build: making a handle with
// no v8::HandleScope open, which V8 reports through the isolate's fatal-error
// hook rather than an assertion that compiles out. unibind opens no V8 scope of
// its own outside a ub::HandleScope, so reaching the native isolate through
// unibind/interop/v8.h and asking for a handle there is the failure, with
// nothing faked.

// Before v8.h, which only forward-declares the CFunction that v8-template.h
// spans over; see unibind/interop/v8.h.
#include <v8-fast-api-calls.h>
#include <v8.h>

#include <cstdio>
// for `_set_abort_behavior`, a CRT extension that the C header declares and <cstdlib> does not promise.
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdlib.h>

#include <tuple>

#include "unibind/interop/v8.h"
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
    // Die quietly rather than into a dialog box no one is there to click.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    const ub::Platform platform(ub::PlatformOptions{.onEngineFault = &Report});
    const auto isolate = ub::Isolate::New();
    if (isolate == nullptr) {
        std::fprintf(stderr, "fatal_v8: could not make an isolate\n");
        return 1;
    }
    std::ignore = v8::Integer::New(ub::interop::V8Isolate(*isolate), 1);
    std::printf("fatal_v8: the engine carried on\n");
    return 1;
}
