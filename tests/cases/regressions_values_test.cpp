/// \file
/// Bugs in values and data - handles, strings, numbers, objects, binary data,
/// serialization and compiling - each pinned by the case that showed it. Every
/// case here failed, or crashed, on the code before its fix, and passes on
/// every backend after it. The comment on each says what it caught.

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/harness.h"

UNIBIND_TEST_CASE(ARRAYS, "regressions: the last index a uint32_t can say is a property, not a crash") {
    // 2^32 - 1 is the one `uint32_t` that is not an array index: it is an
    // ordinary property named "4294967295", and setting it leaves an array's
    // length alone. On a 32-bit build V8's by-index lookup takes it for its own
    // "no index" marker - the same bits as `size_t(-1)` there - and dereferences
    // a name that is not there.
    ub_test::Fixture fixture;
    constexpr std::uint32_t TOP = std::numeric_limits<std::uint32_t>::max();

    const auto object = ub_test::Eval(fixture.context, "({ 4294967295: 'top' })").To<ub::Object>();
    REQUIRE(object.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
    const auto got = object->Get(fixture.context, TOP);
    REQUIRE(got.has_value());
    CHECK(ub_test::TextOf(*got) == "top");

    const auto array = ub::Array::New(fixture.context, 1);
    REQUIRE(array.has_value());
    CHECK(array->Set(fixture.context, TOP, ub::Integer::New(fixture.iso(), 9)).value_or(false));
    CHECK(array->Length() == 1);
    ub_test::Expose(fixture.context, "array", *array);
    CHECK(ub_test::EvalInt(fixture.context, "array[4294967295]") == 9);
    const auto back = array->Get(fixture.context, TOP);
    REQUIRE(back.has_value());
    CHECK(back->ToInt32(fixture.context).value_or(0) == 9);

    // One below it is still an index, and still moves the length.
    CHECK(array->Set(fixture.context, TOP - 1, ub::Integer::New(fixture.iso(), 8)).value_or(false));
    CHECK(array->Length() == TOP);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

UNIBIND_TEST_CASE2(EXCEPTIONS, FUNCTIONS,
                   "regressions: a caught exception stays caught while more work is done before it is asked about") {
    // A `TryCatch` catches when the exception is thrown, not when someone asks
    // it: an embedder may make several calls and look once. SpiderMonkey keeps
    // a throw pending on the context until the handler takes it, and it took it
    // only when asked - so script run in between that threw and caught its own
    // exception cleared the embedder's along with it, and the handler then said
    // nothing had happened.
    ub_test::Fixture fixture;

    const auto object = ub_test::Eval(fixture.context, R"(({
        get bad() { throw new Error('one'); },
        get fine() { try { throw new Error('two'); } catch (e) {} return 5; },
    }))")
                            .To<ub::Object>();
    REQUIRE(object.has_value());
    {
        ub::TryCatch handler(fixture.iso());
        // NOLINTBEGIN(bugprone-unchecked-optional-access) - REQUIRE above guarantees has_value
        CHECK_FALSE(object->Get(fixture.context, "bad").has_value());
        const auto fine = object->Get(fixture.context, "fine");
        // NOLINTEND(bugprone-unchecked-optional-access)
        REQUIRE(fine.has_value());
        CHECK(fine->ToInt32(fixture.context).value_or(0) == 5);
        REQUIRE(handler.HasCaught());
        CHECK(handler.Message(fixture.context).value_or("").find("one") != std::string::npos);
    }
    {
        // The same with the embedder's own throw, and a whole script after it.
        ub::TryCatch handler(fixture.iso());
        fixture.iso().ThrowError(ub::ErrorKind::RangeError, "mine");
        CHECK(ub_test::EvalInt(fixture.context, "try { throw 1; } catch (e) {} 6") == 6);
        REQUIRE(handler.HasCaught());
        CHECK(handler.Message(fixture.context).value_or("").find("mine") != std::string::npos);
    }

    // What the fix must not do: a throw inside a native callback, with no
    // handler of the callback's own, belongs to the script that called it -
    // even when the callback goes on to do more work - and not to the
    // embedder's handler further out.
    const auto native = ub::Function::New(
        fixture.context, +[](const ub::CallbackInfo& info) {
            const auto target = info[0].To<ub::Object>();
            if (target) {
                (void)target->Get(info.GetContext(), "bad");
                (void)ub::Object::New(info.GetContext());
            }
        });
    REQUIRE(native.has_value());
    ub_test::Expose(fixture.context, "native", *native);
    ub_test::Expose(fixture.context, "target", *object);
    ub::TryCatch outer(fixture.iso());
    CHECK(ub_test::EvalText(fixture.context, "try { native(target); 'nothing'; } catch (e) { e.message; }") == "one");
    CHECK_FALSE(outer.HasCaught());
}

UNIBIND_TEST_CASE(OBJECTS, "regressions: a proxy's prototype is the one its trap answers") {
    // `GetPrototype` is `Object.getPrototypeOf`, and on a proxy that is the
    // `getPrototypeOf` trap. V8's own call reads the proxy's map instead - the
    // trap never runs, the answer is null, and a trap that throws does not.
    ub_test::Fixture fixture;

    const auto answered =
        ub_test::Eval(fixture.context, "new Proxy({}, { getPrototypeOf() { return Array.prototype; } })")
            .To<ub::Object>();
    REQUIRE(answered.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - each REQUIRE guarantees has_value
    const auto prototype = answered->GetPrototype(fixture.context);
    REQUIRE(prototype.has_value());
    ub_test::Expose(fixture.context, "prototype", *prototype);
    CHECK(ub_test::EvalTruth(fixture.context, "prototype === Array.prototype"));

    // A proxy with no trap answers for its target, and a proxy of a proxy asks
    // the inner one.
    const auto nested = ub_test::Eval(fixture.context, R"(
        new Proxy(new Proxy({}, { getPrototypeOf() { return Map.prototype; } }), {}))")
                            .To<ub::Object>();
    REQUIRE(nested.has_value());
    const auto inner = nested->GetPrototype(fixture.context);
    REQUIRE(inner.has_value());
    ub_test::Expose(fixture.context, "inner", *inner);
    CHECK(ub_test::EvalTruth(fixture.context, "inner === Map.prototype"));

    for (const char* source : {"new Proxy({}, { getPrototypeOf() { throw new Error('trap'); } })",
                               "(() => { const r = Proxy.revocable({}, {}); r.revoke(); return r.proxy; })()",
                               "new Proxy({}, { getPrototypeOf() { return 5; } })"}) {
        CAPTURE(source);
        const auto refusing = ub_test::Eval(fixture.context, source).To<ub::Object>();
        REQUIRE(refusing.has_value());
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(refusing->GetPrototype(fixture.context).has_value());
        CHECK(handler.HasCaught());
    }
    // NOLINTEND(bugprone-unchecked-optional-access)
}

UNIBIND_TEST_CASE(OBJECTS, "regressions: a prototype that cannot be set is an exception, on every object") {
    // `SetPrototype` is `Object.setPrototypeOf`: true when it took, and a
    // TypeError when the object refuses. V8's own call swallows that TypeError
    // - and anything a proxy's trap throws - and reports true for a prototype
    // that is neither an object nor null without setting anything; the other
    // engine refused that last one with nothing pending at all.
    ub_test::Fixture fixture;
    const auto& context = fixture.context;

    const auto plain = ub::Object::New(context);
    REQUIRE(plain.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) - each REQUIRE guarantees has_value
    CHECK(plain->SetPrototype(context, ub::Null(fixture.iso())).value_or(false));
    CHECK(plain->GetPrototype(context)->IsNull());

    const auto refusals = std::vector<std::pair<const char*, const char*>>{
        {"Object.preventExtensions({})", "({})"},
        {"new Proxy({}, { setPrototypeOf() { return false; } })", "({})"},
        {"new Proxy({}, { setPrototypeOf() { throw new Error('mine'); } })", "({})"},
        {"(() => { const r = Proxy.revocable({}, {}); r.revoke(); return r.proxy; })()", "({})"},
        {"globalThis.cycle = {}", "Object.create(cycle)"},
        {"({})", "5"},
        {"({})", "'text'"},
    };
    for (const auto& [objectSource, prototypeSource] : refusals) {
        CAPTURE(objectSource);
        CAPTURE(prototypeSource);
        const auto object = ub_test::Eval(context, objectSource).To<ub::Object>();
        REQUIRE(object.has_value());
        const auto prototype = ub_test::Eval(context, prototypeSource);
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(object->SetPrototype(context, prototype).has_value());
        REQUIRE(handler.HasCaught());
        ub_test::Expose(context, "thrown", handler.Exception());
        if (std::string_view(objectSource).find("mine") != std::string_view::npos) {
            CHECK(ub_test::EvalText(context, "thrown.message") == "mine");
        } else {
            CHECK(ub_test::EvalTruth(context, "thrown instanceof TypeError"));
        }
    }

    // A trap that agrees is agreed with, and one that does the work is seen.
    const auto agreeing = ub_test::Eval(context, R"(
        new Proxy({}, { setPrototypeOf(target, p) { return Reflect.setPrototypeOf(target, p); } }))")
                              .To<ub::Object>();
    REQUIRE(agreeing.has_value());
    const auto array = ub_test::Eval(context, "Array.prototype");
    CHECK(agreeing->SetPrototype(context, array).value_or(false));
    ub_test::Expose(context, "agreeing", *agreeing);
    CHECK(ub_test::EvalTruth(context, "Object.getPrototypeOf(agreeing) === Array.prototype"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

UNIBIND_TEST_CASE(OBJECTS, "regressions: an own key that is an array index is a number, however large") {
    // An own key that is an array index - 0 to 2^32 - 2 - comes back as a
    // Number, as V8 hands it back; any other string key as a String. The other
    // engine keeps an index above INT32_MAX as a string internally and handed
    // it back as one.
    ub_test::Fixture fixture;

    const auto object = ub_test::Eval(fixture.context, R"(
        ({ 7: 0, 2147483647: 1, 2147483648: 2, 4294967294: 3, 4294967295: 4, '01': 5, x: 6 }))")
                            .To<ub::Object>();
    REQUIRE(object.has_value());
    const auto keys = object->GetOwnPropertyNames(fixture.context);  // NOLINT(bugprone-unchecked-optional-access)
    REQUIRE(keys.has_value());
    REQUIRE(keys->Length() == 7);

    std::vector<std::string> seen;
    for (std::uint32_t at = 0; at < keys->Length(); ++at) {
        const auto key = keys->Get(fixture.context, at);
        REQUIRE(key.has_value());
        const auto text = key->ToString(fixture.context);
        REQUIRE(text.has_value());
        seen.push_back((key->IsNumber() ? "number " : "string ") + text->Utf8Value());
    }
    CHECK(seen == std::vector<std::string>{"number 7", "number 2147483647", "number 2147483648", "number 4294967294",
                                           "string 4294967295", "string 01", "string x"});

    // The same through a proxy, whose keys arrive from its trap as strings.
    const auto proxy = ub_test::Eval(fixture.context, R"(
        new Proxy({}, {
            ownKeys() { return ['3000000000', 'y']; },
            getOwnPropertyDescriptor() { return { value: 1, enumerable: true, configurable: true }; },
        }))")
                           .To<ub::Object>();
    REQUIRE(proxy.has_value());
    const auto proxyKeys = proxy->GetOwnPropertyNames(fixture.context);  // NOLINT(bugprone-unchecked-optional-access)
    REQUIRE(proxyKeys.has_value());
    REQUIRE(proxyKeys->Length() == 2);
    CHECK(proxyKeys->Get(fixture.context, 0U)->IsNumber());  // NOLINT(bugprone-unchecked-optional-access)
    CHECK(proxyKeys->Get(fixture.context, 1U)->IsString());  // NOLINT(bugprone-unchecked-optional-access)
}

UNIBIND_TEST_CASE(BINARY_DATA, "regressions: a typed array over shared memory hands back no ArrayBuffer") {
    // `GetBuffer` on a view is empty when the buffer under it is a
    // `SharedArrayBuffer`: this API has no type for one, and a
    // `Local<ArrayBuffer>` over one is a handle whose type lies. The overload
    // for any view said so and kept to it; the typed-array overload - the one a
    // `Local<TypedArray>` actually picks - handed the shared buffer back.
    ub_test::Fixture fixture;
    if (!ub_test::EvalTruth(fixture.context, "typeof SharedArrayBuffer === 'function'")) {
        ub_test::ReportSkip("this realm has no SharedArrayBuffer, so script cannot make a view over one");
        return;
    }

    const auto view =
        ub_test::Eval(fixture.context, "new Uint8Array(new SharedArrayBuffer(4)).fill(7)").To<ub::TypedArray>();
    REQUIRE(view.has_value());
    CHECK_FALSE(ub::GetBuffer(fixture.context, *view).has_value());
    CHECK_FALSE(ub::GetBuffer(fixture.context, ub::Local<ub::ArrayBufferView>(*view)).has_value());

    // The elements are still readable; only the buffer has no type here.
    std::array<std::uint8_t, 4> out{};
    CHECK(ub::CopyElements<std::uint8_t>(*view, out) == 4);
    CHECK(out == std::array<std::uint8_t, 4>{7, 7, 7, 7});
}

UNIBIND_TEST_CASE(SERIALIZATION, "regressions: a value that will not clone says why, as a caught exception") {
    // The failure convention: an operation that produced no value because
    // something threw leaves the exception for the caller's `TryCatch`. Both
    // engines throw here - a DataCloneError for a value that will not clone, a
    // getter's own error for a getter that throws, an error for a blob that is
    // not one. One backend caught and discarded all three, so a getter's
    // exception vanished and the caller could not tell why nothing came back.
    ub_test::Fixture fixture;

    const auto function = ub_test::Eval(fixture.context, "(function () {})");
    {
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(ub::Serialize(fixture.context, function).has_value());
        CHECK(handler.HasCaught());
    }
    const auto getter = ub_test::Eval(fixture.context, "({ get x() { throw new Error('mine'); } })");
    {
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(ub::Serialize(fixture.context, getter).has_value());
        REQUIRE(handler.HasCaught());
        CHECK(handler.Message(fixture.context).value_or("").find("mine") != std::string::npos);
    }
    {
        ub::TryCatch handler(fixture.iso());
        const std::vector<std::uint8_t> garbage{1, 2, 3, 4, 5};
        CHECK_FALSE(ub::Deserialize(fixture.context, garbage).has_value());
        CHECK(handler.HasCaught());
    }
    CHECK(ub_test::EvalInt(fixture.context, "6 * 7") == 42);
}

UNIBIND_TEST_CASE(SCRIPTS, "regressions: source that is not UTF-8 is a syntax error, and says so") {
    // "Empty if the source did not compile; the syntax error is pending."
    // Source is UTF-8 text, so bytes that are not UTF-8 do not compile - and
    // the caller's `TryCatch` has to hear why. One engine's tokenizer throws a
    // SyntaxError; the other backend could not make a string of the source at
    // all and answered empty with nothing thrown, which is the failure that
    // looks like success to a caller checking `HasCaught`.
    ub_test::Fixture fixture;
    const std::string_view bad("1 + \xFF", 5);

    {
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(ub::Script::Compile(fixture.context, bad).has_value());
        REQUIRE(handler.HasCaught());
        ub_test::Expose(fixture.context, "thrown", handler.Exception());
        CHECK(ub_test::EvalTruth(fixture.context, "thrown instanceof SyntaxError"));
    }
    {
        ub::TryCatch handler(fixture.iso());
        CHECK_FALSE(ub::Evaluate(fixture.context, bad).has_value());
        CHECK(handler.HasCaught());
    }
}

UNIBIND_TEST_CASE(ARRAYS, "regressions: a proxy over an array is an object, not an array") {
    // An `Array` handle is one `Length()` can answer without running script -
    // it is noexcept - so it is a real array, as V8's `IsArray` says. A proxy
    // over one answers its length through a trap, which is script. One backend
    // asked the language's `Array.isArray`, which sees through proxies, and
    // called a proxy an Array.
    ub_test::Fixture fixture;

    const auto proxy = ub_test::Eval(fixture.context, "new Proxy([1, 2, 3], {})");
    CHECK(proxy.Kind() == ub::ValueKind::Object);
    CHECK_FALSE(proxy.IsArray());
    CHECK_FALSE(proxy.Is<ub::Array>());
    CHECK_FALSE(proxy.To<ub::Array>().has_value());
    CHECK(proxy.Is<ub::Object>());

    // Script still sees what the language says it sees.
    ub_test::Expose(fixture.context, "proxy", proxy);
    CHECK(ub_test::EvalTruth(fixture.context, "Array.isArray(proxy)"));
}
