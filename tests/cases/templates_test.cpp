/// \file
/// Object and function templates, and the native accessors that go on them.
/// Minus the native state that `unibind/class.h` adds on top.

#include <array>
#include <cstdint>
#include <string>

#include "support/harness.h"

namespace {

struct Cell {
    std::int32_t value = 0;
    int reads = 0;
    int writes = 0;
};

void ReadCell(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    auto* cell = info.Data<Cell>();
    if (cell == nullptr) {
        info.ThrowTypeError("accessor data missing");
        return;
    }
    ++cell->reads;
    info.GetReturnValue().Set(cell->value);
}

void WriteCell(const ub::Local<ub::Name>& /*property*/, const ub::Local<ub::Value>& value,
               const ub::PropertyCallbackInfo& info) {
    auto* cell = info.Data<Cell>();
    if (cell == nullptr) {
        info.ThrowTypeError("accessor data missing");
        return;
    }
    ++cell->writes;
    const auto asInt = value.ToInt32(info.GetContext());
    if (!asInt) {
        return;
    }
    cell->value = *asInt;
}

struct MakerLog {
    int calls = 0;
    int constructCalls = 0;
};

/// The callback of a constructable template: writes what it was given onto the
/// instance, so a test can tell the callback's result from a fresh empty object.
void Maker(const ub::CallbackInfo& info) {
    auto* log = info.Data<MakerLog>();
    if (log != nullptr) {
        ++log->calls;
        if (info.IsConstructCall()) {
            ++log->constructCalls;
        }
    }
    if (info.Length() == 0) {
        return;
    }
    const auto given = info[0].ToInt32(info.GetContext());
    if (!given) {
        return;
    }
    if (!info.This().Set(info.GetContext(), "made", ub::Integer::New(info.GetIsolate(), *given)).value_or(false)) {
        return;
    }
}

/// The whole of decision 12 in one callback: it is reached both ways and says
/// which, by answering with a string when called and by writing one onto the
/// instance when constructed.
void EitherWay(const ub::CallbackInfo& info) {
    auto* log = info.Data<MakerLog>();
    if (log != nullptr) {
        ++log->calls;
        if (info.IsConstructCall()) {
            ++log->constructCalls;
        }
    }
    if (!info.IsConstructCall()) {
        if (!info.GetReturnValue().Set("called")) {
            info.ThrowTypeError("could not make the string");
        }
        return;
    }
    auto how = ub::String::New(info.GetIsolate(), "constructed");
    if (!how) {
        return;
    }
    if (!info.This().Set(info.GetContext(), "how", *how).value_or(false)) {
        return;
    }
}

void SaysHello(const ub::CallbackInfo& info) {
    if (!info.GetReturnValue().Set("hello")) {
        info.ThrowTypeError("could not make the string");
    }
}

/// Answers with `this.name`, so a test can see which object the accessor was
/// read through.
void ReadReceiverName(const ub::Local<ub::Name>& /*property*/, const ub::PropertyCallbackInfo& info) {
    auto name = info.This().Get(info.GetContext(), "name");
    if (!name) {
        return;
    }
    info.GetReturnValue().Set(*name);
}

void SaysWhoItIs(const ub::CallbackInfo& info) {
    auto name = info.This().Get(info.GetContext(), "name");
    if (!name) {
        return;
    }
    info.GetReturnValue().Set(*name);
}

}  // namespace

UNIBIND_TEST_CASE(TEMPLATES, "templates: an object template installs the constants it was given") {
    ub_test::Fixture fixture;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("flag", ub::Constant(true));
    shape.Set("count", ub::Constant(std::int32_t{7}));
    shape.Set("ratio", ub::Constant(0.5));
    shape.Set("label", ub::Constant(std::string_view("named")));
    shape.Set("nothing", ub::Constant::Null());
    shape.Set("frozen", ub::Constant(std::int32_t{1}), ub::PropertyAttribute::ReadOnly);

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "shaped", *instance);

    CHECK(ub_test::EvalTruth(fixture.context, "shaped.flag === true"));
    CHECK(ub_test::EvalInt(fixture.context, "shaped.count") == 7);
    CHECK(ub_test::EvalNumber(fixture.context, "shaped.ratio") == doctest::Approx(0.5));
    CHECK(ub_test::EvalText(fixture.context, "shaped.label") == "named");
    CHECK(ub_test::EvalTruth(fixture.context, "shaped.nothing === null"));
    CHECK(ub_test::EvalInt(fixture.context, "shaped.frozen = 99, shaped.frozen") == 1);
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a method on a template is callable on every instance") {
    ub_test::Fixture fixture;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("greet", &SaysHello);

    const auto first = shape.NewInstance(fixture.context);
    const auto second = shape.NewInstance(fixture.context);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK_FALSE(first->StrictEquals(*second));

    ub_test::Expose(fixture.context, "a", *first);
    ub_test::Expose(fixture.context, "b", *second);
    CHECK(ub_test::EvalText(fixture.context, "a.greet()") == "hello");
    CHECK(ub_test::EvalText(fixture.context, "b.greet()") == "hello");

    // A method is not enumerable by default, as on a built-in prototype.
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "Object.keys(a).includes('greet')"));
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a method is not enumerable and a constant is") {
    ub_test::Fixture fixture;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("greet", &SaysHello);
    shape.Set("count", ub::Constant(std::int32_t{1}));

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "shaped", *instance);

    // The defaults in unibind/template.h: DontEnum for a method, None for a value.
    CHECK(ub_test::EvalText(fixture.context, "Object.keys(shaped).join(',')") == "count");
    CHECK(ub_test::EvalText(fixture.context, "Object.getOwnPropertyNames(shaped).sort().join(',')") == "count,greet");
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: an accessor sees the instance as its receiver") {
    ub_test::Fixture fixture;

    // An accessor declared on a prototype has to find the *instance* when it is
    // read through one, or nothing that keeps its state on the instance can
    // ever be written as an accessor. See docs/status.md.
    const auto tpl = ub::FunctionTemplate::New(fixture.iso());
    tpl.SetClassName("Holder");
    tpl.InstanceTemplate().Set("name", ub::Constant(std::string_view("the instance")));
    tpl.PrototypeTemplate().SetAccessor("whoseAccessor", &ReadReceiverName);

    ub_test::Expose(fixture.context, "Holder", *tpl.GetFunction(fixture.context));
    CHECK(ub_test::EvalText(fixture.context, "new Holder().whoseAccessor") == "the instance");
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: an accessor runs native code on read and on write") {
    ub_test::Fixture fixture;

    Cell cell{.value = 3};
    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetAccessor("value", &ReadCell, &WriteCell, ub::CallbackData::For(cell));

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "cell", *instance);

    CHECK(ub_test::EvalInt(fixture.context, "cell.value") == 3);
    CHECK(cell.reads == 1);

    REQUIRE(ub::Evaluate(fixture.context, "cell.value = 11").has_value());
    CHECK(cell.writes == 1);
    CHECK(cell.value == 11);
    CHECK(ub_test::EvalInt(fixture.context, "cell.value") == 11);

    // Reading through the native API goes through the accessor too.
    const auto read = instance->Get(fixture.context, "value");
    REQUIRE(read.has_value());
    CHECK(read->To<ub::Integer>()->Int32Value() == 11);
    CHECK(cell.reads == 3);
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a read-only accessor has no setter to run") {
    ub_test::Fixture fixture;

    Cell cell{.value = 42};
    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetAccessor("value", &ReadCell);

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "cell", *instance);

    // Without data the getter throws, which is how this case knows the getter
    // ran at all rather than a data property answering.
    ub::TryCatch tryCatch(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "cell.value").has_value());
    CHECK(tryCatch.HasCaught());
    tryCatch.Reset();
    CHECK(cell.writes == 0);
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a template nests inside another") {
    ub_test::Fixture fixture;

    const auto inner = ub::ObjectTemplate::New(fixture.iso());
    inner.Set("depth", ub::Constant(std::int32_t{2}));

    const auto outer = ub::ObjectTemplate::New(fixture.iso());
    outer.Set("depth", ub::Constant(std::int32_t{1}));
    outer.Set("nested", inner);

    const auto instance = outer.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "tree", *instance);

    CHECK(ub_test::EvalInt(fixture.context, "tree.depth") == 1);
    CHECK(ub_test::EvalInt(fixture.context, "tree.nested.depth") == 2);
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: one template instantiates into several realms") {
    ub_test::Fixture fixture;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("greet", &SaysHello);
    shape.Set("tag", ub::Constant(std::string_view("shared shape")));

    auto second = ub::Context::New(fixture.iso());
    REQUIRE(second.has_value());

    const auto here = shape.NewInstance(fixture.context);
    REQUIRE(here.has_value());
    ub_test::Expose(fixture.context, "shaped", *here);
    CHECK(ub_test::EvalText(fixture.context, "shaped.greet()") == "hello");

    ub::ContextScope entered(*second);
    const auto there = shape.NewInstance(*second);
    REQUIRE(there.has_value());
    REQUIRE(second->GlobalObject().Set(*second, "shaped", *there).value_or(false));
    CHECK(ub_test::EvalText(*second, "shaped.greet()") == "hello");
    CHECK_FALSE(here->StrictEquals(*there));
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a function template is a constructor with a prototype") {
    ub_test::Fixture fixture;

    const auto tpl = ub::FunctionTemplate::New(fixture.iso());
    tpl.SetClassName("Widget");
    tpl.InstanceTemplate().Set("name", ub::Constant(std::string_view("widget")));
    tpl.PrototypeTemplate().Set("whoAmI", &SaysWhoItIs);
    tpl.Set("KIND", ub::Constant(std::string_view("widget-kind")));

    const auto constructor = tpl.GetFunction(fixture.context);
    REQUIRE(constructor.has_value());
    ub_test::Expose(fixture.context, "Widget", *constructor);

    CHECK(ub_test::EvalText(fixture.context, "Widget.name") == "Widget");
    CHECK(ub_test::EvalText(fixture.context, "new Widget().constructor.name") == "Widget");
    CHECK(ub_test::EvalText(fixture.context, "Widget.KIND") == "widget-kind");
    CHECK(ub_test::EvalText(fixture.context, "new Widget().name") == "widget");
    CHECK(ub_test::EvalText(fixture.context, "new Widget().whoAmI()") == "widget");
    // An instance property is the instance's; a prototype method is shared.
    CHECK(ub_test::EvalTruth(fixture.context, "Object.hasOwn(new Widget(), 'name')"));
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "Object.hasOwn(new Widget(), 'whoAmI')"));
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a function template given a callback is constructable") {
    ub_test::Fixture fixture;

    // `FunctionTemplate` is one of the two ways an embedder asks for a
    // constructor (decision 9), so `new` on one has to run the callback as a
    // construct call and hand back what the callback built - not a fresh empty
    // object. Nothing asserted this once, and it broke silently.
    MakerLog log;
    const auto tpl = ub::FunctionTemplate::New(fixture.iso(), &Maker, ub::CallbackData::For(log));
    tpl.SetClassName("Maker");

    const auto constructor = tpl.GetFunction(fixture.context);
    REQUIRE(constructor.has_value());
    ub_test::Expose(fixture.context, "Maker", *constructor);

    CHECK(ub_test::EvalInt(fixture.context, "new Maker(3).made") == 3);
    CHECK(log.constructCalls == 1);
    CHECK(log.calls == 1);
    CHECK(ub_test::EvalTruth(fixture.context, "new Maker(1) instanceof Maker"));

    // From native, with the same answer.
    const std::array<ub::Local<ub::Value>, 1> arguments{ub::Integer::New(fixture.iso(), 5)};
    const auto instance = constructor->NewInstance(fixture.context, arguments);
    REQUIRE(instance.has_value());
    CHECK(instance->Get(fixture.context, "made")->To<ub::Integer>()->Int32Value() == 5);
    CHECK(log.constructCalls == 3);
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a function template is a function as well as a constructor") {
    ub_test::Fixture fixture;

    // Decision 12. One callback, both ways in, telling them apart with
    // `IsConstructCall()` - which is the only thing that makes that question
    // worth asking, and the only way to express a built-in like `Error` that
    // means something whether or not it was `new`-ed.
    MakerLog log;
    const auto tpl = ub::FunctionTemplate::New(fixture.iso(), &EitherWay, ub::CallbackData::For(log));
    tpl.SetClassName("EitherWay");
    ub_test::Expose(fixture.context, "EitherWay", *tpl.GetFunction(fixture.context));

    // Called: the callback runs with IsConstructCall() false and its result is
    // the call's result.
    CHECK(ub_test::EvalText(fixture.context, "EitherWay()") == "called");
    CHECK(log.calls == 1);
    CHECK(log.constructCalls == 0);

    // Constructed: the callback runs with IsConstructCall() true and the
    // instance is what comes back.
    CHECK(ub_test::EvalText(fixture.context, "new EitherWay().how") == "constructed");
    CHECK(log.calls == 2);
    CHECK(log.constructCalls == 1);
    CHECK(ub_test::EvalTruth(fixture.context, "new EitherWay() instanceof EitherWay"));

    // And from native, both ways, with the same answers.
    const auto function = tpl.GetFunction(fixture.context);
    REQUIRE(function.has_value());
    const auto called = function->Call(fixture.context, fixture.context.GlobalObject());
    REQUIRE(called.has_value());
    CHECK(ub_test::TextOf(*called) == "called");

    const auto constructed = function->NewInstance(fixture.context);
    REQUIRE(constructed.has_value());
    const auto how = constructed->Get(fixture.context, "how");
    REQUIRE(how.has_value());
    CHECK(ub_test::TextOf(*how) == "constructed");

    // This is the middle row of a grid the suite pins in three places. The
    // others are `functions: a plain native function is callable and not
    // constructable` and `classes: a constructor called without new is refused`:
    //
    //     made by            f()        new f()
    //     Function::New      yes        TypeError
    //     FunctionTemplate   yes        yes
    //     Class<T>           TypeError  yes
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a method on a template is not a constructor") {
    ub_test::Fixture fixture;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("greet", &SaysHello);

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "shaped", *instance);

    // A method is callable and nothing more, as an ordinary JavaScript method
    // already is.
    CHECK(ub_test::EvalText(fixture.context, "shaped.greet()") == "hello");
    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            try { new shaped.greet(); return false; }
            catch (e) { return e instanceof TypeError; }
        })()
    )"));
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: neither half of an accessor is a constructor") {
    ub_test::Fixture fixture;

    Cell cell{.value = 1};
    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.SetAccessor("value", &ReadCell, &WriteCell, ub::CallbackData::For(cell));

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "cell", *instance);

    CHECK(ub_test::EvalTruth(fixture.context, R"(
        (function () {
            const both = Object.getOwnPropertyDescriptor(cell, 'value');
            if (typeof both.get !== 'function' || typeof both.set !== 'function') { return false; }
            for (const half of [both.get, both.set]) {
                try { new half(); return false; }
                catch (e) { if (!(e instanceof TypeError)) { return false; } }
            }
            return true;
        })()
    )"));
    CHECK(cell.writes == 0);
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a function template can be a property of an object template") {
    ub_test::Fixture fixture;

    MakerLog log;
    const auto inner = ub::FunctionTemplate::New(fixture.iso(), &Maker, ub::CallbackData::For(log));
    inner.SetClassName("Maker");

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("Maker", inner);

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "host", *instance);

    CHECK(ub_test::EvalTruth(fixture.context, "typeof host.Maker === 'function'"));
    CHECK(ub_test::EvalInt(fixture.context, "new host.Maker(6).made") == 6);
    CHECK(log.constructCalls == 1);
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a static method lives on the constructor function") {
    ub_test::Fixture fixture;

    const auto tpl = ub::FunctionTemplate::New(fixture.iso());
    tpl.SetClassName("Widget");
    tpl.Set("describe", &SaysHello);
    tpl.PrototypeTemplate().Set("describe", &SaysWhoItIs);
    tpl.InstanceTemplate().Set("name", ub::Constant(std::string_view("an instance")));

    ub_test::Expose(fixture.context, "Widget", *tpl.GetFunction(fixture.context));

    CHECK(ub_test::EvalText(fixture.context, "Widget.describe()") == "hello");
    CHECK(ub_test::EvalText(fixture.context, "new Widget().describe()") == "an instance");
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "Object.keys(Widget).includes('describe')"));
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: a function template says which values it made") {
    ub_test::Fixture fixture;

    const auto tpl = ub::FunctionTemplate::New(fixture.iso());
    tpl.SetClassName("Widget");

    const auto constructor = tpl.GetFunction(fixture.context);
    REQUIRE(constructor.has_value());
    ub_test::Expose(fixture.context, "Widget", *constructor);

    const auto instance = ub_test::Eval(fixture.context, "new Widget()");
    const auto stranger = ub_test::Eval(fixture.context, "({})");

    CHECK(tpl.HasInstance(fixture.context, instance).value_or(false));
    CHECK_FALSE(tpl.HasInstance(fixture.context, stranger).value_or(true));

    const auto direct = tpl.InstanceTemplate().NewInstance(fixture.context);
    REQUIRE(direct.has_value());
    CHECK(tpl.HasInstance(fixture.context, *direct).value_or(false));
}

UNIBIND_TEST_CASE(TEMPLATES, "templates: inheritance puts the parent's prototype behind the child's") {
    ub_test::Fixture fixture;

    const auto base = ub::FunctionTemplate::New(fixture.iso());
    base.SetClassName("Base");
    base.PrototypeTemplate().Set("fromBase", &SaysHello);

    const auto derived = ub::FunctionTemplate::New(fixture.iso());
    derived.SetClassName("Derived");
    derived.Inherit(base);
    derived.InstanceTemplate().Set("name", ub::Constant(std::string_view("derived")));
    derived.PrototypeTemplate().Set("fromDerived", &SaysWhoItIs);

    ub_test::Expose(fixture.context, "Base", *base.GetFunction(fixture.context));
    ub_test::Expose(fixture.context, "Derived", *derived.GetFunction(fixture.context));

    CHECK(ub_test::EvalText(fixture.context, "new Derived().fromBase()") == "hello");
    CHECK(ub_test::EvalText(fixture.context, "new Derived().fromDerived()") == "derived");
    CHECK(ub_test::EvalTruth(fixture.context, "new Derived() instanceof Base"));
    CHECK(ub_test::EvalTruth(fixture.context, "new Derived() instanceof Derived"));
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "new Base() instanceof Derived"));

    const auto derivedInstance = ub_test::Eval(fixture.context, "new Derived()");
    CHECK(base.HasInstance(fixture.context, derivedInstance).value_or(false));
}

UNIBIND_TEST_CASE2(TEMPLATES, SYMBOL_METHODS, "templates: a template method under a well-known symbol") {
    ub_test::Fixture fixture;

    const auto shape = ub::ObjectTemplate::New(fixture.iso());
    shape.Set("greet", &SaysHello);
    shape.Set(ub::WellKnownSymbol::ToPrimitive, &SaysHello);

    const auto instance = shape.NewInstance(fixture.context);
    REQUIRE(instance.has_value());
    ub_test::Expose(fixture.context, "shaped", *instance);

    CHECK(ub_test::EvalText(fixture.context, "`${shaped}`") == "hello");
}
