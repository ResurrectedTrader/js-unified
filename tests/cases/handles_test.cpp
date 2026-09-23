/// \file
/// The handle model of `docs/lifetimes.md`, section 5, one case per rule.
///
/// This is the design; everything else in the API is downstream of it. A bug
/// here is the expensive kind, and it is the kind that differs between a
/// bump-allocated scope and an exactly-rooted one - so these cases matter more
/// on two backends than any other file in the suite.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "support/harness.h"

namespace {

/// A scope that could be heap-allocated would outlive the stack frame that
/// opened it, which is exactly what SpiderMonkey's `JS::Rooted` forbids.
template <class T, class Arg>
concept HeapAllocatable = requires(Arg& argument) { new T(argument); };

/// The pattern the whole handle model exists to allow: a function that makes a
/// value in a scope of its own and hands it to its caller.
ub::Local<ub::Object> MakePoint(ub::Isolate& isolate, const ub::Context& context, std::int32_t x, std::int32_t y) {
    ub::EscapableHandleScope scope(isolate);
    auto object = ub::Object::New(context);
    REQUIRE(object.has_value());
    REQUIRE(object->Set(context, "x", ub::Integer::New(isolate, x)).value_or(false));
    REQUIRE(object->Set(context, "y", ub::Integer::New(isolate, y)).value_or(false));
    return scope.Escape(*object);
}

std::int32_t FieldOf(const ub::Context& context, const ub::Local<ub::Object>& object, std::string_view name) {
    auto value = object.Get(context, name);
    REQUIRE(value.has_value());
    auto asInteger = value->To<ub::Integer>();
    REQUIRE(asInteger.has_value());
    return asInteger->Int32Value();
}

}  // namespace

// ---------------------------------------------------------------------------
// Rule 1: a HandleScope is a stack object, and scopes close LIFO.
// ---------------------------------------------------------------------------

TEST_CASE("handles: a scope is a stack object and says so in its type") {
    CHECK_FALSE(std::is_copy_constructible_v<ub::HandleScope>);
    CHECK_FALSE(std::is_move_constructible_v<ub::HandleScope>);
    CHECK_FALSE(std::is_copy_assignable_v<ub::HandleScope>);
    CHECK_FALSE(std::is_move_assignable_v<ub::HandleScope>);

    // `operator new` is deleted, so a scope cannot become a member of a heap
    // object by accident. The same rule holds for every other scope here.
    CHECK_FALSE(HeapAllocatable<ub::HandleScope, ub::Isolate>);
    CHECK_FALSE(HeapAllocatable<ub::EscapableHandleScope, ub::Isolate>);
    CHECK_FALSE(HeapAllocatable<ub::ContextScope, const ub::Context>);
    CHECK_FALSE(HeapAllocatable<ub::TryCatch, ub::Isolate>);
}

TEST_CASE("handles: a handle is small, trivially copyable, and owns nothing") {
    // Rule 8: no per-value release call, so a handle has nothing to destroy.
    CHECK(std::is_trivially_copyable_v<ub::Local<ub::Object>>);
    CHECK(std::is_trivially_destructible_v<ub::Local<ub::Object>>);
    CHECK(sizeof(ub::Local<ub::Object>) <= 3 * sizeof(void*));

    // The figure docs/lifetimes.md section 6 quotes, printed rather than
    // assumed: it is a frame pointer and a 32-bit ordinal, so it is the pointer
    // size that moves it, and on x64 the epoch fits in padding that a handle
    // was paying for anyway.
    MESSAGE("sizeof(Local<T>) is ", sizeof(ub::Local<ub::Object>), " bytes with ", sizeof(void*),
            "-byte pointers and handle checks ", UNIBIND_HANDLE_CHECKS);

    // A Global is a root, so it is move-only: copying one would be a second
    // root, and that is spelled Duplicate().
    CHECK_FALSE(std::is_copy_constructible_v<ub::Global<ub::Object>>);
    CHECK(std::is_move_constructible_v<ub::Global<ub::Object>>);
}

TEST_CASE("handles: a default-constructed handle is empty") {
    const ub::Local<ub::Object> empty;
    CHECK(empty.IsEmpty());
    CHECK_FALSE(static_cast<bool>(empty));

    const ub::Global<ub::Object> noRoot;
    CHECK(noRoot.IsEmpty());
    CHECK_FALSE(static_cast<bool>(noRoot));
}

TEST_CASE("handles: nested scopes close in reverse order and leave the parent intact") {
    ub_test::Fixture fixture;

    auto outer = ub::Integer::New(fixture.iso(), 1);
    {
        ub::HandleScope first(fixture.iso());
        auto inFirst = ub::Integer::New(fixture.iso(), 2);
        {
            ub::HandleScope second(fixture.iso());
            auto inSecond = ub::Integer::New(fixture.iso(), 3);
            CHECK(inSecond.Int32Value() == 3);
            CHECK(inFirst.Int32Value() == 2);
            CHECK(outer.Int32Value() == 1);
        }
        // The inner frame is gone; this one is current again and unchanged.
        CHECK(inFirst.Int32Value() == 2);
        CHECK(outer.Int32Value() == 1);
    }
    CHECK(outer.Int32Value() == 1);
}

TEST_CASE("handles: opening and closing many frames does not disturb an outer handle") {
    ub_test::Fixture fixture;

    auto kept = ub::Object::New(fixture.context);
    REQUIRE(kept.has_value());
    REQUIRE(kept->Set(fixture.context, "marker", ub::Integer::New(fixture.iso(), 4242)).value_or(false));

    for (int round = 0; round < 200; ++round) {
        ub::HandleScope inner(fixture.iso());
        for (int i = 0; i < 20; ++i) {
            (void)ub::Integer::New(fixture.iso(), i);
        }
    }

    CHECK(FieldOf(fixture.context, *kept, "marker") == 4242);
}

// ---------------------------------------------------------------------------
// Rule 3: a handle may be copied, passed and stored for the life of its frame.
// ---------------------------------------------------------------------------

TEST_CASE("handles: a handle can be copied, passed and stored while its frame is open") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    REQUIRE(object->Set(fixture.context, "id", ub::Integer::New(fixture.iso(), 8)).value_or(false));

    std::vector<ub::Local<ub::Object>> copies(32, *object);
    const ub::Local<ub::Object> byValue = copies.back();
    const ub::Local<ub::Value> widened = byValue;  // widening is implicit

    CHECK(widened.StrictEquals(*object));
    CHECK(copies.front().StrictEquals(copies.back()));
    CHECK(FieldOf(fixture.context, copies[17], "id") == 8);
}

TEST_CASE("handles: a frame holds far more handles than it has inline slots") {
    ub_test::Fixture fixture;

    // The frame's inline storage is small and spills to the heap above it. A
    // handle made before the spill has to keep meaning the same value after -
    // which it does because a handle names a slot by ordinal, not by address
    // (docs/lifetimes.md section 4).
    constexpr int COUNT = 2000;
    const auto first = ub::Integer::New(fixture.iso(), 1000000);

    std::vector<ub::Local<ub::Integer>> handles;
    handles.reserve(COUNT);
    for (int i = 0; i < COUNT; ++i) {
        handles.push_back(ub::Integer::New(fixture.iso(), i));
        // Read the oldest handle on every append, so a reallocating buffer
        // would be caught at the append that moved it rather than at the end.
        REQUIRE(first.Int32Value() == 1000000);
    }
    for (int i = 0; i < COUNT; ++i) {
        CHECK(handles[static_cast<std::size_t>(i)].Int32Value() == i);
    }
}

// ---------------------------------------------------------------------------
// Rule 4: to return a handle, escape it.
// ---------------------------------------------------------------------------

TEST_CASE("handles: a function returns a handle by escaping it") {
    ub_test::Fixture fixture;

    const auto point = MakePoint(fixture.iso(), fixture.context, 3, 4);
    CHECK(FieldOf(fixture.context, point, "x") == 3);
    CHECK(FieldOf(fixture.context, point, "y") == 4);
}

TEST_CASE("handles: many handles escape from the same scope") {
    ub_test::Fixture fixture;

    // V8 reserves exactly one parent slot when an escapable scope opens, so the
    // second and later escapes take a different path in that backend. They have
    // to be indistinguishable from the first.
    constexpr int COUNT = 16;
    std::vector<ub::Local<ub::Object>> escaped;
    escaped.reserve(COUNT);
    {
        ub::EscapableHandleScope scope(fixture.iso());
        for (int i = 0; i < COUNT; ++i) {
            auto object = ub::Object::New(fixture.context);
            REQUIRE(object.has_value());
            REQUIRE(object->Set(fixture.context, "tag", ub::Integer::New(fixture.iso(), i)).value_or(false));
            escaped.push_back(scope.Escape(*object));
        }
    }

    fixture.iso().RequestGarbageCollection();

    for (int i = 0; i < COUNT; ++i) {
        CHECK(FieldOf(fixture.context, escaped[static_cast<std::size_t>(i)], "tag") == i);
    }
    CHECK_FALSE(escaped.front().StrictEquals(escaped.back()));
}

TEST_CASE("handles: escaping one value twice yields two handles to the same value") {
    ub_test::Fixture fixture;

    ub::Local<ub::Object> once;
    ub::Local<ub::Object> twice;
    {
        ub::EscapableHandleScope scope(fixture.iso());
        auto object = ub::Object::New(fixture.context);
        REQUIRE(object.has_value());
        once = scope.Escape(*object);
        twice = scope.Escape(*object);
    }

    fixture.iso().RequestGarbageCollection();
    CHECK(once.StrictEquals(twice));
    REQUIRE(once.Set(fixture.context, "written", ub::Integer::New(fixture.iso(), 5)).value_or(false));
    CHECK(FieldOf(fixture.context, twice, "written") == 5);
}

TEST_CASE("handles: a value escapes through two frames one at a time") {
    ub_test::Fixture fixture;

    ub::Local<ub::String> text;
    {
        ub::EscapableHandleScope outer(fixture.iso());
        ub::Local<ub::String> fromInner;
        {
            ub::EscapableHandleScope inner(fixture.iso());
            fromInner = inner.Escape(ub_test::Str(fixture.iso(), "two frames up"));
        }
        text = outer.Escape(fromInner);
    }

    fixture.iso().RequestGarbageCollection();
    CHECK(text.Utf8Value() == "two frames up");
}

TEST_CASE("handles: escaping an empty result stays empty") {
    ub_test::Fixture fixture;

    ub::EscapableHandleScope scope(fixture.iso());
    const std::optional<ub::Local<ub::Object>> nothing;
    CHECK_FALSE(scope.Escape(nothing).has_value());

    auto something = ub::Object::New(fixture.context);
    REQUIRE(something.has_value());
    CHECK(scope.Escape(something).has_value());
}

TEST_CASE("handles: an escaped handle survives a collection and a spilled sibling frame") {
    ub_test::Fixture fixture;

    ub::Local<ub::String> escaped;
    {
        ub::EscapableHandleScope outer(fixture.iso());
        {
            ub::HandleScope inner(fixture.iso());
            for (int i = 0; i < 500; ++i) {
                (void)ub_test::Str(fixture.iso(), "filler");
            }
        }
        escaped = outer.Escape(ub_test::Str(fixture.iso(), "kept"));
    }

    fixture.iso().RequestGarbageCollection();
    CHECK(escaped.Utf8Value() == "kept");
}

// ---------------------------------------------------------------------------
// Rule 5: to keep a value past all scopes, move it into a Global.
// ---------------------------------------------------------------------------

TEST_CASE("handles: a Global outlives every handle scope") {
    ub_test::Fixture fixture;

    ub::Global<ub::Object> kept;
    {
        ub::HandleScope inner(fixture.iso());
        auto object = ub::Object::New(fixture.context);
        REQUIRE(object.has_value());
        REQUIRE(object->Set(fixture.context, "marker", ub::Integer::New(fixture.iso(), 99)).value_or(false));
        kept = ub::Global<ub::Object>(fixture.iso(), *object);
    }

    fixture.iso().RequestGarbageCollection();

    REQUIRE_FALSE(kept.IsEmpty());
    CHECK(FieldOf(fixture.context, kept.Get(fixture.iso()), "marker") == 99);

    kept.Reset();
    CHECK(kept.IsEmpty());
}

TEST_CASE("handles: a Global moves, duplicates and resets independently") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    REQUIRE(object->Set(fixture.context, "n", ub::Integer::New(fixture.iso(), 1)).value_or(false));

    ub::Global<ub::Object> original(fixture.iso(), *object);
    ub::Global<ub::Object> copy = original.Duplicate();

    ub::Global<ub::Object> moved = std::move(original);
    CHECK(original.IsEmpty());  // NOLINT(bugprone-use-after-move) - that it is empty is the point
    REQUIRE_FALSE(moved.IsEmpty());

    // Two roots on one value: resetting one leaves the other alive.
    copy.Reset();
    fixture.iso().RequestGarbageCollection();
    CHECK(FieldOf(fixture.context, moved.Get(fixture.iso()), "n") == 1);

    ub::Global<ub::Object> assigned;
    assigned = std::move(moved);
    CHECK(moved.IsEmpty());  // NOLINT(bugprone-use-after-move)
    CHECK(FieldOf(fixture.context, assigned.Get(fixture.iso()), "n") == 1);
}

TEST_CASE("handles: Global::Get materialises into whichever frame is current") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    const ub::Global<ub::Object> kept(fixture.iso(), *object);

    ub::Local<ub::Object> fromInner;
    {
        ub::EscapableHandleScope inner(fixture.iso());
        fromInner = inner.Escape(kept.Get(fixture.iso()));
    }
    const auto fromOuter = kept.Get(fixture.iso());

    CHECK(fromInner.StrictEquals(fromOuter));
    CHECK(fromOuter.StrictEquals(*object));
}

TEST_CASE("handles: a Global survives its isolate's collections with nothing else holding the value") {
    ub_test::Fixture fixture;

    ub::Global<ub::Object> kept;
    {
        ub::HandleScope inner(fixture.iso());
        auto object = ub::Object::New(fixture.context);
        REQUIRE(object.has_value());
        REQUIRE(object->Set(fixture.context, "tag", ub_test::Str(fixture.iso(), "still here")).value_or(false));
        kept = ub::Global<ub::Object>(fixture.iso(), *object);
    }

    for (int round = 0; round < 3; ++round) {
        {
            ub::HandleScope churn(fixture.iso());
            for (int i = 0; i < 2000; ++i) {
                (void)ub::Object::New(fixture.context);
            }
        }
        fixture.iso().RequestGarbageCollection();
    }

    auto tag = kept.Get(fixture.iso()).Get(fixture.context, "tag");
    REQUIRE(tag.has_value());
    CHECK(ub_test::TextOf(*tag) == "still here");
}

// ---------------------------------------------------------------------------
// Rule 9: a frame that cannot grow yields an empty handle, never a value.
// ---------------------------------------------------------------------------

TEST_CASE("handles: a frame that cannot grow yields an empty handle, not undefined") {
    ub_test::Fixture fixture;

    ub::HandleScope scope(fixture.iso());

    // Fill the inline slots, none of which can fail; the next append is the one
    // that has to grow the frame.
    for (int i = 0; i < UNIBIND_FRAME_INLINE_SLOTS; ++i) {
        REQUIRE_FALSE(ub::Integer::New(fixture.iso(), i).IsEmpty());
    }

    ub::TryCatch tryCatch(fixture.iso());

    ub::Local<ub::Integer> beyond;
    long long fired = 0;
    {
        // Armed for exactly one call: the growth is the very next allocation,
        // and whatever the backend does to report the failure gets to allocate
        // normally afterwards.
        ub_test::AllocationFailure failing(1);
        beyond = ub::Integer::New(fixture.iso(), 1234);
        fired = failing.Stop();
    }

    if (fired == 0) {
        // The frame grew without touching the C++ allocator, so this test never
        // reached the branch it is about and has proved nothing. Saying so is
        // the honest answer; asserting would be a guess.
        ub_test::ReportSkip(
            "frame growth on this backend does not go through the C++ allocator, so exhaustion "
            "cannot be provoked from a test - see docs/testing.md");
        return;
    }

    // The rule (docs/lifetimes.md rule 9). The failure mode this exists to
    // prevent is a slot that reads as `undefined`, which would make running out
    // of memory indistinguishable from a property that was not there.
    CHECK(beyond.IsEmpty());
    CHECK_FALSE(static_cast<bool>(beyond));
    CHECK(tryCatch.HasCaught());

    // And the frame is usable again once the allocator is.
    tryCatch.Reset();
    const auto afterwards = ub::Integer::New(fixture.iso(), 7);
    REQUIRE_FALSE(afterwards.IsEmpty());
    CHECK(afterwards.Int32Value() == 7);
}

TEST_CASE("handles: a failed frame growth leaves the handles already in the frame alone") {
    ub_test::Fixture fixture;

    ub::HandleScope scope(fixture.iso());

    std::vector<ub::Local<ub::Integer>> before;
    before.reserve(UNIBIND_FRAME_INLINE_SLOTS);
    for (int i = 0; i < UNIBIND_FRAME_INLINE_SLOTS; ++i) {
        before.push_back(ub::Integer::New(fixture.iso(), i));
    }

    ub::TryCatch tryCatch(fixture.iso());
    long long fired = 0;
    {
        ub_test::AllocationFailure failing(1);
        (void)ub::Integer::New(fixture.iso(), 1234);
        fired = failing.Stop();
    }
    tryCatch.Reset();

    if (fired == 0) {
        ub_test::ReportSkip(
            "frame growth on this backend does not go through the C++ allocator, so exhaustion "
            "cannot be provoked from a test - see docs/testing.md");
        return;
    }

    for (int i = 0; i < UNIBIND_FRAME_INLINE_SLOTS; ++i) {
        CHECK(before[static_cast<std::size_t>(i)].Int32Value() == i);
    }
}

// ---------------------------------------------------------------------------
// Roots that frames take and have to give back.
// ---------------------------------------------------------------------------

TEST_CASE("handles: frames give back everything they took") {
    ub_test::Fixture fixture;

    // Each cycle opens a frame, spills it past its inline slots, and closes it.
    // If a frame leaked its overflow storage there would be one outstanding
    // allocation per cycle and the second window would be far larger than the
    // first; the engines' own bookkeeping cannot answer this.
    const auto cycle = [&] {
        ub::HandleScope scope(fixture.iso());
        for (int i = 0; i < 64; ++i) {
            (void)ub::Integer::New(fixture.iso(), i);
        }
    };

    constexpr int WINDOW = 500;
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    const long long afterWarmup = ub_test::OutstandingAllocations();
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    const long long growth = ub_test::OutstandingAllocations() - afterWarmup;

    INFO("outstanding allocations grew by ", growth, " over ", WINDOW, " frame cycles");
    CHECK(growth < WINDOW / 10);
}

TEST_CASE("handles: globals give back their roots") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());

    const auto cycle = [&] {
        ub::Global<ub::Object> root(fixture.iso(), *object);
        ub::Global<ub::Object> second = root.Duplicate();
        second.Reset();
    };

    constexpr int WINDOW = 500;
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    const long long afterWarmup = ub_test::OutstandingAllocations();
    for (int i = 0; i < WINDOW; ++i) {
        cycle();
    }
    const long long growth = ub_test::OutstandingAllocations() - afterWarmup;

    INFO("outstanding allocations grew by ", growth, " over ", WINDOW, " global cycles");
    CHECK(growth < WINDOW / 10);
}

// ---------------------------------------------------------------------------
// Two roots, one value.
//
// `Duplicate` makes exactly that, and so does rooting the same function twice,
// so "do these name the same value" is not "are these the same handle".
// Comparing the roots answers the wrong question - and answers it *silently*,
// which is why these have to be asserted rather than assumed: a registry of
// callbacks kept as `Global`s removes nothing, and nothing fails.
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(GLOBAL_IDENTITY, "handles: two roots over one object are equal, two over two are not") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    auto other = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    REQUIRE(other.has_value());

    const ub::Global<ub::Object> first(fixture.iso(), *object);
    const ub::Global<ub::Object> second(fixture.iso(), *object);
    const ub::Global<ub::Object> elsewhere(fixture.iso(), *other);

    // Separately made, so two different roots - and the same object.
    CHECK(first.StrictEquals(second));
    CHECK(second.StrictEquals(first));
    CHECK(first.SameValue(second));
    CHECK_FALSE(first.StrictEquals(elsewhere));
    CHECK_FALSE(first.SameValue(elsewhere));

    // A duplicate is the canonical second root over one value.
    const ub::Global<ub::Object> copy = first.Duplicate();
    CHECK(copy.StrictEquals(first));
    CHECK(first.StrictEquals(copy));

    // And against a `Local`, which is what a callback is handed.
    CHECK(first.StrictEquals(*object));
    CHECK(first.SameValue(*object));
    CHECK_FALSE(first.StrictEquals(*other));
}

UNIBIND_TEST_CASE(GLOBAL_IDENTITY, "handles: comparing roots outlives every frame that held the value") {
    // Half of the load-bearing clause: every frame that ever held the value is
    // closed, and a collection has been through since, and the roots still know
    // what they name.
    //
    // The other half - **no scope open at all** - cannot be a case among
    // others, because a backend that gets it wrong does not answer wrongly, it
    // aborts the process. It runs in a process of its own; see
    // `<backend>.global-identity-without-a-scope` in tests/CMakeLists.txt.
    ub_test::Fixture fixture;

    ub::Global<ub::Object> first;
    ub::Global<ub::Object> second;
    ub::Global<ub::Object> elsewhere;
    {
        ub::HandleScope inner(fixture.iso());
        auto object = ub::Object::New(fixture.context);
        auto other = ub::Object::New(fixture.context);
        REQUIRE(object.has_value());
        REQUIRE(other.has_value());
        first = ub::Global<ub::Object>(fixture.iso(), *object);
        second = ub::Global<ub::Object>(fixture.iso(), *object);
        elsewhere = ub::Global<ub::Object>(fixture.iso(), *other);
    }

    fixture.iso().RequestGarbageCollection();
    CHECK(first.StrictEquals(second));
    CHECK(second.StrictEquals(first));
    CHECK(first.SameValue(second));
    CHECK(first.StrictEquals(first.Duplicate()));
    CHECK_FALSE(first.StrictEquals(elsewhere));
    CHECK_FALSE(first.SameValue(elsewhere));
}

UNIBIND_TEST_CASE(GLOBAL_IDENTITY, "handles: an empty root is equal to nothing, including another empty one") {
    // It names no value, so no comparison with it can conclude that two things
    // are the same. `IsEmpty()` is the question that has an answer.
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());

    const ub::Global<ub::Object> empty;
    const ub::Global<ub::Object> alsoEmpty;
    const ub::Global<ub::Object> full(fixture.iso(), *object);

    CHECK(empty.IsEmpty());
    CHECK_FALSE(empty.StrictEquals(alsoEmpty));
    CHECK_FALSE(empty.SameValue(alsoEmpty));
    CHECK_FALSE(empty.StrictEquals(full));
    CHECK_FALSE(full.StrictEquals(empty));
    CHECK_FALSE(empty.StrictEquals(*object));

    // And the other way round: an empty *handle* names no value either, so a
    // root is equal to nothing when asked about one. It is the answer rather
    // than a crash because the library hands an empty handle back itself when
    // a frame cannot grow (docs/lifetimes.md rule 9), so a comparison can be
    // handed one without anyone having written anything strange.
    const ub::Local<ub::Object> emptyHandle;
    CHECK(emptyHandle.IsEmpty());
    CHECK_FALSE(full.StrictEquals(emptyHandle));
    CHECK_FALSE(full.SameValue(emptyHandle));
    CHECK_FALSE(empty.StrictEquals(emptyHandle));
}

TEST_CASE("handles: materialising an empty root yields an empty handle") {
    // A root that names nothing can only hand back a handle that names
    // nothing. The alternative - `undefined` - is worse here than anywhere
    // else in the API, because the handle is *typed*: a `Global<Function>`
    // would answer with a `Local<Function>` that is neither a function nor
    // empty, and the `IsEmpty()` an embedder writes would not fire.
    ub_test::Fixture fixture;

    const ub::Global<ub::Object> emptyObject;
    const ub::Global<ub::Function> emptyFunction;

    const ub::Local<ub::Object> object = emptyObject.Get(fixture.iso());
    const ub::Local<ub::Function> function = emptyFunction.Get(fixture.iso());

    CHECK(object.IsEmpty());
    CHECK(function.IsEmpty());
    CHECK_FALSE(static_cast<bool>(function));

    // And a root that does name something still materialises it.
    auto made = ub::Object::New(fixture.context);
    REQUIRE(made.has_value());
    const ub::Global<ub::Object> full(fixture.iso(), *made);
    const ub::Local<ub::Object> back = full.Get(fixture.iso());
    REQUIRE_FALSE(back.IsEmpty());
    CHECK(back.StrictEquals(*made));
}

UNIBIND_TEST_CASE(GLOBAL_IDENTITY, "handles: the two equalities differ where the language says they do") {
    // Both spellings exist for the same reason `Local` has both: NaN and -0.
    ub_test::Fixture fixture;

    const auto nan = ub_test::Eval(fixture.context, "NaN");
    const auto alsoNan = ub_test::Eval(fixture.context, "Number('not a number')");
    const auto zero = ub_test::Eval(fixture.context, "0");
    const auto minusZero = ub_test::Eval(fixture.context, "-0");

    const ub::Global<ub::Value> rootedNan(fixture.iso(), nan);
    const ub::Global<ub::Value> rootedAlsoNan(fixture.iso(), alsoNan);
    const ub::Global<ub::Value> rootedZero(fixture.iso(), zero);
    const ub::Global<ub::Value> rootedMinusZero(fixture.iso(), minusZero);

    CHECK_FALSE(rootedNan.StrictEquals(rootedAlsoNan));
    CHECK(rootedNan.SameValue(rootedAlsoNan));
    CHECK(rootedZero.StrictEquals(rootedMinusZero));
    CHECK_FALSE(rootedZero.SameValue(rootedMinusZero));
}

UNIBIND_TEST_CASE(GLOBAL_IDENTITY, "handles: a registry keyed by what a root names finds it again") {
    // The bug the comparisons exist to prevent, spelled out: the caller hands
    // back the same function through a different handle, and the removal has to
    // find it.
    ub_test::Fixture fixture;

    const auto handler = ub_test::Eval(fixture.context, "(function onEvent() {})");
    const auto another = ub_test::Eval(fixture.context, "(function onEvent() {})");

    std::vector<ub::Global<ub::Value>> registry;
    registry.emplace_back(fixture.iso(), handler);
    registry.emplace_back(fixture.iso(), another);

    const auto indexOf = [&registry](const ub::Local<ub::Value>& wanted) {
        for (std::size_t at = 0; at < registry.size(); ++at) {
            if (registry[at].StrictEquals(wanted)) {
                return static_cast<int>(at);
            }
        }
        return -1;
    };

    // Two functions with the same name and the same body are not the same
    // function, and a root does not make them one.
    CHECK(indexOf(handler) == 0);
    CHECK(indexOf(another) == 1);

    // The same value, arriving through a root made elsewhere, is still found.
    const ub::Global<ub::Value> arrivedAgain(fixture.iso(), handler);
    CHECK(indexOf(arrivedAgain.Get(fixture.iso())) == 0);
}
