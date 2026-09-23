/// \file
/// The isolate itself: its embedder pointer, its heap figures, and throwing
/// from outside a callback. Small surface, and all of it was reachable from a
/// program and from no test.

#include <cstddef>
#include <memory>
#include <string>

#include "support/harness.h"

namespace {

struct Registry {
    int lookups = 0;
};

struct SomethingElse {
    int unused = 0;
};

/// The reason an isolate carries an embedder pointer at all: a callback that
/// needs the embedder's state and was not given any of its own.
void CountsALookup(const ub::CallbackInfo& info) {
    auto* registry = info.GetIsolate().GetEmbedderData<Registry>();
    if (registry == nullptr) {
        info.ThrowTypeError("no registry on this isolate");
        return;
    }
    ++registry->lookups;
    info.GetReturnValue().Set(registry->lookups);
}

}  // namespace

TEST_CASE("isolate: an embedder pointer is recovered as its real type or not at all") {
    ub_test::Fixture fixture;

    CHECK(fixture.iso().GetEmbedderData<Registry>() == nullptr);

    Registry registry;
    fixture.iso().SetEmbedderData(registry);

    CHECK(fixture.iso().GetEmbedderData<Registry>() == &registry);
    // Asking for the wrong type yields nothing rather than a bad cast - the
    // same rule `CallbackData` follows, for the same reason.
    CHECK(fixture.iso().GetEmbedderData<SomethingElse>() == nullptr);
}

TEST_CASE("isolate: a callback can reach the embedder through the isolate") {
    ub_test::Fixture fixture;

    Registry registry;
    fixture.iso().SetEmbedderData(registry);

    const auto function = ub::Function::New(fixture.context, &CountsALookup);
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "lookup", *function);

    CHECK(ub_test::EvalInt(fixture.context, "lookup()") == 1);
    CHECK(ub_test::EvalInt(fixture.context, "lookup()") == 2);
    CHECK(registry.lookups == 2);
}

TEST_CASE("isolate: native can throw without being inside a callback") {
    ub_test::Fixture fixture;

    ub::Local<ub::Value> thrown;
    {
        ub::TryCatch tryCatch(fixture.iso());
        fixture.iso().ThrowError(ub::ErrorKind::RangeError, "thrown by the embedder");
        CHECK(fixture.iso().HasPendingException());
        REQUIRE(tryCatch.HasCaught());
        thrown = tryCatch.Exception();
        tryCatch.Reset();
    }
    CHECK_FALSE(fixture.iso().HasPendingException());

    // The handle outlives the handler, so what was thrown can still be read.
    const auto asObject = thrown.To<ub::Object>();
    REQUIRE(asObject.has_value());
    ub_test::Expose(fixture.context, "thrown", *asObject);
    CHECK(ub_test::EvalText(fixture.context, "thrown.constructor.name") == "RangeError");
    CHECK(ub_test::EvalText(fixture.context, "thrown.message") == "thrown by the embedder");
}

TEST_CASE("isolate: the free Throw is the same throw") {
    ub_test::Fixture fixture;

    {
        ub::TryCatch tryCatch(fixture.iso());
        ub::Throw(fixture.iso(), ub::ErrorKind::TypeError, "by the free function");
        REQUIRE(tryCatch.HasCaught());
        CHECK(tryCatch.Message(fixture.context).value_or("").find("by the free function") != std::string::npos);
    }

    CHECK_FALSE(fixture.iso().HasPendingException());
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE(HEAP, "isolate: heap figures are reported and only the trend means anything") {
    ub_test::Fixture fixture;

    const auto before = fixture.iso().GetHeapStatistics();
    CHECK(before.usedBytes > 0);

    {
        ub::HandleScope scope(fixture.iso());
        for (int i = 0; i < 20000; ++i) {
            (void)ub::Object::New(fixture.context);
        }
    }
    const auto after = fixture.iso().GetHeapStatistics();

    // The engines measure different things and a collection may have run in
    // between, so the only portable statement is that the figures are figures.
    // See docs/testing.md.
    CHECK(after.usedBytes > 0);
    MESSAGE("used bytes went from ", before.usedBytes, " to ", after.usedBytes, "; limit ", after.limitBytes);
}

UNIBIND_TEST_CASE(HEAP, "isolate: asking for a collection is safe whether or not one happens") {
    ub_test::Fixture fixture;

    auto kept = ub::Object::New(fixture.context);
    REQUIRE(kept.has_value());
    REQUIRE(kept->Set(fixture.context, "marker", ub::Integer::New(fixture.iso(), 5)).value_or(false));

    for (int round = 0; round < 3; ++round) {
        fixture.iso().RequestGarbageCollection();
    }

    CHECK(kept->Get(fixture.context, "marker")->To<ub::Integer>()->Int32Value() == 5);
    CHECK(ub_test::EvalInt(fixture.context, "2 + 3") == 5);
}

TEST_CASE("isolate: a second isolate on one thread is refused, and the next one is not") {
    // Decision 11. The restriction is one backend's and the answer is both
    // backends', because a program that works on one engine and not the other is
    // the failure this library exists to prevent. Empty was already the
    // documented answer for a heap that could not be made, so nothing had to
    // change shape - and a crash never was one of the answers.
    {
        ub_test::Fixture first;

        CHECK(ub::Isolate::New() == nullptr);
        // Refused, not broken: the isolate that does exist is untouched.
        CHECK(ub_test::EvalInt(first.context, "6 * 7") == 42);
        CHECK(ub::Isolate::New() == nullptr);
    }

    // Once the first is gone, the next one is made as usual.
    auto next = ub::Isolate::New();
    REQUIRE(next != nullptr);
    ub::HandleScope scope(*next);
    auto context = ub::Context::New(*next);
    REQUIRE(context.has_value());
    ub::ContextScope entered(*context);
    CHECK(ub_test::EvalInt(*context, "6 * 7") == 42);
}

TEST_CASE("isolate: one isolate knows nothing of the one before it") {
    Registry registry;

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        isolate->SetEmbedderData(registry);
        REQUIRE(ub::Evaluate(*context, "globalThis.onlyInTheFirst = 1").has_value());
        CHECK(ub_test::EvalInt(*context, "onlyInTheFirst") == 1);
        CHECK(isolate->GetEmbedderData<Registry>() == &registry);
    }

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        // A fresh heap: nothing the first one did is here, embedder pointer
        // included.
        CHECK(ub_test::EvalText(*context, "typeof onlyInTheFirst") == "undefined");
        CHECK(isolate->GetEmbedderData<Registry>() == nullptr);
    }
}

// ---------------------------------------------------------------------------
// Options
//
// An option nothing can detect is a comment, not an option - so what is
// asserted here is not that a number was stored but that changing it changes
// what the isolate does.
// ---------------------------------------------------------------------------

namespace {

/// How deep recursion in script gets before the engine stops it. Measured in
/// script, because the point is what the *engine* allowed, and returned as a
/// plain number that outlives the isolate.
[[nodiscard]] int RecursionDepthWith(const ub::IsolateOptions& options) {
    auto isolate = ub::Isolate::New(options);
    REQUIRE(isolate != nullptr);
    ub::HandleScope scope(*isolate);
    auto context = ub::Context::New(*isolate);
    REQUIRE(context.has_value());
    ub::ContextScope entered(*context);

    return ub_test::EvalInt(*context, R"(
        (function () {
            let depth = 0;
            function down() { depth += 1; down(); }
            try { down(); } catch (e) { /* the engine stopped us */ }
            return depth;
        })()
    )");
}

}  // namespace

UNIBIND_TEST_CASE(STACK_LIMIT, "isolate: runaway recursion is an exception, not a dead process") {
    // What the stack limit is *for*: the failure it prevents is not one an
    // embedder can catch by any other means.
    auto isolate = ub::Isolate::New({.stackLimitBytes = std::size_t{512} * 1024});
    REQUIRE(isolate != nullptr);
    ub::HandleScope scope(*isolate);
    auto context = ub::Context::New(*isolate);
    REQUIRE(context.has_value());
    ub::ContextScope entered(*context);

    // Asked in script, because the *kind* of an error is not something to read
    // out of an engine's message text - and reported rather than asserted,
    // because the two engines do not agree on which error this is: one calls it
    // a RangeError, the other an InternalError. What is portable, and what an
    // embedder actually needs, is that script catches it at all.
    const std::string kind = ub_test::EvalText(*context, R"(
        (function () {
            function down() { return down(); }
            try { down(); return 'no throw'; }
            catch (e) { return e.constructor.name; }
        })()
    )");
    MESSAGE("runaway recursion arrives as ", kind);
    CHECK(kind != "no throw");

    // And the isolate is fine afterwards.
    CHECK(ub_test::EvalInt(*context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE(STACK_LIMIT, "isolate: a smaller stack limit is a smaller stack") {
    // The observability assertion. Not a particular depth - that is the
    // engine's business and differs by build - but that the knob moves it.
    // Both well below the thread's own stack: a limit larger than the real
    // stack is not a limit at all, and the engine cannot rescue a thread that
    // has genuinely run out (see docs/testing.md).
    const int shallow = RecursionDepthWith({.stackLimitBytes = std::size_t{128} * 1024});
    const int deeper = RecursionDepthWith({.stackLimitBytes = std::size_t{512} * 1024});

    MESSAGE("recursion reached ", shallow, " frames at 128 KiB and ", deeper, " at 512 KiB");
    CHECK(shallow > 0);
    CHECK(deeper > shallow);
}

UNIBIND_TEST_CASE(WORKER_THREADS, "isolate: what the engine did with the worker-thread hint can be read back") {
    // `workerThreads` is a hint at every value, which is only defensible
    // because the outcome is observable: honoured, clamped and ignored are
    // three different things an embedder has to tell apart, and a hint nobody
    // can observe is the mistake `UsedCodeCache` exists to avoid.
    //
    // So what is asserted is the shape, never a number - the count is engine
    // tuning, and one backend cannot comply at all.
    const auto reported = ub::Platform::WorkerThreads();

    if (!reported) {
        // Empty is not zero: it means the engine would not say, so an embedder
        // that needs none has to assume it has some.
        MESSAGE("this backend does not report a worker-thread count");
        return;
    }

    MESSAGE("the platform reports ", *reported, " worker threads");

    // Self-consistent: the same question, asked twice, has the same answer -
    // a count that moved would mean it was measuring something else.
    CHECK(ub::Platform::WorkerThreads() == reported);

    // And it is an answer about a platform that is up: making an isolate and
    // running script does not change it.
    {
        ub_test::Fixture fixture;
        CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
    }
    CHECK(ub::Platform::WorkerThreads() == reported);
}
