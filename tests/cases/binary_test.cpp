/// \file
/// Binary data, and moving a value between isolates.
///
/// Two areas that arrive together because they are the same question asked
/// twice: how does an embedder get bulk data in and out without the API
/// handing it a pointer into engine storage that the next collection
/// invalidates? The answer both times is a copy, and these cases are what hold
/// it to "the same bytes, the same width".
///
/// The rule for serialization, from `unibind/value.h`: a blob is **opaque bytes
/// belonging to one engine build**. So no case here asserts anything about what
/// is in one, and none compares one across backends - only that a value written
/// in one isolate of this engine comes back in another with equal contents and
/// an identity of its own.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "support/harness.h"
#include "support/ownership.h"

namespace {

/// A native for the "this cannot be cloned" case.
std::unique_ptr<ub_test::Tracked> MakeTracked(const ub::CallbackInfo& /*info*/) {
    return std::make_unique<ub_test::Tracked>(1);
}

}  // namespace

UNIBIND_TEST_CASE(BINARY_DATA, "binary: a span handed over comes back with the same bytes and the same width") {
    ub_test::Fixture fixture;

    constexpr std::array<float, 4> ORIGINAL{1.5F, -2.25F, 0.0F, 1024.5F};
    const auto view = ub::TypedArray::New<float>(fixture.context, ORIGINAL);
    REQUIRE(view.has_value());

    CHECK(ub::GetElementType(*view) == ub::ElementType::Float32);
    CHECK(ub::Length(*view) == ORIGINAL.size());
    CHECK(ub::ByteOffset(*view) == 0);

    std::array<float, 4> readBack{};
    CHECK(ub::CopyElements<float>(*view, readBack) == ORIGINAL.size());
    CHECK(readBack == ORIGINAL);

    // And script sees the width too, which is the half a byte-only API would
    // have lost.
    ub_test::Expose(fixture.context, "view", *view);
    CHECK(ub_test::EvalText(fixture.context, "view.constructor.name") == "Float32Array");
    CHECK(ub_test::EvalNumber(fixture.context, "view[3]") == 1024.5);
    CHECK(ub_test::EvalInt(fixture.context, "view.length") == 4);
}

UNIBIND_TEST_CASE(BINARY_DATA, "binary: every element width this API names survives the round trip") {
    ub_test::Fixture fixture;

    const auto roundTrip = [&fixture]<class T>(std::span<const T> elements, ub::ElementType expected,
                                               std::string_view constructorName) {
        const auto view = ub::TypedArray::New<T>(fixture.context, elements);
        REQUIRE(view.has_value());
        CHECK(ub::GetElementType(*view) == expected);
        CHECK(ub::Length(*view) == elements.size());

        std::vector<T> out(elements.size());
        CHECK(ub::CopyElements<T>(*view, out) == elements.size());
        CHECK(std::equal(out.begin(), out.end(), elements.begin()));

        ub_test::Expose(fixture.context, "probe", *view);
        CHECK(ub_test::EvalText(fixture.context, "probe.constructor.name") == std::string(constructorName));
    };

    constexpr std::array<std::int8_t, 3> I8{-1, 0, 127};
    constexpr std::array<std::uint8_t, 3> U8{0, 128, 255};
    constexpr std::array<std::int16_t, 3> I16{-32768, 0, 32767};
    constexpr std::array<std::uint16_t, 3> U16{0, 1234, 65535};
    constexpr std::array<std::int32_t, 3> I32{-2000000000, 0, 2000000000};
    constexpr std::array<std::uint32_t, 3> U32{0, 4000000000U, 4294967295U};
    constexpr std::array<float, 3> F32{-1.5F, 0.0F, 2.5F};
    constexpr std::array<double, 3> F64{-1.0e300, 0.0, 1.0e300};

    roundTrip(std::span<const std::int8_t>(I8), ub::ElementType::Int8, "Int8Array");
    roundTrip(std::span<const std::uint8_t>(U8), ub::ElementType::Uint8, "Uint8Array");
    roundTrip(std::span<const std::int16_t>(I16), ub::ElementType::Int16, "Int16Array");
    roundTrip(std::span<const std::uint16_t>(U16), ub::ElementType::Uint16, "Uint16Array");
    roundTrip(std::span<const std::int32_t>(I32), ub::ElementType::Int32, "Int32Array");
    roundTrip(std::span<const std::uint32_t>(U32), ub::ElementType::Uint32, "Uint32Array");
    roundTrip(std::span<const float>(F32), ub::ElementType::Float32, "Float32Array");
    roundTrip(std::span<const double>(F64), ub::ElementType::Float64, "Float64Array");
}

UNIBIND_TEST_CASE(BINARY_DATA, "binary: reading a view at the wrong width copies nothing rather than converting") {
    // The "looks like it succeeded" failure this API exists to prevent: a
    // Float64Array read as integers is a mistake, not a rounding.
    ub_test::Fixture fixture;

    constexpr std::array<double, 3> ORIGINAL{1.0, 2.0, 3.0};
    const auto view = ub::TypedArray::New<double>(fixture.context, ORIGINAL);
    REQUIRE(view.has_value());

    std::array<std::int32_t, 3> wrongWidth{9, 9, 9};
    CHECK(ub::CopyElements<std::int32_t>(*view, wrongWidth) == 0);
    CHECK(wrongWidth == std::array<std::int32_t, 3>{9, 9, 9});
}

UNIBIND_TEST_CASE(BINARY_DATA, "binary: bytes come back out of a buffer script made") {
    ub_test::Fixture fixture;

    const auto made = ub_test::Eval(fixture.context, "new Uint8Array([1, 2, 3, 250]).buffer");
    const auto buffer = made.To<ub::ArrayBuffer>();
    REQUIRE(buffer.has_value());
    CHECK(ub::ByteLength(*buffer) == 4);

    std::array<std::byte, 4> bytes{};
    CHECK(ub::CopyBytes(*buffer, bytes) == 4);
    CHECK(std::to_integer<int>(bytes[0]) == 1);
    CHECK(std::to_integer<int>(bytes[3]) == 250);

    // A destination that does not fit takes what it can and says how much.
    std::array<std::byte, 2> half{};
    CHECK(ub::CopyBytes(*buffer, half) == 2);
    CHECK(std::to_integer<int>(half[1]) == 2);
}

UNIBIND_TEST_CASE(BINARY_DATA, "binary: a view over part of a buffer sees only its window") {
    ub_test::Fixture fixture;

    constexpr std::array<std::uint8_t, 8> BYTES{0, 1, 2, 3, 4, 5, 6, 7};
    const auto buffer = ub::ArrayBuffer::New(fixture.context, std::as_bytes(std::span<const std::uint8_t>(BYTES)));
    REQUIRE(buffer.has_value());
    CHECK(ub::ByteLength(*buffer) == 8);

    const auto window = ub::TypedArray::New(fixture.context, ub::ElementType::Uint8, *buffer, 4, 3);
    REQUIRE(window.has_value());
    CHECK(ub::Length(*window) == 3);
    CHECK(ub::ByteOffset(*window) == 4);

    std::array<std::uint8_t, 3> out{};
    CHECK(ub::CopyElements<std::uint8_t>(*window, out) == 3);
    CHECK(out == std::array<std::uint8_t, 3>{4, 5, 6});

    // The buffer under a view is the buffer it was made over.
    const auto under = ub::GetBuffer(fixture.context, *window);
    REQUIRE(under.has_value());
    CHECK(under->StrictEquals(*buffer));

    // A window that does not fit is refused rather than clamped.
    CHECK_FALSE(ub::TypedArray::New(fixture.context, ub::ElementType::Uint8, *buffer, 6, 4).has_value());
}

UNIBIND_TEST_CASE(BINARY_DATA, "binary: a view whose length cannot be counted in bytes is refused") {
    // `length` is in elements, so the bytes it asks for are a multiplication -
    // and a length near the top of `size_t` makes that product *wrap*, which a
    // bounds check written as `byteOffset + length * width > byteLength` reads
    // as a view that fits. The header promises an empty answer for a view that
    // does not fit, and this is the shape of "does not fit" that arrives
    // looking like success.
    ub_test::Fixture fixture;

    const auto buffer = ub::ArrayBuffer::New(fixture.context, 8);
    REQUIRE(buffer.has_value());

    constexpr auto MAX = std::numeric_limits<std::size_t>::max();
    for (const auto type : {ub::ElementType::Int16, ub::ElementType::Int32, ub::ElementType::Float64}) {
        const std::size_t width = ub::ElementSize(type);
        CHECK_FALSE(ub::TypedArray::New(fixture.context, type, *buffer, 0, (MAX / width) + 1).has_value());
        CHECK_FALSE(ub::TypedArray::New(fixture.context, type, *buffer, 0, MAX).has_value());
    }
    // And an offset past the end of the buffer, which the same wrap hides.
    CHECK_FALSE(ub::TypedArray::New(fixture.context, ub::ElementType::Uint8, *buffer, MAX, 1).has_value());
}

UNIBIND_TEST_CASE(BINARY_DATA, "binary: an empty buffer is a buffer") {
    ub_test::Fixture fixture;

    const auto buffer = ub::ArrayBuffer::New(fixture.context, 0);
    REQUIRE(buffer.has_value());
    CHECK(ub::ByteLength(*buffer) == 0);

    const auto view = ub::TypedArray::New(fixture.context, ub::ElementType::Uint8, *buffer, 0, 0);
    REQUIRE(view.has_value());
    CHECK(ub::Length(*view) == 0);

    ub_test::Expose(fixture.context, "empty", *view);
    CHECK(ub_test::EvalInt(fixture.context, "empty.length") == 0);
}

UNIBIND_TEST_CASE(BINARY_DATA, "binary: a fresh buffer of a size is zeroed") {
    ub_test::Fixture fixture;

    const auto buffer = ub::ArrayBuffer::New(fixture.context, 16);
    REQUIRE(buffer.has_value());
    CHECK(ub::ByteLength(*buffer) == 16);

    std::array<std::byte, 16> bytes{};
    bytes.fill(std::byte{0xFF});
    CHECK(ub::CopyBytes(*buffer, bytes) == 16);
    for (const std::byte at : bytes) {
        CHECK(std::to_integer<int>(at) == 0);
    }
}

// ---------------------------------------------------------------------------
// Views in general: a typed array or a DataView, asked in bytes
// ---------------------------------------------------------------------------

namespace {

/// Eight bytes, 0 through 7, so that which bytes a copy took says where it
/// started.
[[nodiscard]] ub::Local<ub::ArrayBuffer> Counting(const ub::Context& context) {
    constexpr std::array<std::uint8_t, 8> BYTES{0, 1, 2, 3, 4, 5, 6, 7};
    const auto buffer = ub::ArrayBuffer::New(context, std::as_bytes(std::span<const std::uint8_t>(BYTES)));
    REQUIRE(buffer.has_value());
    return *buffer;
}

}  // namespace

UNIBIND_TEST_CASE(ARRAY_BUFFER_VIEWS, "views: a DataView and a typed array are both views, and nothing else is") {
    ub_test::Fixture fixture;

    const auto buffer = Counting(fixture.context);
    const auto dataView = ub::DataView::New(fixture.context, buffer, 2, 4);
    REQUIRE(dataView.has_value());
    const auto typed = ub::TypedArray::New(fixture.context, ub::ElementType::Uint8, buffer, 0, 8);
    REQUIRE(typed.has_value());

    const ub::Local<ub::Value> asDataView = *dataView;
    const ub::Local<ub::Value> asTyped = *typed;
    const ub::Local<ub::Value> asBuffer = buffer;

    CHECK(asDataView.IsArrayBufferView());
    CHECK(asDataView.IsDataView());
    CHECK_FALSE(asDataView.IsTypedArray());
    CHECK(asDataView.To<ub::ArrayBufferView>().has_value());
    CHECK(asDataView.To<ub::DataView>().has_value());
    CHECK_FALSE(asDataView.To<ub::TypedArray>().has_value());

    CHECK(asTyped.IsArrayBufferView());
    CHECK_FALSE(asTyped.IsDataView());
    CHECK(asTyped.To<ub::ArrayBufferView>().has_value());
    CHECK_FALSE(asTyped.To<ub::DataView>().has_value());

    CHECK_FALSE(asBuffer.IsArrayBufferView());
    CHECK_FALSE(asBuffer.IsDataView());
    const ub::Local<ub::Value> plain = ub_test::Eval(fixture.context, "({ byteLength: 4, buffer: null })");
    CHECK_FALSE(plain.IsArrayBufferView());
    CHECK_FALSE(plain.To<ub::DataView>().has_value());

    // A view is still an object, and nothing more specific than that to `Kind`.
    CHECK(asDataView.IsObject());
    CHECK(asDataView.Kind() == ub::ValueKind::Object);

    // Script sees the DataView it was given, over the bytes it was given.
    ub_test::Expose(fixture.context, "dv", *dataView);
    CHECK(ub_test::EvalTruth(fixture.context, "dv instanceof DataView"));
    CHECK(ub_test::EvalInt(fixture.context, "dv.byteOffset") == 2);
    CHECK(ub_test::EvalInt(fixture.context, "dv.byteLength") == 4);
    CHECK(ub_test::EvalInt(fixture.context, "dv.getUint8(0)") == 2);
    CHECK(ub_test::EvalInt(fixture.context, "dv.getUint16(0)") == 0x0203);  // big-endian unless asked
}

UNIBIND_TEST_CASE(ARRAY_BUFFER_VIEWS, "views: a view's bytes are its own range of the buffer") {
    // The property the byte-level questions exist for, and the one a copy that
    // started at the buffer would get wrong for any view not at offset zero.
    ub_test::Fixture fixture;

    const auto buffer = Counting(fixture.context);

    SUBCASE("a typed array part way in, counted in bytes rather than elements") {
        const auto view = ub::TypedArray::New(fixture.context, ub::ElementType::Uint16, buffer, 2, 2);
        REQUIRE(view.has_value());
        const ub::Local<ub::ArrayBufferView> asView = *view;
        CHECK(ub::Length(*view) == 2);  // elements
        CHECK(ub::ByteLength(asView) == 4);
        CHECK(ub::ByteOffset(asView) == 2);

        std::array<std::byte, 8> out{};
        out.fill(std::byte{0xEE});
        CHECK(ub::CopyBytes(*view, out) == 4);
        CHECK(std::to_integer<int>(out[0]) == 2);
        CHECK(std::to_integer<int>(out[3]) == 5);
        CHECK(std::to_integer<int>(out[4]) == 0xEE);  // nothing past the view's end
    }

    SUBCASE("a DataView part way in") {
        const auto view = ub::DataView::New(fixture.context, buffer, 5, 3);
        REQUIRE(view.has_value());
        CHECK(ub::ByteLength(*view) == 3);
        CHECK(ub::ByteOffset(*view) == 5);

        std::array<std::byte, 3> out{};
        CHECK(ub::CopyBytes(*view, out) == 3);
        CHECK(std::to_integer<int>(out[0]) == 5);
        CHECK(std::to_integer<int>(out[2]) == 7);

        // A destination that does not fit takes the front of the view.
        std::array<std::byte, 2> half{};
        CHECK(ub::CopyBytes(*view, half) == 2);
        CHECK(std::to_integer<int>(half[1]) == 6);

        // The buffer under it is the buffer it was made over.
        const auto under = ub::GetBuffer(fixture.context, *view);
        REQUIRE(under.has_value());
        CHECK(under->StrictEquals(buffer));
    }

    SUBCASE("an empty DataView at the very end") {
        const auto view = ub::DataView::New(fixture.context, buffer, 8, 0);
        REQUIRE(view.has_value());
        CHECK(ub::ByteLength(*view) == 0);
        CHECK(ub::ByteOffset(*view) == 8);
        std::array<std::byte, 4> out{};
        CHECK(ub::CopyBytes(*view, out) == 0);
    }
}

UNIBIND_TEST_CASE(ARRAY_BUFFER_VIEWS, "views: a DataView script made reads the same way") {
    ub_test::Fixture fixture;

    const auto made = ub_test::Eval(fixture.context, "new DataView(new Uint8Array([9, 8, 7, 6, 5]).buffer, 1, 3)");
    const auto view = made.To<ub::ArrayBufferView>();
    REQUIRE(view.has_value());
    CHECK(made.IsDataView());
    CHECK(ub::ByteLength(*view) == 3);
    CHECK(ub::ByteOffset(*view) == 1);

    std::array<std::byte, 3> out{};
    CHECK(ub::CopyBytes(*view, out) == 3);
    CHECK(std::to_integer<int>(out[0]) == 8);
    CHECK(std::to_integer<int>(out[2]) == 6);

    const auto under = ub::GetBuffer(fixture.context, *view);
    REQUIRE(under.has_value());
    CHECK(ub::ByteLength(*under) == 5);
}

UNIBIND_TEST_CASE(ARRAY_BUFFER_VIEWS, "views: a DataView that does not fit its buffer is refused") {
    ub_test::Fixture fixture;

    const auto buffer = Counting(fixture.context);
    CHECK(ub::DataView::New(fixture.context, buffer, 0, 8).has_value());
    CHECK_FALSE(ub::DataView::New(fixture.context, buffer, 0, 9).has_value());
    CHECK_FALSE(ub::DataView::New(fixture.context, buffer, 6, 3).has_value());
    CHECK_FALSE(ub::DataView::New(fixture.context, buffer, 9, 0).has_value());

    // The shapes that wrap: an offset and a length that add up past the top
    // of `size_t` must not read as a range that fits.
    constexpr auto MAX = std::numeric_limits<std::size_t>::max();
    CHECK_FALSE(ub::DataView::New(fixture.context, buffer, 4, MAX).has_value());
    CHECK_FALSE(ub::DataView::New(fixture.context, buffer, MAX, 2).has_value());

    // Refusing left nothing behind for script to trip over.
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE(ARRAY_BUFFER_VIEWS, "views: a view whose buffer script detached has no bytes") {
    ub_test::Fixture fixture;

    if (!ub_test::EvalTruth(fixture.context, "typeof ArrayBuffer.prototype.transfer === 'function'")) {
        ub_test::ReportSkip("this engine's ArrayBuffer has no transfer(), so script cannot detach one");
        return;
    }

    const auto buffer = Counting(fixture.context);
    const auto dataView = ub::DataView::New(fixture.context, buffer, 2, 4);
    const auto typed = ub::TypedArray::New(fixture.context, ub::ElementType::Uint8, buffer, 4, 4);
    REQUIRE(dataView.has_value());
    REQUIRE(typed.has_value());
    const ub::Local<ub::ArrayBufferView> typedView = *typed;

    ub_test::Expose(fixture.context, "detachMe", buffer);
    CHECK(ub_test::EvalInt(fixture.context, "detachMe.transfer().byteLength") == 8);
    CHECK(ub::ByteLength(buffer) == 0);

    for (const ub::Local<ub::ArrayBufferView>& view : {ub::Local<ub::ArrayBufferView>(*dataView), typedView}) {
        CHECK(ub::ByteLength(view) == 0);
        CHECK(ub::ByteOffset(view) == 0);
        std::array<std::byte, 8> out{};
        out.fill(std::byte{0xEE});
        CHECK(ub::CopyBytes(view, out) == 0);
        CHECK(std::to_integer<int>(out[0]) == 0xEE);
    }

    // And a new view over a detached buffer is not made at all.
    CHECK_FALSE(ub::DataView::New(fixture.context, buffer, 0, 0).has_value());
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

UNIBIND_TEST_CASE(SERIALIZATION, "serialization: a value written down is rebuilt with equal contents") {
    ub_test::Fixture fixture;

    const auto original = ub_test::Eval(fixture.context, R"(
        ({ n: 42, text: 'hello', flag: true, nothing: null, list: [1, 2, [3, 4]], nested: { deep: 'yes' } })
    )");

    const auto blob = ub::Serialize(fixture.context, original);
    REQUIRE(blob.has_value());
    CHECK_FALSE(blob->empty());

    const auto copy = ub::Deserialize(fixture.context, *blob);
    REQUIRE(copy.has_value());
    ub_test::Expose(fixture.context, "copy", *copy);

    CHECK(ub_test::EvalInt(fixture.context, "copy.n") == 42);
    CHECK(ub_test::EvalText(fixture.context, "copy.text") == "hello");
    CHECK(ub_test::EvalTruth(fixture.context, "copy.flag === true"));
    CHECK(ub_test::EvalTruth(fixture.context, "copy.nothing === null"));
    CHECK(ub_test::EvalInt(fixture.context, "copy.list[2][1]") == 4);
    CHECK(ub_test::EvalText(fixture.context, "copy.nested.deep") == "yes");
}

UNIBIND_TEST_CASE(SERIALIZATION, "serialization: what comes back is a copy, not the thing that went in") {
    ub_test::Fixture fixture;

    const auto original = ub_test::Eval(fixture.context, "({ tag: 1 })");
    const auto blob = ub::Serialize(fixture.context, original);
    REQUIRE(blob.has_value());

    const auto first = ub::Deserialize(fixture.context, *blob);
    const auto second = ub::Deserialize(fixture.context, *blob);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    CHECK_FALSE(first->StrictEquals(original));
    CHECK_FALSE(first->StrictEquals(*second));

    // Distinct identity means writing through one is not visible through the
    // other, which is the property a caller actually depends on.
    ub_test::Expose(fixture.context, "a", *first);
    ub_test::Expose(fixture.context, "b", *second);
    CHECK(ub_test::EvalInt(fixture.context, "a.tag = 9; b.tag") == 1);
}

UNIBIND_TEST_CASE(SERIALIZATION, "serialization: binary data survives the trip") {
    ub_test::Fixture fixture;

    constexpr std::array<std::int32_t, 4> ORIGINAL{7, -8, 9, -10};
    const auto view = ub::TypedArray::New<std::int32_t>(fixture.context, ORIGINAL);
    REQUIRE(view.has_value());

    const auto blob = ub::Serialize(fixture.context, *view);
    REQUIRE(blob.has_value());

    const auto copy = ub::Deserialize(fixture.context, *blob);
    REQUIRE(copy.has_value());
    const auto asView = copy->To<ub::TypedArray>();
    REQUIRE(asView.has_value());

    CHECK(ub::GetElementType(*asView) == ub::ElementType::Int32);
    std::array<std::int32_t, 4> out{};
    CHECK(ub::CopyElements<std::int32_t>(*asView, out) == 4);
    CHECK(out == ORIGINAL);
}

UNIBIND_TEST_CASE2(SERIALIZATION, CLASSES,
                   "serialization: a value that cannot be cloned is refused, not half-written") {
    ub_test::Fixture fixture;

    const auto cls = ub::Class<ub_test::Tracked>::New(fixture.iso(), "Native");
    cls.Construct<&MakeTracked>();
    ub_test::Expose(fixture.context, "Native", *cls.GetConstructor(fixture.context));

    const auto function = ub_test::Eval(fixture.context, "(function () {})");
    const auto instance = ub_test::Eval(fixture.context, "new Native()");
    const auto holding = ub_test::Eval(fixture.context, "({ fine: 1, inside: { bad: function () {} } })");

    CHECK_FALSE(ub::Serialize(fixture.context, function).has_value());
    CHECK_FALSE(ub::Serialize(fixture.context, instance).has_value());
    // The whole operation fails - not the offending part quietly becoming
    // undefined, which is what unibind/value.h rules out.
    CHECK_FALSE(ub::Serialize(fixture.context, holding).has_value());

    // Whatever the engine did while refusing, the context still works.
    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE(SERIALIZATION, "serialization: a damaged blob is refused rather than misread") {
    ub_test::Fixture fixture;

    const auto original = ub_test::Eval(fixture.context, "({ a: 1, b: 'two', c: [3] })");
    auto blob = ub::Serialize(fixture.context, original);
    REQUIRE(blob.has_value());
    REQUIRE(blob->size() > 4);

    ub::TryCatch handler(fixture.iso());

    auto damaged = *blob;
    for (auto& byte : damaged) {
        byte = static_cast<std::uint8_t>(~byte);
    }
    CHECK_FALSE(ub::Deserialize(fixture.context, damaged).has_value());
    handler.Reset();

    const std::vector<std::uint8_t> truncated(blob->begin(),
                                              blob->begin() + static_cast<std::ptrdiff_t>(blob->size() / 2));
    CHECK_FALSE(ub::Deserialize(fixture.context, truncated).has_value());
    handler.Reset();

    const std::vector<std::uint8_t> nothing;
    CHECK_FALSE(ub::Deserialize(fixture.context, nothing).has_value());
    handler.Reset();

    CHECK(ub_test::EvalInt(fixture.context, "1 + 1") == 2);
}

UNIBIND_TEST_CASE(SERIALIZATION, "serialization: a value moves from one isolate to another") {
    // The job the facility exists for. One isolate per thread is a rule
    // (decision 11), so two isolates is two threads, and the blob is what
    // crosses - a `std::vector` the receiving thread simply owns.
    ub_test::Fixture fixture;

    const auto original = ub_test::Eval(fixture.context, R"(
        ({ where: 'the first isolate', numbers: [1, 2, 3], nested: { deep: true } })
    )");
    const auto blob = ub::Serialize(fixture.context, original);
    REQUIRE(blob.has_value());

    // Read on the other thread, asserted on this one: doctest counts
    // assertions, and nothing here needs it to do so from two threads at once.
    std::string where;
    int sum = 0;
    bool deep = false;
    bool rebuilt = false;

    std::thread elsewhere([&] {
        auto isolate = ub::Isolate::New();
        if (isolate == nullptr) {
            return;
        }
        ub::HandleScope scope(*isolate);
        auto context = ub::Context::New(*isolate);
        if (!context) {
            return;
        }
        ub::ContextScope entered(*context);

        const auto copy = ub::Deserialize(*context, *blob);
        if (!copy) {
            return;
        }
        const auto asObject = copy->To<ub::Object>();
        if (!asObject) {
            return;
        }
        if (!context->GlobalObject().Set(*context, "arrived", *asObject).value_or(false)) {
            return;
        }
        const auto text = ub::Evaluate(*context, "arrived.where");
        const auto total = ub::Evaluate(*context, "arrived.numbers.reduce((a, b) => a + b, 0)");
        const auto flag = ub::Evaluate(*context, "arrived.nested.deep === true");
        if (!text || !total || !flag) {
            return;
        }
        where = text->ToString(*context).value().Utf8Value();
        sum = total->ToInt32(*context).value_or(0);
        deep = flag->ToBoolean(*context).value_or(false);
        rebuilt = true;
    });
    elsewhere.join();

    REQUIRE(rebuilt);
    CHECK(where == "the first isolate");
    CHECK(sum == 6);
    CHECK(deep);
}
