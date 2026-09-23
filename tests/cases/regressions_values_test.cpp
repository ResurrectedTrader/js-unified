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
