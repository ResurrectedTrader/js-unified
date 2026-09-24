// Stress cases: too slow, or too hard on the machine, for every run. Skipped
// unless asked for by name: `unibind_python_tests -tc="stress:*" --no-skip`.

#include <chrono>
#include <cstdio>

#include "support.h"

TEST_CASE("stress: making isolates until memory runs out fails cleanly" * doctest::skip()) {
    // CPython 3.12 keeps some of every sub-interpreter's memory for good (see
    // docs/python.md), so on x86 a program that makes enough isolates runs its
    // address space out. What must happen then is an empty `Isolate::New` -
    // not a crash, and not a hang.
    int made = 0;
    auto last = std::chrono::steady_clock::now();
    for (; made < 100000; ++made) {
        auto isolate = ub::Isolate::New();
        if (!isolate) {
            break;
        }
        const ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        if (!context) {
            break;
        }
        if (made % 50 == 0) {
            const auto now = std::chrono::steady_clock::now();
            std::printf("stress: %d isolates made, the last batch in %lld ms\n", made,
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count()));
            last = now;
            std::fflush(stdout);
        }
    }
    MESSAGE("made " << made << " isolates before one was refused");
}
