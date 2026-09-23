#pragma once
/// \file
/// The value tag types and their factories.
///
/// A tag is never instantiated - `ub::Object` is not an object, it is the
/// static type of a handle to one. The inheritance between tags is the
/// narrowing lattice: `Local<Function>` is a `Local<Object>` is a
/// `Local<Value>` implicitly, and going back the other way is
/// `To<Function>()`, which is checked.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "unibind/context.h"
#include "unibind/detail/backend.h"
#include "unibind/fwd.h"
#include "unibind/handle.h"
#include "unibind/isolate.h"
#include "unibind/types.h"

namespace ub {

namespace detail {

/// One step of a UTF-8 decoder: how many bytes the sequence at a position
/// takes, and whether they are a character.
struct Utf8Step {
    std::size_t length = 0;
    bool valid = false;
};

/// The sequence starting at `bytes[at]`, decoded the way the WHATWG Encoding
/// Standard decodes it - which is also the way V8 does.
///
/// When the bytes are not a character, `length` is the *maximal subpart*: the
/// longest prefix that could still have begun one. A lead byte whose next byte
/// is out of range stops there, so `E0 80` is two faults (E0 cannot be followed
/// by 80) while `E2 82` at the end of the input is one (a good start, cut
/// short). The narrowed ranges after E0, ED, F0 and F4 are what make an
/// overlong form, an encoded surrogate and anything above U+10FFFF each fail at
/// their second byte rather than decode.
[[nodiscard]] constexpr Utf8Step NextUtf8(std::string_view bytes, std::size_t at) noexcept {
    const auto lead = static_cast<std::uint8_t>(bytes[at]);
    if (lead < 0x80) {
        return {.length = 1, .valid = true};
    }
    std::size_t trailing = 0;
    std::uint8_t lower = 0x80;
    std::uint8_t upper = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        trailing = 1;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        trailing = 2;
        lower = lead == 0xE0 ? 0xA0 : lower;  // below that is an overlong form
        upper = lead == 0xED ? 0x9F : upper;  // above that is a surrogate
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        trailing = 3;
        lower = lead == 0xF0 ? 0x90 : lower;  // below that is an overlong form
        upper = lead == 0xF4 ? 0x8F : upper;  // above that is past U+10FFFF
    } else {
        return {.length = 1, .valid = false};  // a continuation byte, C0, C1, or F5 and up
    }
    std::size_t length = 1;
    for (; length <= trailing; ++length) {
        if (at + length >= bytes.size()) {
            return {.length = length, .valid = false};
        }
        const auto next = static_cast<std::uint8_t>(bytes[at + length]);
        if (next < lower || next > upper) {
            return {.length = length, .valid = false};
        }
        lower = 0x80;
        upper = 0xBF;
    }
    return {.length = length, .valid = true};
}

/// Where the first byte that is not part of a character is, or `npos` if every
/// byte is.
[[nodiscard]] constexpr std::size_t FirstInvalidUtf8(std::string_view bytes) noexcept {
    for (std::size_t at = 0; at < bytes.size();) {
        const Utf8Step step = NextUtf8(bytes, at);
        if (!step.valid) {
            return at;
        }
        at += step.length;
    }
    return std::string_view::npos;
}

/// `bytes`, with each maximal subpart `NextUtf8` finds that is not a character
/// replaced by U+FFFD. `firstInvalid` is where the first one is, so that the
/// scan that found it is not repeated over the valid prefix.
[[nodiscard]] inline std::string ReplaceInvalidUtf8(std::string_view bytes, std::size_t firstInvalid) {
    static constexpr std::string_view REPLACEMENT = "\xEF\xBF\xBD";
    std::string repaired(bytes.substr(0, firstInvalid));
    for (std::size_t at = firstInvalid; at < bytes.size();) {
        const Utf8Step step = NextUtf8(bytes, at);
        repaired.append(step.valid ? bytes.substr(at, step.length) : REPLACEMENT);
        at += step.length;
    }
    return repaired;
}

/// Throw an error whose message is `NewFromUtf8`'s decode of `message`.
///
/// A message is diagnostic text an embedder builds from whatever it has to
/// hand - a file path, a header, the text of something that failed - and V8
/// makes one with its lossy decoder. So every public way of throwing a fresh
/// error comes through here: one byte that is not UTF-8 costs a U+FFFD, not the
/// error, and the backends are handed valid text as they are everywhere else.
inline void ThrowErrorLossy(Isolate& isolate, ErrorKind kind, std::string_view message) {
    const std::size_t firstInvalid = FirstInvalidUtf8(message);
    if (firstInvalid == std::string_view::npos) {
        ThrowError(isolate, kind, message);
        return;
    }
    ThrowError(isolate, kind, ReplaceInvalidUtf8(message, firstInvalid));
}

}  // namespace detail

/// Base of every tag. Deleting the constructor makes the whole lattice
/// uninstantiable, which is the point: these types exist only as parameters.
struct Value {
    Value() = delete;
};

struct Primitive : Value {};

struct Boolean : Primitive {
    [[nodiscard]] static Local<Boolean> New(Isolate& isolate, bool value) noexcept {
        return Local<Boolean>::FromSlot(detail::MakeBoolean(isolate, value));
    }
};

struct Number : Primitive {
    [[nodiscard]] static Local<Number> New(Isolate& isolate, double value) noexcept {
        return Local<Number>::FromSlot(detail::MakeNumber(isolate, value));
    }
};

struct Integer : Number {
    [[nodiscard]] static Local<Integer> New(Isolate& isolate, std::int32_t value) noexcept {
        return Local<Integer>::FromSlot(detail::MakeInteger(isolate, value));
    }
    [[nodiscard]] static Local<Integer> NewFromUnsigned(Isolate& isolate, std::uint32_t value) noexcept {
        return Local<Integer>::FromSlot(detail::MakeUnsigned(isolate, value));
    }
};

/// A property key: a String or a Symbol, and nothing else.
struct Name : Primitive {};

struct String : Name {
    /// Empty if the bytes are not valid UTF-8 or the engine could not
    /// allocate. Takes a view, copies out of it - the engine owns its strings
    /// and a moving collector can relocate them, so there is no borrowing.
    ///
    /// Strict on purpose: a caller told it handed over text should not find a
    /// row of replacement characters where its text was. Bytes that are
    /// *meant* to be repaired - read off a socket, out of a file - are what
    /// `NewFromUtf8` is for.
    [[nodiscard]] static std::optional<Local<String>> New(Isolate& isolate, std::string_view utf8) {
        return detail::WrapSlot<String>(detail::MakeString(isolate, utf8));
    }

    /// V8's `NewFromUtf8`: bytes that are not UTF-8 are decoded anyway, each
    /// maximal invalid subsequence becoming one U+FFFD - the WHATWG Encoding
    /// Standard's rule, and the one V8 follows. Valid input gives exactly what
    /// `New` gives. Empty only if the engine could not allocate.
    ///
    /// The repair is done here rather than by either engine, and that is what
    /// makes the answer the same on both: two decoders agree about valid UTF-8
    /// and are under no obligation to agree about anything else - how many
    /// U+FFFD an overlong form or a truncated sequence is worth is exactly
    /// where they could differ. So the bytes are validated first and copied
    /// only when something in them needs replacing: valid input costs one scan
    /// and no copy, and what an engine is handed is always valid.
    [[nodiscard]] static std::optional<Local<String>> NewFromUtf8(Isolate& isolate, std::string_view bytes) {
        const std::size_t firstInvalid = detail::FirstInvalidUtf8(bytes);
        if (firstInvalid == std::string_view::npos) {
            return New(isolate, bytes);
        }
        return New(isolate, detail::ReplaceInvalidUtf8(bytes, firstInvalid));
    }
};

struct Symbol : Name {
    /// A fresh symbol, equal to nothing else.
    [[nodiscard]] static std::optional<Local<Symbol>> New(Isolate& isolate,
                                                          std::optional<std::string_view> description = {}) {
        return detail::WrapSlot<Symbol>(detail::MakeSymbol(isolate, description));
    }
    /// `Symbol.for(key)`: the same symbol for the same key, across realms.
    [[nodiscard]] static std::optional<Local<Symbol>> For(Isolate& isolate, std::string_view key) {
        return detail::WrapSlot<Symbol>(detail::MakeSymbolFor(isolate, key));
    }
    /// `Symbol.iterator` and friends, as property keys for declaration and
    /// lookup alike.
    [[nodiscard]] static std::optional<Local<Symbol>> WellKnown(Isolate& isolate, WellKnownSymbol which) {
        return detail::WrapSlot<Symbol>(detail::GetWellKnownSymbol(isolate, which));
    }
};

struct BigInt : Primitive {};

struct Object : Value {
    [[nodiscard]] static std::optional<Local<Object>> New(const Context& context) {
        return detail::WrapSlot<Object>(detail::MakeObject(context));
    }
};

struct Array : Object {
    /// An array of `length` holes, as `new Array(length)` makes one - at every
    /// length a `uint32_t` can say, which is every length an array can have.
    [[nodiscard]] static std::optional<Local<Array>> New(const Context& context, std::uint32_t length = 0) {
        return detail::WrapSlot<Array>(detail::MakeArray(context, length));
    }
};

struct Function : Object {
    /// A native function. `data` is the closure: a typed, non-owning pointer
    /// the callback recovers with `info.Data<D>()`.
    ///
    /// **The result is callable, not constructable.** `new f()` throws a
    /// TypeError and `NewInstance` fails the same way. A constructor is
    /// something an embedder asks for on purpose, which is what
    /// `FunctionTemplate` and `Class<T>` are for: a function that can be
    /// `new`-ed without anyone asking hands script a fresh empty object
    /// instead of the callback's result, and the callback cannot tell unless
    /// it thought to check `IsConstructCall()`. It is also what an ordinary
    /// JavaScript method or arrow function does, and the stricter of the two
    /// engine defaults - the engines disagreed here and this is the answer.
    [[nodiscard]] static std::optional<Local<Function>> New(const Context& context, FunctionCallback callback,
                                                            CallbackData data = {}) {
        return detail::WrapSlot<Function>(detail::MakeFunction(context, callback, data));
    }

    /// A native function whose closure is a *script value*, which the callback
    /// reads back as `info.Data()` - V8's `Function::New` with a `Local<Value>`
    /// data. One native callback, many functions, each carrying its own name
    /// or configuration, and nothing for the embedder to keep alive.
    ///
    /// The value lives exactly as long as the function: the collector reaches
    /// it through the function and through nothing else, so it is not taken
    /// while the function is reachable, and a value that refers back to its own
    /// function does not keep the pair alive. The overload above is for state
    /// the engine must not see; this one is for state that is already a value.
    /// A function has one or the other - `info.Data<D>()` is null in one made
    /// here.
    ///
    /// Callable and not constructable, exactly as the overload above.
    ///
    /// A template rather than a `Local<Value>` parameter so that `{}` for the
    /// data still means an empty `CallbackData`, rather than being ambiguous
    /// between the two.
    template <class T>
    [[nodiscard]] static std::optional<Local<Function>> New(const Context& context, FunctionCallback callback,
                                                            const Local<T>& data) {
        return detail::WrapSlot<Function>(detail::MakeFunctionWithValue(context, callback, data.slot()));
    }
};

// ---------------------------------------------------------------------------
// Binary data
//
// An embedder handing script a large numeric result - a grid, a decoded frame,
// a buffer read off a socket - should not do it one `Array::Set` at a time.
// That is N crossings of this boundary and N engine stores to build the type
// script did not want: an ordinary Array of doubles, where indexing a bulk
// numeric result wants a typed array and the element width is observable from
// script. So there is a bulk path, in both directions.
//
// **The bytes are always copied.** An `ArrayBuffer` made here owns engine
// memory holding a copy of what you passed, and reading one copies back out
// into your buffer. The alternative - a buffer backed by embedder memory -
// was considered and rejected on three counts, any one of which would be
// enough:
//
//   * **Detachment.** `ArrayBuffer.prototype.transfer`, and structured clone
//     with a transfer list, can detach a buffer from script. An embedder-backed
//     buffer then has memory whose ownership has moved, on a schedule script
//     controls, and the two engines hand it back on different terms.
//   * **No stable interior pointer.** Both collectors move objects. A pointer
//     *into* a buffer is only valid until the next operation that can collect,
//     which is every operation here, so a borrowed span would be a handle with
//     none of the rules handles have in this API.
//   * **Free-callback contracts differ.** V8's `BackingStore` deleter and
//     SpiderMonkey's external-buffer free hook are not called at the same time,
//     on the same thread, or under the same guarantees.
//
// What the copy costs is one `memcpy` in each direction, against N boundary
// crossings for the alternative that exists today. A future backed-buffer API
// is not precluded; it would be a separate factory with its own ownership
// rules, and it is not worth its rules yet.
//
// This does not disturb `ValueKind` (see `unibind/types.h`): a typed array still
// reports `Object`, because `Object` remains exactly the set of *property*
// operations the API offers for it. The new operations are reached through
// `Is<ArrayBuffer>()` / `To<TypedArray>()`, which answer without a kind of
// their own - which is what the argument for collapsing exotic objects into
// `Object` predicted would happen.
// ---------------------------------------------------------------------------

/// The `ElementType` a C++ arithmetic type maps onto, so that a
/// `std::span<const float>` becomes a `Float32Array` without anyone saying so
/// twice. Specialised below for exactly the types that map.
template <class T>
struct ElementTypeOfTag;

template <class T>
inline constexpr ElementType ElementTypeOf = ElementTypeOfTag<T>::value;

template <>
struct ElementTypeOfTag<std::int8_t> : std::integral_constant<ElementType, ElementType::Int8> {};
template <>
struct ElementTypeOfTag<std::uint8_t> : std::integral_constant<ElementType, ElementType::Uint8> {};
template <>
struct ElementTypeOfTag<std::int16_t> : std::integral_constant<ElementType, ElementType::Int16> {};
template <>
struct ElementTypeOfTag<std::uint16_t> : std::integral_constant<ElementType, ElementType::Uint16> {};
template <>
struct ElementTypeOfTag<std::int32_t> : std::integral_constant<ElementType, ElementType::Int32> {};
template <>
struct ElementTypeOfTag<std::uint32_t> : std::integral_constant<ElementType, ElementType::Uint32> {};
template <>
struct ElementTypeOfTag<float> : std::integral_constant<ElementType, ElementType::Float32> {};
template <>
struct ElementTypeOfTag<double> : std::integral_constant<ElementType, ElementType::Float64> {};
template <>
struct ElementTypeOfTag<std::int64_t> : std::integral_constant<ElementType, ElementType::BigInt64> {};
template <>
struct ElementTypeOfTag<std::uint64_t> : std::integral_constant<ElementType, ElementType::BigUint64> {};

/// Raw bytes script can share. An `ArrayBuffer` has no elements of its own; a
/// `TypedArray` is the view that gives it a width.
struct ArrayBuffer : Object {
    /// A buffer of `byteLength` zero bytes. Empty, with nothing thrown, if the
    /// engine cannot make one that long - a length past its maximum, or one it
    /// cannot allocate.
    [[nodiscard]] static std::optional<Local<ArrayBuffer>> New(const Context& context, std::size_t byteLength) {
        return detail::WrapSlot<ArrayBuffer>(detail::MakeArrayBuffer(context, {}, byteLength));
    }
    /// A buffer holding a copy of `bytes`.
    [[nodiscard]] static std::optional<Local<ArrayBuffer>> New(const Context& context,
                                                               std::span<const std::byte> bytes) {
        return detail::WrapSlot<ArrayBuffer>(detail::MakeArrayBuffer(context, bytes, bytes.size()));
    }
};

/// Any window onto an `ArrayBuffer`: a `TypedArray` or a `DataView`. V8's
/// `ArrayBufferView`, and the static type the byte-level questions below take -
/// the view's length and offset in bytes, the buffer under it, and a copy of
/// exactly its range - so that code which only moves bytes need not care which
/// kind of view script handed it.
///
/// Never made as itself: a view is always one of the two kinds.
struct ArrayBufferView : Object {};

/// A window onto an `ArrayBuffer` with an element width - `Uint8Array`,
/// `Float64Array` and the rest.
struct TypedArray : ArrayBufferView {
    /// A view over `buffer`. `length` is in *elements*; the view must lie
    /// inside the buffer and start at a multiple of its element size, and the
    /// buffer must not have been detached, or this is empty - each of those is
    /// an exception in script, and none is one here.
    [[nodiscard]] static std::optional<Local<TypedArray>> New(const Context& context, ElementType type,
                                                              const Local<ArrayBuffer>& buffer, std::size_t byteOffset,
                                                              std::size_t length) {
        return detail::WrapSlot<TypedArray>(detail::MakeTypedArray(context, type, buffer.slot(), byteOffset, length));
    }

    /// The whole job in one call: a fresh buffer holding a copy of `elements`,
    /// and a view of the matching width over all of it.
    template <class T>
    [[nodiscard]] static std::optional<Local<TypedArray>> New(const Context& context, std::span<const T> elements) {
        auto buffer = ArrayBuffer::New(context, std::as_bytes(elements));
        if (!buffer) {
            return std::nullopt;
        }
        return New(context, ElementTypeOf<T>, *buffer, 0, elements.size());
    }
};

/// A window onto an `ArrayBuffer` with no element width of its own: script
/// reads and writes it at whatever width and byte order each access names.
/// What a binary protocol is handed to script as, where a typed array's one
/// width would be the wrong shape.
struct DataView : ArrayBufferView {
    /// A view of `byteLength` bytes of `buffer`, starting `byteOffset` bytes
    /// in. Empty if that range does not lie inside the buffer, or if script has
    /// detached the buffer - a view over nothing is not one either engine would
    /// hand script.
    [[nodiscard]] static std::optional<Local<DataView>> New(const Context& context, const Local<ArrayBuffer>& buffer,
                                                            std::size_t byteOffset, std::size_t byteLength) {
        return detail::WrapSlot<DataView>(detail::MakeDataView(context, buffer.slot(), byteOffset, byteLength));
    }
};

// ---------------------------------------------------------------------------
// Promises, and the scope of them
//
// What is here: an embedder can make a pending promise, hand it to script, and
// settle it later. That is what makes an asynchronous native result expressible
// at all - a binding that starts work and answers when it finishes.
//
// **What is NOT here, and the consequence you will otherwise spend a day
// chasing: nothing drains the microtask queue by itself.** Script containing
// `async` / `await`, or any `.then`, compiles and runs and its *continuations
// simply never execute* - no error, no exception, the work just does not
// happen. `Isolate::PumpJobs()` is the drain, and an embedder that runs script
// with promises in it has to call it. This API deliberately takes the pump away
// from the engines - V8's default policy would drain after every call and
// SpiderMonkey's would never - so that a continuation runs at the same
// observable moment on both backends. A documented requirement rather than a
// bug; what would be a bug is not saying so.
//
// Also not here, deliberately, and not silently:
//
//   * **Attaching a native continuation** (`then` from C++). An embedder can do
//     it today by getting `then` off the promise and calling it with a
//     `Function::New`, which is one line and needs no new abstraction.
//   * **Unhandled-rejection reporting.** Both engines have a hook; they differ
//     in when it fires and in whether a later handler retracts it, and this API
//     does not promise a difference it cannot make uniform.
//   * **Async functions implemented in native code.** That is a generator
//     protocol, not a promise one.
// ---------------------------------------------------------------------------

/// A promise the embedder settles.
///
/// There is no separate resolver type. V8 has one (`Promise::Resolver`) and
/// SpiderMonkey does not - it settles the promise object itself - so the
/// portable shape is the one both can express: you hold the promise, and you
/// resolve or reject it. The consequence is that handing this value to script
/// hands script the power to settle it too, which is why an embedder gives
/// script the promise and keeps its own `Global` to settle from.
struct Promise : Object {
    /// A pending promise. Empty if the engine could not make one.
    [[nodiscard]] static std::optional<Local<Promise>> New(const Context& context) {
        return detail::WrapSlot<Promise>(detail::MakePromise(context));
    }
};

/// An opaque embedder pointer as a JavaScript value. Not an object: it has no
/// properties and script can only pass it around. Recovery is type-checked.
struct External : Value {
    template <class D>
        requires(!std::is_const_v<D>)
    [[nodiscard]] static std::optional<Local<External>> New(Isolate& isolate, D& data) {
        return detail::WrapSlot<External>(detail::MakeExternal(isolate, CallbackData::For(data)));
    }
};

// --- narrowing questions ---------------------------------------------------

template <>
struct TypeCodeOfTag<Value> : std::integral_constant<TypeCode, TypeCode::Value> {};
template <>
struct TypeCodeOfTag<Primitive> : std::integral_constant<TypeCode, TypeCode::Primitive> {};
template <>
struct TypeCodeOfTag<Boolean> : std::integral_constant<TypeCode, TypeCode::Boolean> {};
template <>
struct TypeCodeOfTag<Number> : std::integral_constant<TypeCode, TypeCode::Number> {};
template <>
struct TypeCodeOfTag<Integer> : std::integral_constant<TypeCode, TypeCode::Integer> {};
template <>
struct TypeCodeOfTag<Name> : std::integral_constant<TypeCode, TypeCode::Name> {};
template <>
struct TypeCodeOfTag<String> : std::integral_constant<TypeCode, TypeCode::String> {};
template <>
struct TypeCodeOfTag<Symbol> : std::integral_constant<TypeCode, TypeCode::Symbol> {};
template <>
struct TypeCodeOfTag<BigInt> : std::integral_constant<TypeCode, TypeCode::BigInt> {};
template <>
struct TypeCodeOfTag<Object> : std::integral_constant<TypeCode, TypeCode::Object> {};
template <>
struct TypeCodeOfTag<Array> : std::integral_constant<TypeCode, TypeCode::Array> {};
template <>
struct TypeCodeOfTag<Function> : std::integral_constant<TypeCode, TypeCode::Function> {};
template <>
struct TypeCodeOfTag<ArrayBuffer> : std::integral_constant<TypeCode, TypeCode::ArrayBuffer> {};
template <>
struct TypeCodeOfTag<ArrayBufferView> : std::integral_constant<TypeCode, TypeCode::ArrayBufferView> {};
template <>
struct TypeCodeOfTag<TypedArray> : std::integral_constant<TypeCode, TypeCode::TypedArray> {};
template <>
struct TypeCodeOfTag<DataView> : std::integral_constant<TypeCode, TypeCode::DataView> {};
template <>
struct TypeCodeOfTag<Promise> : std::integral_constant<TypeCode, TypeCode::Promise> {};
template <>
struct TypeCodeOfTag<External> : std::integral_constant<TypeCode, TypeCode::External> {};

// --- the singletons --------------------------------------------------------

[[nodiscard]] inline Local<Primitive> Undefined(Isolate& isolate) noexcept {
    return Local<Primitive>::FromSlot(detail::MakeUndefined(isolate));
}
[[nodiscard]] inline Local<Primitive> Null(Isolate& isolate) noexcept {
    return Local<Primitive>::FromSlot(detail::MakeNull(isolate));
}
[[nodiscard]] inline Local<Boolean> True(Isolate& isolate) noexcept {
    return Boolean::New(isolate, true);
}
[[nodiscard]] inline Local<Boolean> False(Isolate& isolate) noexcept {
    return Boolean::New(isolate, false);
}

// --- the pieces of Local<T> that need a complete tag ------------------------

static_assert(sizeof(Local<Value>) == sizeof(detail::Slot));
static_assert(std::is_trivially_copyable_v<Local<Value>>);
static_assert(std::is_trivially_destructible_v<Local<Value>>);
static_assert(!std::is_default_constructible_v<Object>, "tags must not be instantiable");

template <class T>
std::optional<Local<Value>> Local<T>::GetByName(const Context& context, std::string_view key) const {
    auto name = String::New(context.GetIsolate(), key);
    if (!name) {
        return std::nullopt;
    }
    return Wrap<Value>(detail::GetProperty(context, slot_, name->slot()));
}

template <class T>
std::optional<bool> Local<T>::SetByName(const Context& context, std::string_view key, detail::Slot value) const {
    auto name = String::New(context.GetIsolate(), key);
    if (!name) {
        return std::nullopt;
    }
    return detail::SetProperty(context, slot_, name->slot(), value);
}

/// The typed pointer inside an External, or null if it is not a `D`.
template <class D>
[[nodiscard]] inline D* ExternalValue(const Local<External>& external) noexcept {
    return detail::ExternalData(external.slot()).template As<D>();
}

// --- reading binary data back out ------------------------------------------

/// Bytes in this buffer. Zero for a buffer script has detached.
[[nodiscard]] inline std::size_t ByteLength(const Local<ArrayBuffer>& buffer) noexcept {
    return detail::ArrayBufferByteLength(buffer.slot());
}

/// Copy the buffer's bytes into `out`, truncating if it does not fit. Returns
/// how many bytes were written.
///
/// A copy, always, and `unibind/value.h` says at length why there is no borrowing
/// alternative. Ask `ByteLength` first if you mean to take all of it.
[[nodiscard]] inline std::size_t CopyBytes(const Local<ArrayBuffer>& buffer, std::span<std::byte> out) noexcept {
    return detail::ArrayBufferCopyOut(buffer.slot(), out);
}

[[nodiscard]] inline ElementType GetElementType(const Local<TypedArray>& view) noexcept {
    return detail::TypedArrayElementType(view.slot());
}
/// Elements, not bytes.
[[nodiscard]] inline std::size_t Length(const Local<TypedArray>& view) noexcept {
    return detail::TypedArrayLength(view.slot());
}
[[nodiscard]] inline std::size_t ByteOffset(const Local<TypedArray>& view) noexcept {
    return detail::TypedArrayByteOffset(view.slot());
}
/// The buffer this view looks at. Empty only if the engine could not hand it
/// over.
[[nodiscard]] inline std::optional<Local<ArrayBuffer>> GetBuffer(const Context& context,
                                                                 const Local<TypedArray>& view) {
    return detail::WrapSlot<ArrayBuffer>(detail::TypedArrayBuffer(context, view.slot()));
}

/// Copy the elements this view covers into `out`, truncating if it does not
/// fit. Returns how many *elements* were written.
///
/// Empty - nothing copied - if `T` is not the view's element type. This does
/// not convert: a `Float64Array` read as `std::int32_t` is a mistake, not a
/// rounding, and silently obliging would be the "looks like it succeeded"
/// failure this API exists to prevent.
template <class T>
[[nodiscard]] inline std::size_t CopyElements(const Local<TypedArray>& view, std::span<T> out) noexcept {
    if (detail::TypedArrayElementType(view.slot()) != ElementTypeOf<T>) {
        return 0;
    }
    return detail::TypedArrayCopyOut(view.slot(), std::as_writable_bytes(out)) / sizeof(T);
}

// --- reading any view, in bytes ---------------------------------------------
//
// V8's `ArrayBufferView` questions, for a typed array and a DataView alike. A
// typed array is a view, so these take one too; where a typed-array function
// above answers the same question in elements, these answer it in bytes.
//
// A view whose buffer script has detached has no bytes: its length and its
// offset are both zero and a copy writes nothing. That is V8's answer, and the
// other backend is made to give it. Zero is also the length of an empty view,
// and nothing here tells the two apart.

/// Bytes this view covers.
[[nodiscard]] inline std::size_t ByteLength(const Local<ArrayBufferView>& view) noexcept {
    return detail::ArrayBufferViewByteLength(view.slot());
}
/// Where this view starts in its buffer, in bytes.
[[nodiscard]] inline std::size_t ByteOffset(const Local<ArrayBufferView>& view) noexcept {
    return detail::ArrayBufferViewByteOffset(view.slot());
}
/// The buffer this view looks at - V8's `Buffer()`, spelled as the typed-array
/// overload above already spells it. Empty if the engine could not hand it
/// over, and empty if it is a `SharedArrayBuffer`: this API has no type for
/// one, so handing it back as an `ArrayBuffer` would be a handle whose type
/// lies. `CopyBytes` still reads a view over one.
[[nodiscard]] inline std::optional<Local<ArrayBuffer>> GetBuffer(const Context& context,
                                                                 const Local<ArrayBufferView>& view) {
    return detail::WrapSlot<ArrayBuffer>(detail::ArrayBufferViewBuffer(context, view.slot()));
}

/// Copy the bytes this view covers - its own range of its buffer, not the
/// buffer from the start - into `out`, truncating if it does not fit. Returns
/// how many bytes were written. V8's `CopyContents`.
///
/// A copy, always, for the reasons at the top of this section. Over a
/// `SharedArrayBuffer` it is a snapshot another thread may be writing while it
/// is taken, which is what shared memory is.
[[nodiscard]] inline std::size_t CopyBytes(const Local<ArrayBufferView>& view, std::span<std::byte> out) noexcept {
    return detail::ArrayBufferViewCopyOut(view.slot(), out);
}

// --- settling a promise ----------------------------------------------------

/// Fulfil `promise` with `value`. Empty if it threw; false if the promise was
/// already settled, which is not an error - it is what the language does.
///
/// The continuations this queues do not run until something drains the
/// microtask queue: `Isolate::PumpJobs()`.
template <class T>
[[nodiscard]] inline std::optional<bool> Resolve(const Context& context, const Local<Promise>& promise,
                                                 const Local<T>& value) {
    return detail::ResolvePromise(context, promise.slot(), value.slot());
}

/// Reject `promise` with `reason`. The same rules as `Resolve`.
template <class T>
[[nodiscard]] inline std::optional<bool> Reject(const Context& context, const Local<Promise>& promise,
                                                const Local<T>& reason) {
    return detail::RejectPromise(context, promise.slot(), reason.slot());
}

[[nodiscard]] inline PromiseState GetState(const Local<Promise>& promise) noexcept {
    return detail::PromiseStateOf(promise.slot());
}

// ---------------------------------------------------------------------------
// Moving a value to another isolate
//
// One isolate per thread is a rule (docs/status.md decision 11), so a program
// that runs several scripts runs several isolates, and a handle belongs to
// exactly one of them. The only portable way to get a value from one to
// another is to write it down and build it again, which is what the
// structured-clone algorithm is: V8 spells it `ValueSerializer`, SpiderMonkey
// spells it `JS_StructuredClone`, and the algorithm is specified, so the
// *semantics* agree even though nothing else does.
// ---------------------------------------------------------------------------

/// Write `value` down so another isolate of this engine can read it back.
///
/// **What a blob is: opaque bytes, belonging to the engine build that wrote
/// them, valid for as long as that build is the one running.** That is the
/// whole of its contract, and it is enough for the job it exists for - moving a
/// value from one isolate to another in this process. It is deliberately not an
/// interchange format: this library selects one engine at build time, values
/// move between isolates of *that* engine, and a format two engines could both
/// read would be a different and much larger thing to design. Keeping a blob
/// past the process - on disk, on a wire - is outside what it promises, and an
/// engine upgrade is where that goes wrong.
///
/// **Owned bytes, not a view into engine storage**, for the same reason
/// `ArrayBuffer` copies: a span into an engine buffer would have to stay valid
/// across operations that collect, and it would have to be released on the
/// isolate's own thread - which is precisely the thread a blob is being carried
/// away from. One copy on the way out buys a `std::vector` the receiving thread
/// can simply own.
///
/// Empty if the value cannot be cloned - a function, a class instance carrying
/// a native, a proxy, or an object holding one of those anywhere inside it. The
/// whole operation fails rather than substituting `undefined` for the offending
/// part: neither engine can do the substitution below the top level without
/// reimplementing the algorithm, and a rule that held for a top-level function
/// but not for one two properties down would be worse than no rule. A caller
/// serialising several values - a list of arguments, say - therefore calls this
/// once per value and decides for itself what an unserialisable one becomes,
/// which keeps the list's shape and puts the decision where the caller can see
/// it.
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>> Serialize(const Context& context,
                                                                        const Local<Value>& value) {
    return detail::SerializeValue(context, value.slot());
}

/// Read back what `Serialize` wrote, into this context's realm - which may
/// belong to a different isolate, and usually does.
///
/// Empty if the blob is not one this engine build wrote, or is damaged. A blob
/// is self-describing enough for both engines to refuse rather than misread
/// one, which is the safety property the opacity above is paying for.
[[nodiscard]] inline std::optional<Local<Value>> Deserialize(const Context& context,
                                                             std::span<const std::uint8_t> blob) {
    return detail::WrapSlot<Value>(detail::DeserializeValue(context, blob));
}

}  // namespace ub
