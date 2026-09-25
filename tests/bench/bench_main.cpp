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

#ifdef UNIBIND_BENCH_V8_BASELINE
#include <v8-fast-api-calls.h>
#include <v8.h>

#include "unibind/interop/v8.h"
#endif

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

#ifdef UNIBIND_BENCH_V8_BASELINE
// The same operations written against V8 directly, on the same isolate and
// context, so the table can say what unibind costs over the engine itself
// rather than only what it costs. Built into the V8 backend's benchmark only.

std::vector<Result> g_baseline;

template <class Body>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
void MeasureBaseline(const char* name, std::uint64_t iterations, Body&& body) {
    body((iterations / 10) + 1);
    const auto start = std::chrono::steady_clock::now();
    body(iterations);
    const auto finish = std::chrono::steady_clock::now();
    const double nanoseconds =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    g_baseline.push_back({.name = name,
                          .nanosecondsPerOperation = nanoseconds / static_cast<double>(iterations),
                          .operations = iterations});
}

void RawCallee(const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(info.Length());
}

int RunV8Baseline(ub::Isolate& isolate, const ub::Context& context, std::uint64_t scale) {
    v8::Isolate* iso = ub::interop::V8Isolate(isolate);
    const v8::HandleScope scope(iso);
    const v8::Local<v8::Context> cx = ub::interop::V8Context(context);

    const v8::Local<v8::Object> object = v8::Object::New(iso);
    const v8::Local<v8::String> key = v8::String::NewFromUtf8Literal(iso, "property");
    v8::Local<v8::Function> function;
    if (!v8::Function::New(cx, &RawCallee).ToLocal(&function) ||
        !object->Set(cx, key, v8::Integer::New(iso, 1)).FromMaybe(false) ||
        !cx->Global()->Set(cx, v8::String::NewFromUtf8Literal(iso, "rawCallee"), function).FromMaybe(false)) {
        return 1;
    }

    MeasureBaseline("make an object", 200 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(iso);
            g_sink += v8::Object::New(iso).IsEmpty() ? 0 : 1;
        }
    });

    MeasureBaseline("read a property (by name handle)", 1000 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(iso);
            v8::Local<v8::Value> value;
            g_sink += object->Get(cx, key).ToLocal(&value) ? value.As<v8::Int32>()->Value() : 0;
        }
    });

    MeasureBaseline("read a property (by string_view)", 1000 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(iso);
            v8::Local<v8::String> name;
            v8::Local<v8::Value> value;
            const bool ok =
                v8::String::NewFromUtf8(iso, "property").ToLocal(&name) && object->Get(cx, name).ToLocal(&value);
            g_sink += ok ? 1 : 0;
        }
    });

    MeasureBaseline("call into native (from native)", 500 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(iso);
            g_sink += function->Call(cx, cx->Global(), 0, nullptr).IsEmpty() ? 0 : 1;
        }
    });

    {
        v8::Local<v8::Script> loop;
        if (!v8::Script::Compile(cx,
                                 v8::String::NewFromUtf8Literal(iso, "for (let i = 0; i < 1000; ++i) rawCallee(i);"))
                 .ToLocal(&loop)) {
            return 1;
        }
        MeasureBaseline("call into native (from script)", 1000 * scale, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n / 1000; ++i) {
                const v8::HandleScope inner(iso);
                g_sink += loop->Run(cx).IsEmpty() ? 0 : 1;
            }
        });
    }

    MeasureBaseline("open and close a frame", 2000 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope inner(iso);
            g_sink += 1;
        }
    });

    MeasureBaseline("create a handle", 2000 * scale, [&](std::uint64_t n) {
        std::uint64_t made = 0;
        while (made < n) {
            const v8::HandleScope inner(iso);
            const std::uint64_t chunk = std::min<std::uint64_t>(1024, n - made);
            for (std::uint64_t i = 0; i < chunk; ++i) {
                g_sink += v8::Integer::New(iso, static_cast<std::int32_t>(i))->Int32Value(cx).FromMaybe(0);
            }
            made += chunk;
        }
    });

    MeasureBaseline("read a handle", 5000 * scale, [&](std::uint64_t n) {
        const v8::HandleScope inner(iso);
        const v8::Local<v8::Integer> value = v8::Integer::New(iso, 3);
        for (std::uint64_t i = 0; i < n; ++i) {
            g_sink += value->Int32Value(cx).FromMaybe(0);
        }
    });

    MeasureBaseline("escape a handle", 500 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const v8::HandleScope outer(iso);
            v8::EscapableHandleScope inner(iso);
            g_sink += inner.Escape(v8::Integer::New(iso, 1))->Int32Value(cx).FromMaybe(0);
        }
    });

    MeasureBaseline("create and release a Global", 200 * scale, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            v8::Global<v8::Object> root(iso, object);
            g_sink += root.IsEmpty() ? 0 : 1;
        }
    });
    return 0;
}

[[nodiscard]] const Result* BaselineFor(const std::string& name) {
    for (const Result& result : g_baseline) {
        if (result.name == name) {
            return &result;
        }
    }
    return nullptr;
}
#endif

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

#ifdef UNIBIND_BENCH_V8_BASELINE
    if (RunV8Baseline(*isolate, *context, scale) != 0) {
        std::fprintf(stderr, "could not set the V8 baseline up\n");
        return 1;
    }
#endif
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
#ifdef UNIBIND_BENCH_V8_BASELINE
    // Beside each unibind figure, the same operation written against V8 directly.
    std::printf("%-36s %14s %14s %8s\n", "operation", "unibind ns/op", "plain V8 ns/op", "ratio");
    std::printf("%-36s %14s %14s %8s\n", "------------------------------------", "--------------", "--------------",
                "--------");
    for (const Result& result : g_results) {
        const Result* baseline = BaselineFor(result.name);
        const double plain = baseline != nullptr ? baseline->nanosecondsPerOperation : 0.0;
        std::printf("%-36s %14.1f %14.1f %7.2fx\n", result.name.c_str(), result.nanosecondsPerOperation, plain,
                    plain > 0.0 ? result.nanosecondsPerOperation / plain : 0.0);
    }
#else
    std::printf("%-36s %14s %14s\n", "operation", "ns/op", "ops/sec");
    std::printf("%-36s %14s %14s\n", "------------------------------------", "--------------", "--------------");
    for (const Result& result : g_results) {
        const double perSecond = result.nanosecondsPerOperation > 0.0 ? 1e9 / result.nanosecondsPerOperation : 0.0;
        std::printf("%-36s %14.1f %14.0f\n", result.name.c_str(), result.nanosecondsPerOperation, perSecond);
    }
#endif
    std::printf("\n");
    return 0;
}
