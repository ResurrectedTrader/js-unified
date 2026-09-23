/// \file
/// Objects: get, set, delete, has, own-key enumeration, prototypes and
/// property attributes.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "support/harness.h"

namespace {

/// The own keys of an object, as strings, in the order the engine reports them.
std::vector<std::string> OwnKeys(const ub::Context& context, const ub::Local<ub::Object>& object,
                                 ub::KeyFilter filter = {}) {
    auto names = object.GetOwnPropertyNames(context, filter);
    REQUIRE(names.has_value());

    std::vector<std::string> keys;
    for (std::uint32_t i = 0; i < names->Length(); ++i) {
        auto key = names->Get(context, i);
        REQUIRE(key.has_value());
        auto text = key->ToString(context);
        REQUIRE(text.has_value());
        keys.push_back(text->Utf8Value());
    }
    return keys;
}

bool Contains(const std::vector<std::string>& keys, std::string_view wanted) {
    return std::ranges::find(keys, wanted) != keys.end();
}

}  // namespace

TEST_CASE("objects: a property set from native reads back from script and back again") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    REQUIRE(object->Set(fixture.context, "answer", ub::Integer::New(fixture.iso(), 42)).value_or(false));

    ub_test::Expose(fixture.context, "probe", *object);
    CHECK(ub_test::EvalInt(fixture.context, "probe.answer") == 42);

    REQUIRE(ub::Evaluate(fixture.context, "probe.answer = 43").has_value());
    const auto readBack = object->Get(fixture.context, "answer");
    REQUIRE(readBack.has_value());
    CHECK(readBack->To<ub::Integer>()->Int32Value() == 43);
}

TEST_CASE("objects: a property can be named by string, by handle, or by index") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());

    const auto key = ub_test::Str(fixture.iso(), "byHandle");
    REQUIRE(object->Set(fixture.context, "byName", ub::Integer::New(fixture.iso(), 1)).value_or(false));
    REQUIRE(object->Set(fixture.context, key, ub::Integer::New(fixture.iso(), 2)).value_or(false));
    REQUIRE(object->Set(fixture.context, 7U, ub::Integer::New(fixture.iso(), 3)).value_or(false));

    CHECK(object->Get(fixture.context, "byName")->To<ub::Integer>()->Int32Value() == 1);
    CHECK(object->Get(fixture.context, key)->To<ub::Integer>()->Int32Value() == 2);
    CHECK(object->Get(fixture.context, 7U)->To<ub::Integer>()->Int32Value() == 3);

    // An index is a string key in JavaScript, and reading it either way agrees.
    CHECK(object->Get(fixture.context, "7")->To<ub::Integer>()->Int32Value() == 3);
}

TEST_CASE("objects: a missing property is undefined, not a failure") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());

    const auto missing = object->Get(fixture.context, "nope");
    REQUIRE(missing.has_value());
    CHECK(missing->Kind() == ub::ValueKind::Undefined);
    CHECK_FALSE(object->Has(fixture.context, ub_test::Str(fixture.iso(), "nope")).value_or(true));
}

TEST_CASE("objects: has looks up the prototype chain, hasOwn does not") {
    ub_test::Fixture fixture;

    const auto value = ub_test::Eval(fixture.context, "Object.create({ inherited: 1 }, { own: { value: 2 } })");
    const auto object = value.To<ub::Object>();
    REQUIRE(object.has_value());

    const auto inherited = ub_test::Str(fixture.iso(), "inherited");
    const auto own = ub_test::Str(fixture.iso(), "own");

    CHECK(object->Has(fixture.context, inherited).value_or(false));
    CHECK_FALSE(object->HasOwn(fixture.context, inherited).value_or(true));
    CHECK(object->Has(fixture.context, own).value_or(false));
    CHECK(object->HasOwn(fixture.context, own).value_or(false));
}

TEST_CASE("objects: delete removes an own property and says whether it could") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    const auto key = ub_test::Str(fixture.iso(), "gone");
    REQUIRE(object->Set(fixture.context, key, ub::Integer::New(fixture.iso(), 1)).value_or(false));

    CHECK(object->Delete(fixture.context, key).value_or(false));
    CHECK_FALSE(object->HasOwn(fixture.context, key).value_or(true));
    // Deleting what is not there succeeds, as in the language.
    CHECK(object->Delete(fixture.context, key).value_or(false));
}

TEST_CASE("objects: own keys are the own keys, filtered as asked") {
    ub_test::Fixture fixture;

    const auto value = ub_test::Eval(fixture.context, R"(
        (function () {
            const parent = { fromPrototype: 0 };
            const object = Object.create(parent);
            object.visible = 1;
            Object.defineProperty(object, 'hidden', { value: 2, enumerable: false });
            return object;
        })()
    )");
    const auto object = value.To<ub::Object>();
    REQUIRE(object.has_value());

    const auto enumerable = OwnKeys(fixture.context, *object);
    CHECK(Contains(enumerable, "visible"));
    CHECK_FALSE(Contains(enumerable, "hidden"));
    CHECK_FALSE(Contains(enumerable, "fromPrototype"));

    const auto all = OwnKeys(fixture.context, *object, {.includeNonEnumerable = true});
    CHECK(Contains(all, "visible"));
    CHECK(Contains(all, "hidden"));
    CHECK_FALSE(Contains(all, "fromPrototype"));
}

TEST_CASE("objects: a defined property honours its attributes") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    ub_test::Expose(fixture.context, "probe", *object);

    const auto readOnly = ub_test::Str(fixture.iso(), "readOnly");
    const auto hidden = ub_test::Str(fixture.iso(), "hidden");
    const auto permanent = ub_test::Str(fixture.iso(), "permanent");

    REQUIRE(object
                ->DefineOwnProperty(fixture.context, readOnly, ub::Integer::New(fixture.iso(), 1),
                                    ub::PropertyAttribute::ReadOnly)
                .value_or(false));
    REQUIRE(object
                ->DefineOwnProperty(fixture.context, hidden, ub::Integer::New(fixture.iso(), 2),
                                    ub::PropertyAttribute::DontEnum)
                .value_or(false));
    REQUIRE(object
                ->DefineOwnProperty(fixture.context, permanent, ub::Integer::New(fixture.iso(), 3),
                                    ub::PropertyAttribute::DontDelete)
                .value_or(false));

    // Attributes are observable from the language, which is the portable check:
    // a sloppy-mode assignment to a read-only property is silently dropped.
    CHECK(ub_test::EvalInt(fixture.context, "probe.readOnly = 99, probe.readOnly") == 1);
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "Object.keys(probe).includes('hidden')"));
    CHECK(ub_test::EvalTruth(fixture.context, "Object.getOwnPropertyNames(probe).includes('hidden')"));
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "delete probe.permanent"));
    CHECK(ub_test::EvalInt(fixture.context, "probe.permanent") == 3);
}

TEST_CASE("objects: define installs a data property, set goes through a setter") {
    ub_test::Fixture fixture;

    const auto value = ub_test::Eval(fixture.context, R"(
        (function () {
            const parent = {};
            let written = null;
            Object.defineProperty(parent, 'through', {
                set(v) { written = v; },
                get() { return written; },
                configurable: true,
            });
            const object = Object.create(parent);
            object.reportWritten = () => written;
            return object;
        })()
    )");
    const auto object = value.To<ub::Object>();
    REQUIRE(object.has_value());
    ub_test::Expose(fixture.context, "probe", *object);

    const auto key = ub_test::Str(fixture.iso(), "through");
    REQUIRE(object->Set(fixture.context, key, ub::Integer::New(fixture.iso(), 5)).value_or(false));
    CHECK(ub_test::EvalInt(fixture.context, "probe.reportWritten()") == 5);
    CHECK_FALSE(object->HasOwn(fixture.context, key).value_or(true));

    // Define ignores the inherited setter and makes an own property.
    REQUIRE(object->DefineOwnProperty(fixture.context, key, ub::Integer::New(fixture.iso(), 6)).value_or(false));
    CHECK(object->HasOwn(fixture.context, key).value_or(false));
    CHECK(ub_test::EvalInt(fixture.context, "probe.reportWritten()") == 5);
}

UNIBIND_TEST_CASE(PROPERTY_ATTRIBUTES, "objects: property attributes can be read back") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());

    const auto plain = ub_test::Str(fixture.iso(), "plain");
    const auto locked = ub_test::Str(fixture.iso(), "locked");
    REQUIRE(object->Set(fixture.context, plain, ub::Integer::New(fixture.iso(), 1)).value_or(false));
    REQUIRE(object
                ->DefineOwnProperty(fixture.context, locked, ub::Integer::New(fixture.iso(), 2),
                                    ub::PropertyAttribute::ReadOnly | ub::PropertyAttribute::DontDelete)
                .value_or(false));

    const auto plainAttributes = object->GetPropertyAttributes(fixture.context, plain);
    REQUIRE(plainAttributes.has_value());
    CHECK(*plainAttributes == ub::PropertyAttribute::None);

    const auto lockedAttributes = object->GetPropertyAttributes(fixture.context, locked);
    REQUIRE(lockedAttributes.has_value());
    CHECK(ub::HasAttribute(*lockedAttributes, ub::PropertyAttribute::ReadOnly));
    CHECK(ub::HasAttribute(*lockedAttributes, ub::PropertyAttribute::DontDelete));
    CHECK_FALSE(ub::HasAttribute(*lockedAttributes, ub::PropertyAttribute::DontEnum));
}

UNIBIND_TEST_CASE(PROPERTY_ATTRIBUTES, "objects: an absent property has no attributes rather than none") {
    ub_test::Fixture fixture;

    auto object = ub::Object::New(fixture.context);
    REQUIRE(object.has_value());
    REQUIRE(object->Set(fixture.context, "here", ub::Integer::New(fixture.iso(), 1)).value_or(false));

    // `None` is what an ordinary writable, enumerable, configurable property
    // reports, so an absent property must answer with no value at all - or the
    // two are indistinguishable.
    CHECK_FALSE(object->GetPropertyAttributes(fixture.context, ub_test::Str(fixture.iso(), "absent")).has_value());
    CHECK(object->GetPropertyAttributes(fixture.context, ub_test::Str(fixture.iso(), "here")).has_value());
}

UNIBIND_TEST_CASE(PROPERTY_ATTRIBUTES, "objects: an absent property and a lookup that threw answer differently") {
    // Both answer empty - that is the failure convention (unibind/types.h) - and
    // the *cause* is the second question, asked of the isolate. An absent
    // property is not a failure and leaves nothing pending; a lookup that
    // threw leaves the exception, so a caller that cannot tell them apart from
    // the return value can always tell them apart from the handler.
    ub_test::Fixture fixture;

    const auto name = ub_test::Str(fixture.iso(), "wanted");

    auto plain = ub::Object::New(fixture.context);
    REQUIRE(plain.has_value());
    {
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(plain->GetPropertyAttributes(fixture.context, name).has_value());
        CHECK_FALSE(handler.HasCaught());
        CHECK_FALSE(fixture.iso().HasPendingException());
    }

    // A proxy whose lookup throws. It traps `has` and nothing else, which is
    // the shape that tells whether the backends ask the same question.
    const auto thrower = ub_test::Eval(fixture.context, "new Proxy({}, { has() { throw new TypeError('no'); } })");
    const auto asObject = thrower.To<ub::Object>();
    REQUIRE(asObject.has_value());
    {
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(asObject->GetPropertyAttributes(fixture.context, name).has_value());
        CHECK(handler.HasCaught());
        CHECK(handler.Message(fixture.context).value_or("").find("no") != std::string::npos);
    }

    CHECK_FALSE(fixture.iso().HasPendingException());
    CHECK(ub_test::EvalInt(fixture.context, "2 + 2") == 4);
}

TEST_CASE("objects: a prototype can be read and replaced") {
    ub_test::Fixture fixture;

    auto parent = ub::Object::New(fixture.context);
    REQUIRE(parent.has_value());
    REQUIRE(parent->Set(fixture.context, "fromParent", ub::Integer::New(fixture.iso(), 11)).value_or(false));

    auto child = ub::Object::New(fixture.context);
    REQUIRE(child.has_value());
    REQUIRE(child->SetPrototype(fixture.context, *parent).value_or(false));

    const auto prototype = child->GetPrototype(fixture.context);
    REQUIRE(prototype.has_value());
    CHECK(prototype->StrictEquals(*parent));
    CHECK(child->Get(fixture.context, "fromParent")->To<ub::Integer>()->Int32Value() == 11);

    REQUIRE(child->SetPrototype(fixture.context, ub::Null(fixture.iso())).value_or(false));
    const auto none = child->GetPrototype(fixture.context);
    REQUIRE(none.has_value());
    CHECK(none->Kind() == ub::ValueKind::Null);
}

TEST_CASE("objects: an array grows by index and reports its length") {
    ub_test::Fixture fixture;

    auto array = ub::Array::New(fixture.context, 3);
    REQUIRE(array.has_value());
    CHECK(array->Length() == 3);

    for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE(array->Set(fixture.context, i, ub::Integer::New(fixture.iso(), static_cast<std::int32_t>(i * i)))
                    .value_or(false));
    }
    REQUIRE(array->Set(fixture.context, 9U, ub::Integer::New(fixture.iso(), 81)).value_or(false));
    CHECK(array->Length() == 10);

    CHECK(array->Get(fixture.context, 2U)->To<ub::Integer>()->Int32Value() == 4);
    // A hole reads as undefined, exactly as it does from script.
    CHECK(array->Get(fixture.context, 5U)->Kind() == ub::ValueKind::Undefined);

    ub_test::Expose(fixture.context, "probe", *array);
    CHECK(ub_test::EvalTruth(fixture.context, "Array.isArray(probe)"));
    CHECK(ub_test::EvalInt(fixture.context, "probe.length") == 10);
}

TEST_CASE("objects: an empty array is empty") {
    ub_test::Fixture fixture;

    const auto array = ub::Array::New(fixture.context);
    REQUIRE(array.has_value());
    CHECK(array->Length() == 0);
    CHECK(array->Kind() == ub::ValueKind::Array);
}

TEST_CASE("objects: an array too long to make is refused rather than made short") {
    // The length is a `uint32_t` because that is what a JavaScript array's
    // length is, and one engine's own factory takes an `int` and reads a
    // negative one as *zero*. A length that cannot be made has to come back
    // empty: an array of length 0 reporting success is the failure that looks
    // like a result.
    ub_test::Fixture fixture;

    const auto huge = ub::Array::New(fixture.context, 3000000000U);
    CHECK_FALSE(huge.has_value());

    // The boundary itself still works, so the refusal is about what cannot be
    // made rather than about large arrays.
    const auto large = ub::Array::New(fixture.context, 1000U);
    REQUIRE(large.has_value());
    CHECK(large->Length() == 1000U);
}

TEST_CASE("objects: a property whose getter throws reports no value") {
    ub_test::Fixture fixture;

    const auto value = ub_test::Eval(fixture.context, "({ get boom() { throw new Error('bang'); } })");
    const auto object = value.To<ub::Object>();
    REQUIRE(object.has_value());

    ub::TryCatch tryCatch(fixture.iso());
    const auto read = object->Get(fixture.context, "boom");
    CHECK_FALSE(read.has_value());
    REQUIRE(tryCatch.HasCaught());
    CHECK(tryCatch.Message(fixture.context).value_or("").find("bang") != std::string::npos);
}
