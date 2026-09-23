#pragma once
/// \file
/// Plain value types shared by the whole API: enums, the compile-time type
/// identity used for checked unwrapping, and the small structs that carry
/// engine answers back out.
///
/// Nothing here knows about an engine, a handle, or a scope.
///
/// ---------------------------------------------------------------------------
/// The failure convention
/// ---------------------------------------------------------------------------
///
/// Everything in this API that can fail says so with `std::optional`, and an
/// empty optional always means the same thing: **the operation did not produce
/// a value**. It never means "the value was undefined" - that is a value, and
/// it arrives as one.
///
/// Why it failed is a second question, asked of the isolate rather than
/// encoded in the result:
///
///   * If **script threw** - a getter, a proxy trap, a constructor, a
///     conversion that ran user code - the exception is *pending* on the
///     isolate. Either a `TryCatch` in scope catches it, or the caller must
///     return to script promptly without calling further into the engine.
///     `TryCatch::HasCaught()` is what distinguishes this case.
///   * If the **engine** failed - out of memory, a string that will not
///     encode, a frame that could not grow - there may be no JavaScript
///     exception at all. `Isolate::HasPendingException()` answers whether
///     there is one.
///   * If **execution was terminated** from another thread, neither of the
///     above applies: `TryCatch::HasTerminated()` is the question, and there is
///     no value, message or stack to read. See `unibind/isolate.h`.
///
/// A handle is the one exception to the shape, and deliberately: a
/// value-making operation that cannot grow the frame hands back an **empty
/// `Local`**, not an empty optional, because wrapping `Undefined()`,
/// `GlobalObject()` and `info.This()` in an optional would put an unwrap at
/// every call site in the API forever. `Local::IsEmpty()` is that check. See
/// `docs/lifetimes.md` rule 9.
///
/// There was once a `ub::Maybe<T>` alias for `std::optional<T>`. It is gone:
/// renaming a standard type buys nothing and costs every reader a lookup. The
/// convention above is what was worth keeping, and it lives here.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "unibind/config.h"

namespace ub {

namespace detail {
/// An erased pointer to an embedder type. Never defined and never dereferenced
/// as itself; a reinterpret_cast to this type and back to the real one is
/// guaranteed to round-trip. It exists so that no signature in this API has to
/// say `void*`.
struct Opaque;
}  // namespace detail

/// What a value is. Mirrors V8's Value::IsXxx predicates, collapsed into the
/// one question a caller usually asks: what can I do with this?
///
/// **A value this API does not model separately reports the kind whose
/// operations work on it.** A Date, a RegExp, a Proxy, a typed array: every one
/// of them is `Object`, because `Object` is exactly the set of operations the
/// API offers for them. There is no "exotic" kind and asking for one would be
/// unportable in both directions - the list differs by engine and by version,
/// and it is a *negative* answer ("something I did not recognise") that no
/// caller can act on anyway.
///
/// `Proxy` is the case that settles it. An engine with no interceptor of its
/// own builds `unibind`'s interceptors out of proxies (see unibind/template.h), so a
/// kind that singled proxies out would make a sandbox object report one thing
/// on one backend and another on the other, purely because of how we built it.
///
/// `Other` is therefore not the kind for any of those. It is the defined answer
/// for a value a backend cannot classify at all - which on both engines today
/// is no value, since a value is either a primitive this enum names or an
/// object. A backend that finds itself returning it has found a kind this enum
/// is missing.
enum class ValueKind : std::uint8_t {
    Undefined,
    Null,
    Boolean,
    Number,
    String,
    Symbol,
    BigInt,
    Object,
    Array,
    Function,
    External,
    Other,  ///< see above: unclassifiable, not "exotic object"
};

/// The narrowing questions a handle can be asked. One per tag type in
/// `unibind/value.h`; `Local<T>::Is<U>()` turns into one `IsType` call.
enum class TypeCode : std::uint8_t {
    Value,  ///< always true
    Primitive,
    Boolean,
    Number,
    Integer,  ///< a Number whose value is an exact int32
    Name,     ///< String or Symbol
    String,
    Symbol,
    BigInt,
    Object,
    Array,
    Function,
    ArrayBuffer,  ///< raw bytes script can share
    TypedArray,   ///< a view onto an ArrayBuffer, with an element width
    Promise,
    External,
};

/// Where a promise has got to. `Pending` is the only state that can still
/// change.
enum class PromiseState : std::uint8_t {
    Pending,
    Fulfilled,
    Rejected,
};

/// ECMAScript property attributes, as the negative-sense flags V8 uses.
enum class PropertyAttribute : std::uint8_t {
    None = 0,
    ReadOnly = 1 << 0,    ///< not writable
    DontEnum = 1 << 1,    ///< not enumerable
    DontDelete = 1 << 2,  ///< not configurable
};

[[nodiscard]] constexpr PropertyAttribute operator|(PropertyAttribute a, PropertyAttribute b) noexcept {
    return static_cast<PropertyAttribute>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr PropertyAttribute operator&(PropertyAttribute a, PropertyAttribute b) noexcept {
    return static_cast<PropertyAttribute>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr bool HasAttribute(PropertyAttribute set, PropertyAttribute one) noexcept {
    return (set & one) == one;
}

/// Answer of an interceptor: did it handle this property, or should the
/// ordinary lookup continue? Same three-state model V8 moved to in 12.x, and
/// the only one a SpiderMonkey resolve hook can implement.
enum class Intercepted : bool {
    No = false,
    Yes = true,
};

/// Error constructors every engine has.
enum class ErrorKind : std::uint8_t {
    Error,
    TypeError,
    RangeError,
    ReferenceError,
    SyntaxError,
};

/// Well-known symbols. Only the ones an embedder actually needs to define or
/// look up; extend as required, but never expose an engine's own enum.
enum class WellKnownSymbol : std::uint8_t {
    Iterator,
    AsyncIterator,
    HasInstance,
    ToPrimitive,
    ToStringTag,
};

/// The element types every engine's typed arrays have in common. See the
/// "Binary data" section of `unibind/value.h` for what an embedder does with them.
///
/// `BigInt64Array` and `BigUint64Array` are absent because this API has no way
/// to make a BigInt, so an element of one could be read but never written.
enum class ElementType : std::uint8_t {
    Int8,
    Uint8,
    Uint8Clamped,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float32,
    Float64,
};

/// Bytes per element of a typed array of this type.
[[nodiscard]] constexpr std::size_t ElementSize(ElementType type) noexcept {
    switch (type) {
        case ElementType::Int8:
        case ElementType::Uint8:
        case ElementType::Uint8Clamped:
            return 1;
        case ElementType::Int16:
        case ElementType::Uint16:
            return 2;
        case ElementType::Int32:
        case ElementType::Uint32:
        case ElementType::Float32:
            return 4;
        case ElementType::Float64:
            return 8;
    }
    return 0;
}

/// Which own keys to collect.
struct KeyFilter {
    bool includeNonEnumerable = false;
    bool includeSymbols = false;
};

/// Where a script came from, for diagnostics.
struct ScriptOrigin {
    std::string_view resourceName = "<anonymous>";
    int lineOffset = 0;
    int columnOffset = 0;
};

/// Heap figures, named after V8's `v8::HeapStatistics`.
///
/// Engines measure different things; only `usedBytes` is comparable across
/// them, and even that only as a trend. The first three every engine reports.
/// The rest are the figures V8 gives and SpiderMonkey has no cheap answer for -
/// its only source is a full memory report that walks the heap, which is not
/// something a statistics call should cost - so they are empty there rather
/// than zero, which would be a claim.
struct HeapStatistics {
    /// Bytes occupied by live and not-yet-collected objects.
    std::uint64_t usedBytes = 0;
    /// Bytes the collector has reserved for the heap, used or not.
    std::uint64_t totalBytes = 0;
    /// The ceiling the heap may grow to.
    std::uint64_t limitBytes = 0;

    /// Of `totalBytes`, how much is backed by physical memory.
    std::optional<std::uint64_t> physicalBytes;
    /// Memory held outside the heap on the heap's behalf - array buffer
    /// backing stores, chiefly.
    std::optional<std::uint64_t> externalBytes;
    /// What the engine has allocated through `malloc` for itself, and the
    /// highest figure it has recorded. V8 records its peak only at certain
    /// points, so the current figure can briefly be the larger of the two.
    std::optional<std::uint64_t> mallocedBytes;
    std::optional<std::uint64_t> peakMallocedBytes;
    /// The pool persistent handles (`Global<T>`, and the engine's own) live
    /// in: how much of it is in use, and how large it is.
    std::optional<std::uint64_t> usedGlobalHandlesBytes;
    std::optional<std::uint64_t> totalGlobalHandlesBytes;
};

/// One frame of a JavaScript stack, as both engines can describe it.
///
/// The three fields an engine will name are the function, the script and the
/// line; everything else about a stack - the layout of the text, whether
/// native frames appear, how eval frames are spelled - differs, and is not
/// modelled. `columnNumber` is reported where the engine gives one.
///
/// An empty `functionName` is a frame with no name of its own: top-level code,
/// or an anonymous function. An empty `scriptName` is source compiled with no
/// origin. `lineNumber` and `columnNumber` are 1-based, and 0 means the engine
/// did not say.
///
/// Reading a stack allocates, deliberately: a stack is a diagnostic, not a hot
/// path, and the alternative is a callback-shaped API for something an
/// embedder invariably wants as a whole.
struct StackFrame {
    std::string functionName;
    std::string scriptName;
    std::int32_t lineNumber = 0;
    std::int32_t columnNumber = 0;
};

/// Where a caught exception was raised: V8's `v8::Message` -
/// `GetScriptResourceName`, `GetLineNumber`, `GetStartColumn` and
/// `GetSourceLine` - read in one call. See `TryCatch::Location`.
///
/// `lineNumber` and `columnNumber` follow `StackFrame`: 1-based, 0 when the
/// engine did not say. `sourceLine` is the text of that line without its line
/// terminator, empty when the source is not available.
struct MessageLocation {
    std::string scriptName;
    std::int32_t lineNumber = 0;
    std::int32_t columnNumber = 0;
    std::optional<std::string> sourceLine;
};

/// TRANSITIONAL. `Maybe<T>` was a rename of `std::optional<T>` and is being
/// removed; the failure convention it used to document is at the top of this
/// file. Nothing under `include/unibind/`, `src/backends/v8/` or `tests/` names
/// it any more. The last uses are in `src/backends/spidermonkey/`; this line
/// goes when they do.
template <class T>
using Maybe = std::optional<T>;

// ---------------------------------------------------------------------------
// Compile-time type identity
//
// Used to check that native state recovered from an object really is the type
// the caller asked for, without RTTI. The identity of a type is the address of
// a byte that exists once per type in the program.
// ---------------------------------------------------------------------------

namespace detail {
template <class T>
struct TypeTagByte {
    static constexpr char value = 0;
};
}  // namespace detail

class TypeId {
   public:
    constexpr TypeId() noexcept = default;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return id_ != nullptr; }
    friend constexpr bool operator==(TypeId, TypeId) noexcept = default;

   private:
    template <class T>
    friend constexpr TypeId TypeIdOf() noexcept;

    constexpr explicit TypeId(const char* id) noexcept : id_(id) {}

    const char* id_ = nullptr;
};

/// The identity of `T`, stable for the life of the program and distinct for
/// every distinct type. Cv-qualifiers are ignored.
template <class T>
[[nodiscard]] constexpr TypeId TypeIdOf() noexcept {
    return TypeId(&detail::TypeTagByte<std::remove_cv_t<T>>::value);
}

/// A typed, non-owning pointer handed to a native callback - this API's answer
/// to V8's `Local<Value> data` closure slot, and the reason no signature here
/// needs a `void*`.
///
/// The pointee is the embedder's, and must outlive every callback that can see
/// it. Recovery is checked: `As<D>()` yields null unless `D` is exactly the
/// type that was stored.
class CallbackData {
   public:
    constexpr CallbackData() noexcept = default;

    template <class D>
        requires(!std::is_const_v<D>)
    [[nodiscard]] static CallbackData For(D& data) noexcept {
        return CallbackData(TypeIdOf<D>(), reinterpret_cast<detail::Opaque*>(std::addressof(data)));
    }

    template <class D>
    [[nodiscard]] D* As() const noexcept {
        return type_ == TypeIdOf<D>() ? reinterpret_cast<D*>(ptr_) : nullptr;
    }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return ptr_ == nullptr; }

   private:
    constexpr CallbackData(TypeId type, detail::Opaque* ptr) noexcept : type_(type), ptr_(ptr) {}

    TypeId type_;
    detail::Opaque* ptr_ = nullptr;
};

// ---------------------------------------------------------------------------
// Constants usable at template-declaration time
//
// A template (see unibind/template.h) is built before any context exists and
// before any handle scope is open, so the plain values it installs cannot be
// handles. `Constant` is the closed set of values that can be spelled without
// an engine.
// ---------------------------------------------------------------------------

class Constant {
   public:
    enum class Kind : std::uint8_t { Undefined, Null, Boolean, Number, Integer, String };

    constexpr Constant() noexcept = default;
    constexpr Constant(bool v) noexcept : kind_(Kind::Boolean), boolean_(v) {}            // NOLINT(*-explicit-*)
    constexpr Constant(double v) noexcept : kind_(Kind::Number), number_(v) {}            // NOLINT(*-explicit-*)
    constexpr Constant(std::int32_t v) noexcept : kind_(Kind::Integer), integer_(v) {}    // NOLINT(*-explicit-*)
    constexpr Constant(std::string_view v) noexcept : kind_(Kind::String), string_(v) {}  // NOLINT(*-explicit-*)
    /// Without this, `Constant("literal")` would be `Constant(true)`: a
    /// pointer-to-bool conversion is standard and beats the user-defined one
    /// to string_view, silently and without a warning.
    constexpr Constant(const char* v) noexcept : kind_(Kind::String), string_(v) {}  // NOLINT(*-explicit-*)

    [[nodiscard]] static constexpr Constant Null() noexcept { return Constant(Kind::Null); }

    [[nodiscard]] constexpr Kind GetKind() const noexcept { return kind_; }
    [[nodiscard]] constexpr bool AsBoolean() const noexcept { return boolean_; }
    [[nodiscard]] constexpr double AsNumber() const noexcept { return number_; }
    [[nodiscard]] constexpr std::int32_t AsInteger() const noexcept { return integer_; }
    [[nodiscard]] constexpr std::string_view AsString() const noexcept { return string_; }

   private:
    constexpr explicit Constant(Kind k) noexcept : kind_(k) {}

    Kind kind_ = Kind::Undefined;
    bool boolean_ = false;
    double number_ = 0.0;
    std::int32_t integer_ = 0;
    std::string_view string_;
};

}  // namespace ub
