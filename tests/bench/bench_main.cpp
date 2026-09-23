/// \file
/// A small benchmark for the hot operations: calling into
/// native, reading a property, making an object. Plus the handle operations the
/// model of docs/lifetimes.md charges for, because the claim being checked is
/// "one extra load per value access and one extra word per handle" and a table
/// in a document is not a measurement.
///
/// Numbers are per backend and are only comparable against each other on the
/// same machine. Written against `ub::` alone, like the rest of the suite.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "unibind/unibind.h"

namespace {

struct Result {
    std::string name;
    double nanosecondsPerOperation = 0.0;
    std::uint64_t operations = 0;
};

std::vector<Result> g_results;

// `body` is invoked twice - a warm-up pass and the measured one - so forwarding it
// would hand the second call something already moved from.
template <class Body>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
void Measure(const char* name, std::uint64_t iterations, Body&& body) {
    // One untimed pass, so a first-call cost does not become the measurement.
    body((iterations / 10) + 1);

    const auto start = std::chrono::steady_clock::now();
    body(iterations);
    const auto finish = std::chrono::steady_clock::now();

    const double nanoseconds =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    g_results.push_back({.name = name,
                         .nanosecondsPerOperation = nanoseconds / static_cast<double>(iterations),
                         .operations = iterations});
}

/// Kept out of the optimiser's reach without a compiler-specific barrier.
volatile std::int32_t g_sink = 0;

void NativeCallee(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(static_cast<std::int32_t>(info.Length()));
}

int Run(std::uint64_t scale) {
    auto isolate = ub::Isolate::New();
    if (isolate == nullptr) {
        std::fprintf(stderr, "could not create an isolate\n");
        return 1;
    }

    ub::HandleScope scope(*isolate);
    auto context = ub::Context::New(*isolate);
    if (!context) {
        std::fprintf(stderr, "could not create a context\n");
        return 1;
    }
    ub::ContextScope entered(*context);

    auto object = ub::Object::New(*context);
    auto key = ub::String::New(*isolate, "property");
    auto function = ub::Function::New(*context, &NativeCallee);
    if (!object || !key || !function) {
        std::fprintf(stderr, "could not set the benchmark up\n");
        return 1;
    }
    if (!object->Set(*context, *key, ub::Integer::New(*isolate, 1)).value_or(false)) {
        return 1;
    }
    if (!context->GlobalObject().Set(*context, "callee", *function).value_or(false)) {
        return 1;
    }
    if (!context->GlobalObject().Set(*context, "probe", *object).value_or(false)) {
        return 1;
    }

    Measure("make an object", 200 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            ub::HandleScope inner(*isolate);
            auto made = ub::Object::New(*context);
            g_sink += made ? 1 : 0;
        }
    });

    Measure("read a property (by name handle)", 1000 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            ub::HandleScope inner(*isolate);
            auto value = object->Get(*context, *key);
            g_sink += value ? value->To<ub::Integer>()->Int32Value() : 0;
        }
    });

    Measure("read a property (by string_view)", 1000 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            ub::HandleScope inner(*isolate);
            auto value = object->Get(*context, "property");
            g_sink += value ? 1 : 0;
        }
    });

    Measure("call into native (from native)", 500 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            ub::HandleScope inner(*isolate);
            auto result = function->Call(*context, context->GlobalObject());
            g_sink += result ? 1 : 0;
        }
    });

    {
        auto loop = ub::Script::Compile(*context, "for (let i = 0; i < 1000; ++i) callee(i);");
        if (!loop) {
            return 1;
        }
        Measure("call into native (from script)", 1000 * scale, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n / 1000; ++i) {
                ub::HandleScope inner(*isolate);
                g_sink += loop->Run(*context) ? 1 : 0;
            }
        });
    }

    Measure("open and close a frame", 2000 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            ub::HandleScope inner(*isolate);
            g_sink += 1;
        }
    });

    Measure("create a handle", 2000 * scale, [&](std::uint64_t n) {
        // A fresh frame every 1024 handles, so this measures appending to a
        // frame rather than one frame growing without bound.
        std::uint64_t made = 0;
        while (made < n) {
            ub::HandleScope inner(*isolate);
            const std::uint64_t chunk = std::min<std::uint64_t>(1024, n - made);
            for (std::uint64_t i = 0; i < chunk; ++i) {
                g_sink += ub::Integer::New(*isolate, static_cast<std::int32_t>(i)).Int32Value();
            }
            made += chunk;
        }
    });

    Measure("read a handle", 5000 * scale, [&](std::uint64_t n) {
        ub::HandleScope inner(*isolate);
        const auto value = ub::Integer::New(*isolate, 3);
        for (std::uint64_t i = 0; i < n; ++i) {
            g_sink += value.Int32Value();
        }
    });

    Measure("escape a handle", 500 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            ub::HandleScope outer(*isolate);
            ub::EscapableHandleScope inner(*isolate);
            g_sink += inner.Escape(ub::Integer::New(*isolate, 1)).Int32Value();
        }
    });

    Measure("create and release a Global", 200 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            ub::Global<ub::Object> root(*isolate, *object);
            g_sink += root.IsEmpty() ? 0 : 1;
        }
    });

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t scale = 100;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = std::strtoull(argv[i + 1], nullptr, 10);
            ++i;
        }
    }
    if (scale == 0) {
        scale = 1;
    }

    const ub::Platform platform;
    const int failed = Run(scale);
    if (failed != 0) {
        return failed;
    }

    std::printf("\nbenchmark backend=%s scale=%llu\n", std::string(ub::Platform::BackendName()).c_str(),
                static_cast<unsigned long long>(scale));
    std::printf("%-36s %14s %14s\n", "operation", "ns/op", "ops/sec");
    std::printf("%-36s %14s %14s\n", "------------------------------------", "--------------", "--------------");
    for (const Result& result : g_results) {
        const double perSecond = result.nanosecondsPerOperation > 0.0 ? 1e9 / result.nanosecondsPerOperation : 0.0;
        std::printf("%-36s %14.1f %14.0f\n", result.name.c_str(), result.nanosecondsPerOperation, perSecond);
    }
    std::printf("\n");
    return 0;
}
