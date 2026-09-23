/// \file
/// One thing an embedder would recognise, built out of the public API: a
/// sandbox.
///
/// Every other case in this suite exercises one operation. This file is the
/// first that *composes* the hard ones - a second realm, an interceptor over
/// every property of an object, native state holding an engine root across
/// calls, a class method that has to survive the interceptor, and source
/// evaluated in the inner realm whose result comes back out. Each part passes
/// on its own; composition is where an abstraction that looks portable stops
/// being.
///
/// The composition also depends on two decisions that were settled separately
/// and had never been asked at once (docs/status.md): a value made in one realm
/// is usable in another (4), and a script sees the globals of the realm it runs
/// in rather than the one it was compiled in (10).
///
/// The parity risk to watch, and the reason several cases below assert
/// containment rather than equality: an engine with no interceptor of its own
/// has to build ours out of proxies, and a proxy shows through in exactly two
/// places - object identity, and the order and the set of keys an enumeration
/// produces. Where the two engines differ, `docs/testing.md` says so.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "support/harness.h"
#include "support/ownership.h"

namespace {

/// What a sandbox is: a realm of its own, and a root to that realm's global
/// object held across calls. The `Tracked` member is there so the last case can
/// ask whether the native was destroyed exactly once.
struct SandboxState {
    SandboxState(ub::Isolate& isolate, ub::Context&& made)
        : realm(std::move(made)), scope(isolate, realm.GlobalObject()) {}

    ub::Context realm;
    ub::Global<ub::Object> scope;
    ub_test::Tracked marker;
};

std::unique_ptr<SandboxState> MakeSandbox(const ub::CallbackInfo& info) {
    auto realm = ub::Context::New(info.GetIsolate());
    if (!realm) {
        info.ThrowTypeError("could not make a realm for the sandbox");
        return nullptr;
    }
    return std::make_unique<SandboxState>(info.GetIsolate(), std::move(*realm));
}

/// The sandbox behind a property hook. `Holder()` is the object carrying the
/// handler (docs/status.md decision 1), which for a class instance handler is
/// the instance.
SandboxState* SandboxOf(const ub::PropertyCallbackInfo& info) {
    return ub::Class<SandboxState>::Unwrap(info.Holder());
}

/// Compile and run in the inner realm, and hand the result back to the outer
/// one.
void EvaluateInside(SandboxState& self, const ub::CallbackInfo& info) {
    if (info.Length() < 1) {
        info.ThrowTypeError("evaluate expects source");
        return;
    }
    const auto source = info[0].ToString(info.GetContext());
    if (!source) {
        return;
    }
    const std::string text = source->Utf8Value();

    ub::ContextScope inside(self.realm);
    auto result = ub::Evaluate(self.realm, text, {.resourceName = "sandbox.js"});
    if (!result) {
        return;  // whatever the inner realm threw is pending; it propagates out
    }
    info.GetReturnValue().Set(*result);
}

/// Every hook below touches the *inner* realm's global object, and every one of
/// them enters that realm first.
///
/// That is not tidiness, it is required, and it is the first thing this
/// composition found: V8 access-checks a foreign realm's global proxy, so
/// reading a property of the inner global while the outer realm is current
/// throws `TypeError: no access` before any of our code runs. An embedder
/// working around it in V8 directly would give the two contexts a shared
/// security token; `ub::Context` has no such control, so entering the realm
/// that owns the object is the portable answer. See docs/testing.md.
ub::Intercepted SandboxGet(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    SandboxState* self = SandboxOf(info);
    if (self == nullptr) {
        return ub::Intercepted::No;
    }
    const auto scope = self->scope.Get(info.GetIsolate());
    ub::ContextScope inside(self->realm);

    const auto has = scope.HasOwn(self->realm, property);
    if (!has || !*has) {
        // Declining is what lets `evaluate` - a method on the class's own
        // prototype - still be found. This is the whole reason the answer is
        // three-state.
        return ub::Intercepted::No;
    }
    const auto value = scope.Get(self->realm, property);
    if (!value) {
        return ub::Intercepted::Yes;  // the read threw; the exception is pending
    }
    info.GetReturnValue().Set(*value);
    return ub::Intercepted::Yes;
}

ub::Intercepted SandboxSet(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& value,
                           const ub::PropertyCallbackInfo& info) {
    SandboxState* self = SandboxOf(info);
    if (self == nullptr) {
        return ub::Intercepted::No;
    }
    const auto scope = self->scope.Get(info.GetIsolate());
    ub::ContextScope inside(self->realm);
    (void)scope.Set(self->realm, property, value);
    return ub::Intercepted::Yes;
}

std::optional<ub::PropertyAttribute> SandboxQuery(const ub::Local<ub::Name>& property,
                                                  const ub::PropertyCallbackInfo& info) {
    SandboxState* self = SandboxOf(info);
    if (self == nullptr) {
        return std::nullopt;
    }
    const auto scope = self->scope.Get(info.GetIsolate());
    ub::ContextScope inside(self->realm);
    const auto has = scope.HasOwn(self->realm, property);
    if (!has || !*has) {
        return std::nullopt;
    }
    return ub::PropertyAttribute::None;
}

std::optional<bool> SandboxDelete(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    SandboxState* self = SandboxOf(info);
    if (self == nullptr) {
        return std::nullopt;
    }
    const auto scope = self->scope.Get(info.GetIsolate());
    ub::ContextScope inside(self->realm);
    const auto has = scope.HasOwn(self->realm, property);
    if (!has || !*has) {
        return std::nullopt;
    }
    return scope.Delete(self->realm, property);
}

std::optional<ub::Local<ub::Array>> SandboxEnumerate(const ub::PropertyCallbackInfo& info) {
    SandboxState* self = SandboxOf(info);
    if (self == nullptr) {
        return std::nullopt;
    }
    const auto scope = self->scope.Get(info.GetIsolate());
    ub::ContextScope inside(self->realm);
    return scope.GetOwnPropertyNames(self->realm);
}

ub::Class<SandboxState> DeclareSandbox(ub::Isolate& isolate) {
    auto cls = ub::Class<SandboxState>::New(isolate, "Sandbox");
    cls.Construct<&MakeSandbox>();
    cls.Method<&EvaluateInside>("evaluate");
    cls.SetHandler(ub::NamedPropertyHandler{.getter = &SandboxGet,
                                            .setter = &SandboxSet,
                                            .query = &SandboxQuery,
                                            .deleter = &SandboxDelete,
                                            .enumerator = &SandboxEnumerate});
    return cls;
}

/// Declares the class and puts its constructor on the outer global.
void InstallSandbox(ub_test::Fixture& fixture) {
    const auto cls = DeclareSandbox(fixture.iso());
    const auto constructor = cls.GetConstructor(fixture.context);
    REQUIRE(constructor.has_value());
    ub_test::Expose(fixture.context, "Sandbox", *constructor);
}

}  // namespace

UNIBIND_TEST_CASE2(CLASSES, INTERCEPTORS, "sandbox: a prototype method survives an interceptor over every property") {
    // The interceptor sees `evaluate` too. It has to decline - `Intercepted::No`
    // for a name the inner scope does not have - or the class's own method is
    // unreachable and the sandbox has no way in.
    ub_test::Fixture fixture;
    InstallSandbox(fixture);

    CHECK(ub_test::EvalTruth(fixture.context, "typeof new Sandbox().evaluate === 'function'"));
    CHECK(ub_test::EvalInt(fixture.context, "new Sandbox().evaluate('1 + 1')") == 2);
    CHECK(ub_test::EvalTruth(fixture.context, "new Sandbox() instanceof Sandbox"));

    // And a name the inner scope *does* have wins over anything above it.
    CHECK(ub_test::EvalText(fixture.context, R"(
        var s = new Sandbox();
        s.evaluate("globalThis.toString = () => 'from inside'");
        s.toString()
    )") == "from inside");
}

UNIBIND_TEST_CASE2(CLASSES, INTERCEPTORS, "sandbox: a write through the object lands inside it, and nowhere else") {
    ub_test::Fixture fixture;
    InstallSandbox(fixture);

    CHECK(ub_test::EvalInt(fixture.context, R"(
        var s = new Sandbox();
        s.answer = 42;
        s.evaluate("answer")
    )") == 42);

    // The outer realm never saw it.
    CHECK(ub_test::EvalTruth(fixture.context, "typeof answer === 'undefined'"));
    // And what the inner realm declares is readable back through the object.
    CHECK(ub_test::EvalInt(fixture.context, R"(
        var s = new Sandbox();
        s.evaluate("var made = 7");
        s.made
    )") == 7);
}

UNIBIND_TEST_CASE2(CLASSES, INTERCEPTORS, "sandbox: code inside does not see the realm that made it") {
    ub_test::Fixture fixture;
    InstallSandbox(fixture);

    CHECK(ub_test::EvalText(fixture.context, R"(
        globalThis.outerOnly = 'outer';
        new Sandbox().evaluate("typeof outerOnly")
    )") == "undefined");

    // It does see its own globals, which is decision 10 seen from the other
    // side: the script runs in the realm it was given, not the one it was
    // compiled in.
    CHECK(ub_test::EvalTruth(fixture.context, "new Sandbox().evaluate('typeof Object === \"function\"')"));
}

UNIBIND_TEST_CASE2(CLASSES, INTERCEPTORS, "sandbox: two sandboxes do not see each other") {
    ub_test::Fixture fixture;
    InstallSandbox(fixture);

    CHECK(ub_test::EvalTruth(fixture.context, R"(
        var a = new Sandbox();
        var b = new Sandbox();
        a.shared = 'a';
        b.shared = 'b';
        a.evaluate("shared") === 'a' && b.evaluate("shared") === 'b'
    )"));
    CHECK(ub_test::EvalText(fixture.context, R"(
        var a = new Sandbox();
        var b = new Sandbox();
        a.evaluate("var onlyInA = 1");
        b.evaluate("typeof onlyInA")
    )") == "undefined");
}

UNIBIND_TEST_CASE2(CLASSES, INTERCEPTORS, "sandbox: in, keys, descriptors and delete all reflect the inner scope") {
    ub_test::Fixture fixture;
    InstallSandbox(fixture);

    (void)ub_test::Eval(fixture.context, "globalThis.s = new Sandbox(); s.kept = 1; s.evaluate('var declared = 2')");

    CHECK(ub_test::EvalTruth(fixture.context, "'kept' in s"));
    CHECK(ub_test::EvalTruth(fixture.context, "'declared' in s"));
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "'neverSet' in s"));

    // Containment, not equality: what else an engine's fresh realm global lists
    // is its own business, and a proxy-backed interceptor has invariants of its
    // own to satisfy. What both must agree on is that what we put in is there
    // and that the prototype's method is not.
    CHECK(ub_test::EvalTruth(fixture.context, "Object.keys(s).includes('kept')"));
    CHECK(ub_test::EvalTruth(fixture.context, "Object.keys(s).includes('declared')"));
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "Object.keys(s).includes('evaluate')"));
    MESSAGE("Object.keys(sandbox) has ", ub_test::EvalInt(fixture.context, "Object.keys(s).length"), " entries");

    const auto descriptor = ub_test::Eval(fixture.context, "Object.getOwnPropertyDescriptor(s, 'kept')");
    REQUIRE(descriptor.Kind() == ub::ValueKind::Object);
    CHECK(ub_test::EvalInt(fixture.context, "Object.getOwnPropertyDescriptor(s, 'kept').value") == 1);

    CHECK(ub_test::EvalTruth(fixture.context, "delete s.kept"));
    CHECK_FALSE(ub_test::EvalTruth(fixture.context, "'kept' in s"));
    CHECK(ub_test::EvalText(fixture.context, "s.evaluate('typeof kept')") == "undefined");
}

UNIBIND_TEST_CASE2(CLASSES, INTERCEPTORS, "sandbox: a value made inside is a value outside") {
    // Decision 4, reached through the whole composition rather than by handing
    // an object between two contexts directly.
    ub_test::Fixture fixture;
    InstallSandbox(fixture);

    const auto made = ub_test::Eval(fixture.context, "new Sandbox().evaluate('({ a: 1, b: [2, 3] })')");
    REQUIRE(made.Kind() == ub::ValueKind::Object);
    const auto asObject = made.To<ub::Object>();
    REQUIRE(asObject.has_value());

    // Read it natively, from the outer realm, with no wrapping ceremony.
    const auto a = asObject->Get(fixture.context, "a");
    REQUIRE(a.has_value());
    CHECK(a->ToInt32(fixture.context).value_or(0) == 1);

    // And from script in the outer realm.
    CHECK(ub_test::EvalInt(fixture.context, "new Sandbox().evaluate('({ a: 1, b: [2, 3] })').b[1]") == 3);
    CHECK(ub_test::EvalInt(fixture.context, "new Sandbox().evaluate('[4, 5, 6]').length") == 3);
    CHECK(ub_test::EvalInt(fixture.context, "new Sandbox().evaluate('(x => x * 2)')(21)") == 42);
}

UNIBIND_TEST_CASE2(CLASSES, INTERCEPTORS, "sandbox: reading the same thing twice gives the same thing") {
    // Identity is where a proxy-backed interceptor would show through: a
    // wrapper made per access would make each of these false while every other
    // case in this file still passed.
    ub_test::Fixture fixture;
    InstallSandbox(fixture);

    CHECK(ub_test::EvalTruth(fixture.context, "(() => { const s = new Sandbox(); return s === s; })()"));
    CHECK(ub_test::EvalTruth(fixture.context, R"(
        var s = new Sandbox();
        s.evaluate("globalThis.thing = { tag: 1 }");
        s.thing === s.thing
    )"));
    CHECK(ub_test::EvalTruth(fixture.context, R"(
        var s = new Sandbox();
        s.given = { tag: 2 };
        s.given === s.evaluate("given")
    )"));
    // What is *not* asserted here, and is a limit worth knowing: handing the
    // inner realm's `globalThis` out to the outer one. It crosses as a value,
    // and on V8 script in the outer realm cannot then read through it - a
    // global proxy is access-checked, which is the one exception to "a value
    // made in one realm is a value in the other" (decision 4). An object the
    // inner realm made is fine; its *global* is not. See docs/testing.md.
    const auto innerGlobal = ub_test::Eval(fixture.context, "new Sandbox().evaluate('globalThis')");
    CHECK(innerGlobal.Kind() == ub::ValueKind::Object);
}

UNIBIND_TEST_CASE2(CLASSES, INTERCEPTORS, "sandbox: what the source throws is catchable outside it") {
    ub_test::Fixture fixture;
    InstallSandbox(fixture);

    CHECK(ub_test::EvalText(fixture.context, R"JS(
        var s = new Sandbox();
        try { s.evaluate("throw new Error('from inside')"); 'no throw'; }
        catch (e) { e.message }
    )JS") == "from inside");

    // A syntax error is the same question asked earlier - and asked by name,
    // because the error was built in the inner realm and `instanceof` does not
    // cross one (`realms: instanceof does not cross a realm`). That is the
    // price of the error arriving intact rather than being re-wrapped, and it
    // is what an embedder has to know about a sandbox's exceptions.
    CHECK(ub_test::EvalText(fixture.context, R"(
        var s = new Sandbox();
        try { s.evaluate("this is not javascript"); 'no throw'; }
        catch (e) { e.constructor.name }
    )") == "SyntaxError");

    // And from native, through a handler in the outer isolate.
    ub::TryCatch handler(fixture.iso());
    CHECK_FALSE(ub::Evaluate(fixture.context, "new Sandbox().evaluate('throw new TypeError(\"nope\")')").has_value());
    CHECK(handler.HasCaught());
    CHECK(handler.Message(fixture.context).value_or("").find("nope") != std::string::npos);
    handler.Reset();

    // The sandbox still works afterwards.
    CHECK(ub_test::EvalInt(fixture.context, "new Sandbox().evaluate('3 + 4')") == 7);
}

UNIBIND_TEST_CASE2(CLASSES, INTERCEPTORS, "sandbox: every sandbox is destroyed exactly once, roots and all") {
    // The composition's lifetime question: the native holds an engine root
    // (`Global`) and a realm across calls, so a sandbox that leaked would leak
    // three things at once - and one still rooted at teardown is the case that
    // decision 7 exists for.
    ub_test::Lives::Reset();
    constexpr int MADE = 8;

    {
        auto isolate = ub::Isolate::New();
        REQUIRE(isolate != nullptr);
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        REQUIRE(context.has_value());
        ub::ContextScope entered(*context);

        const auto cls = DeclareSandbox(*isolate);
        REQUIRE(context->GlobalObject().Set(*context, "Sandbox", *cls.GetConstructor(*context)).value_or(false));

        // Half unreachable, half rooted at teardown.
        {
            ub::HandleScope inner(*isolate);
            for (int i = 0; i < MADE / 2; ++i) {
                REQUIRE(ub::Evaluate(*context, "new Sandbox().evaluate('1')").has_value());
            }
        }
        std::vector<ub::Global<ub::Object>> kept;
        for (int i = 0; i < MADE / 2; ++i) {
            auto made = ub::Evaluate(*context, "new Sandbox()");
            REQUIRE(made.has_value());
            const auto asObject = made->To<ub::Object>();
            REQUIRE(asObject.has_value());
            kept.emplace_back(*isolate, *asObject);
        }

        REQUIRE(ub_test::Lives::TotalBorn() == MADE);
        isolate->RequestGarbageCollection();
        CHECK_FALSE(ub_test::Lives::AnyDestroyedTwice());
    }

    CHECK(ub_test::Lives::TotalDeaths() == MADE);
    CHECK(ub_test::Lives::EachDestroyedExactlyOnce());
}
