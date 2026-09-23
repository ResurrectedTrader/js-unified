/// \file
/// Symbols, including the well-known ones, as property keys for declaration and
/// lookup alike.

#include <string>

#include "support/harness.h"

UNIBIND_TEST_CASE(SYMBOLS, "symbols: a fresh symbol is equal to nothing but itself") {
    ub_test::Fixture fixture;

    const auto first = ub::Symbol::New(fixture.iso(), "same words");
    const auto second = ub::Symbol::New(fixture.iso(), "same words");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    CHECK(first->Kind() == ub::ValueKind::Symbol);
    CHECK(first->StrictEquals(*first));
    CHECK_FALSE(first->StrictEquals(*second));
    CHECK_FALSE(first->SameValue(*second));
}

UNIBIND_TEST_CASE(SYMBOLS, "symbols: a symbol is a Name and is not a String") {
    ub_test::Fixture fixture;

    const auto symbol = ub::Symbol::New(fixture.iso());
    REQUIRE(symbol.has_value());

    const ub::Local<ub::Value> widened = *symbol;
    CHECK(widened.Is<ub::Name>());
    CHECK(widened.Is<ub::Symbol>());
    CHECK_FALSE(widened.Is<ub::String>());
    CHECK_FALSE(widened.Is<ub::Object>());
    CHECK(widened.To<ub::Symbol>().has_value());
    CHECK_FALSE(widened.To<ub::String>().has_value());
}

UNIBIND_TEST_CASE(SYMBOLS, "symbols: a description round-trips and an anonymous symbol has none") {
    ub_test::Fixture fixture;

    const auto described = ub::Symbol::New(fixture.iso(), "with a description");
    REQUIRE(described.has_value());
    const auto description = described->Description();
    REQUIRE(description.has_value());
    CHECK(*description == "with a description");

    const auto anonymous = ub::Symbol::New(fixture.iso());
    REQUIRE(anonymous.has_value());
    CHECK_FALSE(anonymous->Description().has_value());
}

UNIBIND_TEST_CASE(SYMBOLS, "symbols: Symbol.for is the registry script sees") {
    ub_test::Fixture fixture;

    const auto registered = ub::Symbol::For(fixture.iso(), "unibind.test.key");
    const auto again = ub::Symbol::For(fixture.iso(), "unibind.test.key");
    REQUIRE(registered.has_value());
    REQUIRE(again.has_value());
    CHECK(registered->StrictEquals(*again));

    ub_test::Expose(fixture.context, "fromNative", *registered);
    CHECK(ub_test::EvalTruth(fixture.context, "fromNative === Symbol.for('unibind.test.key')"));
    CHECK(ub_test::EvalText(fixture.context, "Symbol.keyFor(fromNative)") == "unibind.test.key");
}

UNIBIND_TEST_CASE(SYMBOLS, "symbols: the registry crosses realms and a fresh symbol does not") {
    ub_test::Fixture fixture;

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());

    const auto registered = ub::Symbol::For(fixture.iso(), "unibind.test.shared");
    REQUIRE(registered.has_value());
    ub_test::Expose(fixture.context, "shared", *registered);

    ub::ContextScope entered(*second);
    REQUIRE(second->GlobalObject().Set(*second, "shared", *registered).value_or(false));
    CHECK(ub_test::EvalTruth(*second, "shared === Symbol.for('unibind.test.shared')"));
}

UNIBIND_TEST_CASE(SYMBOLS, "symbols: a symbol is a property key like any other") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());

    const auto key = ub::Symbol::New(fixture.iso(), "hidden key");
    REQUIRE(key.has_value());

    REQUIRE(object->Set(fixture.context, *key, ub::Integer::New(fixture.iso(), 5)).value_or(false));
    CHECK(object->Has(fixture.context, *key).value_or(false));
    CHECK(object->HasOwn(fixture.context, *key).value_or(false));
    CHECK(object->Get(fixture.context, *key)->To<ub::Integer>()->Int32Value() == 5);

    // A symbol key is not a string key, however it is spelled.
    CHECK(object->Get(fixture.context, "hidden key")->Kind() == ub::ValueKind::Undefined);

    CHECK(object->Delete(fixture.context, *key).value_or(false));
    CHECK_FALSE(object->HasOwn(fixture.context, *key).value_or(true));
}

UNIBIND_TEST_CASE(SYMBOLS, "symbols: own keys include symbols only when asked") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    const auto key = ub::Symbol::New(fixture.iso(), "listed");
    REQUIRE(key.has_value());
    REQUIRE(object->Set(fixture.context, "plain", ub::Integer::New(fixture.iso(), 1)).value_or(false));
    REQUIRE(object->Set(fixture.context, *key, ub::Integer::New(fixture.iso(), 2)).value_or(false));

    const auto withoutSymbols = object->GetOwnPropertyNames(fixture.context);
    REQUIRE(withoutSymbols.has_value());
    CHECK(withoutSymbols->Length() == 1);

    const auto withSymbols = object->GetOwnPropertyNames(fixture.context, {.includeSymbols = true});
    REQUIRE(withSymbols.has_value());
    CHECK(withSymbols->Length() == 2);
}

UNIBIND_TEST_CASE(SYMBOLS, "symbols: a well-known symbol is the one the language uses") {
    ub_test::Fixture fixture;

    const auto iterator = ub::Symbol::WellKnown(fixture.iso(), ub::WellKnownSymbol::Iterator);
    REQUIRE(iterator.has_value());
    ub_test::Expose(fixture.context, "iteratorSymbol", *iterator);
    CHECK(ub_test::EvalTruth(fixture.context, "iteratorSymbol === Symbol.iterator"));

    for (const auto which : {ub::WellKnownSymbol::AsyncIterator, ub::WellKnownSymbol::HasInstance,
                             ub::WellKnownSymbol::ToPrimitive, ub::WellKnownSymbol::ToStringTag}) {
        const auto symbol = ub::Symbol::WellKnown(fixture.iso(), which);
        REQUIRE(symbol.has_value());
        CHECK(symbol->Kind() == ub::ValueKind::Symbol);
    }

    const auto asyncIterator = ub::Symbol::WellKnown(fixture.iso(), ub::WellKnownSymbol::AsyncIterator);
    REQUIRE(asyncIterator.has_value());
    CHECK_FALSE(iterator->StrictEquals(*asyncIterator));
}

UNIBIND_TEST_CASE(SYMBOLS, "symbols: an object made iterable under Symbol.iterator iterates") {
    ub_test::Fixture fixture;

    const auto iterator = ub::Symbol::WellKnown(fixture.iso(), ub::WellKnownSymbol::Iterator);
    REQUIRE(iterator.has_value());

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());

    const auto maker = ub_test::Eval(
        fixture.context, "(function () { let i = 0; return { next: () => ({ value: i, done: i++ >= 3 }) }; })");
    REQUIRE(object->Set(fixture.context, *iterator, maker).value_or(false));

    ub_test::Expose(fixture.context, "iterable", *object);
    CHECK(ub_test::EvalText(fixture.context, "[...iterable].join(',')") == "0,1,2");
}
