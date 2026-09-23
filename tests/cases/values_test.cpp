/// \file
/// Values: what a value is, what it narrows to, what it equals, and what it
/// converts to.

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "support/harness.h"

namespace {

struct Payload {
    int value = 0;
};

struct Other {
    int value = 0;
};

}  // namespace

TEST_CASE("values: the backend identifies itself") {
    CHECK(ub::Platform::IsInitialized());
    CHECK_FALSE(ub::Platform::BackendName().empty());
    // The version string's *shape* is the engine's and is deliberately not
    // promised, so the only portable assertion is that there is one. What it
    // says is reported instead - and it is worth reporting, because "which
    // build of the engine" is the question a cache blob is keyed on.
    CHECK_FALSE(ub::Platform::BackendVersion().empty());
    MESSAGE("backend ", ub::Platform::BackendName(), " ", ub::Platform::BackendVersion(), "; provides ",
            UNIBIND_TEST_CAPABILITIES_PRESENT, "; lacks ", UNIBIND_TEST_CAPABILITIES_ABSENT);
}

TEST_CASE("values: every kind reports itself") {
    ub_test::Fixture fixture;

    CHECK(ub::Undefined(fixture.iso()).Kind() == ub::ValueKind::Undefined);
    CHECK(ub::Null(fixture.iso()).Kind() == ub::ValueKind::Null);
    CHECK(ub::True(fixture.iso()).Kind() == ub::ValueKind::Boolean);
    CHECK(ub::Number::New(fixture.iso(), 1.5).Kind() == ub::ValueKind::Number);
    CHECK(ub::Integer::New(fixture.iso(), 7).Kind() == ub::ValueKind::Number);
    CHECK(ub_test::Str(fixture.iso(), "text").Kind() == ub::ValueKind::String);
    CHECK(ub_test::Eval(fixture.context, "({})").Kind() == ub::ValueKind::Object);
    CHECK(ub_test::Eval(fixture.context, "[1,2]").Kind() == ub::ValueKind::Array);
    CHECK(ub_test::Eval(fixture.context, "(function () {})").Kind() == ub::ValueKind::Function);
}

TEST_CASE("values: an exotic object is an Object, because that is what you can do with it") {
    ub_test::Fixture fixture;

    // There is no exotic kind. A kind answers what you can do with a value, and
    // `Object` is exactly the operation set this API offers for all of these.
    //
    // The Proxy is what the decision turns on: an engine with no interceptor of
    // its own builds ours out of proxies, so a kind that singled proxies out
    // would make a sandbox object report differently on two backends purely
    // because of how the interceptor was implemented. See docs/status.md
    // decision 8.
    for (const char* source :
         {"new Date(0)", "/x/", "new Proxy({}, {})", "new Uint8Array(4)", "new Map()", "(function* () {})()"}) {
        CAPTURE(source);
        const auto value = ub_test::Eval(fixture.context, source);
        CHECK(value.Kind() == ub::ValueKind::Object);
        CHECK(value.Is<ub::Object>());
        CHECK_FALSE(value.Is<ub::Primitive>());

        const auto asObject = value.To<ub::Object>();
        REQUIRE(asObject.has_value());
        CHECK(asObject->Set(fixture.context, "stuck_on", ub::Integer::New(fixture.iso(), 1)).value_or(false));
        CHECK(asObject->Get(fixture.context, "stuck_on")->To<ub::Integer>()->Int32Value() == 1);
    }

    // An interceptor-backed object is the case the decision exists for, and it
    // reports the same kind as any other object.
    CHECK(fixture.context.GlobalObject().Kind() == ub::ValueKind::Object);
}

TEST_CASE("values: asking what a revoked proxy is answers, and leaves nothing pending") {
    // The only value in the language that *throws* when asked what it is: an
    // engine answering `Array.isArray` on a revoked proxy raises a TypeError
    // rather than saying yes or no. `Kind()` and `Is<T>()` are `noexcept`
    // queries with nowhere to report that, so the throw must not be left on
    // the isolate for whatever the caller does next to trip over - it would
    // arrive attributed to an operation that never threw.
    ub_test::Fixture fixture;

    const auto revoked =
        ub_test::Eval(fixture.context, "(() => { const r = Proxy.revocable([], {}); r.revoke(); return r.proxy; })()");

    CHECK(revoked.Kind() == ub::ValueKind::Object);
    CHECK_FALSE(fixture.iso().HasPendingException());
    CHECK_FALSE(revoked.Is<ub::Array>());
    CHECK_FALSE(fixture.iso().HasPendingException());

    // And the next ordinary operation is unaffected, which is the part a
    // leaked exception would break.
    CHECK(ub::Object::New(fixture.context).has_value());
    CHECK_FALSE(fixture.iso().HasPendingException());
}

TEST_CASE("values: narrowing is checked and widening is implicit") {
    ub_test::Fixture fixture;

    const auto number = ub::Number::New(fixture.iso(), 3.5);
    const ub::Local<ub::Value> widened = number;  // implicit, no cast

    CHECK(widened.Is<ub::Number>());
    CHECK(widened.Is<ub::Primitive>());
    CHECK_FALSE(widened.Is<ub::String>());
    CHECK_FALSE(widened.Is<ub::Object>());
    CHECK_FALSE(widened.To<ub::String>().has_value());

    // An Integer is a Number that happens to be an exact int32.
    CHECK_FALSE(widened.To<ub::Integer>().has_value());
    CHECK(ub::Number::New(fixture.iso(), 4.0).Is<ub::Integer>());
    CHECK_FALSE(ub::Number::New(fixture.iso(), 4.5).Is<ub::Integer>());

    const auto asNumber = widened.To<ub::Number>();
    REQUIRE(asNumber.has_value());
    CHECK(asNumber->NumberValue() == doctest::Approx(3.5));
}

TEST_CASE("values: a BigInt is a kind of its own, even with no way to make one") {
    ub_test::Fixture fixture;

    // Nothing in the API makes a BigInt, but script does, and a handle to one
    // has to answer for itself - otherwise the tag and the TypeCode are a
    // promise the suite never checks.
    const auto value = ub_test::Eval(fixture.context, "9007199254740993n");
    CHECK(value.Kind() == ub::ValueKind::BigInt);
    CHECK(value.Is<ub::BigInt>());
    CHECK(value.Is<ub::Primitive>());
    CHECK_FALSE(value.Is<ub::Number>());
    CHECK_FALSE(value.Is<ub::Object>());
    CHECK(value.To<ub::BigInt>().has_value());
    CHECK_FALSE(value.To<ub::Number>().has_value());

    const auto asText = value.ToString(fixture.context);
    REQUIRE(asText.has_value());
    CHECK(asText->Utf8Value() == "9007199254740993");
}

TEST_CASE("values: a Name is a String or a Symbol and nothing else") {
    ub_test::Fixture fixture;

    const ub::Local<ub::Value> text = ub_test::Str(fixture.iso(), "key");
    CHECK(text.Is<ub::Name>());
    CHECK(text.Is<ub::String>());
    CHECK_FALSE(text.Is<ub::Symbol>());

    CHECK_FALSE(ub::Integer::New(fixture.iso(), 1).Is<ub::Name>());
    CHECK_FALSE(ub_test::Eval(fixture.context, "({})").Is<ub::Name>());
}

TEST_CASE("values: a function narrows to an object and back") {
    ub_test::Fixture fixture;

    const auto value = ub_test::Eval(fixture.context, "(function named(a, b) { return a + b; })");
    CHECK(value.Is<ub::Function>());
    CHECK(value.Is<ub::Object>());

    const auto asObject = value.To<ub::Object>();
    REQUIRE(asObject.has_value());
    const auto backToFunction = asObject->To<ub::Function>();
    REQUIRE(backToFunction.has_value());

    const ub::Local<ub::Object> widened = *backToFunction;  // implicit
    CHECK(widened.StrictEquals(value));
}

TEST_CASE("values: an array narrows to an object and knows its length") {
    ub_test::Fixture fixture;

    const auto value = ub_test::Eval(fixture.context, "[10, 20, 30]");
    const auto asArray = value.To<ub::Array>();
    REQUIRE(asArray.has_value());
    CHECK(asArray->Length() == 3);
    CHECK(asArray->Is<ub::Object>());

    const auto second = asArray->Get(fixture.context, 1U);
    REQUIRE(second.has_value());
    CHECK(second->To<ub::Integer>()->Int32Value() == 20);
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

TEST_CASE("values: strict equality is JavaScript's, not the slot's") {
    ub_test::Fixture fixture;

    const auto one = ub::Integer::New(fixture.iso(), 1);
    const auto alsoOne = ub::Integer::New(fixture.iso(), 1);
    CHECK(one.StrictEquals(alsoOne));  // two slots, one value

    const auto object = ub_test::Eval(fixture.context, "({})");
    const auto other = ub_test::Eval(fixture.context, "({})");
    CHECK_FALSE(object.StrictEquals(other));

    const auto nan = ub_test::Eval(fixture.context, "NaN");
    CHECK_FALSE(nan.StrictEquals(nan));
    CHECK(nan.SameValue(nan));

    const auto zero = ub_test::Eval(fixture.context, "0");
    const auto minusZero = ub_test::Eval(fixture.context, "-0");
    CHECK(zero.StrictEquals(minusZero));
    CHECK_FALSE(zero.SameValue(minusZero));
}

TEST_CASE("values: loose equality runs the language's coercions") {
    ub_test::Fixture fixture;

    const auto one = ub::Integer::New(fixture.iso(), 1);
    const auto oneText = ub_test::Str(fixture.iso(), "1");
    CHECK(one.Equals(fixture.context, oneText).value_or(false));
    CHECK_FALSE(one.StrictEquals(oneText));

    CHECK(ub::Null(fixture.iso()).Equals(fixture.context, ub::Undefined(fixture.iso())).value_or(false));
    CHECK_FALSE(ub::Null(fixture.iso()).StrictEquals(ub::Undefined(fixture.iso())));
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

TEST_CASE("values: the coercions agree with the language") {
    ub_test::Fixture fixture;

    const auto text = ub_test::Str(fixture.iso(), "42");
    CHECK(text.ToNumber(fixture.context).value_or(0.0) == doctest::Approx(42.0));
    CHECK(text.ToInt32(fixture.context).value_or(0) == 42);
    CHECK(text.ToBoolean(fixture.context).value_or(false));

    CHECK(ub_test::Str(fixture.iso(), "").ToBoolean(fixture.context).value_or(true) == false);
    CHECK(ub::Number::New(fixture.iso(), 3.9).ToInt32(fixture.context).value_or(0) == 3);
    CHECK(ub::Number::New(fixture.iso(), -3.9).ToInt32(fixture.context).value_or(0) == -3);
    CHECK(ub::Integer::New(fixture.iso(), -1).ToUint32(fixture.context).value_or(0) == 4294967295U);

    const auto object = ub_test::Eval(fixture.context, "({})");
    const auto asText = object.ToString(fixture.context);
    REQUIRE(asText.has_value());
    CHECK(asText->Utf8Value() == "[object Object]");

    const auto boxed = ub::Integer::New(fixture.iso(), 5).ToObject(fixture.context);
    REQUIRE(boxed.has_value());
    CHECK(boxed->Is<ub::Object>());
    CHECK(boxed->ToNumber(fixture.context).value_or(0.0) == doctest::Approx(5.0));
}

TEST_CASE("values: a coercion that throws reports no value") {
    ub_test::Fixture fixture;

    const auto hostile = ub_test::Eval(fixture.context, "({ valueOf() { throw new Error('no'); } })");

    ub::TryCatch tryCatch(fixture.iso());
    const auto asNumber = hostile.ToNumber(fixture.context);
    CHECK_FALSE(asNumber.has_value());
    REQUIRE(tryCatch.HasCaught());

    const auto message = tryCatch.Message(fixture.context);
    REQUIRE(message.has_value());
    CHECK(message->find("no") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

TEST_CASE("values: a string round-trips through UTF-8") {
    ub_test::Fixture fixture;

    // Spelled as bytes so the test does not depend on the compiler's idea of
    // the source encoding: "ab" + U+20AC + U+1F600.
    const std::string original = "ab\xE2\x82\xAC\xF0\x9F\x98\x80";
    const auto text = ub_test::Str(fixture.iso(), original);

    CHECK(text.Utf8Length() == original.size());
    CHECK(text.Utf8Value() == original);

    std::array<char, 64> buffer{};
    const std::size_t written = text.WriteUtf8(buffer);
    CHECK(written == original.size());
    CHECK(std::string(buffer.data(), written) == original);
}

TEST_CASE("values: writing a string into a buffer that is too small stops at a code point") {
    ub_test::Fixture fixture;

    const std::string original = "a\xE2\x82\xAC";  // 1 + 3 bytes
    const auto text = ub_test::Str(fixture.iso(), original);
    REQUIRE(text.Utf8Length() == 4);

    // Three bytes cannot hold the whole thing and must not hold half of the
    // euro sign; the only valid answer is the "a".
    std::array<char, 3> tight{};
    const std::size_t written = text.WriteUtf8(tight);
    CHECK(written <= tight.size());
    CHECK(original.compare(0, written, tight.data(), written) == 0);
    CHECK(written != 2);  // that would be a split code point
}

TEST_CASE("values: bytes that are not UTF-8 make no string") {
    // `String::New` says "empty if the bytes are not valid UTF-8", and the
    // alternative an engine offers - substituting U+FFFD and reporting success
    // - is the failure this API exists to prevent: the caller is told it
    // handed over text when what arrived was a row of replacement characters.
    ub_test::Fixture fixture;

    const std::vector<std::string> bad = {
        std::string("a\xFF"),                  // not a lead byte at all
        std::string("a\xE2\x82"),              // a three-byte sequence cut short
        std::string("a\xED\xA0\x80"),          // a lone surrogate, encoded
        std::string("a\xC0\xAF"),              // an overlong '/'
        std::string("a\xF8\x88\x80\x80\x80"),  // five bytes, which UTF-8 has never had
        std::string("a\xF4\x90\x80\x80"),      // beyond U+10FFFF
    };
    for (const std::string& bytes : bad) {
        CAPTURE(bytes.size());
        CHECK_FALSE(ub::String::New(fixture.iso(), bytes).has_value());
    }

    // And nothing valid is refused, including the awkward valid ones.
    for (const std::string& good :
         {std::string("a\xE2\x82\xAC"), std::string("a\xF0\x9F\x98\x80"), std::string("a\0b", 3), std::string("")}) {
        CHECK(ub::String::New(fixture.iso(), good).has_value());
    }
}

TEST_CASE("values: a string script made out of half a surrogate pair still comes out as UTF-8") {
    // JavaScript strings are UTF-16 and script can make one that is not
    // encodable - a lone surrogate. `Utf8Value` promises UTF-8, so the only
    // answers available are the replacement character or a failure; what it
    // must not do is hand back a byte sequence that is not UTF-8 at all, which
    // is what the naive encoding of a surrogate (ED A0 80) is.
    ub_test::Fixture fixture;

    const auto value = ub_test::Eval(fixture.context, "String.fromCharCode(0xD800)");
    const auto text = value.To<ub::String>();
    REQUIRE(text.has_value());

    const std::string bytes = text->Utf8Value();
    CHECK(bytes == "\xEF\xBF\xBD");  // U+FFFD
    CHECK(text->Utf8Length() == bytes.size());

    // And what comes out goes back in, which is the point of it being UTF-8.
    CHECK(ub::String::New(fixture.iso(), bytes).has_value());
}

TEST_CASE("values: an empty string is a string") {
    ub_test::Fixture fixture;

    const auto empty = ub_test::Str(fixture.iso(), "");
    CHECK(empty.Kind() == ub::ValueKind::String);
    CHECK(empty.Utf8Length() == 0);
    CHECK(empty.Utf8Value().empty());
    CHECK_FALSE(empty.ToBoolean(fixture.context).value_or(true));
}

TEST_CASE("values: a string with an embedded NUL keeps its length") {
    ub_test::Fixture fixture;

    const std::string original("a\0b", 3);
    const auto text = ub_test::Str(fixture.iso(), original);
    CHECK(text.Utf8Length() == 3);
    CHECK(text.Utf8Value() == original);
}

// ---------------------------------------------------------------------------
// Externals
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(EXTERNALS, "values: an external carries a typed pointer and will not be mistyped") {
    ub_test::Fixture fixture;

    Payload payload{.value = 17};
    const auto external = ub::External::New(fixture.iso(), payload);
    REQUIRE(external.has_value());
    CHECK(external->Kind() == ub::ValueKind::External);

    auto* recovered = ub::ExternalValue<Payload>(*external);
    REQUIRE(recovered != nullptr);
    CHECK(recovered->value == 17);
    CHECK(recovered == &payload);

    // Recovery is checked: the wrong type yields nothing, never a bad cast.
    CHECK(ub::ExternalValue<Other>(*external) == nullptr);
}

UNIBIND_TEST_CASE(EXTERNALS, "values: an external survives a round trip through script") {
    ub_test::Fixture fixture;

    Payload payload{.value = 3};
    const auto external = ub::External::New(fixture.iso(), payload);
    REQUIRE(external.has_value());

    ub_test::Expose(fixture.context, "opaque", *external);

    const auto roundTrip = ub_test::Eval(fixture.context, "[opaque][0]");
    CHECK(roundTrip.StrictEquals(*external));
    REQUIRE(roundTrip.Is<ub::External>());
    const auto asExternal = roundTrip.To<ub::External>();
    REQUIRE(asExternal.has_value());
    CHECK(ub::ExternalValue<Payload>(*asExternal) == &payload);
}
