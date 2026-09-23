/// \file
/// Bugs in the native bindings - classes, templates, interceptors, callbacks
/// and realms - each pinned by the case that showed it. Every case here failed
/// on the code before its fix, and passes on every backend after it. The
/// comment on each says what it caught.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

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
