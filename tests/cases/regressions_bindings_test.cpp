/// \file
/// Bugs in the native bindings - classes, templates, interceptors, callbacks
/// and realms - each pinned by the case that showed it. Every case here failed
/// on the code before its fix, and passes on every backend after it. The
/// comment on each says what it caught.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "support/harness.h"

namespace {

struct Gadget {
    std::int32_t value = 7;
};

/// A constructor that also writes an object to its return slot - as a
/// constructor shared with a `FunctionTemplate` callback might.
std::unique_ptr<Gadget> MakeGadgetAndAnswerAnother(const ub::CallbackInfo& info) {
    const auto other = ub::Object::New(info.GetContext());
    if (other) {
        info.GetReturnValue().Set(*other);
    }
    return std::make_unique<Gadget>();
}

void GadgetValue(Gadget& self, const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(self.value);
}

/// Declare `Class<Gadget>` under `name`, make it constructable with `Make`
/// (plain calls too when `callable`), and install it on the global object.
template <auto Make>
ub::Class<Gadget> ExposeGadgetClass(ub_test::Fixture& fixture, std::string_view name, bool callable = false) {
    const auto cls = ub::Class<Gadget>::New(fixture.iso(), name);
    if (callable) {
        cls.template ConstructOrCall<Make>();
    } else {
        cls.template Construct<Make>();
    }
    cls.Method<&GadgetValue>("value");
    const auto constructor = cls.GetConstructor(fixture.context);
    REQUIRE(constructor.has_value());
    ub_test::Expose(fixture.context, name, *constructor);  // NOLINT(bugprone-unchecked-optional-access)
    return cls;
}

}  // namespace

UNIBIND_TEST_CASE2(CLASSES, CALLABLE_CLASS,
                   "regressions: what a class constructor writes to its return slot is not the instance") {
    // `Class<T>` promises that what comes out of it carries a `T`. V8 honours
    // an object a constructor callback leaves in its return slot - for a
    // `FunctionTemplate` that is the language's `return` from a constructor -
    // and the class trampoline handed its constructor the call's own slot, so
    // `new Gadget()` evaluated to the stray object: no native, no methods.
    ub_test::Fixture fixture;
    const auto cls = ExposeGadgetClass<&MakeGadgetAndAnswerAnother>(fixture, "Gadget");

    CHECK(cls.IsInstance(ub_test::Eval(fixture.context, "new Gadget()")));
    CHECK(ub_test::EvalInt(fixture.context, "new Gadget().value()") == 7);

    // And a plain call to a class that answers one, which yields an instance
    // the same way.
    const auto callable = ExposeGadgetClass<&MakeGadgetAndAnswerAnother>(fixture, "CallableGadget", true);
    CHECK(callable.IsInstance(ub_test::Eval(fixture.context, "new CallableGadget()")));
    CHECK(callable.IsInstance(ub_test::Eval(fixture.context, "CallableGadget()")));
}

namespace {

/// Writes to its receiver before it answers, as a binding author would.
std::unique_ptr<Gadget> MakeGadgetStamping(const ub::CallbackInfo& info) {
    if (!info.This().Set(info.GetContext(), "stamped", ub::Integer::New(info.GetIsolate(), 42)).value_or(false)) {
        info.ThrowTypeError("the constructor could not write to its receiver");
        return nullptr;
    }
    return std::make_unique<Gadget>();
}

}  // namespace

UNIBIND_TEST_CASE2(CLASSES, CALLABLE_CLASS,
                   "regressions: a plain call to a class constructs on the instance it returns") {
    // A plain call yields an instance "made the same way `new` makes one", and
    // a constructor's receiver is the instance being made. On V8 a plain call
    // ran the constructor with the call's receiver - the global object - and
    // made the instance afterwards, so what the constructor wrote to `this`
    // landed on `globalThis` and the instance came back without it.
    ub_test::Fixture fixture;
    const auto cls = ExposeGadgetClass<&MakeGadgetStamping>(fixture, "Stamped", true);

    CHECK(ub_test::EvalInt(fixture.context, "new Stamped().stamped") == 42);
    CHECK(ub_test::EvalInt(fixture.context, "Stamped().stamped") == 42);
    CHECK(ub_test::EvalText(fixture.context, "typeof globalThis.stamped") == "undefined");
    CHECK(cls.IsInstance(ub_test::Eval(fixture.context, "Stamped.call({})")));
}

namespace {

/// Answers a construct call with an object of its own when given one, and with
/// a number otherwise.
void AnswerWithArgument(const ub::CallbackInfo& info) {
    if (info.Length() > 0) {
        info.GetReturnValue().Set(info[0]);
        return;
    }
    info.GetReturnValue().Set(1);
}

}  // namespace

UNIBIND_TEST_CASE(TEMPLATES, "regressions: a function template's constructor may answer with an object of its own") {
    // A `FunctionTemplate` is V8's, and V8 makes an object a construct call
    // leaves in its return slot the result of `new` - the language's own rule
    // for a constructor that returns an object - while a primitive there is
    // ignored. SpiderMonkey's backend gave the callback a slot that went
    // nowhere, so `new F(o)` was a fresh instance on one engine and `o` on the
    // other.
    ub_test::Fixture fixture;
    const auto tpl = ub::FunctionTemplate::New(fixture.iso(), &AnswerWithArgument);
    const auto function = tpl.GetFunction(fixture.context);
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "F", *function);  // NOLINT(bugprone-unchecked-optional-access)

    CHECK(ub_test::EvalTruth(fixture.context, "(() => { const o = {}; return new F(o) === o; })()"));
    CHECK(ub_test::EvalTruth(fixture.context, "new F() instanceof F"));
    CHECK(ub_test::EvalTruth(fixture.context, "new F(5) instanceof F"));
    CHECK(ub_test::EvalInt(fixture.context, "F()") == 1);
}

namespace {

void AnswerReceiver(const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(info.This());
}

/// Reads through its receiver, as an accessor on a prototype does.
void ReadReceiverValueOf(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    const auto valueOf = info.This().Get(info.GetContext(), "valueOf");
    if (!valueOf) {
        return;
    }
    info.GetReturnValue().Set(info.This());
}

}  // namespace

UNIBIND_TEST_CASE(TEMPLATES, "regressions: a primitive receiver reaches a native boxed, as an object") {
    // `This()` is a `Local<Object>`. V8 converts a callback's receiver the way
    // a sloppy-mode function's is converted: undefined and null become the
    // global object, and a primitive its wrapper. SpiderMonkey's backend
    // replaced a primitive with the global object in a function, and handed
    // an accessor half the primitive itself - a number in a `Local<Object>`,
    // which reading a property through crashed.
    ub_test::Fixture fixture;

    const auto plain = ub::Function::New(fixture.context, &AnswerReceiver);
    REQUIRE(plain.has_value());
    ub_test::Expose(fixture.context, "plain", *plain);  // NOLINT(bugprone-unchecked-optional-access)

    const auto constructable = ub::FunctionTemplate::New(fixture.iso(), &AnswerReceiver).GetFunction(fixture.context);
    REQUIRE(constructable.has_value());
    ub_test::Expose(fixture.context, "constructable", *constructable);  // NOLINT(bugprone-unchecked-optional-access)

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("method", &AnswerReceiver);
    shape.SetAccessor("accessor", &ReadReceiverValueOf);
    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "o", *instance);  // NOLINT(bugprone-unchecked-optional-access)
    ub_test::Eval(fixture.context, "var getter = Object.getOwnPropertyDescriptor(o, 'accessor').get");

    for (const std::string_view callee : {"plain", "constructable", "o.method", "getter"}) {
        CAPTURE(callee);
        const std::string call = std::string(callee) + ".call";
        CHECK(ub_test::EvalText(fixture.context, "(() => { const r = " + call +
                                                     "(5); return typeof r + ' ' + r.valueOf(); })()") == "object 5");
        CHECK(ub_test::EvalText(fixture.context,
                                "(() => { const r = " + call + "('text'); return typeof r + ' ' + r.valueOf(); })()") ==
              "object text");
        CHECK(ub_test::EvalTruth(fixture.context, call + "(undefined) === globalThis"));
        CHECK(ub_test::EvalTruth(fixture.context, call + "(null) === globalThis"));
        CHECK(ub_test::EvalTruth(fixture.context, "(() => { const r = {}; return " + call + "(r) === r; })()"));
    }
}

namespace {

/// Which half of a handler each access reached, and with what.
struct HookLog {
    std::string seen;
};

ub::Intercepted EchoIndex(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    info.Data<HookLog>()->seen += "i" + std::to_string(index) + " ";
    info.GetReturnValue().Set(index);
    return ub::Intercepted::Yes;
}

ub::Intercepted StoreIndex(std::uint32_t index, const ub::Local<ub::Value>& /*value*/,
                           const ub::PropertyCallbackInfo& info) {
    info.Data<HookLog>()->seen += "set-i" + std::to_string(index) + " ";
    return ub::Intercepted::Yes;
}

std::optional<bool> DeleteIndex(std::uint32_t index, const ub::PropertyCallbackInfo& info) {
    info.Data<HookLog>()->seen += "delete-i" + std::to_string(index) + " ";
    return true;
}

ub::Intercepted EchoName(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const auto text = property.To<ub::String>();
    if (!text) {
        return ub::Intercepted::No;
    }
    info.Data<HookLog>()->seen += "n" + text->Utf8Value() + " ";
    if (!info.GetReturnValue().Set("named")) {
        return ub::Intercepted::No;
    }
    return ub::Intercepted::Yes;
}

ub::Intercepted StoreName(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& /*value*/,
                          const ub::PropertyCallbackInfo& info) {
    const auto text = property.To<ub::String>();
    info.Data<HookLog>()->seen += "set-n" + (text ? text->Utf8Value() : std::string("?")) + " ";
    return ub::Intercepted::Yes;
}

std::optional<bool> DeleteName(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const auto text = property.To<ub::String>();
    info.Data<HookLog>()->seen += "delete-n" + (text ? text->Utf8Value() : std::string("?")) + " ";
    return true;
}

}  // namespace

UNIBIND_TEST_CASE(INTERCEPTORS, "regressions: every array index reaches the indexed handler, however large") {
    // An array index is any integer below 2^32 - 1, and V8 hands every one of
    // them to the indexed half of a handler. SpiderMonkey keeps only indices
    // that fit an int32 as integer keys; the rest are strings, and the backend
    // sent those to the *named* half - so `o[3000000000]` reached a different
    // hook, as text, on one engine.
    ub_test::Fixture fixture;
    HookLog log;
    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::IndexedPropertyHandler{
        .getter = &EchoIndex, .setter = &StoreIndex, .deleter = &DeleteIndex, .data = ub::CallbackData::For(log)});
    shape.SetHandler(ub::NamedPropertyHandler{
        .getter = &EchoName, .setter = &StoreName, .deleter = &DeleteName, .data = ub::CallbackData::For(log)});
    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "o", *instance);  // NOLINT(bugprone-unchecked-optional-access)

    CHECK(ub_test::EvalNumber(fixture.context, "o[2147483647]") == 2147483647.0);
    CHECK(ub_test::EvalNumber(fixture.context, "o[2147483648]") == 2147483648.0);
    CHECK(ub_test::EvalNumber(fixture.context, "o['4294967294']") == 4294967294.0);
    ub_test::Eval(fixture.context, "o[3000000000] = 1; delete o[3000000000]");
    CHECK(log.seen == "i2147483647 i2147483648 i4294967294 set-i3000000000 delete-i3000000000 ");

    // 2^32 - 1 is not an array index, and neither is a non-canonical spelling.
    log.seen.clear();
    CHECK(ub_test::EvalText(fixture.context, "o[4294967295]") == "named");
    CHECK(ub_test::EvalText(fixture.context, "o['01']") == "named");
    CHECK(log.seen == "n4294967295 n01 ");
}

namespace {

std::optional<ub::Local<ub::Array>> ListRepeatedKeys(const ub::PropertyCallbackInfo& info) {
    const auto keys = ub::Evaluate(info.GetContext(), "['a', 'a', 'declared', 'b', 'a']");
    if (!keys) {
        return std::nullopt;
    }
    return keys->To<ub::Array>();
}

ub::Intercepted AnswerListedKeys(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    const auto text = property.To<ub::String>();
    if (!text || (text->Utf8Value() != "a" && text->Utf8Value() != "b")) {
        return ub::Intercepted::No;
    }
    info.GetReturnValue().Set(1);
    return ub::Intercepted::Yes;
}

}  // namespace

UNIBIND_TEST_CASE(INTERCEPTORS, "regressions: a key an enumerator lists twice is one own key") {
    // An object has no key twice. V8 folds an enumerator's repeats, and a key
    // the object already has, into one; SpiderMonkey's backend appended the
    // hook's list to the object's own keys as it stood, so `Object.keys`
    // listed `a` three times and the declared property twice.
    ub_test::Fixture fixture;
    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("declared", ub::Constant(1));
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &AnswerListedKeys, .enumerator = &ListRepeatedKeys});
    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "o", *instance);  // NOLINT(bugprone-unchecked-optional-access)

    CHECK(ub_test::EvalText(fixture.context, "Object.keys(o).join()") == "declared,a,b");
    CHECK(ub_test::EvalText(fixture.context, "Reflect.ownKeys(o).join()") == "declared,a,b");
    CHECK(ub_test::EvalText(fixture.context,
                            "(() => { const r = []; for (const k in o) r.push(k); return r.join(); })()") ==
          "declared,a,b");
}

UNIBIND_TEST_CASE2(TEMPLATES, INTERCEPTORS, "regressions: a template nested in another keeps its interceptor") {
    // `ObjectTemplate::Set(name, template)` puts an instance of the inner
    // template on every instance of the outer one, and an instance of a
    // template with a handler is intercepted. SpiderMonkey's backend built the
    // inner object by replaying only its declared properties onto a plain
    // object, so the handler - the whole point of a nested scope object - was
    // silently left off.
    ub_test::Fixture fixture;
    HookLog log;
    const auto inner = ub::ObjectTemplate::New(fixture.iso());
    inner.Set("declared", ub::Constant(1));
    inner.SetHandler(ub::NamedPropertyHandler{.getter = &EchoName, .data = ub::CallbackData::For(log)});
    const auto outer = ub::ObjectTemplate::New(fixture.iso());
    outer.Set("inner", inner);
    const auto first = outer.NewInstance(fixture.context);
    const auto second = outer.NewInstance(fixture.context);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    ub_test::Expose(fixture.context, "first", *first);    // NOLINT(bugprone-unchecked-optional-access)
    ub_test::Expose(fixture.context, "second", *second);  // NOLINT(bugprone-unchecked-optional-access)

    CHECK(ub_test::EvalText(fixture.context, "first.inner.anything") == "named");
    CHECK(log.seen == "nanything ");
    CHECK(ub_test::EvalTruth(fixture.context, "first.inner !== second.inner"));
}

UNIBIND_TEST_CASE(TEMPLATES, "regressions: HasInstance asks what made an object, not what its prototype chain says") {
    // `HasInstance` is "whether the value was made by this template - the
    // check to do before unwrapping", and V8 answers it from the template the
    // object was made from. SpiderMonkey's backend answered `instanceof`
    // instead, which script controls: an object made with
    // `Object.create(F.prototype)` passed, an instance whose prototype was
    // swapped failed, and so did every instance asked about from another realm.
    ub_test::Fixture fixture;
    const auto parent = ub::FunctionTemplate::New(fixture.iso());
    const auto child = ub::FunctionTemplate::New(fixture.iso());
    child.Inherit(parent);
    const auto intercepted = ub::FunctionTemplate::New(fixture.iso());
    intercepted.InstanceTemplate().SetHandler(ub::NamedPropertyHandler{.getter = &AnswerListedKeys});
    for (const auto& [name, tpl] :
         {std::pair{"Parent", parent}, std::pair{"Child", child}, std::pair{"Scoped", intercepted}}) {
        const auto function = tpl.GetFunction(fixture.context);
        REQUIRE(function.has_value());
        ub_test::Expose(fixture.context, name, *function);  // NOLINT(bugprone-unchecked-optional-access)
    }
    const auto made = [&](std::string_view source) { return ub_test::Eval(fixture.context, source); };
    const auto yes = std::optional<bool>(true);
    const auto no = std::optional<bool>(false);

    CHECK(parent.HasInstance(fixture.context, made("new Parent()")) == yes);
    CHECK(parent.HasInstance(fixture.context, made("Object.create(Parent.prototype)")) == no);
    CHECK(parent.HasInstance(fixture.context, made("Object.setPrototypeOf(new Parent(), {})")) == yes);
    CHECK(parent.HasInstance(fixture.context, made("new (class extends Parent {})()")) == yes);
    CHECK(parent.HasInstance(fixture.context, made("({})")) == no);

    // Inheritance runs one way.
    CHECK(parent.HasInstance(fixture.context, made("new Child()")) == yes);
    CHECK(child.HasInstance(fixture.context, made("new Parent()")) == no);

    // An intercepted instance is an instance, and nothing script wrote is.
    CHECK(intercepted.HasInstance(fixture.context, made("new Scoped()")) == yes);
    CHECK(intercepted.HasInstance(fixture.context, made("Object.create(Scoped.prototype)")) == no);
    const auto shaped = intercepted.InstanceTemplate().NewInstance(fixture.context);
    REQUIRE(shaped.has_value());
    CHECK(intercepted.HasInstance(fixture.context, *shaped) == yes);  // NOLINT(bugprone-unchecked-optional-access)

    // And from another realm, where neither prototype is the one it knows.
    const auto instance = made("new Child()");
    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());
    {
        const ub::ContextScope entered(*second);              // NOLINT(bugprone-unchecked-optional-access)
        CHECK(parent.HasInstance(*second, instance) == yes);  // NOLINT(bugprone-unchecked-optional-access)
        CHECK(child.HasInstance(*second, instance) == yes);   // NOLINT(bugprone-unchecked-optional-access)
    }
}

namespace {

ub::Intercepted DeclineRead(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& /*info*/) {
    return ub::Intercepted::No;
}

}  // namespace

UNIBIND_TEST_CASE(INTERCEPTORS, "regressions: an intercepted object from a plain template inherits from Object") {
    // An object template that belongs to no function template makes ordinary
    // objects, which inherit from `Object.prototype` - with a handler or
    // without. SpiderMonkey's backend gave one with a handler no prototype at
    // all, so it had no `toString`, `String(o)` threw, and it was not an
    // `Object` to `instanceof`.
    ub_test::Fixture fixture;
    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetHandler(ub::NamedPropertyHandler{.getter = &DeclineRead});
    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "o", *instance);  // NOLINT(bugprone-unchecked-optional-access)

    CHECK(ub_test::EvalTruth(fixture.context, "Object.prototype.isPrototypeOf(o)"));
    CHECK(ub_test::EvalTruth(fixture.context, "o instanceof Object"));
    CHECK(ub_test::EvalText(fixture.context, "typeof o.hasOwnProperty") == "function");
    CHECK(ub_test::EvalText(fixture.context, "String(o)") == "[object Object]");
}

UNIBIND_TEST_CASE(INTERCEPTORS,
                  "regressions: an intercepted object looks things up through the prototype it was given") {
    // `Object.setPrototypeOf` on an intercepted object has to change the chain
    // a declined lookup walks. On SpiderMonkey the object is a proxy that was
    // made with a prototype of its own, and the engine answers
    // `setPrototypeOf` on such a proxy by changing that - without asking the
    // handler - while a declined lookup walked the chain of the hidden target,
    // which never changed. `Object.getPrototypeOf` said one thing and every
    // property read did another.
    ub_test::Fixture fixture;
    const auto tpl = ub::FunctionTemplate::New(fixture.iso());
    tpl.PrototypeTemplate().Set("inherited", ub::Constant(1));
    tpl.InstanceTemplate().SetHandler(ub::NamedPropertyHandler{.getter = &DeclineRead});
    const auto function = tpl.GetFunction(fixture.context);
    REQUIRE(function.has_value());
    ub_test::Expose(fixture.context, "F", *function);  // NOLINT(bugprone-unchecked-optional-access)
    ub_test::Eval(fixture.context, "var o = new F(), q = { swapped: 2 }");

    CHECK(ub_test::EvalInt(fixture.context, "o.inherited") == 1);
    CHECK(ub_test::EvalTruth(fixture.context, "Object.setPrototypeOf(o, q) === o && Object.getPrototypeOf(o) === q"));
    CHECK(ub_test::EvalInt(fixture.context, "o.swapped") == 2);
    CHECK(ub_test::EvalTruth(fixture.context, "'swapped' in o && !('inherited' in o) && o.inherited === undefined"));
    CHECK(ub_test::EvalTruth(fixture.context, "o instanceof F") == false);
    CHECK(tpl.HasInstance(fixture.context, ub_test::Eval(fixture.context, "o")) == std::optional<bool>(true));
}
