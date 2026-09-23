/// \file
/// Several realms in one isolate, and running code in a chosen one.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "support/harness.h"

TEST_CASE("realms: two contexts in one isolate have separate globals") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    CHECK(&second->GetIsolate() == &fixture.iso());
    CHECK_FALSE(second->GlobalObject().StrictEquals(fixture.context.GlobalObject()));

    {
        ub::ContextScope entered(*second);
        REQUIRE(ub::Evaluate(*second, "globalThis.onlyHere = 5").has_value());
        CHECK(ub_test::EvalInt(*second, "onlyHere") == 5);
    }

    CHECK(ub_test::EvalText(fixture.context, "typeof onlyHere") == "undefined");
}

TEST_CASE("realms: a value made in one realm is a value in the other") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());

    ub::Local<ub::Object> fromSecond;
    {
        ub::ContextScope entered(*second);
        auto object = ub::Object::New(*second);
        REQUIRE(object.has_value());
        REQUIRE(object->Set(*second, "made", ub_test::Str(fixture.iso(), "over there")).value_or(false));
        fromSecond = *object;
    }

    // Read it from the first realm; a handle belongs to an isolate, not a realm.
    // That a value *works* in another realm is the guarantee, and it is what
    // makes a sandbox useful at all.
    const auto made = fromSecond.Get(fixture.context, "made");
    REQUIRE(made.has_value());
    CHECK(ub_test::TextOf(*made) == "over there");

    ub_test::Expose(fixture.context, "visitor", fromSecond);
    CHECK(ub_test::EvalText(fixture.context, "visitor.made") == "over there");

    // What is deliberately NOT asserted here, and must not be added later: that
    // the object seen from this realm is the *same object* as the one made in
    // the other. An engine that wraps across realm boundaries hands out a
    // distinct wrapper, so a StrictEquals across realms is a question with no
    // portable answer. Compare within one realm, or compare something the
    // values carry - as the property read above does.
}

TEST_CASE("realms: each realm has its own built-ins, so instanceof does not cross") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());

    ub::Local<ub::Value> arrayFromSecond;
    {
        ub::ContextScope entered(*second);
        arrayFromSecond = ub_test::Eval(*second, "[1, 2, 3]");
    }

    ub_test::Expose(fixture.context, "visitor", arrayFromSecond);

    // Not a quirk of this API - it is what every engine does, and the reason a
    // cross-realm check has to ask what a value *is* rather than what it
    // inherits from.
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "visitor instanceof Array"));
    CHECK(ub_test::EvalTruth(fixture.context, "Array.isArray(visitor)"));
    CHECK(arrayFromSecond.Is<ub::Array>());
}

TEST_CASE("realms: a native function made in one realm is callable from the other") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());

    const auto function = ub_test::Eval(fixture.context, "(function () { return 'from the first realm'; })");
    {
        ub::ContextScope entered(*second);
        REQUIRE(second->GlobalObject().Set(*second, "borrowed", function).value_or(false));
        CHECK(ub_test::EvalText(*second, "borrowed()") == "from the first realm");
    }
}

TEST_CASE("realms: a context is reference counted, so a copy keeps the realm alive") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());

    ub::Context copy = *second;
    CHECK(copy);
    CHECK(copy.rec() == second->rec());

    second->Reset();
    CHECK(second->IsEmpty());

    // The copy still names a working realm.
    ub::ContextScope entered(copy);
    CHECK(ub_test::EvalInt(copy, "6 * 7") == 42);
    CHECK(&copy.GetIsolate() == &fixture.iso());
}

TEST_CASE("realms: a context moves without disturbing the realm") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    {
        ub::ContextScope entered(*second);
        REQUIRE(ub::Evaluate(*second, "globalThis.marker = 'kept'").has_value());
    }

    ub::Context moved = std::move(*second);
    CHECK(second->IsEmpty());  // NOLINT(bugprone-use-after-move) - that it is empty is the point

    ub::ContextScope entered(moved);
    CHECK(ub_test::EvalText(moved, "marker") == "kept");
}

TEST_CASE("realms: entered realms nest and unwind") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    auto third = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    REQUIRE(ub::Evaluate(fixture.context, "globalThis.name_ = 'one'").has_value());
    {
        ub::ContextScope enteredSecond(*second);
        REQUIRE(ub::Evaluate(*second, "globalThis.name_ = 'two'").has_value());
        {
            ub::ContextScope enteredThird(*third);
            REQUIRE(ub::Evaluate(*third, "globalThis.name_ = 'three'").has_value());
            CHECK(ub_test::EvalText(*third, "name_") == "three");
        }
        CHECK(ub_test::EvalText(*second, "name_") == "two");
    }
    CHECK(ub_test::EvalText(fixture.context, "name_") == "one");
}

TEST_CASE("realms: many realms can live at once") {
    ub_test::Fixture fixture;

    constexpr int COUNT = 16;
    std::vector<ub::Context> realms;
    realms.reserve(COUNT);
    for (int i = 0; i < COUNT; ++i) {
        auto made = ub::Context::New(fixture.iso());
        REQUIRE(made.has_value());
        ub::ContextScope entered(*made);
        REQUIRE(made->GlobalObject().Set(*made, "index", ub::Integer::New(fixture.iso(), i)).value_or(false));
        realms.push_back(std::move(*made));
    }

    fixture.iso().RequestGarbageCollection();

    for (int i = 0; i < COUNT; ++i) {
        ub::ContextScope entered(realms[static_cast<std::size_t>(i)]);
        CHECK(ub_test::EvalInt(realms[static_cast<std::size_t>(i)], "index") == i);
    }
}
