#pragma once
/// \file
/// Handles and the scopes that own them. Read `docs/lifetimes.md` before
/// changing anything here.
///
///   Local<T>              a slot in an open frame. A frame pointer and a
///                         32-bit ordinal - 8 bytes on x86, 16 on x64 -
///                         trivially copyable, valid exactly as long as its
///                         scope. See docs/lifetimes.md section 6.
///   HandleScope           opens a frame. Stack only, closes LIFO.
///   EscapableHandleScope  same, plus Escape() to hand one handle to the
///                         caller's frame.
///   Global<T>             a root that outlives every frame. Move-only.

#include <cmath>
#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "unibind/detail/backend.h"
#include "unibind/detail/slot.h"
#include "unibind/fwd.h"
#include "unibind/types.h"

namespace ub {

/// A handle to a JavaScript value.
///
/// `T` is one of the tag types in `unibind/value.h` and decides which operations
/// exist: `Local<Object>` has `Get`, `Local<Value>` does not. Narrowing is
/// always checked (`To<Object>()` returns an empty optional if it is not one);
/// widening is implicit.
///
/// A `Local` is valid while the `HandleScope` that produced it is open, and
/// may be copied and stored freely within that lifetime. To return one, use
/// `EscapableHandleScope::Escape`. To keep one longer, use `Global<T>`.
template <class T>
class Local {
   public:
    using TagType = T;

    constexpr Local() noexcept = default;

    /// Widening: Local<Function> -> Local<Object> -> Local<Value>.
    template <class U>
        requires(!std::same_as<U, T> && std::derived_from<U, T>)
    constexpr Local(const Local<U>& other) noexcept  // NOLINT(google-explicit-constructor)
        : slot_(other.slot()) {}

    /// Implementation detail: how a backend and the scopes make a handle.
    /// Callers have no business calling this.
    [[nodiscard]] static constexpr Local FromSlot(detail::Slot slot) noexcept {
        Local local;
        local.slot_ = slot;
        return local;
    }

    [[nodiscard]] constexpr detail::Slot slot() const noexcept { return slot_; }

    /// An empty handle names no value at all. A default-constructed `Local` is
    /// one, and so is what a value-making operation hands back when the frame
    /// could not grow to hold the result.
    ///
    /// **An empty handle is not `undefined`.** Reading one is a programming
    /// error, diagnosed in checked builds and a hard failure otherwise, which
    /// is what keeps an allocation failure from arriving as a plausible value.
    /// Code that has to survive running out of memory asks this once after a
    /// batch of allocations; code that does not gets a crash at the point of
    /// use rather than a wrong answer downstream. See rule 9 of
    /// docs/lifetimes.md.
    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return slot_.IsEmpty(); }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return !slot_.IsEmpty(); }

    // --- what is this? ----------------------------------------------------

    [[nodiscard]] ValueKind Kind() const noexcept { return detail::KindOf(slot_); }

    template <class U>
    [[nodiscard]] bool Is() const noexcept {
        return detail::IsType(slot_, TypeCodeOf<U>);
    }

    // V8's type predicates, spelled as V8 spells them. Each is `Kind()` or
    // `Is<U>()` underneath; they exist because a binding asks these questions
    // constantly, and one name per question reads better than a comparison.
    [[nodiscard]] bool IsUndefined() const noexcept { return Kind() == ValueKind::Undefined; }
    [[nodiscard]] bool IsNull() const noexcept { return Kind() == ValueKind::Null; }
    [[nodiscard]] bool IsNullOrUndefined() const noexcept { return IsNull() || IsUndefined(); }
    [[nodiscard]] bool IsBoolean() const noexcept { return Kind() == ValueKind::Boolean; }
    [[nodiscard]] bool IsTrue() const noexcept { return IsBoolean() && detail::BooleanValue(slot_); }
    [[nodiscard]] bool IsFalse() const noexcept { return IsBoolean() && !detail::BooleanValue(slot_); }
    [[nodiscard]] bool IsNumber() const noexcept { return Kind() == ValueKind::Number; }
    [[nodiscard]] bool IsString() const noexcept { return Kind() == ValueKind::String; }
    [[nodiscard]] bool IsSymbol() const noexcept { return Kind() == ValueKind::Symbol; }
    [[nodiscard]] bool IsName() const noexcept { return IsString() || IsSymbol(); }
    [[nodiscard]] bool IsBigInt() const noexcept { return Kind() == ValueKind::BigInt; }
    /// True for every object, arrays and functions included, as in V8 - and
    /// false for an `External`, which V8 itself counts as one and this API does
    /// not: an External is a value with no properties, and `Is<Object>()` says
    /// the same.
    [[nodiscard]] bool IsObject() const noexcept { return detail::IsType(slot_, TypeCode::Object); }
    [[nodiscard]] bool IsArray() const noexcept { return Kind() == ValueKind::Array; }
    [[nodiscard]] bool IsFunction() const noexcept { return Kind() == ValueKind::Function; }
    [[nodiscard]] bool IsExternal() const noexcept { return Kind() == ValueKind::External; }
    /// A number that is exactly an `int32_t`, however the engine stores it, and
    /// not `-0`, which V8 does not count as one.
    [[nodiscard]] bool IsInt32() const noexcept { return detail::IsType(slot_, TypeCode::Integer); }
    /// A number that is exactly a `uint32_t` - an integer from 0 to 2^32 - 1,
    /// and not `-0`, which V8 does not count as one either.
    [[nodiscard]] bool IsUint32() const noexcept {
        if (!IsNumber()) {
            return false;
        }
        const double value = detail::NumberValue(slot_);
        return value >= 0 && value <= 4294967295.0 && value == static_cast<double>(static_cast<std::uint32_t>(value)) &&
               !(value == 0 && std::signbit(value));
    }
    [[nodiscard]] bool IsArrayBuffer() const noexcept { return detail::IsType(slot_, TypeCode::ArrayBuffer); }
    /// A typed array or a DataView - anything with a buffer, an offset and a
    /// length, as in V8.
    [[nodiscard]] bool IsArrayBufferView() const noexcept { return detail::IsType(slot_, TypeCode::ArrayBufferView); }
    [[nodiscard]] bool IsTypedArray() const noexcept { return detail::IsType(slot_, TypeCode::TypedArray); }
    [[nodiscard]] bool IsDataView() const noexcept { return detail::IsType(slot_, TypeCode::DataView); }
    [[nodiscard]] bool IsPromise() const noexcept { return detail::IsType(slot_, TypeCode::Promise); }

    /// Checked narrowing. Empty if the value is not a `U`.
    template <class U>
        requires std::derived_from<U, T>
    [[nodiscard]] std::optional<Local<U>> To() const noexcept {
        if (!detail::IsType(slot_, TypeCodeOf<U>)) {
            return std::nullopt;
        }
        return Local<U>::FromSlot(slot_);
    }

    // --- identity ---------------------------------------------------------

    template <class U>
    [[nodiscard]] bool StrictEquals(const Local<U>& other) const noexcept {
        return detail::StrictEquals(slot_, other.slot());
    }
    template <class U>
    [[nodiscard]] bool SameValue(const Local<U>& other) const noexcept {
        return detail::SameValue(slot_, other.slot());
    }
    template <class U>
    [[nodiscard]] std::optional<bool> Equals(const Context& context, const Local<U>& other) const {
        return detail::LooseEquals(context, slot_, other.slot());
    }

    // --- coercion (may run user code and throw) ---------------------------

    [[nodiscard]] std::optional<bool> ToBoolean(const Context& context) const {
        return detail::ToBoolean(context, slot_);
    }
    [[nodiscard]] std::optional<double> ToNumber(const Context& context) const {
        return detail::ToNumber(context, slot_);
    }
    [[nodiscard]] std::optional<std::int32_t> ToInt32(const Context& context) const {
        return detail::ToInt32(context, slot_);
    }
    [[nodiscard]] std::optional<std::uint32_t> ToUint32(const Context& context) const {
        return detail::ToUint32(context, slot_);
    }
    [[nodiscard]] std::optional<Local<String>> ToString(const Context& context) const {
        auto s = detail::ToJsString(context, slot_);
        if (!s) {
            return std::nullopt;
        }
        return Local<String>::FromSlot(*s);
    }
    [[nodiscard]] std::optional<Local<Object>> ToObject(const Context& context) const {
        auto o = detail::ToJsObject(context, slot_);
        if (!o) {
            return std::nullopt;
        }
        return Local<Object>::FromSlot(*o);
    }

    // --- primitives -------------------------------------------------------

    [[nodiscard]] bool BooleanValue() const noexcept
        requires std::derived_from<T, Boolean>
    {
        return detail::BooleanValue(slot_);
    }
    [[nodiscard]] double NumberValue() const noexcept
        requires std::derived_from<T, Number>
    {
        return detail::NumberValue(slot_);
    }
    [[nodiscard]] std::int32_t Int32Value() const noexcept
        requires std::derived_from<T, Integer>
    {
        return detail::Int32Value(slot_);
    }

    // --- strings ----------------------------------------------------------

    [[nodiscard]] std::size_t Utf8Length() const noexcept
        requires std::derived_from<T, String>
    {
        return detail::Utf8Length(slot_);
    }
    /// Copies out. On a moving collector there is no borrowing a string's
    /// bytes, so this allocates; `WriteUtf8` into your own buffer if that
    /// matters.
    [[nodiscard]] std::string Utf8Value() const
        requires std::derived_from<T, String>
    {
        return detail::ToStdString(slot_);
    }
    [[nodiscard]] std::size_t WriteUtf8(std::span<char> out) const noexcept
        requires std::derived_from<T, String>
    {
        return detail::WriteUtf8(slot_, out);
    }

    // --- symbols ----------------------------------------------------------

    [[nodiscard]] std::optional<std::string> Description() const
        requires std::derived_from<T, Symbol>
    {
        return detail::SymbolDescription(slot_);
    }

    // --- objects ----------------------------------------------------------

    [[nodiscard]] std::optional<Local<Value>> Get(const Context& context, const Local<Name>& key) const
        requires std::derived_from<T, Object>
    {
        return Wrap<Value>(detail::GetProperty(context, slot_, key.slot()));
    }
    [[nodiscard]] std::optional<Local<Value>> Get(const Context& context, std::string_view key) const
        requires std::derived_from<T, Object>
    {
        return GetByName(context, key);
    }
    [[nodiscard]] std::optional<Local<Value>> Get(const Context& context, std::uint32_t index) const
        requires std::derived_from<T, Object>
    {
        return Wrap<Value>(detail::GetIndex(context, slot_, index));
    }

    template <class U>
    [[nodiscard]] std::optional<bool> Set(const Context& context, const Local<Name>& key, const Local<U>& value) const
        requires std::derived_from<T, Object>
    {
        return detail::SetProperty(context, slot_, key.slot(), value.slot());
    }
    template <class U>
    [[nodiscard]] std::optional<bool> Set(const Context& context, std::string_view key, const Local<U>& value) const
        requires std::derived_from<T, Object>
    {
        return SetByName(context, key, value.slot());
    }
    template <class U>
    [[nodiscard]] std::optional<bool> Set(const Context& context, std::uint32_t index, const Local<U>& value) const
        requires std::derived_from<T, Object>
    {
        return detail::SetIndex(context, slot_, index, value.slot());
    }

    /// Data property with explicit attributes; ignores setters on the
    /// prototype chain, unlike `Set`.
    template <class U>
    [[nodiscard]] std::optional<bool> DefineOwnProperty(const Context& context, const Local<Name>& key,
                                                        const Local<U>& value,
                                                        PropertyAttribute attributes = PropertyAttribute::None) const
        requires std::derived_from<T, Object>
    {
        return detail::DefineProperty(context, slot_, key.slot(), value.slot(), attributes);
    }

    /// An accessor property on this one object: `getter` runs on every read
    /// and `setter`, if there is one, on every write, each handed `data`.
    /// V8's `Object::SetAccessor` - for an object not stamped from a template,
    /// such as one instance that carries a member its class does not.
    ///
    /// It installs the same property `ObjectTemplate::SetAccessor` does: an
    /// accessor with real getter and setter functions, so
    /// `Object.getOwnPropertyDescriptor` reports `get` and `set` on both
    /// engines. With no setter it is read-only and `ReadOnly` adds nothing.
    ///
    /// The callbacks are recorded for the life of the isolate, as a template's
    /// declarations are, so this is for objects made a bounded number of times
    /// - once per realm, say - and not for something made on every call.
    [[nodiscard]] std::optional<bool> SetAccessor(const Context& context, std::string_view name,
                                                  AccessorGetterCallback getter,
                                                  AccessorSetterCallback setter = nullptr, CallbackData data = {},
                                                  PropertyAttribute attributes = PropertyAttribute::None) const
        requires std::derived_from<T, Object>
    {
        return detail::SetAccessorProperty(context, slot_, name, getter, setter, data, attributes);
    }

    [[nodiscard]] std::optional<bool> Has(const Context& context, const Local<Name>& key) const
        requires std::derived_from<T, Object>
    {
        return detail::HasProperty(context, slot_, key.slot());
    }
    [[nodiscard]] std::optional<bool> HasOwn(const Context& context, const Local<Name>& key) const
        requires std::derived_from<T, Object>
    {
        return detail::HasOwnProperty(context, slot_, key.slot());
    }
    [[nodiscard]] std::optional<bool> Delete(const Context& context, const Local<Name>& key) const
        requires std::derived_from<T, Object>
    {
        return detail::DeleteProperty(context, slot_, key.slot());
    }
    [[nodiscard]] std::optional<PropertyAttribute> GetPropertyAttributes(const Context& context,
                                                                         const Local<Name>& key) const
        requires std::derived_from<T, Object>
    {
        return detail::GetPropertyAttributes(context, slot_, key.slot());
    }
    [[nodiscard]] std::optional<Local<Array>> GetOwnPropertyNames(const Context& context, KeyFilter filter = {}) const
        requires std::derived_from<T, Object>
    {
        return Wrap<Array>(detail::GetOwnPropertyNames(context, slot_, filter));
    }
    [[nodiscard]] std::optional<Local<Value>> GetPrototype(const Context& context) const
        requires std::derived_from<T, Object>
    {
        return Wrap<Value>(detail::GetPrototype(context, slot_));
    }
    template <class U>
    [[nodiscard]] std::optional<bool> SetPrototype(const Context& context, const Local<U>& prototype) const
        requires std::derived_from<T, Object>
    {
        return detail::SetPrototype(context, slot_, prototype.slot());
    }

    // --- arrays -----------------------------------------------------------

    [[nodiscard]] std::uint32_t Length() const noexcept
        requires std::derived_from<T, Array>
    {
        return detail::ArrayLength(slot_);
    }

    // --- functions --------------------------------------------------------

    template <class R>
    [[nodiscard]] std::optional<Local<Value>> Call(const Context& context, const Local<R>& receiver,
                                                   std::span<const Local<Value>> arguments = {}) const
        requires std::derived_from<T, Function>
    {
        return Wrap<Value>(detail::CallFunction(context, slot_, receiver.slot(), AsSlots(arguments)));
    }
    [[nodiscard]] std::optional<Local<Object>> NewInstance(const Context& context,
                                                           std::span<const Local<Value>> arguments = {}) const
        requires std::derived_from<T, Function>
    {
        return Wrap<Object>(detail::ConstructObject(context, slot_, AsSlots(arguments)));
    }

   private:
    template <class U>
    [[nodiscard]] static std::optional<Local<U>> Wrap(std::optional<detail::Slot> slot) noexcept {
        if (!slot) {
            return std::nullopt;
        }
        return Local<U>::FromSlot(*slot);
    }

    /// `Local<Value>` and `detail::Slot` have the same representation, so a
    /// span of handles is already a span of slots. Asserted below.
    [[nodiscard]] static std::span<const detail::Slot> AsSlots(std::span<const Local<Value>> arguments) noexcept {
        return {reinterpret_cast<const detail::Slot*>(arguments.data()), arguments.size()};
    }

    [[nodiscard]] std::optional<Local<Value>> GetByName(const Context& context, std::string_view key) const;
    [[nodiscard]] std::optional<bool> SetByName(const Context& context, std::string_view key, detail::Slot value) const;

    detail::Slot slot_;
};

namespace detail {
/// Lift a backend result into a typed handle. Empty stays empty.
template <class T>
[[nodiscard]] inline std::optional<Local<T>> WrapSlot(std::optional<Slot> slot) noexcept {
    if (!slot) {
        return std::nullopt;
    }
    return Local<T>::FromSlot(*slot);
}
}  // namespace detail
/// A root that outlives every handle scope. One per engine root object, so it
/// costs an allocation on SpiderMonkey and a global handle on V8 - which is
/// why it is a separate type from `Local` rather than the default.
///
/// It outlives every scope and **not its isolate**: `Reset` it, or let it go,
/// before the isolate does. See `unibind/isolate.h` - the root it holds is that
/// isolate's, and giving it back afterwards is undefined behaviour rather than
/// a late tidy-up.
///
/// Move-only: copying a root should be visible, so it is spelled `Duplicate`.
template <class T>
class Global {
   public:
    constexpr Global() noexcept = default;
    Global(Isolate& isolate, const Local<T>& value) : node_(detail::MakeGlobal(isolate, value.slot())) {}

    Global(const Global&) = delete;
    Global& operator=(const Global&) = delete;

    Global(Global&& other) noexcept : node_(std::exchange(other.node_, nullptr)) {}
    Global& operator=(Global&& other) noexcept {
        if (this != &other) {
            Reset();
            node_ = std::exchange(other.node_, nullptr);
        }
        return *this;
    }
    ~Global() { Reset(); }

    /// A second root over the same value. Spelled out rather than being what
    /// copying does, because a root costs something and that should be visible.
    ///
    /// Like the comparisons below, it needs **no open `HandleScope`**: it is a
    /// root made from a root, and the caller never sees the handle in between.
    [[nodiscard]] Global Duplicate() const { return Global(detail::DuplicateGlobal(node_)); }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return node_ == nullptr; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return node_ != nullptr; }

    // --- identity ---------------------------------------------------------
    //
    // Two `Global`s can be two different *roots* over one object - `Duplicate`
    // makes exactly that, and so does rooting the same function twice. So the
    // question "do these name the same value" is not the question "are these
    // the same handle", and comparing the handles answers the wrong one
    // silently. The canonical way to get bitten is a registry of callbacks kept
    // as `Global`s: the caller passes back the same function through a
    // different handle, the removal compares roots, finds nothing, and removes
    // nothing.
    //
    // Only the engine can answer it, because on a moving collector a root's
    // address says nothing about what is in it. These materialise both sides
    // and compare the values.
    //
    // They need **no open `HandleScope`**: the backend roots what it needs for
    // the duration of the comparison and hands back a `bool`, so there is no
    // frame to run out of and no failure to mistake for "not equal". An empty
    // `Global` is equal to nothing at all, including another empty one - it
    // names no value, so no comparison with it can conclude that two things are
    // the same. Ask `IsEmpty()` for that.
    //
    // Both spellings exist for the same reason `Local` has both: they differ on
    // `NaN` and on `-0`. There is no `operator==`, again matching `Local` - a
    // language with two defensible equalities should not have one of them
    // spelled with the punctuation that hides which. Loose equality is absent
    // on purpose: it runs user code and can throw, so it needs a `Context` and
    // a fallible result - `Get(isolate)` and `Local::Equals` if you want it.

    template <class U>
    [[nodiscard]] bool StrictEquals(const Global<U>& other) const noexcept {
        return detail::GlobalStrictEquals(node_, other.node());
    }
    template <class U>
    [[nodiscard]] bool StrictEquals(const Local<U>& other) const noexcept {
        return detail::GlobalStrictEqualsSlot(node_, other.slot());
    }
    template <class U>
    [[nodiscard]] bool SameValue(const Global<U>& other) const noexcept {
        return detail::GlobalSameValue(node_, other.node());
    }
    template <class U>
    [[nodiscard]] bool SameValue(const Local<U>& other) const noexcept {
        return detail::GlobalSameValueSlot(node_, other.slot());
    }

    /// Implementation detail: the backend's root. Callers have no business
    /// with it - in particular it is not an identity, which is what the
    /// comparisons above are for.
    [[nodiscard]] constexpr detail::GlobalNode* node() const noexcept { return node_; }

    /// Materialise into the isolate's current frame. A `HandleScope` must be
    /// open.
    [[nodiscard]] Local<T> Get(Isolate& isolate) const noexcept {
        return Local<T>::FromSlot(detail::GlobalToSlot(isolate, node_));
    }

    void Reset() noexcept {
        if (node_ != nullptr) {
            detail::ReleaseGlobal(node_);
            node_ = nullptr;
        }
    }

   private:
    explicit Global(detail::GlobalNode* node) noexcept : node_(node) {}

    detail::GlobalNode* node_ = nullptr;
};

/// Opens a frame. Every handle made while it is open dies when it closes.
///
/// Stack only, and enforced: `operator new` is deleted, copy and move are
/// deleted, so a scope cannot be stored, returned, or heap-allocated. On
/// SpiderMonkey the frame *is* a `JS::Rooted`, which has exactly these rules;
/// the API is not being conservative, it is being accurate.
class HandleScope {
   public:
    explicit HandleScope(Isolate& isolate) noexcept : frame_(detail::OpenFrame(isolate, storage_, false)) {}
    ~HandleScope() { detail::CloseFrame(frame_); }

    HandleScope(const HandleScope&) = delete;
    HandleScope& operator=(const HandleScope&) = delete;
    HandleScope(HandleScope&&) = delete;
    HandleScope& operator=(HandleScope&&) = delete;

    void* operator new(std::size_t) = delete;
    void* operator new[](std::size_t) = delete;

    [[nodiscard]] Isolate& GetIsolate() const noexcept { return detail::IsolateOf(frame_); }

   protected:
    /// Tag for the escapable variant: whether a frame can escape is decided
    /// when it is opened, not when Escape is called.
    struct Escapable {};
    HandleScope(Isolate& isolate, Escapable /*escapable*/) noexcept
        : frame_(detail::OpenFrame(isolate, storage_, true)) {}

    // Protected, not private: `EscapableHandleScope` is this class with one
    // more operation, and it reaches both of these.
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    detail::FrameStorage storage_;
    detail::Frame& frame_;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
};

/// A `HandleScope` one of whose handles may be handed to the caller.
///
/// `Escape` copies the value into the parent frame while both frames are live
/// roots, so the value is never momentarily unrooted - which is why this is a
/// scope member and not a free function.
class EscapableHandleScope : public HandleScope {
   public:
    explicit EscapableHandleScope(Isolate& isolate) noexcept : HandleScope(isolate, Escapable{}) {}

    template <class T>
    [[nodiscard]] Local<T> Escape(const Local<T>& value) noexcept {
        return Local<T>::FromSlot(detail::EscapeSlot(frame_, value.slot()));
    }

    template <class T>
    [[nodiscard]] std::optional<Local<T>> Escape(const std::optional<Local<T>>& value) noexcept {
        if (!value) {
            return std::nullopt;
        }
        return Escape(*value);
    }
};

}  // namespace ub
