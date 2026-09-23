/// \file
/// Interceptors: a catch-all handler for every named or indexed property on an
/// object. This is what a sandbox or a
/// proxy-like scope object needs.

#include <cstdint>
#include <map>
#include <string>

#include "support/harness.h"

namespace {

/// The thing behind the interceptor: a plain C++ map that script sees as if it
/// were the object's own properties.
struct Store {
    std::map<std::string, std::int32_t, std::less<>> named;
    std::map<std::uint32_t, std::int32_t> indexed;
    int getterCalls = 0;
    int setterCalls = 0;
};

Store* StoreOf(const ub::PropertyCallbackInfo& info) {
    return info.Data<Store>();
}

/// A named key this interceptor is willing to answer for, or nothing.
std::optional<std::string> KeyOf(const ub::Local<ub::Name>& property) {
    const auto asString = property.To<ub::String>();
    if (!asString) {
        return std::nullopt;  // a symbol key: not ours
    }
    return asString->Utf8Value();
}

/// Throws for one key and declines for everything, including that one - the
/// contradiction `unibind/template.h` settles: the throw wins, so the ordinary
/// property underneath must not answer.
ub::Intercepted ThrowsAndDeclines(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const auto key = KeyOf(property);
    if (key && *key == "boom") {
        info.ThrowTypeError("the hook threw");
    }
    return ub::Intercepted::No;
}

std::optional<bool> DeleterThrowsAndDeclines(const ub::Local<ub::Name>& property,
                                             const ub::PropertyCallbackInfo& info) {
    const auto key = KeyOf(property);
    if (key && *key == "boom") {
        info.ThrowTypeError("the deleter threw");
    }
    return std::nullopt;
}

ub::Intercepted GetNamed(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    const auto key = KeyOf(property);
    if (store == nullptr || !key) {
        return ub::Intercepted::No;
    }
    ++store->getterCalls;
    const auto found = store->named.find(*key);
    if (found == store->named.end()) {
        return ub::Intercepted::No;  // carry on with the ordinary lookup
    }
    info.GetReturnValue().Set(found->second);
    return ub::Intercepted::Yes;
}

ub::Intercepted SetNamed(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& value,
                         const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    const auto key = KeyOf(property);
    if (store == nullptr || !key || key->rfind("own_", 0) == 0) {
        return ub::Intercepted::No;
    }
    const auto asInt = value.ToInt32(info.GetContext());
    if (!asInt) {
        return ub::Intercepted::Yes;  // the coercion threw
    }
    ++store->setterCalls;
    store->named[*key] = *asInt;
    return ub::Intercepted::Yes;
}

std::optional<ub::PropertyAttribute> QueryNamed(const ub::Local<ub::Name>& property,
                                                const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    const auto key = KeyOf(property);
    if (store == nullptr || !key || !store->named.contains(*key)) {
        return std::nullopt;
    }
    return ub::PropertyAttribute::None;
}

std::optional<bool> DeleteNamed(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    const auto key = KeyOf(property);
    if (store == nullptr || !key || !store->named.contains(*key)) {
        return std::nullopt;
    }
    store->named.erase(*key);
    return true;
}

std::optional<ub::Local<ub::Array>> EnumerateNamed(const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    if (store == nullptr) {
        return std::nullopt;
    }
    auto keys = ub::Array::New(info.GetContext(), 0);
    if (!keys) {
        return std::nullopt;
    }
    std::uint32_t at = 0;
    for (const auto& entry : store->named) {
        auto name = ub::String::New(info.GetIsolate(), entry.first);
        if (!name || !keys->Set(info.GetContext(), at, *name).value_or(false)) {
            return std::nullopt;
        }
        ++at;
    }
    return keys;
}

ub::Intercepted GetIndexed(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    if (store == nullptr) {
        return ub::Intercepted::No;
    }
    const auto found = store->indexed.find(index);
    if (found == store->indexed.end()) {
        return ub::Intercepted::No;
    }
    info.GetReturnValue().Set(found->second);
    return ub::Intercepted::Yes;
}

ub::Intercepted SetIndexed(std::uint32_t index, const ub::Local<ub::Value>& value,
                           const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    if (store == nullptr) {
        return ub::Intercepted::No;
    }
    const auto asInt = value.ToInt32(info.GetContext());
    if (!asInt) {
        return ub::Intercepted::Yes;
    }
    store->indexed[index] = *asInt;
    return ub::Intercepted::Yes;
}

/// A setter that also writes to its return slot. Nothing reads that slot - the
/// hook's answer is its C++ return value - so this must change nothing, and in
/// particular must not change what the assignment expression evaluates to.
ub::Intercepted SetNamedAndScribble(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& value,
                                    const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(std::int32_t{999});
    return SetNamed(property, value, info);
}

/// An enumerator with nothing to report. That is "no own keys", not "declined".
std::optional<ub::Local<ub::Array>> EnumerateNothing(const ub::PropertyCallbackInfo& info) {
    return ub::Array::New(info.GetContext(), 0);
}

std::optional<bool> DeleteIndexed(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    if (store == nullptr || !store->indexed.contains(index)) {
        return std::nullopt;
    }
    store->indexed.erase(index);
    return true;
}

std::optional<ub::PropertyAttribute> QueryIndexed(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    if (store == nullptr || !store->indexed.contains(index)) {
        return std::nullopt;
    }
    return ub::PropertyAttribute::None;
}

std::optional<ub::Local<ub::Array>> EnumerateIndexed(const ub::PropertyCallbackInfo& info) {
    Store* store = StoreOf(info);
    if (store == nullptr) {
        return std::nullopt;
    }
    auto keys = ub::Array::New(info.GetContext(), 0);
    if (!keys) {
        return std::nullopt;
    }
    std::uint32_t at = 0;
    for (const auto& entry : store->indexed) {
        if (!keys->Set(info.GetContext(), at, ub::Integer::NewFromUnsigned(info.GetIsolate(), entry.first))
                 .value_or(false)) {
            return std::nullopt;
        }
        ++at;
    }
    return keys;
}

/// Answers `holder` and `receiver` with the objects the hook was told about.
ub::Intercepted GetHolderOrThis(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const auto key = KeyOf(property);
    if (!key) {
        return ub::Intercepted::No;
    }
    if (*key == "holder") {
        info.GetReturnValue().Set(info.Holder());
        return ub::Intercepted::Yes;
    }
    if (*key == "receiver") {
        info.GetReturnValue().Set(info.This());
        return ub::Intercepted::Yes;
    }
    return ub::Intercepted::No;
}

}  // namespace

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: a named handler answers for properties that are not there") {
    ub_test::Fixture fixture;

    Store store;
    store.named["fromTheStore"] = 11;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(
        ub::NamedPropertyHandler{.getter = &GetNamed, .setter = &SetNamed, .data = ub::CallbackData::For(store)});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "sandbox", *instance);

    CHECK(ub_test::EvalInt(fixture.context, "sandbox.fromTheStore") == 11);
    CHECK(store.getterCalls > 0);

    REQUIRE(ub::Evaluate(fixture.context, "sandbox.written = 22").has_value());
    CHECK(store.setterCalls == 1);
    CHECK(store.named["written"] == 22);
    CHECK(ub_test::EvalInt(fixture.context, "sandbox.written") == 22);
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: declining lets the ordinary lookup carry on") {
    ub_test::Fixture fixture;

    Store store;
    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("declared", ub::Constant(std::int32_t{5}));
    shape.SetHandler(
        ub::NamedPropertyHandler{.getter = &GetNamed, .setter = &SetNamed, .data = ub::CallbackData::For(store)});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "sandbox", *instance);

    // The store has no such key, so the interceptor declines and the template's
    // own property answers.
    CHECK(ub_test::EvalInt(fixture.context, "sandbox.declared") == 5);

    // The setter declines for anything named own_*, so the write lands on the
    // object itself rather than in the store.
    REQUIRE(ub::Evaluate(fixture.context, "sandbox.own_thing = 9").has_value());
    CHECK(store.named.find("own_thing") == store.named.end());
    CHECK(ub_test::EvalInt(fixture.context, "sandbox.own_thing") == 9);
    CHECK(ub_test::EvalTruth(fixture.context, "Object.hasOwn(sandbox, 'own_thing')"));

    // A missing property is still undefined, not an error.
    CHECK(ub_test::Eval(fixture.context, "sandbox.neverHeardOf").Kind() == ub::ValueKind::Undefined);
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: query, delete and enumerate make the store look like own properties") {
    ub_test::Fixture fixture;

    Store store;
    store.named["alpha"] = 1;
    store.named["beta"] = 2;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetNamed,
                                              .setter = &SetNamed,
                                              .query = &QueryNamed,
                                              .deleter = &DeleteNamed,
                                              .enumerator = &EnumerateNamed,
                                              .data = ub::CallbackData::For(store)});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "sandbox", *instance);

    CHECK(ub_test::EvalTruth(fixture.context, "'alpha' in sandbox"));
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "'gamma' in sandbox"));
    CHECK(ub_test::EvalText(fixture.context, "Object.keys(sandbox).sort().join(',')") == "alpha,beta");

    CHECK(ub_test::EvalTruth(fixture.context, "delete sandbox.alpha"));
    CHECK(store.named.find("alpha") == store.named.end());
    CHECK(ub_test::EvalText(fixture.context, "Object.keys(sandbox).join(',')") == "beta");

    // The same questions asked through the native API agree.
    CHECK(instance->Has(fixture.context, ub_test::Str(fixture.iso(), "beta")).value_or(false));
    const auto names = instance->GetOwnPropertyNames(fixture.context);
    REQUIRE(names.has_value());
    CHECK(names->Length() == 1);
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: an indexed handler answers for element access") {
    ub_test::Fixture fixture;

    Store store;
    store.indexed[3] = 30;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::IndexedPropertyHandler{
        .getter = &GetIndexed, .setter = &SetIndexed, .deleter = &DeleteIndexed, .data = ub::CallbackData::For(store)});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "slots", *instance);

    CHECK(ub_test::EvalInt(fixture.context, "slots[3]") == 30);
    CHECK(ub_test::Eval(fixture.context, "slots[4]").Kind() == ub::ValueKind::Undefined);

    REQUIRE(ub::Evaluate(fixture.context, "slots[7] = 70").has_value());
    CHECK(store.indexed[7] == 70);
    CHECK(ub_test::EvalInt(fixture.context, "slots[7]") == 70);

    CHECK(ub_test::EvalTruth(fixture.context, "delete slots[7]"));
    CHECK(store.indexed.find(7) == store.indexed.end());

    // Reading an index through the native API takes the same path.
    const auto read = instance->Get(fixture.context, 3U);
    REQUIRE(read.has_value());
    CHECK(read->To<ub::Integer>()->Int32Value() == 30);
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: a getter on its own is a whole handler") {
    ub_test::Fixture fixture;

    Store store;
    store.named["only"] = 1;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetNamed, .data = ub::CallbackData::For(store)});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "sandbox", *instance);

    CHECK(ub_test::EvalInt(fixture.context, "sandbox.only") == 1);
    // With no setter declared, a write is an ordinary write to the object.
    REQUIRE(ub::Evaluate(fixture.context, "sandbox.other = 2").has_value());
    CHECK(ub_test::EvalTruth(fixture.context, "Object.hasOwn(sandbox, 'other')"));
    CHECK(store.named.find("other") == store.named.end());
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: a hook that writes to its return slot changes nothing") {
    ub_test::Fixture fixture;

    Store store;
    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::NamedPropertyHandler{
        .getter = &GetNamed, .setter = &SetNamedAndScribble, .data = ub::CallbackData::For(store)});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "sandbox", *instance);

    // The setter's answer is its C++ return value; what it wrote through
    // GetReturnValue() is discarded, so the assignment still evaluates to the
    // value that was assigned. See unibind/function.h.
    CHECK(ub_test::EvalInt(fixture.context, "sandbox.written = 7") == 7);
    CHECK(store.named["written"] == 7);
    CHECK(ub_test::EvalInt(fixture.context, "sandbox.written") == 7);
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: an enumerator with nothing to say means no own keys") {
    ub_test::Fixture fixture;

    Store store;
    store.named["notListed"] = 1;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetNamed,
                                              .query = &QueryNamed,
                                              .enumerator = &EnumerateNothing,
                                              .data = ub::CallbackData::For(store)});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "sandbox", *instance);

    // An empty answer is "this object claims no own keys" rather than "carry on
    // without me" - the engines' enumerators do not all have a decline path.
    CHECK(ub_test::EvalInt(fixture.context, "Object.keys(sandbox).length") == 0);
    // Which says nothing about lookup: the property is still there.
    CHECK(ub_test::EvalInt(fixture.context, "sandbox.notListed") == 1);
    CHECK(ub_test::EvalTruth(fixture.context, "'notListed' in sandbox"));
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: a query answers getOwnPropertyDescriptor") {
    ub_test::Fixture fixture;

    Store store;
    store.named["described"] = 12;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetNamed,
                                              .setter = &SetNamed,
                                              .query = &QueryNamed,
                                              .enumerator = &EnumerateNamed,
                                              .data = ub::CallbackData::For(store)});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "sandbox", *instance);

    CHECK(ub_test::EvalTruth(fixture.context, "Object.getOwnPropertyDescriptor(sandbox, 'described') !== undefined"));
    CHECK(ub_test::EvalInt(fixture.context, "Object.getOwnPropertyDescriptor(sandbox, 'described').value") == 12);
    CHECK(ub_test::EvalTruth(fixture.context, "Object.getOwnPropertyDescriptor(sandbox, 'described').enumerable"));
    CHECK(ub_test::EvalTruth(fixture.context, "Object.getOwnPropertyDescriptor(sandbox, 'absent') === undefined"));
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: a hook is told which object it was reached through") {
    ub_test::Fixture fixture;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetHolderOrThis});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "sandbox", *instance);

    // The handler lives on this object, so the holder is this object. Engines
    // differ over whether a property hook is told the receiver separately; the
    // contract says the two are the same object where one is not offered, so
    // only the holder is asserted here. See unibind/function.h.
    CHECK(ub_test::EvalTruth(fixture.context, "sandbox.holder === sandbox"));
    CHECK(ub_test::Eval(fixture.context, "sandbox.receiver").Kind() == ub::ValueKind::Object);
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: the indexed handler answers query and enumerate too") {
    ub_test::Fixture fixture;

    Store store;
    store.indexed[0] = 100;
    store.indexed[1] = 101;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::IndexedPropertyHandler{.getter = &GetIndexed,
                                                .setter = &SetIndexed,
                                                .query = &QueryIndexed,
                                                .deleter = &DeleteIndexed,
                                                .enumerator = &EnumerateIndexed,
                                                .data = ub::CallbackData::For(store)});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "slots", *instance);

    CHECK(ub_test::EvalTruth(fixture.context, "0 in slots"));
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "9 in slots"));
    CHECK(ub_test::EvalText(fixture.context, "Object.keys(slots).join(',')") == "0,1");
    CHECK(ub_test::EvalInt(fixture.context, "Object.getOwnPropertyDescriptor(slots, 1).value") == 101);
    CHECK(ub_test::EvalTruth(fixture.context, "delete slots[0]"));
    CHECK(ub_test::EvalText(fixture.context, "Object.keys(slots).join(',')") == "1");
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: a named handler does not see indexed access, and the other way round") {
    ub_test::Fixture fixture;

    Store store;
    store.named["5"] = 55;
    store.indexed[5] = 5000;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &GetNamed, .data = ub::CallbackData::For(store)});
    shape.SetHandler(ub::IndexedPropertyHandler{.getter = &GetIndexed, .data = ub::CallbackData::For(store)});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "both", *instance);

    // An integer-like key goes to the indexed handler, which is what the
    // language means by an element.
    CHECK(ub_test::EvalInt(fixture.context, "both[5]") == 5000);
    CHECK(ub_test::EvalInt(fixture.context, "both['5']") == 5000);
    CHECK(ub_test::Eval(fixture.context, "both['alpha']").Kind() == ub::ValueKind::Undefined);
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: a hook that throws has intercepted the access whatever it returned") {
    // The contradiction: declining says "carry on with the ordinary lookup",
    // and throwing says "stop". `unibind/template.h` settles it - the throw wins
    // - and the reason is not tidiness: one engine forbids re-entering its own
    // lookup with an exception pending, so a backend may not hand it a decline
    // with a throw left behind.
    //
    // What that must look like from script: the exception arrives, and the
    // property that *is* there does not answer.
    ub_test::Fixture fixture;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("boom", ub::Constant(11));  // the ordinary property underneath
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &ThrowsAndDeclines});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "probe", *instance);

    CHECK(ub_test::EvalText(fixture.context, "try { String(probe.boom) } catch (e) { 'threw: ' + e.message }") ==
          "threw: the hook threw");
    // Nothing is left pending afterwards: script caught it, and the isolate is
    // usable without a handler having to mop up.
    CHECK_FALSE(fixture.iso().HasPendingException());
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);

    // A key the hook does not throw for still reaches the ordinary property,
    // which is what declining is for.
    CHECK(ub_test::EvalInt(fixture.context, "probe.quiet = 5; probe.quiet") == 5);
}

UNIBIND_TEST_CASE(INTERCEPTORS, "interceptors: a deleter that throws stops the delete rather than declining it") {
    ub_test::Fixture fixture;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("boom", ub::Constant(11));
    shape.SetHandler(ub::NamedPropertyHandler{.deleter = &DeleterThrowsAndDeclines});

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "probe", *instance);

    CHECK(ub_test::EvalText(fixture.context, "try { String(delete probe.boom) } catch (e) { 'threw: ' + e.message }") ==
          "threw: the deleter threw");
    CHECK_FALSE(fixture.iso().HasPendingException());
    // And the property is still there: a delete that threw did not happen.
    CHECK(ub_test::EvalInt(fixture.context, "probe.boom") == 11);
}
