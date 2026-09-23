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
