/// \file
/// Who owns a native, and when it is destroyed.
///
/// `cases/classes_test.cpp` covers what a `Class<T>` *is*. This file covers the
/// one property of it that a passing suite will happily hide: a native
/// destroyed twice, or not at all, or while something can still reach it. Every
/// case here asks per native - `Lives` counts deaths by id - because a total
/// that comes out right is exactly what a double destruction plus a leak looks
/// like.
///
/// The invariant is two sentences, not one, since decision 14 separated the
/// wrapper's cell from the native it holds a share of:
///
///   1. **The engine gives back every share it took, exactly once, by the time
///      its isolate is gone.** That is the half a backend works for, and why an
///      isolate keeps a list of live instances and finishes the survivors as it
///      goes away.
///   2. **A native is destroyed exactly once, when its last share goes** -
///      which may be *after* the isolate, because the last share may be the
///      embedder's. That is not a leak and not a violation of (1).
///
/// The single old sentence - every native destroyed by the time the isolate is
/// gone - forbade in plain terms the case the redesign exists for.
///
/// *When* inside that window is the collector's business and the two engines
/// answer differently, so timing is reported, never asserted - except where a
/// reference is one the test holds, which is the whole reason the shared cases
/// below can be deterministic where the collector cases cannot.

#include "support/ownership.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <vector>

#include "support/harness.h"

namespace {

using ub_test::Lives;
using ub_test::Tracked;

std::unique_ptr<Tracked> MakeTracked(const ub::CallbackInfo& info) {
    std::int32_t start = 0;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return nullptr;  // the coercion threw
        }
        start = *asInt;
    }
    return std::make_unique<Tracked>(start);
}

/// Makes its native and then refuses, so the callback's own `unique_ptr` is the
/// only thing that ever owned it.
std::unique_ptr<Tracked> MakeThenRefuse(const ub::CallbackInfo& info) {
    auto native = std::make_unique<Tracked>(1);
    info.ThrowTypeError("this constructor changed its mind");
    return nullptr;
}

void ReadValue(Tracked& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.value);
}

void WriteValue(Tracked& self, const ub::Local<ub::Value>& value, const ub::PropertyCallbackInfo& info) {
    const auto asInt = value.ToInt32(info.GetContext());
    if (!asInt) {
        return;
    }
    self.value = *asInt;
}

void Bump(Tracked& self, const ub::CallbackInfo& info) {
    ++self.value;
    info.GetReturnValue().Set(self.value);
}

ub::Class<Tracked> DeclareTracked(ub::Isolate& isolate) {
    auto cls = ub::Class<Tracked>::New(isolate, "Tracked");
    cls.Construct<&MakeTracked>();
    cls.Accessor<&ReadValue, &WriteValue>("value");
    cls.Method<&Bump>("bump");
    return cls;
}

/// Make the collector want to run, without depending on it running.
void Churn(ub::Isolate& isolate, const ub::Context& context) {
    for (int round = 0; round < 4; ++round) {
        {
            ub::HandleScope churn(isolate);
            for (int i = 0; i < 2000; ++i) {
                (void)ub::Object::New(context);
            }
        }
        isolate.RequestGarbageCollection();
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// One wrapper, one native: the ordinary case, and the guarantee that has to
// hold whether or not anything else shares.
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: every native is destroyed exactly once, and no native twice") {
    Lives::Reset();
    constexpr int MADE = 64;

    std::vector<int> keptIds;
    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        const auto cls = DeclareTracked(*isolate);

        // Garbage: nothing names these once the inner frame closes.
        {
            ub::HandleScope inner(*isolate);
            for (int i = 0; i < MADE / 2; ++i) {
                REQUIRE(cls.Wrap(*context, std::make_unique<Tracked>(i)).has_value());
            }
        }

        // Survivors: rooted, and still rooted when the isolate goes.
        std::vector<ub::Global<ub::Object>> kept;
        kept.reserve(MADE / 2);
        for (int i = 0; i < MADE / 2; ++i) {
            auto native = std::make_unique<Tracked>(i);
            keptIds.push_back(native->id);
            auto instance = cls.Wrap(*context, std::move(native));
            REQUIRE(instance.has_value());
            kept.emplace_back(*isolate, *instance);
        }
        REQUIRE(Lives::TotalBorn() == MADE);

        Churn(*isolate, *context);

        MESSAGE("of ", MADE, " natives, ", Lives::TotalDeaths(), " were destroyed while the isolate was alive");
        // Nothing rooted may be finalised, whatever the collector felt like
        // doing with the rest.
        for (const int id : keptIds) {
            CHECK(Lives::Deaths(id) == 0);
        }
        CHECK_FALSE(Lives::AnyDestroyedTwice());
    }

    // The isolate is gone, so the window closed: this is the invariant.
    CHECK(Lives::TotalDeaths() == MADE);
    CHECK(Lives::EachDestroyedExactlyOnce());
    CHECK(Lives::Alive() == 0);
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: a native the collector left alone is still whole") {
    // The other half of "destroyed exactly once": not destroyed *early*. A
    // finalizer that ran while the object was still reachable would leave the
    // reads below looking at freed memory, which no count would show.
    Lives::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareTracked(fixture.iso());

    std::vector<ub::Global<ub::Object>> kept;
    std::vector<int> ids;
    for (int i = 0; i < 8; ++i) {
        auto native = std::make_unique<Tracked>(i * 10);
        ids.push_back(native->id);
        auto instance = cls.Wrap(fixture.context, std::move(native));
        REQUIRE(instance.has_value());
        kept.emplace_back(fixture.iso(), *instance);
    }

    Churn(fixture.iso(), fixture.context);

    for (std::size_t i = 0; i < kept.size(); ++i) {
        const auto instance = kept[i].Get(fixture.iso());
        REQUIRE_FALSE(instance.IsEmpty());
        Tracked* native = ub::Class<Tracked>::Unwrap(instance);
        REQUIRE(native != nullptr);
        CHECK(native->id == ids[i]);
        CHECK(native->value == static_cast<std::int32_t>(i) * 10);
        CHECK(Lives::Deaths(native->id) == 0);
    }
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: a constructor that makes a native and then refuses destroys it once") {
    Lives::Reset();
    ub_test::Fixture fixture;

    const auto cls = ub::Class<Tracked>::New(fixture.iso(), "Refuses");
    cls.Construct<&MakeThenRefuse>();
    ub_test::Expose(fixture.context, "Refuses", *cls.GetConstructor(fixture.context));

    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "new Refuses()").has_value());
    CHECK(tryCatch.HasCaught());
    tryCatch.Reset();

    // The native existed - the callback made it - and exactly one thing ever
    // owned it, so exactly one thing destroyed it.
    REQUIRE(Lives::TotalBorn() == 1);
    CHECK(Lives::Deaths(0) == 1);
    CHECK(Lives::Alive() == 0);
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: an instantiation that fails does not take the native with it") {
    // A failed hand-over has two ways to be wrong and they look the same from
    // the outside: the native is destroyed although the caller still owns it,
    // or it is leaked although nobody does. The caller keeps a `unique_ptr`
    // here, so both are visible - it must still own a live native afterwards,
    // and the native must go when *it* does.
    Lives::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareTracked(fixture.iso());

    auto native = std::make_unique<Tracked>(5);
    const int id = native->id;

    ub::TryCatch tryCatch(fixture.iso());
    long long fired = 0;
    {
        ub_test::AllocationFailure failing(1);
        try {
            (void)cls.Wrap(fixture.context, std::move(native));
        } catch (const std::bad_alloc&) {
            // However the failure arrives - an empty result or a throw out of
            // the allocator - the question is the same one.
        }
        fired = failing.Stop();
    }
    tryCatch.Reset();

    if (fired == 0) {
        ub_test::ReportSkip(
            "wrapping a native on this backend made no C++ allocation, so a failing one proves nothing");
        return;
    }

    // Where the failure lands decides which side is still holding the native -
    // that is the allocator's business, not the API's. What must hold either
    // way: it is destroyed once at most now, it is still whole if the caller
    // has it, and it is destroyed exactly once by the time the caller lets go.
    CHECK(Lives::Deaths(id) <= 1);
    // Reading a moved-from `unique_ptr` is the question here: a failed `Wrap` may or
    // may not have taken it, and the rules have to hold either way.
    // NOLINTBEGIN(bugprone-use-after-move)
    if (native) {
        CHECK(Lives::Deaths(id) == 0);
        CHECK(native->value == 5);
        native.reset();
    }
    // NOLINTEND(bugprone-use-after-move)
    CHECK(Lives::Deaths(id) == 1);
    CHECK(Lives::Alive() == 0);
    CHECK_FALSE(Lives::AnyDestroyedTwice());
}

// ---------------------------------------------------------------------------
// Shared ownership: a wrapper owns a *share*, so two wrappers can name one
// native and the embedder can hold one of its own (unibind/class.h).
//
// The shape of every case below is the same, and it is what makes them
// deterministic where a collector case cannot be: the embedder holds a share of
// its own, so "has the engine given its share back" is a question about a
// `use_count()` the test can read, not about a destructor that has not run yet.
// A count that never drops is reported rather than asserted - that the
// collector ran at all is the one thing neither engine promises.
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: two wrappers over one native name the same native") {
    Lives::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareTracked(fixture.iso());
    auto shared = std::make_shared<Tracked>(3);

    auto first = cls.Wrap(fixture.context, shared);
    auto second = cls.Wrap(fixture.context, shared);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    // Two wrappers, one native: the same address, not an equal value.
    CHECK(ub::Class<Tracked>::Unwrap(*first) == shared.get());
    CHECK(ub::Class<Tracked>::Unwrap(*second) == shared.get());
    CHECK_FALSE(first->StrictEquals(*second));  // two objects, though

    // And a write through one is a read through the other.
    ub_test::Expose(fixture.context, "a", *first);
    ub_test::Expose(fixture.context, "b", *second);
    CHECK(ub_test::EvalInt(fixture.context, "a.value = 42; b.value") == 42);
    CHECK(shared->value == 42);

    CHECK(Lives::Deaths(shared->id) == 0);
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: a native reachable through two wrappers outlives the first") {
    Lives::Reset();
    auto shared = std::make_shared<Tracked>(7);
    const int id = shared->id;

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        const auto cls = DeclareTracked(*isolate);

        ub::Global<ub::Object> kept;
        {
            ub::HandleScope inner(*isolate);
            auto doomed = cls.Wrap(*context, shared);
            auto survivor = cls.Wrap(*context, shared);
            REQUIRE(doomed.has_value());
            REQUIRE(survivor.has_value());
            kept = ub::Global<ub::Object>(*isolate, *survivor);
        }
        // `doomed` is now unreachable from anywhere. The engine holds two
        // shares, the test holds one.
        const long engineShares = shared.use_count() - 1;
        CHECK(engineShares == 2);

        Churn(*isolate, *context);

        if (shared.use_count() - 1 == engineShares) {
            MESSAGE(
                "the collector did not take the unreachable wrapper, so this case proved only that "
                "nothing was destroyed early");
        }

        // The first of the two is gone, or will be: either way the native is
        // not, because the second still names it. This is the assertion the
        // whole redesign is for.
        CHECK(Lives::Deaths(id) == 0);
        const auto instance = kept.Get(*isolate);
        REQUIRE_FALSE(instance.IsEmpty());
        CHECK(ub::Class<Tracked>::Unwrap(instance) == shared.get());
        CHECK(shared->value == 7);
    }

    // The isolate is gone, so every share it held is given back - both of them,
    // once each. The test's own share is all that is left, and the native is
    // still whole.
    CHECK(Lives::Deaths(id) == 0);
    CHECK(shared.use_count() == 1);
    CHECK(shared->value == 7);

    shared.reset();
    CHECK(Lives::Deaths(id) == 1);
    CHECK(Lives::EachDestroyedExactlyOnce());
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: a native the embedder co-owns outlives its wrapper") {
    Lives::Reset();
    auto shared = std::make_shared<Tracked>(11);
    const int id = shared->id;

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        const auto cls = DeclareTracked(*isolate);
        {
            ub::HandleScope inner(*isolate);
            REQUIRE(cls.Wrap(*context, shared).has_value());
        }
        CHECK(shared.use_count() == 2);

        Churn(*isolate, *context);
        // Collected or not, the embedder's share is what keeps it alive.
        CHECK(Lives::Deaths(id) == 0);
        CHECK(shared->value == 11);
    }

    // Decision 7 says the isolate destroys the natives that survived to
    // teardown. A co-owned native is the case that decision cannot cover: what
    // the isolate gives back is its *share*, and the native goes when the last
    // share does.
    CHECK(Lives::Deaths(id) == 0);
    CHECK(shared.use_count() == 1);
    shared->value = 12;
    CHECK(shared->value == 12);

    shared.reset();
    CHECK(Lives::Deaths(id) == 1);
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: a native outlives the embedder's reference") {
    // The other direction of the same rule: the embedder lets go first and the
    // wrapper is what is left holding it.
    Lives::Reset();
    int id = -1;

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        const auto cls = DeclareTracked(*isolate);

        auto shared = std::make_shared<Tracked>(21);
        id = shared->id;
        auto instance = cls.Wrap(*context, shared);
        REQUIRE(instance.has_value());
        ub::Global<ub::Object> kept(*isolate, *instance);

        shared.reset();  // the embedder is done with it

        CHECK(Lives::Deaths(id) == 0);
        Churn(*isolate, *context);
        CHECK(Lives::Deaths(id) == 0);

        const auto found = kept.Get(*isolate);
        REQUIRE_FALSE(found.IsEmpty());
        Tracked* native = ub::Class<Tracked>::Unwrap(found);
        REQUIRE(native != nullptr);
        CHECK(native->value == 21);
    }

    CHECK(Lives::Deaths(id) == 1);
    CHECK(Lives::EachDestroyedExactlyOnce());
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: the native goes with the last reference, whichever one that is") {
    // Two runs of the same arrangement, differing only in which owner lets go
    // second. The observable outcome has to be identical: no death at the first
    // release, exactly one at the second.
    for (int engineLast = 0; engineLast < 2; ++engineLast) {
        CAPTURE(engineLast);
        Lives::Reset();

        auto shared = std::make_shared<Tracked>(31);
        const int id = shared->id;

        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        {
            ub::HandleScope scope(*isolate);
            auto context = ub::Context::New(*isolate);
            REQUIRE(context.has_value());
            ub::ContextScope entered(*context);

            const auto cls = DeclareTracked(*isolate);
            auto instance = cls.Wrap(*context, shared);
            REQUIRE(instance.has_value());
            ub::Global<ub::Object> kept(*isolate, *instance);

            if (engineLast == 1) {
                shared.reset();
                CHECK(Lives::Deaths(id) == 0);
            }
        }

        if (engineLast == 1) {
            isolate.reset();
            CHECK(Lives::Deaths(id) == 1);
        } else {
            isolate.reset();
            CHECK(Lives::Deaths(id) == 0);
            CHECK(shared.use_count() == 1);
            shared.reset();
            CHECK(Lives::Deaths(id) == 1);
        }

        CHECK(Lives::EachDestroyedExactlyOnce());
    }
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: many wrappers over one native destroy it once") {
    Lives::Reset();
    constexpr int WRAPPERS = 16;
    int id = -1;

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        const auto cls = DeclareTracked(*isolate);
        auto shared = std::make_shared<Tracked>(0);
        id = shared->id;

        std::vector<ub::Global<ub::Object>> kept;
        kept.reserve(WRAPPERS);
        for (int i = 0; i < WRAPPERS; ++i) {
            auto instance = cls.Wrap(*context, shared);
            REQUIRE(instance.has_value());
            kept.emplace_back(*isolate, *instance);
        }
        CHECK(shared.use_count() == WRAPPERS + 1);

        // A native the engine also owns exclusively, alongside, so a backend
        // that keeps one list for both models has something to get wrong.
        REQUIRE(cls.Wrap(*context, std::make_unique<Tracked>(99)).has_value());

        shared.reset();
        CHECK(Lives::Deaths(id) == 0);  // sixteen wrappers still hold it
    }

    CHECK(Lives::Deaths(id) == 1);
    CHECK(Lives::TotalBorn() == 2);
    CHECK(Lives::EachDestroyedExactlyOnce());
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: a shared instantiation that fails gives back only its own share") {
    Lives::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareTracked(fixture.iso());
    auto shared = std::make_shared<Tracked>(5);
    const int id = shared->id;

    ub::TryCatch tryCatch(fixture.iso());
    long long fired = 0;
    {
        ub_test::AllocationFailure failing(1);
        try {
            (void)cls.Wrap(fixture.context, shared);
        } catch (const std::bad_alloc&) {
        }
        fired = failing.Stop();
    }
    tryCatch.Reset();

    if (fired == 0) {
        ub_test::ReportSkip(
            "wrapping a native on this backend made no C++ allocation, so a failing one proves nothing");
        return;
    }

    // A failure that leaked the engine's share would leave the count above one
    // forever; one that released a share it never took would take the native
    // down with it.
    CHECK(shared.use_count() == 1);
    CHECK(Lives::Deaths(id) == 0);
    CHECK(shared->value == 5);
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: a share taken out of a wrapper outlives the wrapper") {
    // `UnwrapShared` is how an embedder keeps a native it found through script
    // - a callback handed an instance, wanting to hold on to what it carries.
    // A plain `Unwrap` would give a pointer that is only as good as the wrapper
    // it came from; a share is the thing that says so in the type.
    Lives::Reset();
    std::shared_ptr<Tracked> taken;
    int id = -1;

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        const auto cls = DeclareTracked(*isolate);
        ub_test::Expose(*context, "Tracked", *cls.GetConstructor(*context));

        const auto made = ub_test::Eval(*context, "new Tracked(17)");
        taken = ub::Class<Tracked>::UnwrapShared(made);
        REQUIRE(taken != nullptr);
        id = taken->id;
        CHECK(taken->value == 17);

        // Nothing but the engine and this share names it.
        CHECK(taken.use_count() == 2);

        // And the share goes back the way it came: wrapping it again yields a
        // second wrapper that agrees with the first about what it carries.
        const auto again = cls.Wrap(*context, taken);
        REQUIRE(again.has_value());
        CHECK(ub::Class<Tracked>::Unwrap(*again) == taken.get());
        CHECK(ub::Class<Tracked>::Unwrap(made) == ub::Class<Tracked>::Unwrap(*again));
        CHECK(ub::Class<Tracked>::UnwrapShared(*again).get() == taken.get());
        CHECK_FALSE(made.StrictEquals(*again));  // two wrappers, one native
        CHECK(taken.use_count() >= 3);

        ub_test::Expose(*context, "first", made);
        ub_test::Expose(*context, "second", *again);
        CHECK(ub_test::EvalInt(*context, "first.value = 18; second.value") == 18);
        taken->value = 17;
    }

    CHECK(Lives::Deaths(id) == 0);
    CHECK(taken.use_count() == 1);
    CHECK(taken->value == 17);

    taken.reset();
    CHECK(Lives::Deaths(id) == 1);
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: a native in the embedder's own storage is not destroyed by the engine") {
    // The escape hatch unibind/class.h documents: a share with a no-op deleter, for
    // a native whose storage is not the engine's to free. The statement is made
    // once, at the call site, and this is what it has to mean.
    Lives::Reset();
    Tracked onTheStack(64);
    const int id = onTheStack.id;

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        const auto cls = DeclareTracked(*isolate);
        auto instance = cls.Wrap(*context, std::shared_ptr<Tracked>(&onTheStack, [](Tracked*) {}));
        REQUIRE(instance.has_value());
        ub_test::Expose(*context, "borrowed", *instance);

        CHECK(ub_test::EvalInt(*context, "borrowed.value") == 64);
        CHECK(ub_test::EvalInt(*context, "borrowed.bump()") == 65);
    }

    // The isolate gave back its share, and the share did nothing.
    CHECK(Lives::Deaths(id) == 0);
    CHECK(onTheStack.value == 65);
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: a native is destroyed on the thread that owned its isolate") {
    // Foreground finalization is a requirement, not tidiness. A box's destroy
    // drops a `std::shared_ptr` whose other holders are the embedder's, so an
    // engine that finalized on a helper thread would race them - and a race in
    // a finalizer is the kind of failure that does not reproduce, so a test
    // that only counts destructions would never see it.
    //
    // The isolate is made on a thread of its own, which is what makes the
    // question sharp: a death on the *main* thread would be as wrong as a death
    // on an engine's helper thread, and both are distinguishable from a death
    // on the thread that owned the isolate.
    Lives::Reset();
    constexpr int MADE = 32;

    std::thread::id owner;
    bool ran = false;

    std::thread elsewhere([&owner, &ran] {
        owner = std::this_thread::get_id();

        auto isolate = ub::Isolate::New();
        if (isolate == nullptr) {
            return;
        }
        {
            ub::HandleScope scope(*isolate);
            auto context = ub::Context::New(*isolate);
            if (!context) {
                return;
            }
            ub::ContextScope entered(*context);

            const auto cls = DeclareTracked(*isolate);

            // Half collectable, half alive at teardown, so both paths to a
            // destructor are exercised - the collector's and the isolate's.
            {
                ub::HandleScope inner(*isolate);
                for (int i = 0; i < MADE / 2; ++i) {
                    if (!cls.Wrap(*context, std::make_unique<Tracked>(i)).has_value()) {
                        return;
                    }
                }
            }
            std::vector<ub::Global<ub::Object>> kept;
            kept.reserve(MADE / 2);
            for (int i = 0; i < MADE / 2; ++i) {
                auto instance = cls.Wrap(*context, std::make_unique<Tracked>(i));
                if (!instance) {
                    return;
                }
                kept.emplace_back(*isolate, *instance);
            }

            Churn(*isolate, *context);
        }
        isolate.reset();
        ran = true;
    });
    elsewhere.join();

    REQUIRE(ran);
    CHECK(Lives::TotalBorn() == MADE);
    CHECK(Lives::EachDestroyedExactlyOnce());

    // The assertion. Every destructor ran on the thread that owned the isolate:
    // none on this one, and none on a thread belonging to the engine.
    CHECK(Lives::DeathsElsewhere(owner) == 0);
    CHECK(Lives::AllDeathsOn(owner));
    CHECK_FALSE(Lives::AllDeathsOn(std::this_thread::get_id()));
}

// ---------------------------------------------------------------------------
// What an allocation failure *inside* the hand-over costs
//
// `Wrap` allocates more than once - the box, the engine's own record of the
// instance, the slot the handle needs - and only the first of those is covered
// above, because a single injected failure always lands on the first. The
// sweep below walks the failure through the call, and the question at every
// stop is the one a total cannot answer: was the native given back exactly
// once?
//
// The failure it is written for: a box the engine has already been told about,
// and which the caller then frees as well.
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: a hand-over that runs out of memory gives the native back exactly once") {
    Lives::Reset();
    constexpr long long SITES = 8;
    std::vector<int> ids;
    long long reached = 0;

    {
        ub_test::Fixture fixture;
        const auto cls = DeclareTracked(fixture.iso());

        for (long long skip = 0; skip < SITES; ++skip) {
            CAPTURE(skip);
            auto shared = std::make_shared<Tracked>(7);
            const int id = shared->id;
            ids.push_back(id);

            // A frame with nothing left inline, so the slot the wrapper needs
            // is one of the allocations that can fail - the last of them, and
            // the one that happens after the engine already has the box.
            ub::HandleScope crowded(fixture.iso());
            std::vector<ub::Local<ub::Integer>> filler;
            filler.reserve(UNIBIND_FRAME_INLINE_SLOTS);
            for (int i = 0; i < UNIBIND_FRAME_INLINE_SLOTS; ++i) {
                filler.push_back(ub::Integer::New(fixture.iso(), i));
            }

            ub::TryCatch tryCatch(fixture.iso());
            std::optional<ub::Local<ub::Object>> made;
            long long fired = 0;
            {
                ub_test::AllocationFailure failing(1, skip);
                try {
                    made = cls.Wrap(fixture.context, shared);
                } catch (const std::bad_alloc&) {
                    // A failure may arrive as an empty result or as a throw out
                    // of the allocator. The ownership question is the same one.
                }
                fired = failing.Stop();
            }
            tryCatch.Reset();
            if (fired == 0) {
                continue;
            }
            ++reached;

            // Nothing has been destroyed yet whatever happened: the embedder
            // still holds a share of its own.
            CHECK(Lives::Deaths(id) == 0);
            CHECK(shared->value == 7);
            // The engine may hold a share even though the wrapper was not
            // made: a hand-over that failed *after* the box reached the engine
            // is one the engine gives back, at teardown rather than now. What
            // must never happen is a share going twice, or going while the
            // embedder still holds one - which is what the per-native count
            // after the isolate is gone asks.
            CHECK(shared.use_count() >= 1);
            CHECK(shared.use_count() <= 2);
        }
    }

    if (reached == 0) {
        ub_test::ReportSkip(
            "wrapping a native on this backend made no C++ allocation, so a failing one proves nothing");
        return;
    }

    INFO("failures landed at ", reached, " of ", SITES, " allocation sites");
    for (const int id : ids) {
        CAPTURE(id);
        CHECK(Lives::Deaths(id) == 1);
    }
    CHECK_FALSE(Lives::AnyDestroyedTwice());
    CHECK(Lives::EachDestroyedExactlyOnce());
    CHECK(Lives::Alive() == 0);
}

// ---------------------------------------------------------------------------
// What a value costs its isolate
//
// A `Function::New` or an `External::New` is a *value*: it is collected like
// any other, and nothing in the API says it leaves anything behind. The
// backends both need a small record per callback - the pair an engine will not
// carry for them - and the question these ask is whether that record dies with
// the value or with the isolate.
// ---------------------------------------------------------------------------

namespace {

void AnswersSeven(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(7);
}

}  // namespace

TEST_CASE("ownership: a native function costs its isolate nothing once it is collected") {
    ub_test::Fixture fixture;

    const auto cycle = [&] {
        ub::HandleScope scope(fixture.iso());
        for (int i = 0; i < 100; ++i) {
            (void)ub::Function::New(fixture.context, &AnswersSeven);
        }
    };

    constexpr int WINDOW = 20;  // 2000 functions per window
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    Churn(fixture.iso(), fixture.context);
    const long long afterWarmup = ub_test::OutstandingAllocations();
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    Churn(fixture.iso(), fixture.context);
    const long long growth = ub_test::OutstandingAllocations() - afterWarmup;

    INFO("outstanding allocations grew by ", growth, " over ", WINDOW * 100, " functions");
    CHECK(growth < (WINDOW * 100) / 10);
}

UNIBIND_TEST_CASE(EXTERNALS, "ownership: an external costs its isolate nothing once it is collected") {
    ub_test::Fixture fixture;
    Tracked payload(1);

    const auto cycle = [&] {
        ub::HandleScope scope(fixture.iso());
        for (int i = 0; i < 100; ++i) {
            (void)ub::External::New(fixture.iso(), payload);
        }
    };

    constexpr int WINDOW = 20;
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    Churn(fixture.iso(), fixture.context);
    const long long afterWarmup = ub_test::OutstandingAllocations();
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    Churn(fixture.iso(), fixture.context);
    const long long growth = ub_test::OutstandingAllocations() - afterWarmup;

    INFO("outstanding allocations grew by ", growth, " over ", WINDOW * 100, " externals");
    CHECK(growth < (WINDOW * 100) / 10);
}

UNIBIND_TEST_CASE(OWNERSHIP, "ownership: finalizing an instance costs the same whatever the isolate has seen") {
    // A backend that finds an instance's record by walking a list pays for
    // every instance the isolate ever made, not for the ones it still has - so
    // the cost of a collection grows with the isolate's history and nothing
    // reports it. The shape is a *ratio* rather than a figure: the same work,
    // done later, must not cost materially more.
    Lives::Reset();
    ub_test::Fixture fixture;

    const auto cls = DeclareTracked(fixture.iso());
    const auto window = [&] {
        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < 20000; ++i) {
            ub::HandleScope scope(fixture.iso());
            (void)cls.Wrap(fixture.context, std::make_shared<Tracked>(i));
        }
        Churn(fixture.iso(), fixture.context);
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count();
    };

    const long long first = window();
    for (int i = 0; i < 4; ++i) {
        (void)window();
    }
    const long long last = window();

    MESSAGE("first window ", first, " ms, sixth window ", last, " ms");
    // Deliberately loose: this is a *growth* test, not a benchmark, and the
    // failure it is written for is linear in the isolate's history - six
    // windows in, the leak hunt measured twelve times the first window.
    CHECK(last < (first + 20) * 5);
}
