#pragma once
/// \file
/// What a native callback is handed, and how it answers.
///
/// Shaped after V8: a call gets a `CallbackInfo` carrying the receiver, the
/// arguments and a return slot; a property access gets a
/// `PropertyCallbackInfo`, which has a receiver and a return slot but no
/// arguments, so a getter cannot read an argument it does not have.

#include <cstdint>
#include <string_view>

#include "unibind/context.h"
#include "unibind/detail/backend.h"
#include "unibind/fwd.h"
#include "unibind/handle.h"
#include "unibind/isolate.h"
#include "unibind/types.h"
#include "unibind/value.h"

namespace ub {

namespace detail {

// The callback half of the backend interface. See backend.h for the rules.
//
// `CallbackState` is the backend's per-call state, and ONE type answers every
// shape of call: a function call, a constructor call, an accessor get or set,
// and each of the five interceptor hooks. The public wrapper decides what may
// be asked of it - `CallbackInfo` exposes the argument list, `PropertyCallbackInfo`
// does not - so a backend never has to answer `CallbackArgumentCount` or
// `CallbackIsConstruct` for a property hook.
//
// The one thing a backend does have to erase is where the result goes. V8
// types a callback's return slot by the hook it belongs to
// (`PropertyCallbackInfo<Value>` for a getter, `<Integer>` for a query,
// `<Boolean>` for a deleter, `<Array>` for an enumerator), so the six
// SetReturn* entry points below cannot name one engine type; the backend
// carries whatever erasure it needs in its own `CallbackState`. See
// docs/status.md for the shape the V8 backend chose.
//
// Two consequences a second backend must match, because they are observable:
//
//  * A hook whose result the language does not read - an interceptor setter,
//    query, deleter or enumerator, whose answer is the callback's C++ return
//    value - DISCARDS anything written through `GetReturnValue()`. Writing
//    there is legal and does nothing; it is not an error and must not corrupt
//    the hook's protocol.
//  * `CallbackHolder` is the object carrying the handler that was invoked, and
//    `CallbackThis` is the receiver of the access. Where the engine does not
//    hand a property hook the receiver separately - V8 15.6 gives an
//    interceptor only the holder - the two are the same object.
Isolate& CallbackIsolate(const CallbackState& state) noexcept;
const Context& CallbackContext(const CallbackState& state) noexcept;
std::uint32_t CallbackArgumentCount(const CallbackState& state) noexcept;
/// Out of range yields `undefined`, as in JavaScript.
Slot CallbackArgument(const CallbackState& state, std::uint32_t index) noexcept;
Slot CallbackThis(const CallbackState& state) noexcept;
Slot CallbackHolder(const CallbackState& state) noexcept;
bool CallbackIsConstruct(const CallbackState& state) noexcept;
CallbackData CallbackDataOf(const CallbackState& state) noexcept;

void SetReturnSlot(const CallbackState& state, Slot value) noexcept;
void SetReturnUndefined(const CallbackState& state) noexcept;
void SetReturnNull(const CallbackState& state) noexcept;
void SetReturnBoolean(const CallbackState& state, bool value) noexcept;
void SetReturnNumber(const CallbackState& state, double value) noexcept;
void SetReturnInteger(const CallbackState& state, std::int32_t value) noexcept;

}  // namespace detail

/// Where a callback writes its result. A callback that writes nothing returns
/// `undefined`, as in V8.
class ReturnValue {
   public:
    explicit constexpr ReturnValue(const detail::CallbackState& state) noexcept : state_(&state) {}

    template <class T>
    void Set(const Local<T>& value) const noexcept {
        detail::SetReturnSlot(*state_, value.slot());
    }
    /// A value held across calls, returned as it is now. V8's `Set(const
    /// Global<S>&)`; an empty `Global` returns `undefined`.
    template <class T>
    void Set(const Global<T>& value) const noexcept {
        if (value.IsEmpty()) {
            SetUndefined();
            return;
        }
        Set(value.Get(detail::CallbackIsolate(*state_)));
    }
    void Set(bool value) const noexcept { detail::SetReturnBoolean(*state_, value); }
    void Set(double value) const noexcept { detail::SetReturnNumber(*state_, value); }
    void Set(std::int32_t value) const noexcept { detail::SetReturnInteger(*state_, value); }

    // V8's other integer widths. Each is the integer it names when it fits in an
    // `int32_t` and a Number otherwise - which for a 64-bit value beyond 2^53 is
    // the nearest double, exactly as V8 answers. Without them a `uint32_t` is
    // ambiguous between the `int32_t` and `double` overloads above.
    void Set(std::int16_t value) const noexcept { Set(static_cast<std::int32_t>(value)); }
    void Set(std::uint16_t value) const noexcept { Set(static_cast<std::int32_t>(value)); }
    void Set(std::uint32_t value) const noexcept {
        if (value <= static_cast<std::uint32_t>(INT32_MAX)) {
            Set(static_cast<std::int32_t>(value));
        } else {
            Set(static_cast<double>(value));
        }
    }
    void Set(std::int64_t value) const noexcept {
        if (value >= INT32_MIN && value <= INT32_MAX) {
            Set(static_cast<std::int32_t>(value));
        } else {
            Set(static_cast<double>(value));
        }
    }
    void Set(std::uint64_t value) const noexcept {
        if (value <= static_cast<std::uint64_t>(INT32_MAX)) {
            Set(static_cast<std::int32_t>(value));
        } else {
            Set(static_cast<double>(value));
        }
    }

    void SetUndefined() const noexcept { detail::SetReturnUndefined(*state_); }
    void SetNull() const noexcept { detail::SetReturnNull(*state_); }
    void SetFalse() const noexcept { Set(false); }
    /// `""`. Empty if the string could not be made, in which case nothing was
    /// set - as for `Set(std::string_view)`.
    [[nodiscard]] bool SetEmptyString() const { return Set(std::string_view()); }

    /// Convenience for the common case; empty if the string could not be made,
    /// in which case nothing was set.
    [[nodiscard]] bool Set(std::string_view utf8) const;
    /// Without this, `Set("literal")` would pick the `bool` overload.
    [[nodiscard]] bool Set(const char* utf8) const { return Set(std::string_view(utf8)); }

   private:
    const detail::CallbackState* state_;
};

/// Common ground between a function call and a property access.
class CallbackContextBase {
   public:
    [[nodiscard]] Isolate& GetIsolate() const noexcept { return detail::CallbackIsolate(*state_); }
    /// The realm the call is happening in - what a callback should use for
    /// property access and for making values, rather than a context it
    /// captured earlier.
    [[nodiscard]] const Context& GetContext() const noexcept { return detail::CallbackContext(*state_); }

    /// The receiver of the call or the property access.
    [[nodiscard]] Local<Object> This() const noexcept { return Local<Object>::FromSlot(detail::CallbackThis(*state_)); }
    /// The object carrying the handler that was invoked, which is not always
    /// the receiver - for an inherited accessor it is the prototype. Engines
    /// differ in whether a property hook is told both; where one is not
    /// offered, this and `This()` name the same object.
    [[nodiscard]] Local<Object> Holder() const noexcept {
        return Local<Object>::FromSlot(detail::CallbackHolder(*state_));
    }

    /// The embedder pointer this callback was declared with, or null if it was
    /// declared with none or with a different type.
    template <class D>
    [[nodiscard]] D* Data() const noexcept {
        return detail::CallbackDataOf(*state_).template As<D>();
    }

    [[nodiscard]] ReturnValue GetReturnValue() const noexcept { return ReturnValue(*state_); }

    /// Throw and stop. The exception takes effect when the callback returns;
    /// the callback should return immediately after calling this and must not
    /// call further into the engine.
    void Throw(ErrorKind kind, std::string_view message) const { detail::ThrowError(GetIsolate(), kind, message); }
    void ThrowTypeError(std::string_view message) const { Throw(ErrorKind::TypeError, message); }

    /// Implementation detail: the backend's per-call state.
    [[nodiscard]] constexpr const detail::CallbackState& state() const noexcept { return *state_; }

   protected:
    explicit constexpr CallbackContextBase(const detail::CallbackState& state) noexcept : state_(&state) {}

   private:
    const detail::CallbackState* state_;
};

/// A native function call.
class CallbackInfo : public CallbackContextBase {
   public:
    explicit constexpr CallbackInfo(const detail::CallbackState& state) noexcept : CallbackContextBase(state) {}

    [[nodiscard]] std::uint32_t Length() const noexcept { return detail::CallbackArgumentCount(state()); }

    /// Reading past the end gives `undefined`, which is what script would see.
    [[nodiscard]] Local<Value> operator[](std::uint32_t index) const noexcept {
        return Local<Value>::FromSlot(detail::CallbackArgument(state(), index));
    }

    /// True when script reached this callback through `new`. Only a
    /// `FunctionTemplate` or a `Class<T>` makes something constructable, so
    /// this is always false for a function from `Function::New` - `new` on one
    /// of those is a TypeError before the callback runs at all.
    [[nodiscard]] bool IsConstructCall() const noexcept { return detail::CallbackIsConstruct(state()); }
};

/// A native property access: a get, a set, or an interceptor hook.
///
/// A hook whose answer is its C++ return value - an interceptor's query,
/// deleter, enumerator or setter - ignores `GetReturnValue()`. The setters are
/// still there because one type serves every hook; writing through them is
/// legal and does nothing.
class PropertyCallbackInfo : public CallbackContextBase {
   public:
    explicit constexpr PropertyCallbackInfo(const detail::CallbackState& state) noexcept : CallbackContextBase(state) {}
};

inline bool ReturnValue::Set(std::string_view utf8) const {
    auto string = String::New(detail::CallbackIsolate(*state_), utf8);
    if (!string) {
        return false;
    }
    detail::SetReturnSlot(*state_, string->slot());
    return true;
}

}  // namespace ub
