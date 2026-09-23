// The SpiderMonkey backend: frames, values, objects, functions, scripts,
// exceptions, globals and contexts. Templates, classes and interceptors are in
// templates.cpp.
//
// Everything SpiderMonkey-specific in the library lives behind these two files:
// they define the types the public headers only declare (`detail::Frame`,
// `ContextRec`, ...) and the functions the public headers only declare.
// Nothing above them includes a SpiderMonkey header, which is what
// `unibind_headers_only` proves.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <tuple>

#include "internal.h"

// For GetCurrentThreadStackLimits, which is how the stack quota gets clamped to
// a stack that actually exists. WIN32_LEAN_AND_MEAN and NOMINMAX come from the
// project's compile definitions.
#include <js/ArrayBuffer.h>
#include <js/BuildId.h>
#include <js/ColumnNumber.h>
#include <js/Equality.h>
#include <js/GCAPI.h>
#include <js/HeapAPI.h>
#include <js/Interrupt.h>
#include <js/MemoryCallbacks.h>
#include <js/SavedFrameAPI.h>
#include <js/ScalarType.h>
#include <js/ScriptPrivate.h>
#include <js/Stack.h>
#include <js/String.h>
#include <js/StructuredClone.h>
#include <js/experimental/TypedData.h>
#include <windows.h>
// For JS::ThreadStackQuotaForSize, which lives with the frontend-context API
// rather than with JS_SetNativeStackQuota.
#include <js/experimental/CompileScript.h>

namespace ub {
namespace detail {

/// Defined below; the finalizers in the anonymous namespace need it first.
void ReleaseCallbackRecord(CallbackRecord* record) noexcept;

namespace {

/// Externals are not objects in this API, but SpiderMonkey has no value kind
/// that is neither a primitive nor an object. So an External is an object of a
/// private class, and `KindOf` / `IsType` ask the class before they ask
/// anything else - which is also what V8 does, for the same reason.
constexpr std::size_t EXTERNAL_RECORD_SLOT = 0;

/// An external and a `Function::New` function both carry a record the engine
/// will not carry for them, and both are *values*: nothing says they cost
/// their isolate anything once they are collected. These two finalizers are
/// what makes that true.
///
/// Foreground, and it is required rather than tidy: the record is a plain C++
/// object in a map the isolate's own thread reads and writes, so finalizing it
/// on a helper thread would race every other use of that map. See `NativeBox`
/// in unibind/detail/backend.h.
void ReleaseRecordSlot(JSObject* object, std::size_t slot) noexcept {
    const JS::Value value = JS::GetReservedSlot(object, static_cast<std::uint32_t>(slot));
    if (value.isUndefined()) {
        return;
    }
    ReleaseCallbackRecord(static_cast<CallbackRecord*>(value.toPrivate()));
}

void ExternalFinalize(JS::GCContext* /*gcx*/, JSObject* object) {
    ReleaseRecordSlot(object, EXTERNAL_RECORD_SLOT);
}

const JSClassOps EXTERNAL_CLASS_OPS = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &ExternalFinalize, nullptr, nullptr, nullptr,
};

const JSClass EXTERNAL_CLASS = {"ub::External", JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_FOREGROUND_FINALIZE,
                                &EXTERNAL_CLASS_OPS};

/// What holds a plain function's record, because a `JSFunction` has no
/// finalizer of ours to hang one on. It lives in the function's second
/// reserved slot, so it is reachable exactly as long as the function is, and
/// its finalizer is what gives the record back.
///
/// Its second slot is the script value of a function made with one, which is
/// kept alive by exactly the same reachability: the collector traces a
/// reserved slot, so the value goes when the function does and not before, and
/// a value that refers back to its function does not pin either.
constexpr std::size_t RECORD_HOLDER_SLOT = 0;
constexpr std::size_t RECORD_HOLDER_VALUE_SLOT = 1;

void RecordHolderFinalize(JS::GCContext* /*gcx*/, JSObject* object) {
    ReleaseRecordSlot(object, RECORD_HOLDER_SLOT);
}

const JSClassOps RECORD_HOLDER_CLASS_OPS = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &RecordHolderFinalize, nullptr, nullptr, nullptr,
};

const JSClass RECORD_HOLDER_CLASS = {"ub::CallbackRecord", JSCLASS_HAS_RESERVED_SLOTS(2) | JSCLASS_FOREGROUND_FINALIZE,
                                     &RECORD_HOLDER_CLASS_OPS};

[[nodiscard]] bool IsExternalObject(const JS::Value& value) noexcept {
    JSObject* object = Unwrapped(value);
    return object != nullptr && JS::GetClass(object) == &EXTERNAL_CLASS;
}

/// A question about a value that was asked without a `Context`, answered in a
/// realm the value is legal in. A handle belongs to an isolate rather than to
/// a realm, so the realm that happens to be current need not be one the value
/// can be operated on from.
class ValueRealm {
   public:
    ValueRealm(JSContext* cx, const JS::Value& value) noexcept
        : realm_(cx, value.isObject() ? &value.toObject() : JS::CurrentGlobalOrNull(cx)) {}

    ValueRealm(const ValueRealm&) = delete;
    ValueRealm& operator=(const ValueRealm&) = delete;
    ValueRealm(ValueRealm&&) = delete;
    ValueRealm& operator=(ValueRealm&&) = delete;
    ~ValueRealm() = default;

   private:
    JSAutoNullableRealm realm_;
};

const JSClassOps GLOBAL_CLASS_OPS = {
    nullptr,  // addProperty
    nullptr,  // delProperty
    nullptr,  // enumerate
    nullptr,  // newEnumerate
    nullptr,  // resolve
    nullptr,  // mayResolve
    nullptr,  // finalize
    nullptr,  // call
    nullptr,  // construct
    JS_GlobalObjectTraceHook,
};

/// One reserved slot past the ones every global needs, holding the realm's
/// `ContextRec*`. That is how a native call finds the `Context` it is in.
const JSClass GLOBAL_CLASS = {"ub::Global", JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(2), &GLOBAL_CLASS_OPS};

}  // namespace

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------

bool Terminating(const Context& context) noexcept {
    return Terminating(OwnerOf(context));
}

JSObject* UtilityGlobal(Isolate& isolate) noexcept {
    JSContext* cx = Raw(isolate);
    if (isolate.impl().utility != nullptr) {
        return isolate.impl().utility;
    }
    JS::RealmOptions options;
    JS::RootedObject global(cx, JS_NewGlobalObject(cx, &GLOBAL_CLASS, nullptr, JS::FireOnNewGlobalHook, options));
    if (global == nullptr) {
        JS_ClearPendingException(cx);
        return nullptr;
    }
    {
        // The standard classes, because what gets made in here includes an
        // Error - `Throw(isolate, ...)` with nothing entered - and an error
        // needs its realm's prototype.
        JSAutoRealm realm(cx, global);
        if (!JS::InitRealmStandardClasses(cx)) {
            JS_ClearPendingException(cx);
            return nullptr;
        }
    }
    // Registered with the root list before it holds anything: a
    // `PersistentRooted` that was only assigned to is not a root, and a global
    // nothing roots is a global the collector may take while the isolate is
    // still using it.
    if (!isolate.impl().utility.initialized()) {
        isolate.impl().utility.init(cx);
    }
    isolate.impl().utility = global;
    return isolate.impl().utility;
}

JSString* MakeRawString(JSContext* cx, std::string_view utf8) noexcept {
    return JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(utf8.data(), utf8.size()));
}

// Not `JS_EncodeStringToUTF8`: that hands back a NUL-terminated buffer, which
// silently truncates a JavaScript string containing a NUL. Length first, then
// exactly that many bytes.
std::string EncodeToStdString(JSContext* cx, JS::HandleString string) {
    JSLinearString* linear = JS_EnsureLinearString(cx, string);
    if (linear == nullptr) {
        JS_ClearPendingException(cx);
        return {};
    }
    std::string out(JS::GetDeflatedUTF8StringLength(linear), '\0');
    const std::size_t written = JS::DeflateStringToUTF8Buffer(linear, mozilla::Span(out.data(), out.size()));
    out.resize(written);
    return out;
}

bool ToPropertyKey(JSContext* cx, Slot key, JS::MutableHandleId out) noexcept {
    JS::RootedValue value(cx);
    return ResolveHere(cx, key, &value) && JS_ValueToId(cx, value, out);
}

// The two vocabularies have opposite senses: V8 names what a property is NOT,
// SpiderMonkey names what it IS.
unsigned ToNativeAttributes(PropertyAttribute attributes) noexcept {
    unsigned flags = 0;
    if (!HasAttribute(attributes, PropertyAttribute::DontEnum)) {
        flags |= JSPROP_ENUMERATE;
    }
    if (HasAttribute(attributes, PropertyAttribute::ReadOnly)) {
        flags |= JSPROP_READONLY;
    }
    if (HasAttribute(attributes, PropertyAttribute::DontDelete)) {
        flags |= JSPROP_PERMANENT;
    }
    return flags;
}

JSExnType ToExnType(ErrorKind kind) noexcept {
    switch (kind) {
        case ErrorKind::TypeError:
            return JSEXN_TYPEERR;
        case ErrorKind::RangeError:
            return JSEXN_RANGEERR;
        case ErrorKind::ReferenceError:
            return JSEXN_REFERENCEERR;
        case ErrorKind::SyntaxError:
            return JSEXN_SYNTAXERR;
        case ErrorKind::Error:
            break;
    }
    return JSEXN_ERR;
}

bool MakeErrorValue(JSContext* cx, ErrorKind kind, std::string_view message, JS::MutableHandleValue out) noexcept {
    JS::RootedString text(cx, MakeRawString(cx, message));
    if (text == nullptr) {
        return false;
    }
    JS::RootedObject stack(cx);
    JS::RootedString fileName(cx, JS_NewStringCopyZ(cx, "<native>"));
    if (fileName == nullptr) {
        return false;
    }
    JS::Rooted<mozilla::Maybe<JS::Value>> cause(cx, mozilla::Nothing());
    return JS::CreateError(cx, ToExnType(kind), stack, fileName, 0, JS::ColumnNumberOneOrigin(), nullptr, text, cause,
                           out);
}

ContextRec* RecOfGlobal(JSObject* global) noexcept {
    if (global == nullptr || JS::GetClass(global) != &GLOBAL_CLASS) {
        return nullptr;
    }
    const JS::Value slot = JS::GetReservedSlot(global, GLOBAL_REC_SLOT);
    if (slot.isUndefined()) {
        return nullptr;
    }
    return static_cast<ContextRec*>(slot.toPrivate());
}

Context CurrentContext(JSContext* cx) noexcept {
    ContextRec* rec = RecOfGlobal(JS::CurrentGlobalOrNull(cx));
    if (rec == nullptr) {
        return {};
    }
    return Context::FromRec(rec);
}

// ---------------------------------------------------------------------------
// Frames
//
// This is the load-bearing part of the whole design. A frame IS a
// `JS::RootedVector<JS::Value>` - the engine's own stack root - constructed in
// place in the caller's `HandleScope`, which lives on the caller's stack.
// Construction links it onto the context's rooted list; destruction unlinks
// it, asserting inside the engine that the unlink is the innermost one. The
// API's LIFO rule and the engine's LIFO rule are therefore literally the same
// rule, enforced by the engine rather than by us.
//
// `escapable` is ignored: SpiderMonkey has nothing to reserve up front, so
// every frame here can escape as many handles as it likes. The flag exists for
// V8's benefit (docs/lifetimes.md section 4).
// ---------------------------------------------------------------------------

Frame& OpenFrame(Isolate& isolate, FrameStorage& storage, bool /*escapable*/) noexcept {
    auto* frame = ::new (static_cast<void*>(storage.bytes)) Frame(isolate, isolate.impl().current, nullptr);
    isolate.impl().current = frame;
    return *frame;
}

void CloseFrame(Frame& frame) noexcept {
    Isolate& owner = *frame.owner;
    assert(owner.impl().current == &frame && "handle scopes must close in reverse order of opening");
    owner.impl().current = frame.parent;
    frame.~Frame();
}

// A `JS::Value` is 8 bytes of data, not a pointer into the closing frame, so
// escaping is a copy into the parent's vector and nothing more. Both frames
// are live roots for the whole of it, so the value is never momentarily
// unrooted, and there is no limit on how many times a frame may escape.
Slot EscapeSlot(Frame& closing, Slot value) noexcept {
    Frame* parent = closing.parent;
    assert(parent != nullptr && "nothing to escape into: no enclosing HandleScope");
    const SlotIndex index = parent->Push(Resolve(value));
    if (index == Frame::NO_SLOT) {
        return NoSlot(*closing.owner);
    }
    return MakeSlot(*parent, index);
}

Isolate& IsolateOf(Frame& frame) noexcept {
    return *frame.owner;
}

Frame* CurrentFrame(Isolate& isolate) noexcept {
    return isolate.impl().current;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

ValueKind KindOf(Slot value) noexcept {
    const JS::Value raw = Resolve(value);
    if (raw.isUndefined()) {
        return ValueKind::Undefined;
    }
    if (raw.isNull()) {
        return ValueKind::Null;
    }
    if (raw.isBoolean()) {
        return ValueKind::Boolean;
    }
    if (raw.isNumber()) {
        return ValueKind::Number;
    }
    if (raw.isString()) {
        return ValueKind::String;
    }
    if (raw.isSymbol()) {
        return ValueKind::Symbol;
    }
    if (raw.isBigInt()) {
        return ValueKind::BigInt;
    }
    if (IsExternalObject(raw)) {
        return ValueKind::External;
    }
    if (raw.isObject()) {
        JSObject* object = Unwrapped(raw);
        if (JS::IsCallable(object)) {
            return ValueKind::Function;
        }
        JSContext* cx = Raw(IsolateFor(value));
        ValueRealm realm(cx, raw);
        JS::RootedObject rooted(cx, object);
        bool isArray = false;
        // A revoked proxy answers this by throwing, and this is a noexcept
        // query with nowhere to report it. Taking that throw away is the only
        // honest answer - but only the one this call raised, never something
        // that was already pending when it was asked.
        const bool hadPending = JS_IsExceptionPending(cx);
        if (!JS::IsArray(cx, rooted, &isArray)) {
            if (!hadPending) {
                JS_ClearPendingException(cx);
            }
            return ValueKind::Object;
        }
        return isArray ? ValueKind::Array : ValueKind::Object;
    }
    return ValueKind::Other;
}

bool IsType(Slot value, TypeCode type) noexcept {
    const JS::Value raw = Resolve(value);
    switch (type) {
        case TypeCode::Value:
            return true;
        case TypeCode::Primitive:
            return !raw.isObject();
        case TypeCode::Boolean:
            return raw.isBoolean();
        case TypeCode::Number:
            return raw.isNumber();
        case TypeCode::Integer:
            // V8's IsInt32 is true for a double that happens to be an exact
            // int32, so ask the same question rather than asking how the
            // engine happens to be storing it.
            return raw.isInt32() || (raw.isDouble() &&
                                     raw.toDouble() == static_cast<double>(static_cast<std::int32_t>(raw.toDouble())) &&
                                     !(raw.toDouble() == 0.0 && std::signbit(raw.toDouble())));
        case TypeCode::Name:
            return raw.isString() || raw.isSymbol();
        case TypeCode::String:
            return raw.isString();
        case TypeCode::Symbol:
            return raw.isSymbol();
        case TypeCode::BigInt:
            return raw.isBigInt();
        case TypeCode::Object:
            return raw.isObject() && !IsExternalObject(raw);
        case TypeCode::Array: {
            if (!raw.isObject()) {
                return false;
            }
            JSContext* cx = Raw(IsolateFor(value));
            ValueRealm realm(cx, raw);
            JS::RootedObject object(cx, Unwrapped(raw));
            bool isArray = false;
            // As in `KindOf`: a revoked proxy throws rather than answering, and
            // a query that says `noexcept` must not leave that behind for
            // whatever the caller does next.
            const bool hadPending = JS_IsExceptionPending(cx);
            if (!JS::IsArray(cx, object, &isArray)) {
                if (!hadPending) {
                    JS_ClearPendingException(cx);
                }
                return false;
            }
            return isArray;
        }
        case TypeCode::Function:
            return raw.isObject() && JS::IsCallable(Unwrapped(raw));
        // Asked of the object itself rather than of whatever stands in for it
        // in the current realm: a buffer made in one realm and asked about from
        // another is still a buffer.
        case TypeCode::ArrayBuffer:
            return raw.isObject() && JS::IsArrayBufferObject(Unwrapped(raw));
        case TypeCode::ArrayBufferView:
            return raw.isObject() && JS_IsArrayBufferViewObject(Unwrapped(raw));
        case TypeCode::TypedArray:
            return raw.isObject() && JS_IsTypedArrayObject(Unwrapped(raw));
        case TypeCode::DataView:
            return raw.isObject() && static_cast<bool>(JS::DataView::fromObject(Unwrapped(raw)));
        case TypeCode::Promise: {
            if (!raw.isObject()) {
                return false;
            }
            JSContext* cx = Raw(IsolateFor(value));
            ValueRealm realm(cx, raw);
            JS::RootedObject object(cx, Unwrapped(raw));
            return JS::IsPromiseObject(object);
        }
        case TypeCode::External:
            return IsExternalObject(raw);
    }
    return false;
}

namespace {

/// Both sides of a comparison, in one realm. The left one's realm is chosen
/// and the right is wrapped into it, because the two handles may legitimately
/// name values from different realms of the same isolate.
bool Compare(Slot lhs, Slot rhs, bool (*how)(JSContext*, JS::Handle<JS::Value>, JS::Handle<JS::Value>, bool*)) {
    JSContext* cx = Raw(IsolateFor(lhs));
    const JS::Value left = Resolve(lhs);
    ValueRealm realm(cx, left);
    JS::RootedValue a(cx, left);
    JS::RootedValue b(cx);
    bool answer = false;
    if (!ResolveHere(cx, rhs, &b) || !how(cx, a, b, &answer)) {
        JS_ClearPendingException(cx);
        return false;
    }
    return answer;
}

}  // namespace

bool StrictEquals(Slot lhs, Slot rhs) noexcept {
    return Compare(lhs, rhs, &JS::StrictlyEqual);
}

bool SameValue(Slot lhs, Slot rhs) noexcept {
    return Compare(lhs, rhs, &JS::SameValue);
}

Maybe<bool> LooseEquals(const Context& context, Slot lhs, Slot rhs) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedValue a(cx);
    JS::RootedValue b(cx);
    bool equal = false;
    if (!ResolveHere(cx, lhs, &a) || !ResolveHere(cx, rhs, &b) || !JS::LooselyEqual(cx, a, b, &equal)) {
        return std::nullopt;
    }
    return equal;
}

// ---------------------------------------------------------------------------
// Reading primitives
// ---------------------------------------------------------------------------

bool BooleanValue(Slot value) noexcept {
    JSContext* cx = Raw(IsolateFor(value));
    JS::RootedValue raw(cx, Resolve(value));
    return JS::ToBoolean(raw);
}

double NumberValue(Slot value) noexcept {
    const JS::Value raw = Resolve(value);
    return raw.isNumber() ? raw.toNumber() : std::numeric_limits<double>::quiet_NaN();
}

std::int32_t Int32Value(Slot value) noexcept {
    const JS::Value raw = Resolve(value);
    if (raw.isInt32()) {
        return raw.toInt32();
    }
    if (raw.isDouble()) {
        return static_cast<std::int32_t>(raw.toDouble());
    }
    return 0;
}

std::size_t Utf8Length(Slot string) noexcept {
    JSContext* cx = Raw(IsolateFor(string));
    const JS::Value raw = Resolve(string);
    if (!raw.isString()) {
        return 0;
    }
    JS::RootedString rooted(cx, raw.toString());
    JSLinearString* linear = JS_EnsureLinearString(cx, rooted);
    if (linear == nullptr) {
        JS_ClearPendingException(cx);
        return 0;
    }
    return JS::GetDeflatedUTF8StringLength(linear);
}

std::size_t WriteUtf8(Slot string, std::span<char> out) noexcept {
    if (out.empty()) {
        return 0;
    }
    JSContext* cx = Raw(IsolateFor(string));
    const JS::Value raw = Resolve(string);
    if (!raw.isString()) {
        return 0;
    }
    JS::RootedString rooted(cx, raw.toString());
    auto written = JS_EncodeStringToUTF8BufferPartial(cx, rooted, mozilla::Span(out.data(), out.size()));
    if (written.isNothing()) {
        JS_ClearPendingException(cx);
        return 0;
    }
    return std::get<1>(*written);
}

std::string ToStdString(Slot string) {
    JSContext* cx = Raw(IsolateFor(string));
    const JS::Value raw = Resolve(string);
    if (!raw.isString()) {
        return {};
    }
    JS::RootedString rooted(cx, raw.toString());
    return EncodeToStdString(cx, rooted);
}

Maybe<std::string> SymbolDescription(Slot symbol) {
    JSContext* cx = Raw(IsolateFor(symbol));
    const JS::Value raw = Resolve(symbol);
    if (!raw.isSymbol()) {
        return std::nullopt;
    }
    JS::Rooted<JS::Symbol*> rooted(cx, raw.toSymbol());
    JSString* description = JS::GetSymbolDescription(rooted);
    if (description == nullptr) {
        return std::nullopt;
    }
    JS::RootedString text(cx, description);
    return EncodeToStdString(cx, text);
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

Maybe<bool> ToBoolean(const Context& context, Slot value) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedValue raw(cx);
    if (!ResolveHere(cx, value, &raw)) {
        return std::nullopt;
    }
    return JS::ToBoolean(raw);
}

Maybe<double> ToNumber(const Context& context, Slot value) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedValue raw(cx);
    double number = 0.0;
    if (!ResolveHere(cx, value, &raw) || !JS::ToNumber(cx, raw, &number)) {
        return std::nullopt;
    }
    return number;
}

Maybe<std::int32_t> ToInt32(const Context& context, Slot value) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedValue raw(cx);
    std::int32_t number = 0;
    if (!ResolveHere(cx, value, &raw) || !JS::ToInt32(cx, raw, &number)) {
        return std::nullopt;
    }
    return number;
}

Maybe<std::uint32_t> ToUint32(const Context& context, Slot value) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedValue raw(cx);
    std::uint32_t number = 0;
    if (!ResolveHere(cx, value, &raw) || !JS::ToUint32(cx, raw, &number)) {
        return std::nullopt;
    }
    return number;
}

Maybe<Slot> ToJsString(const Context& context, Slot value) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedValue raw(cx);
    if (!ResolveHere(cx, value, &raw)) {
        return std::nullopt;
    }
    JSString* string = JS::ToString(cx, raw);
    if (string == nullptr) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), JS::StringValue(string));
}

Maybe<Slot> ToJsObject(const Context& context, Slot value) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedValue raw(cx);
    if (!ResolveHere(cx, value, &raw)) {
        return std::nullopt;
    }
    JSObject* object = JS::ToObject(cx, raw);
    if (object == nullptr) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), object);
}

// ---------------------------------------------------------------------------
// Making values
// ---------------------------------------------------------------------------

Slot MakeUndefined(Isolate& isolate) noexcept {
    return Push(isolate, JS::UndefinedValue());
}
Slot MakeNull(Isolate& isolate) noexcept {
    return Push(isolate, JS::NullValue());
}
Slot MakeBoolean(Isolate& isolate, bool value) noexcept {
    return Push(isolate, JS::BooleanValue(value));
}
Slot MakeNumber(Isolate& isolate, double value) noexcept {
    return Push(isolate, JS::NumberValue(value));
}
Slot MakeInteger(Isolate& isolate, std::int32_t value) noexcept {
    return Push(isolate, JS::Int32Value(value));
}
Slot MakeUnsigned(Isolate& isolate, std::uint32_t value) noexcept {
    return Push(isolate, JS::NumberValue(value));
}

Maybe<Slot> MakeString(Isolate& isolate, std::string_view utf8) {
    IsolateRealm realm(isolate);
    if (!realm.Usable()) {
        return std::nullopt;
    }
    JSString* string = MakeRawString(Raw(isolate), utf8);
    if (string == nullptr) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, JS::StringValue(string));
}

Maybe<Slot> MakeSymbol(Isolate& isolate, Maybe<std::string_view> description) {
    JSContext* cx = Raw(isolate);
    IsolateRealm realm(isolate);
    if (!realm.Usable()) {
        return std::nullopt;
    }
    JS::RootedString text(cx);
    if (description) {
        text = MakeRawString(cx, *description);
        if (text == nullptr) {
            return std::nullopt;
        }
    }
    JS::Symbol* symbol = JS::NewSymbol(cx, text);
    if (symbol == nullptr) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, JS::SymbolValue(symbol));
}

Maybe<Slot> MakeSymbolFor(Isolate& isolate, std::string_view key) {
    JSContext* cx = Raw(isolate);
    IsolateRealm realm(isolate);
    if (!realm.Usable()) {
        return std::nullopt;
    }
    JS::RootedString text(cx, MakeRawString(cx, key));
    if (text == nullptr) {
        return std::nullopt;
    }
    JS::Symbol* symbol = JS::GetSymbolFor(cx, text);
    if (symbol == nullptr) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, JS::SymbolValue(symbol));
}

Maybe<Slot> GetWellKnownSymbol(Isolate& isolate, WellKnownSymbol which) {
    JS::SymbolCode code = JS::SymbolCode::iterator;
    switch (which) {
        case WellKnownSymbol::Iterator:
            code = JS::SymbolCode::iterator;
            break;
        case WellKnownSymbol::AsyncIterator:
            code = JS::SymbolCode::asyncIterator;
            break;
        case WellKnownSymbol::HasInstance:
            code = JS::SymbolCode::hasInstance;
            break;
        case WellKnownSymbol::ToPrimitive:
            code = JS::SymbolCode::toPrimitive;
            break;
        case WellKnownSymbol::ToStringTag:
            code = JS::SymbolCode::toStringTag;
            break;
    }
    JS::Symbol* symbol = JS::GetWellKnownSymbol(Raw(isolate), code);
    if (symbol == nullptr) {
        return std::nullopt;
    }
    return PushOrNothing(isolate, JS::SymbolValue(symbol));
}

Maybe<Slot> MakeObject(const Context& context) {
    RealmGuard realm(context);
    JSObject* object = JS_NewPlainObject(Raw(context));
    if (object == nullptr) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), object);
}

Maybe<Slot> MakeArray(const Context& context, std::uint32_t length) {
    RealmGuard realm(context);
    JSObject* array = JS::NewArrayObject(Raw(context), length);
    if (array == nullptr) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), array);
}

Maybe<Slot> MakeError(const Context& context, ErrorKind kind, std::string_view message) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedValue error(cx);
    if (!MakeErrorValue(cx, kind, message, &error)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), error);
}

// ---------------------------------------------------------------------------
// Native functions and externals
// ---------------------------------------------------------------------------

void DestroyCallbackRecord(CallbackRecord* record) noexcept {
    if (record == nullptr) {
        return;
    }
    record->~CallbackRecord();
    FrameRelease(record);
}

CallbackRecord* StoreCallback(Isolate& isolate, CallbackRecord record) {
    // Through `FrameAllocate` rather than `new`, so that a replaced
    // `operator new` - an embedder's, or the suite's - can see it. See
    // `frame_alloc.h`: a `new` in this translation unit is `moz_xmalloc`.
    void* memory = FrameAllocate(sizeof(CallbackRecord));
    if (memory == nullptr) {
        return nullptr;
    }
    record.owner = &isolate;
    auto* raw = ::new (memory) CallbackRecord(record);
    try {
        isolate.impl().callbacks.emplace(raw, CallbackRecordPtr(raw));
    } catch (const std::bad_alloc&) {
        DestroyCallbackRecord(raw);
        return nullptr;
    }
    return raw;
}

/// Give a record back when the value that carries it is finalised.
///
/// Called from a finalizer, so it allocates nothing and touches no GC thing:
/// erasing from the map destroys the record, which is a plain C++ object.
void ReleaseCallbackRecord(CallbackRecord* record) noexcept {
    if (record == nullptr || record->owner == nullptr) {
        return;
    }
    record->owner->impl().callbacks.erase(record);
}

bool FunctionTrampoline(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    // Read the callee before rval() is touched: SpiderMonkey reuses that slot.
    auto* record =
        static_cast<CallbackRecord*>(js::GetFunctionNativeReserved(&args.callee(), FUNCTION_RECORD_SLOT).toPrivate());
    // And, for the same reason, the data value - which is reached through the
    // callee - before anything writes the result.
    JS::Value dataValue = JS::UndefinedValue();
    if (record->hasValue) {
        const JS::Value holder = js::GetFunctionNativeReserved(&args.callee(), FUNCTION_HOLDER_SLOT);
        dataValue = JS::GetReservedSlot(&holder.toObject(), RECORD_HOLDER_VALUE_SLOT);
    }

    auto* isolate = static_cast<Isolate*>(JS_GetContextPrivate(cx));
    CallFrame frame(*isolate, &args);

    JS::Value thisValue = args.thisv();
    if (!thisValue.isObject()) {
        // Sloppy-mode boxing, roughly: the API hands a callback a Local<Object>
        // and there is nothing useful to say about an undefined receiver.
        JSObject* global = JS::CurrentGlobalOrNull(cx);
        if (global != nullptr) {
            thisValue = JS::ObjectValue(*global);
        }
    }
    const SlotIndex self = frame.frame().Push(thisValue);
    // Rooted in the frame before anything can collect: `dataValue` is a copy
    // out of a reserved slot, and a moving collector would leave it stale.
    const SlotIndex value = record->hasValue ? frame.frame().Push(dataValue) : Frame::NO_SLOT;

    args.rval().setUndefined();
    CallbackState state{.owner = isolate,
                        .frame = &frame.frame(),
                        .context = CurrentContext(cx),
                        .call = &args,
                        .result = args.rval().address(),
                        .thisSlot = self,
                        .holderSlot = self,
                        .data = record->data,
                        .isConstruct = args.isConstructing(),
                        .valueSlot = value,
                        .hasValue = record->hasValue};
    record->callback(CallbackInfo(state));
    return FinishNativeCall(cx, args);
}

JSObject* NewNativeFunction(JSContext* cx, CallbackRecord* record, std::string_view name) {
    const std::string owned(name);
    JSFunction* function = js::NewFunctionWithReserved(cx, &FunctionTrampoline, 0, 0, owned.c_str());
    if (function == nullptr) {
        return nullptr;
    }
    JSObject* object = JS_GetFunctionObject(function);
    js::SetFunctionNativeReserved(object, FUNCTION_RECORD_SLOT, JS::PrivateValue(record));
    return object;
}

namespace {

/// `Function::New`, with or without a script value for its data. `value` is
/// null for the first.
Maybe<Slot> NewPlainFunction(const Context& context, FunctionCallback callback, CallbackData data, const Slot* value) {
    JSContext* cx = Raw(context);
    Isolate& owner = OwnerOf(context);
    RealmGuard realm(context);
    // Into this realm before anything is allocated: a value from another realm
    // of the isolate is legal to hand over, and the holder that keeps it is in
    // this one.
    JS::RootedValue dataValue(cx);
    if (value != nullptr && !ResolveHere(cx, *value, &dataValue)) {
        return std::nullopt;
    }
    CallbackRecord* record =
        StoreCallback(owner, CallbackRecord{.callback = callback, .data = data, .hasValue = value != nullptr});
    if (record == nullptr) {
        return std::nullopt;
    }
    JS::RootedObject function(cx, NewNativeFunction(cx, record, ""));
    if (function == nullptr) {
        ReleaseCallbackRecord(record);
        return std::nullopt;
    }
    // A function made here is a *value*: script drops it, the collector takes
    // it, and the record it carries has to go with it. A `JSFunction` has no
    // finalizer of ours, so the holder in its second reserved slot is what has
    // one - reachable exactly as long as the function, and collected with it.
    JSObject* holder = JS_NewObject(cx, &RECORD_HOLDER_CLASS);
    if (holder == nullptr) {
        ReleaseCallbackRecord(record);
        return std::nullopt;
    }
    JS::SetReservedSlot(holder, RECORD_HOLDER_SLOT, JS::PrivateValue(record));
    JS::SetReservedSlot(holder, RECORD_HOLDER_VALUE_SLOT, dataValue);
    js::SetFunctionNativeReserved(function, FUNCTION_HOLDER_SLOT, JS::ObjectValue(*holder));
    return PushOrNothing(owner, function);
}

}  // namespace

Maybe<Slot> MakeFunction(const Context& context, FunctionCallback callback, CallbackData data) {
    return NewPlainFunction(context, callback, data, nullptr);
}

Maybe<Slot> MakeFunctionWithValue(const Context& context, FunctionCallback callback, Slot data) {
    return NewPlainFunction(context, callback, {}, &data);
}

Maybe<Slot> MakeExternal(Isolate& isolate, CallbackData data) {
    JSContext* cx = Raw(isolate);
    IsolateRealm realm(isolate);
    if (!realm.Usable()) {
        return std::nullopt;
    }
    CallbackRecord* record = StoreCallback(isolate, CallbackRecord{.data = data});
    if (record == nullptr) {
        return std::nullopt;
    }
    JSObject* object = JS_NewObject(cx, &EXTERNAL_CLASS);
    if (object == nullptr) {
        ReleaseCallbackRecord(record);
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    // The external's own finalizer gives the record back; nothing else knows
    // when an external has gone.
    JS::SetReservedSlot(object, EXTERNAL_RECORD_SLOT, JS::PrivateValue(record));
    return PushOrNothing(isolate, object);
}

CallbackData ExternalData(Slot external) noexcept {
    const JS::Value raw = Resolve(external);
    if (!IsExternalObject(raw)) {
        return {};
    }
    auto* record = static_cast<CallbackRecord*>(JS::GetReservedSlot(Unwrapped(raw), EXTERNAL_RECORD_SLOT).toPrivate());
    return record == nullptr ? CallbackData{} : record->data;
}

// ---------------------------------------------------------------------------
// Binary data
//
// Every read here copies, and that is the contract rather than laziness: both
// collectors move an ArrayBuffer's storage - SpiderMonkey keeps a small one
// inline in the object's GC header, where `JS::GetArrayBufferData` warns the
// pointer "can become invalid on GC" - so a pointer handed to an embedder would
// be a pointer it must not hold across anything that allocates. There is no way
// to say that in a signature, so nothing here says it.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] JS::Scalar::Type ToScalarType(ElementType type) noexcept {
    switch (type) {
        case ElementType::Int8:
            return JS::Scalar::Int8;
        case ElementType::Uint8:
            return JS::Scalar::Uint8;
        case ElementType::Uint8Clamped:
            return JS::Scalar::Uint8Clamped;
        case ElementType::Int16:
            return JS::Scalar::Int16;
        case ElementType::Uint16:
            return JS::Scalar::Uint16;
        case ElementType::Int32:
            return JS::Scalar::Int32;
        case ElementType::Uint32:
            return JS::Scalar::Uint32;
        case ElementType::Float32:
            return JS::Scalar::Float32;
        case ElementType::BigInt64:
            return JS::Scalar::BigInt64;
        case ElementType::BigUint64:
            return JS::Scalar::BigUint64;
        case ElementType::Float16:
            return JS::Scalar::Float16;
        case ElementType::Float64:
            break;
    }
    return JS::Scalar::Float64;
}

[[nodiscard]] ElementType FromScalarType(JS::Scalar::Type type) noexcept {
    switch (type) {
        case JS::Scalar::Int8:
            return ElementType::Int8;
        case JS::Scalar::Uint8:
            return ElementType::Uint8;
        case JS::Scalar::Uint8Clamped:
            return ElementType::Uint8Clamped;
        case JS::Scalar::Int16:
            return ElementType::Int16;
        case JS::Scalar::Uint16:
            return ElementType::Uint16;
        case JS::Scalar::Int32:
            return ElementType::Int32;
        case JS::Scalar::Uint32:
            return ElementType::Uint32;
        case JS::Scalar::Float32:
            return ElementType::Float32;
        case JS::Scalar::BigInt64:
            return ElementType::BigInt64;
        case JS::Scalar::BigUint64:
            return ElementType::BigUint64;
        case JS::Scalar::Float16:
            return ElementType::Float16;
        default:
            break;
    }
    return ElementType::Float64;
}

/// The object behind a handle, seen through whatever wrapper stands in for it
/// in the current realm. Asking an ArrayBuffer its length has to be asked of
/// the buffer, not of a cross-compartment wrapper for one.
[[nodiscard]] JSObject* UnwrappedObjectOf(Slot value) noexcept {
    const JS::Value raw = Resolve(value);
    return raw.isObject() ? js::UncheckedUnwrap(&raw.toObject()) : nullptr;
}

/// Copy out of a buffer or a view, under a no-GC guard because the pointer is
/// only valid for as long as nothing allocates.
[[nodiscard]] std::size_t CopyBytesOut(JSObject* object, std::size_t offset, std::size_t available,
                                       std::span<std::byte> out) noexcept {
    const std::size_t count = available < out.size() ? available : out.size();
    if (count == 0) {
        return 0;
    }
    JS::AutoCheckCannotGC nogc;
    bool isShared = false;
    const uint8_t* data = JS::GetArrayBufferData(object, &isShared, nogc);
    if (data == nullptr) {
        return 0;
    }
    std::memcpy(out.data(), data + offset, count);
    return count;
}

}  // namespace

Maybe<Slot> MakeArrayBuffer(const Context& context, std::span<const std::byte> bytes, std::size_t byteLength) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedObject buffer(cx, JS::NewArrayBuffer(cx, byteLength));
    if (buffer == nullptr) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    if (!bytes.empty()) {
        const std::size_t count = bytes.size() < byteLength ? bytes.size() : byteLength;
        JS::AutoCheckCannotGC nogc;
        bool isShared = false;
        uint8_t* data = JS::GetArrayBufferData(buffer, &isShared, nogc);
        if (data == nullptr) {
            return std::nullopt;
        }
        std::memcpy(data, bytes.data(), count);
    }
    return PushOrNothing(OwnerOf(context), JS::ObjectValue(*buffer));
}

std::size_t ArrayBufferByteLength(Slot buffer) noexcept {
    JSObject* object = UnwrappedObjectOf(buffer);
    if (object == nullptr || !JS::IsArrayBufferObject(object)) {
        return 0;
    }
    return JS::GetArrayBufferByteLength(object);
}

std::size_t ArrayBufferCopyOut(Slot buffer, std::span<std::byte> out) noexcept {
    JSObject* object = UnwrappedObjectOf(buffer);
    if (object == nullptr || !JS::IsArrayBufferObject(object)) {
        return 0;
    }
    return CopyBytesOut(object, 0, JS::GetArrayBufferByteLength(object), out);
}

Maybe<Slot> MakeTypedArray(const Context& context, ElementType type, Slot buffer, std::size_t byteOffset,
                           std::size_t length) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedValue raw(cx);
    if (!ResolveHere(cx, buffer, &raw) || !raw.isObject()) {
        return std::nullopt;
    }
    JS::RootedObject target(cx, &raw.toObject());

    JSObject* unwrapped = js::UncheckedUnwrap(target);
    if (unwrapped == nullptr || !JS::IsArrayBufferObject(unwrapped)) {
        return std::nullopt;
    }
    // Checked here rather than left to the engine, because the engine's answer
    // to a view that does not fit is a RangeError thrown into script, and this
    // is not script: the header says empty.
    const std::size_t byteLength = JS::GetArrayBufferByteLength(unwrapped);
    const std::size_t elementSize = ElementSize(type);
    // Checked before the multiplication, not after: a byte count that wrapped
    // would pass the bounds test below and reach the engine as a length it
    // never agreed to.
    if (elementSize == 0 || length > static_cast<std::size_t>(-1) / elementSize) {
        return std::nullopt;
    }
    const std::size_t wanted = length * elementSize;
    if (byteOffset > byteLength || wanted > byteLength - byteOffset) {
        return std::nullopt;
    }

    JSObject* view = nullptr;
    const auto count = static_cast<std::int64_t>(length);
    switch (type) {
        case ElementType::Int8:
            view = JS_NewInt8ArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::Uint8:
            view = JS_NewUint8ArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::Uint8Clamped:
            view = JS_NewUint8ClampedArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::Int16:
            view = JS_NewInt16ArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::Uint16:
            view = JS_NewUint16ArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::Int32:
            view = JS_NewInt32ArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::Uint32:
            view = JS_NewUint32ArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::Float32:
            view = JS_NewFloat32ArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::Float64:
            view = JS_NewFloat64ArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::BigInt64:
            view = JS_NewBigInt64ArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::BigUint64:
            view = JS_NewBigUint64ArrayWithBuffer(cx, target, byteOffset, count);
            break;
        case ElementType::Float16:
            view = JS_NewFloat16ArrayWithBuffer(cx, target, byteOffset, count);
            break;
    }
    if (view == nullptr) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), JS::ObjectValue(*view));
}

ElementType TypedArrayElementType(Slot view) noexcept {
    JSObject* object = UnwrappedObjectOf(view);
    if (object == nullptr || !JS_IsTypedArrayObject(object)) {
        return ElementType::Uint8;
    }
    return FromScalarType(JS_GetArrayBufferViewType(object));
}

std::size_t TypedArrayLength(Slot view) noexcept {
    JSObject* object = UnwrappedObjectOf(view);
    if (object == nullptr || !JS_IsTypedArrayObject(object)) {
        return 0;
    }
    return JS_GetTypedArrayLength(object);
}

std::size_t TypedArrayByteOffset(Slot view) noexcept {
    JSObject* object = UnwrappedObjectOf(view);
    if (object == nullptr || !JS_IsTypedArrayObject(object)) {
        return 0;
    }
    return JS_GetTypedArrayByteOffset(object);
}

Maybe<Slot> TypedArrayBuffer(const Context& context, Slot view) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedValue raw(cx);
    if (!ResolveHere(cx, view, &raw) || !raw.isObject()) {
        return std::nullopt;
    }
    JS::RootedObject target(cx, &raw.toObject());
    if (!JS_IsArrayBufferViewObject(js::UncheckedUnwrap(target))) {
        return std::nullopt;
    }
    bool isShared = false;
    JSObject* buffer = JS_GetArrayBufferViewBuffer(cx, target, &isShared);
    if (buffer == nullptr) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), JS::ObjectValue(*buffer));
}

std::size_t TypedArrayCopyOut(Slot view, std::span<std::byte> out) noexcept {
    JSObject* object = UnwrappedObjectOf(view);
    if (object == nullptr || !JS_IsTypedArrayObject(object)) {
        return 0;
    }
    const std::size_t bytes =
        JS_GetTypedArrayLength(object) * ElementSize(FromScalarType(JS_GetArrayBufferViewType(object)));
    const std::size_t count = bytes < out.size() ? bytes : out.size();
    if (count == 0) {
        return 0;
    }
    JS::AutoCheckCannotGC nogc;
    bool isShared = false;
    // The view's own data pointer, which already has the byte offset applied -
    // asking the buffer instead would mean adding it back by hand.
    uint8_t* data = nullptr;
    std::size_t available = 0;
    js::GetArrayBufferViewLengthAndData(object, &available, &isShared, &data);
    if (data == nullptr) {
        return 0;
    }
    const std::size_t safe = available < count ? available : count;
    std::memcpy(out.data(), data, safe);
    return safe;
}

// ---------------------------------------------------------------------------
// Any view, typed array or DataView
//
// The engine's generic `JS_GetArrayBufferView*` answers, with one thing added:
// a view whose buffer has been detached answers zero for its offset as well as
// its length. V8 does, and the offset a view had into a buffer it no longer
// has is not an answer to anything.
// ---------------------------------------------------------------------------

namespace {

/// The view behind a handle, or a null one if the handle does not name a view
/// or names one whose buffer is gone.
[[nodiscard]] JS::ArrayBufferView LiveViewOf(Slot view) noexcept {
    JSObject* object = UnwrappedObjectOf(view);
    if (object == nullptr) {
        return JS::ArrayBufferView::fromObject(nullptr);
    }
    JS::ArrayBufferView found = JS::ArrayBufferView::fromObject(object);
    if (!found || found.isDetached()) {
        return JS::ArrayBufferView::fromObject(nullptr);
    }
    return found;
}

}  // namespace

std::size_t ArrayBufferViewByteLength(Slot view) noexcept {
    const JS::ArrayBufferView live = LiveViewOf(view);
    return live ? JS_GetArrayBufferViewByteLength(live.asObjectUnbarriered()) : 0;
}

std::size_t ArrayBufferViewByteOffset(Slot view) noexcept {
    const JS::ArrayBufferView live = LiveViewOf(view);
    return live ? JS_GetArrayBufferViewByteOffset(live.asObjectUnbarriered()) : 0;
}

Maybe<Slot> ArrayBufferViewBuffer(const Context& context, Slot view) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedValue raw(cx);
    if (!ResolveHere(cx, view, &raw) || !raw.isObject()) {
        return std::nullopt;
    }
    JS::RootedObject target(cx, &raw.toObject());
    if (!JS_IsArrayBufferViewObject(js::UncheckedUnwrap(target))) {
        return std::nullopt;
    }
    bool isShared = false;
    JSObject* buffer = JS_GetArrayBufferViewBuffer(cx, target, &isShared);
    if (buffer == nullptr) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    // A SharedArrayBuffer is not an ArrayBuffer to this API, and the handle
    // this becomes would say it was one.
    if (isShared) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), JS::ObjectValue(*buffer));
}

std::size_t ArrayBufferViewCopyOut(Slot view, std::span<std::byte> out) noexcept {
    JS::ArrayBufferView live = LiveViewOf(view);
    if (!live || out.empty()) {
        return 0;
    }
    JS::AutoCheckCannotGC nogc;
    bool isShared = false;
    // The view's own range: the span starts at its byte offset into the buffer
    // and is as long as the view, so nothing has to be added back by hand.
    const mozilla::Span<uint8_t> data = live.getData(&isShared, nogc);
    const std::size_t count = data.size() < out.size() ? data.size() : out.size();
    if (count == 0 || data.data() == nullptr) {
        return 0;
    }
    std::memcpy(out.data(), data.data(), count);
    return count;
}

Maybe<Slot> MakeDataView(const Context& context, Slot buffer, std::size_t byteOffset, std::size_t byteLength) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedValue raw(cx);
    if (!ResolveHere(cx, buffer, &raw) || !raw.isObject()) {
        return std::nullopt;
    }
    JS::RootedObject target(cx, &raw.toObject());

    JSObject* unwrapped = js::UncheckedUnwrap(target);
    if (unwrapped == nullptr || !JS::IsArrayBufferObject(unwrapped)) {
        return std::nullopt;
    }
    // Checked here for the reason `MakeTypedArray` checks: the engine's answer
    // to a view that does not fit, or to a detached buffer, is an exception
    // thrown into script, and this is not script.
    if (JS::IsDetachedArrayBufferObject(unwrapped)) {
        return std::nullopt;
    }
    const std::size_t available = JS::GetArrayBufferByteLength(unwrapped);
    if (byteOffset > available || byteLength > available - byteOffset) {
        return std::nullopt;
    }
    JSObject* view = JS_NewDataView(cx, target, byteOffset, byteLength);
    if (view == nullptr) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), JS::ObjectValue(*view));
}

// ---------------------------------------------------------------------------
// Promises
//
// `JS::NewPromiseObject(cx, nullptr)` is a promise with no executor, which is
// the shape the API wants: the embedder holds it and settles it later, rather
// than handing the engine a function to be called back through.
// ---------------------------------------------------------------------------

Maybe<Slot> MakePromise(const Context& context) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedObject executor(cx);
    JSObject* promise = JS::NewPromiseObject(cx, executor);
    if (promise == nullptr) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), JS::ObjectValue(*promise));
}

namespace {

/// Both settle paths want the promise as an object in this realm and the value
/// wrapped into it. A promise made in one realm and settled from another is an
/// ordinary thing to do once `Global` can carry it across.
bool SettleArgs(JSContext* cx, Slot promise, Slot value, JS::MutableHandleObject outPromise,
                JS::MutableHandleValue outValue) {
    JS::RootedValue raw(cx);
    if (!ResolveHere(cx, promise, &raw) || !raw.isObject()) {
        return false;
    }
    outPromise.set(&raw.toObject());
    if (!JS::IsPromiseObject(outPromise)) {
        return false;
    }
    return ResolveHere(cx, value, outValue);
}

}  // namespace

Maybe<bool> ResolvePromise(const Context& context, Slot promise, Slot value) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedObject target(cx);
    JS::RootedValue with(cx);
    if (!SettleArgs(cx, promise, value, &target, &with)) {
        return std::nullopt;
    }
    if (!JS::ResolvePromise(cx, target, with)) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    return true;
}

Maybe<bool> RejectPromise(const Context& context, Slot promise, Slot reason) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedObject target(cx);
    JS::RootedValue with(cx);
    if (!SettleArgs(cx, promise, reason, &target, &with)) {
        return std::nullopt;
    }
    if (!JS::RejectPromise(cx, target, with)) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    return true;
}

PromiseState PromiseStateOf(Slot promise) noexcept {
    JSObject* object = UnwrappedObjectOf(promise);
    if (object == nullptr) {
        return PromiseState::Pending;
    }
    JSContext* cx = Raw(IsolateFor(promise));
    JS::RootedObject rooted(cx, object);
    if (!JS::IsPromiseObject(rooted)) {
        return PromiseState::Pending;
    }
    switch (JS::GetPromiseState(rooted)) {
        case JS::PromiseState::Fulfilled:
            return PromiseState::Fulfilled;
        case JS::PromiseState::Rejected:
            return PromiseState::Rejected;
        case JS::PromiseState::Pending:
            break;
    }
    return PromiseState::Pending;
}

// ---------------------------------------------------------------------------
// Structured clone
//
// `DifferentProcess` rather than `SameProcess`, and not because anything here
// crosses a process. The two scopes differ in exactly the way the public
// contract cares about: `SameProcess` may write *pointers* into the blob - an
// ArrayBuffer's contents stay where they are and the blob refers to them, owned
// by the `JSStructuredCloneData` that produced it - while `DifferentProcess`
// copies those contents into the bytes. `unibind/value.h` promises owned bytes the
// receiving thread can simply hold, so the blob has to survive the writer being
// destroyed, and under `SameProcess` it would not: the vector would come back
// full of pointers into freed memory, and reading it would look like it worked.
// ---------------------------------------------------------------------------

namespace {

constexpr JS::StructuredCloneScope CLONE_SCOPE = JS::StructuredCloneScope::DifferentProcess;

}  // namespace

Maybe<std::vector<std::uint8_t>> SerializeValue(const Context& context, Slot value) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedValue raw(cx);
    if (!ResolveHere(cx, value, &raw)) {
        return std::nullopt;
    }

    JSStructuredCloneData data(CLONE_SCOPE);
    const JS::CloneDataPolicy policy;
    JS::RootedValue transferable(cx, JS::UndefinedValue());
    if (!JS_WriteStructuredClone(cx, raw, &data, CLONE_SCOPE, policy, nullptr, nullptr, transferable)) {
        // A function, a class instance carrying a native, a proxy, or anything
        // holding one of those: the engine throws DataCloneError and the whole
        // operation fails, which is what the header asks for. The throw is the
        // engine telling us, not something the caller asked for, so it does not
        // travel any further.
        JS_ClearPendingException(cx);
        return std::nullopt;
    }

    std::vector<std::uint8_t> out;
    out.reserve(data.Size());
    const bool copied = data.ForEachDataChunk([&out](const char* chunk, std::size_t size) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(chunk);
        out.insert(out.end(), bytes, bytes + size);
        return true;
    });
    if (!copied) {
        return std::nullopt;
    }
    return out;
}

Maybe<Slot> DeserializeValue(const Context& context, std::span<const std::uint8_t> blob) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }

    JSStructuredCloneData data(CLONE_SCOPE);
    if (!data.AppendBytes(reinterpret_cast<const char*>(blob.data()), blob.size())) {
        return std::nullopt;
    }

    JS::RootedValue out(cx);
    const JS::CloneDataPolicy policy;
    // The reader checks its own header before it reads anything else, so a blob
    // this engine did not write - or one that was truncated on the way - is
    // refused rather than misread. That is the safety the opacity is paying
    // for, and it is the engine's own check rather than one added here.
    if (!JS_ReadStructuredClone(cx, data, JS_STRUCTURED_CLONE_VERSION, CLONE_SCOPE, &out, policy, nullptr, nullptr)) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), out);
}

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

namespace {

/// Every object operation needs the same three things: the realm, the object
/// as a rooted handle in that realm, and the key as a `jsid`. The object may
/// have been made in a different realm of the same isolate, which is legal, so
/// it is wrapped into this one rather than used where it stands.
struct ObjectOp {
    ObjectOp(const Context& context, Slot object)
        : cx(Raw(context)), realm(context), scratch(cx), target(cx), live(!Terminating(context)) {
        target = ResolveObjectHere(cx, object, &scratch);
    }

    /// False if there is no object to work on, or if the isolate is
    /// terminating - any of these operations can reach a getter, a setter or a
    /// proxy trap, so all of them are things that "would run script". This is
    /// the gate for every object operation at once; see `detail::Terminating`.
    [[nodiscard]] bool Valid() const noexcept { return live && target != nullptr; }

    /// An incoming value, likewise wrapped into this realm.
    [[nodiscard]] bool Value(Slot value, JS::MutableHandleValue out) const { return ResolveHere(cx, value, out); }

    JSContext* cx;
    RealmGuard realm;
    JS::RootedValue scratch;
    JS::RootedObject target;
    bool live;
};

}  // namespace

Maybe<Slot> GetProperty(const Context& context, Slot object, Slot key) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedId id(op.cx);
    if (!ToPropertyKey(op.cx, key, &id)) {
        return std::nullopt;
    }
    JS::RootedValue result(op.cx);
    if (!JS_GetPropertyById(op.cx, op.target, id, &result)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), result);
}

Maybe<Slot> GetIndex(const Context& context, Slot object, std::uint32_t index) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedValue result(op.cx);
    if (!JS_GetElement(op.cx, op.target, index, &result)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), result);
}

Maybe<bool> SetProperty(const Context& context, Slot object, Slot key, Slot value) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedId id(op.cx);
    if (!ToPropertyKey(op.cx, key, &id)) {
        return std::nullopt;
    }
    JS::RootedValue raw(op.cx);
    if (!op.Value(value, &raw) || !JS_SetPropertyById(op.cx, op.target, id, raw)) {
        return std::nullopt;
    }
    return true;
}

Maybe<bool> SetIndex(const Context& context, Slot object, std::uint32_t index, Slot value) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedValue raw(op.cx);
    if (!op.Value(value, &raw) || !JS_SetElement(op.cx, op.target, index, raw)) {
        return std::nullopt;
    }
    return true;
}

Maybe<bool> DefineProperty(const Context& context, Slot object, Slot key, Slot value, PropertyAttribute attributes) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedId id(op.cx);
    if (!ToPropertyKey(op.cx, key, &id)) {
        return std::nullopt;
    }
    JS::RootedValue raw(op.cx);
    if (!op.Value(value, &raw)) {
        return std::nullopt;
    }
    // The form that reports a refusal rather than throwing one: V8's
    // `DefineOwnProperty` answers as `Reflect.defineProperty` does - false for
    // a frozen object or a property that is not configurable - and is empty
    // only when something threw.
    JS::Rooted<JS::PropertyDescriptor> descriptor(op.cx,
                                                  JS::PropertyDescriptor::Data(raw, ToNativeAttributes(attributes)));
    JS::ObjectOpResult result;
    if (!JS_DefinePropertyById(op.cx, op.target, id, descriptor, result)) {
        return std::nullopt;
    }
    return result.ok();
}

Maybe<bool> SetAccessorProperty(const Context& context, Slot object, std::string_view name,
                                AccessorGetterCallback getter, AccessorSetterCallback setter, CallbackData data,
                                PropertyAttribute attributes) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    CallbackRecord* record =
        StoreCallback(context.GetIsolate(), CallbackRecord{.getter = getter, .setter = setter, .data = data});
    // Refused is false, as for `DefineProperty` above.
    JS::ObjectOpResult result;
    if (record == nullptr || !DefineAccessor(op.cx, op.target, std::string(name), record, attributes, &result)) {
        return std::nullopt;
    }
    return result.ok();
}

Maybe<bool> HasProperty(const Context& context, Slot object, Slot key) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedId id(op.cx);
    if (!ToPropertyKey(op.cx, key, &id)) {
        return std::nullopt;
    }
    bool found = false;
    if (!JS_HasPropertyById(op.cx, op.target, id, &found)) {
        return std::nullopt;
    }
    return found;
}

Maybe<bool> HasOwnProperty(const Context& context, Slot object, Slot key) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedId id(op.cx);
    if (!ToPropertyKey(op.cx, key, &id)) {
        return std::nullopt;
    }
    bool found = false;
    if (!JS_HasOwnPropertyById(op.cx, op.target, id, &found)) {
        return std::nullopt;
    }
    return found;
}

Maybe<bool> DeleteProperty(const Context& context, Slot object, Slot key) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedId id(op.cx);
    if (!ToPropertyKey(op.cx, key, &id)) {
        return std::nullopt;
    }
    JS::ObjectOpResult result;
    if (!JS_DeletePropertyById(op.cx, op.target, id, result)) {
        return std::nullopt;
    }
    return static_cast<bool>(result);
}

Maybe<PropertyAttribute> GetPropertyAttributes(const Context& context, Slot object, Slot key) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedId id(op.cx);
    if (!ToPropertyKey(op.cx, key, &id)) {
        return std::nullopt;
    }
    // Whether it is there at all, asked first and asked as the language asks
    // it. Two reasons, and the second is the one that shows: a property that
    // is absent must answer *empty* rather than `None`, which is also what an
    // ordinary writable, enumerable, configurable property answers; and a
    // proxy may trap `has` without trapping `getOwnPropertyDescriptor`, so the
    // descriptor alone would answer for a proxy that never ran its own hook.
    // A throw from that hook stays pending: empty says the operation produced
    // no value, and the isolate says why (unibind/types.h).
    bool present = false;
    if (!JS_HasPropertyById(op.cx, op.target, id, &present)) {
        return std::nullopt;
    }
    if (!present) {
        return std::nullopt;
    }
    JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> descriptor(op.cx);
    if (!JS_GetPropertyDescriptorById(op.cx, op.target, id, &descriptor, &op.target)) {
        return std::nullopt;
    }
    if (descriptor.get().isNothing()) {
        return std::nullopt;
    }
    const JS::PropertyDescriptor& found = *descriptor.get();
    PropertyAttribute attributes = PropertyAttribute::None;
    if (!found.enumerable()) {
        attributes = attributes | PropertyAttribute::DontEnum;
    }
    if (!found.configurable()) {
        attributes = attributes | PropertyAttribute::DontDelete;
    }
    if (found.isDataDescriptor() && !found.writable()) {
        attributes = attributes | PropertyAttribute::ReadOnly;
    }
    return attributes;
}

Maybe<Slot> GetOwnPropertyNames(const Context& context, Slot object, KeyFilter filter) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    unsigned flags = JSITER_OWNONLY;
    if (filter.includeNonEnumerable) {
        flags |= JSITER_HIDDEN;
    }
    if (filter.includeSymbols) {
        flags |= JSITER_SYMBOLS;
    }
    JS::RootedVector<JS::PropertyKey> keys(op.cx);
    if (!js::GetPropertyKeys(op.cx, op.target, flags, &keys)) {
        return std::nullopt;
    }
    JS::RootedVector<JS::Value> values(op.cx);
    if (!values.reserve(keys.length())) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < keys.length(); ++i) {
        JS::RootedId id(op.cx, keys[i]);
        JS::RootedValue value(op.cx);
        if (!JS_IdToValue(op.cx, id, &value)) {
            return std::nullopt;
        }
        if (!values.append(value)) {
            return std::nullopt;
        }
    }
    JSObject* array = JS::NewArrayObject(op.cx, values);
    if (array == nullptr) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), array);
}

Maybe<Slot> GetPrototype(const Context& context, Slot object) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedObject prototype(op.cx);
    if (!JS_GetPrototype(op.cx, op.target, &prototype)) {
        return std::nullopt;
    }
    if (prototype == nullptr) {
        return PushOrNothing(OwnerOf(context), JS::NullValue());
    }
    return PushOrNothing(OwnerOf(context), prototype);
}

Maybe<bool> SetPrototype(const Context& context, Slot object, Slot prototype) {
    ObjectOp op(context, object);
    if (!op.Valid()) {
        return std::nullopt;
    }
    JS::RootedValue raw(op.cx);
    if (!op.Value(prototype, &raw) || (!raw.isObject() && !raw.isNull())) {
        return std::nullopt;
    }
    JS::RootedObject value(op.cx, raw.isObject() ? &raw.toObject() : nullptr);
    if (!JS_SetPrototype(op.cx, op.target, value)) {
        return std::nullopt;
    }
    return true;
}

std::uint32_t ArrayLength(Slot array) noexcept {
    JSContext* cx = Raw(IsolateFor(array));
    const JS::Value raw = Resolve(array);
    if (!raw.isObject()) {
        return 0;
    }
    ValueRealm realm(cx, raw);
    JS::RootedObject object(cx, Unwrapped(raw));
    std::uint32_t length = 0;
    if (!JS::GetArrayLength(cx, object, &length)) {
        JS_ClearPendingException(cx);
        return 0;
    }
    return length;
}

// ---------------------------------------------------------------------------
// Calling
// ---------------------------------------------------------------------------

namespace {

/// Slots are (frame, index) pairs, so an argument list has to be resolved into
/// a rooted array before the call. `RootedValueVector` is one stack root for
/// the lot, which is the same trick a frame plays.
[[nodiscard]] bool FillArguments(JSContext* cx, std::span<const Slot> slots, JS::RootedVector<JS::Value>& out) {
    if (!out.reserve(slots.size())) {
        return false;
    }
    JS::RootedValue argument(cx);
    for (const Slot& slot : slots) {
        if (!ResolveHere(cx, slot, &argument) || !out.append(argument)) {
            return false;
        }
    }
    return true;
}

}  // namespace

Maybe<Slot> CallFunction(const Context& context, Slot function, Slot receiver, std::span<const Slot> arguments) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedVector<JS::Value> argv(cx);
    if (!FillArguments(cx, arguments, argv)) {
        return std::nullopt;
    }
    JS::RootedValue callee(cx);
    JS::RootedValue self(cx);
    JS::RootedValue result(cx);
    if (!ResolveHere(cx, function, &callee) || !ResolveHere(cx, receiver, &self) ||
        !JS::Call(cx, self, callee, argv, &result)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), result);
}

Maybe<Slot> ConstructObject(const Context& context, Slot function, std::span<const Slot> arguments) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedVector<JS::Value> argv(cx);
    if (!FillArguments(cx, arguments, argv)) {
        return std::nullopt;
    }
    JS::RootedValue callee(cx);
    JS::RootedObject result(cx);
    if (!ResolveHere(cx, function, &callee) || !JS::Construct(cx, callee, argv, &result)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), result);
}

// ---------------------------------------------------------------------------
// What a callback can ask
// ---------------------------------------------------------------------------

Isolate& CallbackIsolate(const CallbackState& state) noexcept {
    return *state.owner;
}

const Context& CallbackContext(const CallbackState& state) noexcept {
    return state.context;
}

std::uint32_t CallbackArgumentCount(const CallbackState& state) noexcept {
    return state.call == nullptr ? 0U : state.call->length();
}

Slot CallbackArgument(const CallbackState& state, std::uint32_t index) noexcept {
    if (index >= CallbackArgumentCount(state)) {
        return Push(*state.owner, JS::UndefinedValue());
    }
    // Borrowed: the argument is already slot `index` of the call's frame, and
    // the interpreter has rooted it on the VM stack for the whole call.
    return MakeSlot(*state.frame, index);
}

Slot CallbackThis(const CallbackState& state) noexcept {
    return SlotOrEmpty(*state.frame, state.thisSlot);
}

Slot CallbackHolder(const CallbackState& state) noexcept {
    return SlotOrEmpty(*state.frame, state.holderSlot);
}

bool CallbackIsConstruct(const CallbackState& state) noexcept {
    return state.isConstruct;
}

CallbackData CallbackDataOf(const CallbackState& state) noexcept {
    return state.data;
}

Slot CallbackValueData(const CallbackState& state) noexcept {
    if (!state.hasValue) {
        return Push(*state.owner, JS::UndefinedValue());
    }
    return SlotOrEmpty(*state.frame, state.valueSlot);
}

void SetReturnSlot(const CallbackState& state, Slot value) noexcept {
    *state.result = Resolve(value);
}
void SetReturnUndefined(const CallbackState& state) noexcept {
    state.result->setUndefined();
}
void SetReturnNull(const CallbackState& state) noexcept {
    state.result->setNull();
}
void SetReturnBoolean(const CallbackState& state, bool value) noexcept {
    state.result->setBoolean(value);
}
void SetReturnNumber(const CallbackState& state, double value) noexcept {
    state.result->setNumber(value);
}
void SetReturnInteger(const CallbackState& state, std::int32_t value) noexcept {
    state.result->setInt32(value);
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

GlobalNode* MakeGlobal(Isolate& isolate, Slot value) {
    auto* node = new GlobalNode{.owner = &isolate, .value = JS::PersistentRooted<JS::Value>(Raw(isolate))};
    ++isolate.impl().embedderRefs;
    node->value = Resolve(value);
    return node;
}

GlobalNode* DuplicateGlobal(GlobalNode* node) {
    if (node == nullptr) {
        return nullptr;
    }
    auto* copy = new GlobalNode{.owner = node->owner, .value = JS::PersistentRooted<JS::Value>(Raw(*node->owner))};
    ++copy->owner->impl().embedderRefs;
    copy->value = node->value.get();
    return copy;
}

void ReleaseGlobal(GlobalNode* node) noexcept {
    if (node == nullptr) {
        return;
    }
    --node->owner->impl().embedderRefs;
    delete node;
}

Slot GlobalToSlot(Isolate& isolate, GlobalNode* node) noexcept {
    // A root that names nothing materialises as a handle that names nothing.
    // `undefined` would be worse here than anywhere else in the API, because
    // the handle is typed: a `Global<Function>` would answer with a
    // `Local<Function>` that is neither a function nor empty.
    if (node == nullptr) {
        return Slot{};
    }
    return Push(isolate, node->value.get());
}

namespace {

/// Compare what two roots *name*, never the roots themselves.
///
/// Two roots over one object are two separate `PersistentRooted` nodes at two
/// separate addresses, so comparing the handles answers "different" for two
/// names of the same thing - and it answers it silently. The symptom is an
/// embedder that cannot remove the callback it registered, because the root it
/// kept is not the root it is holding now.
///
/// No frame is opened and none is needed: the values are rooted here for the
/// length of the comparison, so a caller with no `HandleScope` gets an answer
/// rather than a failure that would read as "not equal".
bool CompareGlobals(const GlobalNode* lhs, const GlobalNode* rhs,
                    bool (*how)(JSContext*, JS::Handle<JS::Value>, JS::Handle<JS::Value>, bool*)) noexcept {
    // An empty root names nothing, so there is nothing for it to be equal to -
    // including another empty one. Two absences are not a match.
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    // Values belong to an isolate, and two isolates share no heap and no
    // thread. Asking the question at all is a mistake; answering "not equal" is
    // the only answer that is not a lie.
    if (lhs->owner != rhs->owner) {
        return false;
    }

    JSContext* cx = Raw(*lhs->owner);
    JS::RootedValue a(cx, lhs->value.get());
    JS::RootedValue b(cx, rhs->value.get());
    // The left one's realm, with the right wrapped into it - the same rule the
    // slot comparisons follow, and for the same reason: the two may name values
    // from different realms of one isolate.
    ValueRealm realm(cx, a);
    bool answer = false;
    if (!JS_WrapValue(cx, &b) || !how(cx, a, b, &answer)) {
        JS_ClearPendingException(cx);
        return false;
    }
    return answer;
}

/// The same question with one side already in a frame. Borrowing the slot's
/// value into a root of our own keeps this free of the frame's lifetime.
bool CompareGlobalToSlot(const GlobalNode* lhs, Slot rhs,
                         bool (*how)(JSContext*, JS::Handle<JS::Value>, JS::Handle<JS::Value>, bool*)) noexcept {
    // An empty handle names nothing, exactly as an empty root does, so it is
    // equal to nothing rather than a value to go and resolve - and resolving
    // one would be reading through a null frame.
    if (lhs == nullptr || rhs.IsEmpty()) {
        return false;
    }
    JSContext* cx = Raw(*lhs->owner);
    JS::RootedValue a(cx, lhs->value.get());
    ValueRealm realm(cx, a);
    JS::RootedValue b(cx);
    bool answer = false;
    if (!ResolveHere(cx, rhs, &b) || !how(cx, a, b, &answer)) {
        JS_ClearPendingException(cx);
        return false;
    }
    return answer;
}

}  // namespace

bool GlobalStrictEquals(const GlobalNode* lhs, const GlobalNode* rhs) noexcept {
    return CompareGlobals(lhs, rhs, &JS::StrictlyEqual);
}

bool GlobalSameValue(const GlobalNode* lhs, const GlobalNode* rhs) noexcept {
    return CompareGlobals(lhs, rhs, &JS::SameValue);
}

bool GlobalStrictEqualsSlot(const GlobalNode* lhs, Slot rhs) noexcept {
    return CompareGlobalToSlot(lhs, rhs, &JS::StrictlyEqual);
}

bool GlobalSameValueSlot(const GlobalNode* lhs, Slot rhs) noexcept {
    return CompareGlobalToSlot(lhs, rhs, &JS::SameValue);
}

// ---------------------------------------------------------------------------
// Exceptions
//
// SpiderMonkey has no TryCatch: an exception is simply pending on the context
// until somebody takes it. So the backend keeps its own stack of handlers, and
// "catching" is taking the pending exception the first time anyone asks.
// ---------------------------------------------------------------------------

namespace {

/// Take whatever is pending on the context into this handler. Idempotent.
void Drain(TryCatchState& state) noexcept {
    JSContext* cx = Raw(*state.owner);
    // A termination is not a throw and leaves nothing pending, so it has to be
    // noticed here rather than found on the context. `HasCaught` is true for
    // one - a handler did stop something - but `HasTerminated` is what says
    // what, and there is no value, message or stack to go with it.
    if (Terminating(*state.owner)) {
        state.caught = true;
        state.terminated = true;
        state.exception = JS::UndefinedValue();
        state.stack = nullptr;
        return;
    }
    if (!JS_IsExceptionPending(cx)) {
        return;
    }
    JS::ExceptionStack captured(cx);
    if (JS::StealPendingExceptionStack(cx, &captured)) {
        state.exception = captured.exception();
        state.stack = captured.stack();
    } else {
        // An uncatchable termination, or the exception could not be captured.
        JS_ClearPendingException(cx);
        state.exception = JS::UndefinedValue();
        state.stack = nullptr;
    }
    state.caught = true;
}

}  // namespace

void ThrowValue(Isolate& isolate, Slot value) {
    JSContext* cx = Raw(isolate);
    IsolateRealm realm(isolate);
    if (!realm.Usable()) {
        return;
    }
    JS::RootedValue raw(cx);
    if (!ResolveHere(cx, value, &raw)) {
        return;
    }
    JS_SetPendingException(cx, raw);
}

void ThrowError(Isolate& isolate, ErrorKind kind, std::string_view message) {
    JSContext* cx = Raw(isolate);
    IsolateRealm realm(isolate);
    if (!realm.Usable()) {
        return;
    }
    JS::RootedValue error(cx);
    if (!MakeErrorValue(cx, kind, message, &error)) {
        return;
    }
    JS_SetPendingException(cx, error);
}

bool HasPendingException(Isolate& isolate) noexcept {
    if (JS_IsExceptionPending(Raw(isolate))) {
        return true;
    }
    TryCatchState* handler = isolate.impl().tryCatch;
    return handler != nullptr && handler->caught;
}

void TryCatchOpen(Isolate& isolate, TryCatchState& storage) noexcept {
    JSContext* cx = Raw(isolate);
    auto* state = ::new (static_cast<void*>(&storage)) TryCatchState{.owner = &isolate,
                                                                     .prev = isolate.impl().tryCatch,
                                                                     .exception = JS::PersistentRooted<JS::Value>(cx),
                                                                     .stack = JS::PersistentRootedObject(cx),
                                                                     .outer = JS::PersistentRooted<JS::Value>(cx)};
    // Anything already pending was thrown before this handler existed, so it
    // is not ours to catch. Park it and put it back when we close.
    if (JS_IsExceptionPending(cx)) {
        JS::RootedValue pending(cx);
        if (JS_GetPendingException(cx, &pending)) {
            state->outer = pending;
            state->hadOuter = true;
        }
        JS_ClearPendingException(cx);
    }
    isolate.impl().tryCatch = state;
}

void TryCatchClose(TryCatchState& state) noexcept {
    JSContext* cx = Raw(*state.owner);
    Drain(state);
    state.owner->impl().tryCatch = state.prev;

    // A termination is the one thing a handler does not consume: the isolate's
    // flag is still set, so the unwind carries on past this handler whether or
    // not `ReThrow` was called. Nothing to put back on the context either -
    // there was never an exception. A handler that swallowed a stop would leave
    // the script it was told to stop running.
    if (state.terminated) {
        state.~TryCatchState();
        return;
    }

    // V8's rule, which is what parity means here: a caught exception is
    // consumed by the handler that caught it unless the handler asked for it
    // to continue outwards.
    if (state.caught && state.rethrow) {
        JS::RootedValue value(cx, state.exception.get());
        JS::RootedObject stack(cx, state.stack.get());
        if (stack != nullptr) {
            JS::ExceptionStack exceptionStack(cx, value, stack);
            JS::SetPendingExceptionStack(cx, exceptionStack);
        } else {
            JS_SetPendingException(cx, value);
        }
    } else if (state.hadOuter) {
        JS::RootedValue outer(cx, state.outer.get());
        JS_SetPendingException(cx, outer);
    }
    state.~TryCatchState();
}

bool TryCatchHasCaught(const TryCatchState& state) noexcept {
    Drain(const_cast<TryCatchState&>(state));
    return state.caught;
}

Slot TryCatchException(const TryCatchState& state, Isolate& isolate) noexcept {
    Drain(const_cast<TryCatchState&>(state));
    // Nothing caught, or a termination - which carries no value at all. Both
    // answer with an EMPTY handle, which is what unibind/exception.h promises
    // and the only answer that cannot be mistaken for someone having thrown
    // `undefined`.
    if (!state.caught || state.terminated) {
        return Slot{};
    }
    return Push(isolate, state.exception.get());
}

Maybe<std::string> TryCatchMessage(const TryCatchState& state, const Context& context) {
    Drain(const_cast<TryCatchState&>(state));
    if (!state.caught || state.terminated) {
        return std::nullopt;
    }
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    // The engine's own report of the value, built without side effects - the
    // text it would print for the exception uncaught. Converting the value
    // with `ToString` instead ran script's own `toString` - reading the text of
    // a caught exception is not something script should be able to see, or
    // throw from - and had no answer at all for a value that will not convert:
    // a symbol, an object with no `toString`, one whose `toString` throws. V8
    // runs nothing and answers for each of those.
    JS::RootedValue value(cx, state.exception.get());
    JS::RootedObject stack(cx, state.stack.get());
    const JS::ExceptionStack exception(cx, value, stack);
    JS::ErrorReportBuilder report(cx);
    if (!report.init(cx, exception, JS::ErrorReportBuilder::NoSideEffects) || !report.toStringResult()) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    return std::string(report.toStringResult().c_str());
}

Maybe<std::string> TryCatchStackTrace(const TryCatchState& state, const Context& context) {
    Drain(const_cast<TryCatchState&>(state));
    if (!state.caught || state.terminated || state.stack == nullptr) {
        return std::nullopt;
    }
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedObject stack(cx, state.stack.get());
    JS::RootedString text(cx);
    if (!JS::BuildStackString(cx, nullptr, stack, &text) || text == nullptr) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }
    return EncodeToStdString(cx, text);
}

namespace {

/// A frame's function name as `StackFrame` promises it, from the display name
/// the engine recorded.
///
/// For a function with no name of its own the engine records the name it
/// guessed, in a notation of its own: `/` joins an enclosing function's name to
/// what was guessed inside it, and `<` stands for "an anonymous function" -
/// `g/<` is an anonymous function inside `g`, `outer/obj.n` a function assigned
/// to `obj.n` inside `outer`. V8 names what a function was assigned to and
/// nothing else, so the prefix goes, and a guess that is only "anonymous" is
/// no name at all. A real name - an identifier, `get x`, a computed key - has
/// neither character, short of a computed key written with one.
std::string FunctionNameOf(std::string displayName) {
    const std::size_t slash = displayName.rfind('/');
    if (slash != std::string::npos) {
        displayName.erase(0, slash + 1);
    }
    if (displayName.find('<') != std::string::npos) {
        displayName.clear();
    }
    return displayName;
}

/// Walk a `SavedFrame` chain into the plain structs the API hands out.
///
/// `limit` of 0 means everything the chain holds. Nothing here allocates an
/// engine value the caller has to root: names come out as `std::string`, which
/// is what makes `CaptureStackFrames` usable with no open `HandleScope`.
///
/// Only script's frames: every accessor is asked to skip the engine's
/// self-hosted ones, which are this engine's built-ins - `Array.prototype.map`
/// is JavaScript here and native code on V8 - so a stack names the same
/// frames on both, and an error a built-in throws is placed at the script
/// that called it rather than at a line of the engine's own source.
std::vector<StackFrame> ReadSavedFrames(JSContext* cx, JS::HandleObject top, std::uint32_t limit) {
    constexpr auto SCRIPT_ONLY = JS::SavedFrameSelfHosted::Exclude;
    std::vector<StackFrame> frames;
    JS::RootedObject frame(cx, top);
    while (frame != nullptr && (limit == 0 || frames.size() < limit)) {
        StackFrame out;

        // Asked first, because it is also the question of whether any script
        // frame is left: past the last one the answer is not `Ok`.
        JS::RootedString source(cx);
        if (JS::GetSavedFrameSource(cx, nullptr, frame, &source, SCRIPT_ONLY) != JS::SavedFrameResult::Ok) {
            break;
        }
        if (source != nullptr) {
            out.scriptName = EncodeToStdString(cx, source);
        }
        JS::RootedString name(cx);
        if (JS::GetSavedFrameFunctionDisplayName(cx, nullptr, frame, &name, SCRIPT_ONLY) == JS::SavedFrameResult::Ok &&
            name != nullptr) {
            out.functionName = FunctionNameOf(EncodeToStdString(cx, name));
        }
        std::uint32_t line = 0;
        if (JS::GetSavedFrameLine(cx, nullptr, frame, &line, SCRIPT_ONLY) == JS::SavedFrameResult::Ok) {
            out.lineNumber = static_cast<std::int32_t>(line);
        }
        JS::TaggedColumnNumberOneOrigin column;
        if (JS::GetSavedFrameColumn(cx, nullptr, frame, &column, SCRIPT_ONLY) == JS::SavedFrameResult::Ok) {
            out.columnNumber = static_cast<std::int32_t>(column.oneOriginValue());
        }

        frames.push_back(std::move(out));

        JS::RootedObject parent(cx);
        if (JS::GetSavedFrameParent(cx, nullptr, frame, &parent, SCRIPT_ONLY) != JS::SavedFrameResult::Ok) {
            break;
        }
        frame = parent;
    }
    return frames;
}

}  // namespace

std::vector<StackFrame> CaptureStack(Isolate& isolate, std::uint32_t limit) {
    // A cap of zero frames is zero frames, as on V8 - not "no cap", which is
    // what zero means to `ReadSavedFrames` and what the engine's `MaxFrames`
    // cannot be asked for.
    if (limit == 0) {
        return {};
    }
    JSContext* cx = Raw(isolate);
    // `CaptureCurrentStack` allocates `SavedFrame` objects, so it needs a realm
    // like everything else - and `unibind/exception.h` says this one needs not
    // even a `HandleScope`, let alone a `ContextScope`.
    IsolateRealm realm(isolate);
    if (!realm.Usable()) {
        return {};
    }
    JS::RootedObject stack(cx);
    JS::StackCapture capture{JS::MaxFrames(limit)};
    if (!JS::CaptureCurrentStack(cx, &stack, std::move(capture))) {
        JS_ClearPendingException(cx);
        return {};
    }
    // Null is the honest answer when no JavaScript is on the stack - a native
    // called from the embedder rather than from script - not a failure.
    if (stack == nullptr) {
        return {};
    }
    return ReadSavedFrames(cx, stack, limit);
}

std::optional<std::vector<StackFrame>> TryCatchStackFrames(const TryCatchState& state, const Context& context) {
    Drain(const_cast<TryCatchState&>(state));
    // A termination carries no stack by definition, and asking for one would be
    // asking about an exception that does not exist.
    if (!state.caught || state.terminated || state.stack == nullptr) {
        return std::nullopt;
    }
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    JS::RootedObject stack(cx, state.stack.get());
    return ReadSavedFrames(cx, stack, 0);
}

namespace {

/// Where the line terminator at `text[at]` ends, or `at` if there is none
/// there. JavaScript ends a line at LF, CR, CRLF, U+2028 and U+2029, and the
/// engine numbers lines by exactly that rule, so cutting a line by any other
/// would quote a line other than the one it numbered.
template <class Char>
[[nodiscard]] std::size_t PastTerminator(const Char* text, std::size_t length, std::size_t at) noexcept {
    const auto unit = static_cast<std::uint32_t>(static_cast<std::make_unsigned_t<Char>>(text[at]));
    if (unit == '\n') {
        return at + 1;
    }
    if (unit == '\r') {
        return at + 1 < length && text[at + 1] == Char('\n') ? at + 2 : at + 1;
    }
    if constexpr (sizeof(Char) == 1) {
        // U+2028 and U+2029 in UTF-8: E2 80 A8 and E2 80 A9.
        if (unit == 0xE2 && at + 2 < length && static_cast<std::uint8_t>(text[at + 1]) == 0x80 &&
            (static_cast<std::uint8_t>(text[at + 2]) == 0xA8 || static_cast<std::uint8_t>(text[at + 2]) == 0xA9)) {
            return at + 3;
        }
    } else if (unit == 0x2028 || unit == 0x2029) {
        return at + 1;
    }
    return at;
}

/// Where line `wanted` of `text` starts and how long it is without its
/// terminator, counting the first line as `firstLine`; nothing if there is no
/// such line.
template <class Char>
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> FindLine(const Char* text, std::size_t length,
                                                                          std::int64_t firstLine,
                                                                          std::int64_t wanted) noexcept {
    if (wanted < firstLine) {
        return std::nullopt;
    }
    std::int64_t line = firstLine;
    std::size_t start = 0;
    for (std::size_t at = 0; at < length;) {
        const std::size_t past = PastTerminator(text, length, at);
        if (past == at) {
            ++at;
            continue;
        }
        if (line == wanted) {
            return std::make_pair(start, at - start);
        }
        ++line;
        start = past;
        at = past;
    }
    if (line == wanted) {
        return std::make_pair(start, length - start);
    }
    return std::nullopt;
}

/// Line `lineNumber` of the script named `scriptName`, without its terminator,
/// or nothing if no retained script has that name and that line - or if more
/// than one live script has that name and they do not all have the same text.
///
/// The engine names a script in an error only by its resource name, and two
/// scripts may share one: every script compiled with no origin shares the
/// default. Which of them raised the error is then not something this backend
/// can find out, and quoting any one of them is a wrong answer that looks like
/// a right one - so it quotes nothing. See `Isolate::Impl::sources`.
std::optional<std::string> RetainedLine(const Isolate& isolate, const std::string& scriptName,
                                        std::int32_t lineNumber) {
    const Isolate::Impl::RetainedSource* found = nullptr;
    for (const auto& [key, source] : isolate.impl().sources) {
        if (source->name != scriptName) {
            continue;
        }
        if (found != nullptr && (found->text != source->text || found->firstLine != source->firstLine)) {
            return std::nullopt;
        }
        found = source.get();
    }
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto line = FindLine(found->text.data(), found->text.size(), found->firstLine, lineNumber);
    if (!line) {
        return std::nullopt;
    }
    return found->text.substr(line->first, line->second);
}

}  // namespace

Maybe<MessageLocation> TryCatchLocation(const TryCatchState& state, const Context& context) {
    Drain(const_cast<TryCatchState&>(state));
    if (!state.caught || state.terminated) {
        return std::nullopt;
    }
    JSContext* cx = Raw(context);
    RealmGuard realm(context);

    MessageLocation location;
    bool located = false;
    // Where it was thrown, as V8 places every exception: the engine captured
    // the stack at the throw, and its top frame is that place. For an Error
    // that is *not* where it was made - `const e = new Error(); ...; throw e;`
    // is placed at the `throw` - which is why the stack comes before the
    // error's own report.
    //
    // The one exception is a syntax error, found while compiling: the stack at
    // that throw is whatever called the compile, and the position that matters
    // is in the source being compiled. The engine says which errors those are
    // by quoting the offending line itself, which it does for nothing else.
    JS::RootedValue value(cx, state.exception.get());
    JS::RootedObject object(cx, value.isObject() ? &value.toObject() : nullptr);
    JS::BorrowedErrorReport report(cx);
    const bool reported = object != nullptr && JS_ErrorFromException(cx, object, report);
    const bool compileError = reported && report->linebuf() != nullptr;
    if (!compileError && state.stack != nullptr) {
        JS::RootedObject stack(cx, state.stack.get());
        std::vector<StackFrame> top = ReadSavedFrames(cx, stack, 1);
        if (!top.empty()) {
            location.scriptName = std::move(top.front().scriptName);
            location.lineNumber = top.front().lineNumber;
            location.columnNumber = top.front().columnNumber;
            located = true;
        }
    }
    // An error with no stack at the throw - a syntax error, or one thrown from
    // native code with no script running - is placed where its report says.
    if (!located && reported) {
        if (report->filename) {
            location.scriptName = report->filename.c_str();
        }
        location.lineNumber = static_cast<std::int32_t>(report->lineno);
        location.columnNumber = static_cast<std::int32_t>(report->column.oneOriginValue());
        if (compileError) {
            JS::RootedString line(cx, JS_NewUCStringCopyN(cx, report->linebuf(), report->linebufLength()));
            if (line != nullptr) {
                location.sourceLine = EncodeToStdString(cx, line);
            }
        }
        located = true;
    }
    if (JS_IsExceptionPending(cx)) {
        JS_ClearPendingException(cx);
    }
    if (!located) {
        return std::nullopt;
    }
    if (!location.sourceLine) {
        location.sourceLine = RetainedLine(*state.owner, location.scriptName, location.lineNumber);
    }
    return location;
}

bool TryCatchHasTerminated(const TryCatchState& state) noexcept {
    Drain(const_cast<TryCatchState&>(state));
    return state.terminated;
}

void TryCatchReThrow(TryCatchState& state) noexcept {
    Drain(state);
    state.rethrow = true;
}

void TryCatchReset(TryCatchState& state) noexcept {
    // `Reset` means nothing on a termination and says so by doing nothing:
    // only `Isolate::CancelTerminateExecution` clears that, and a handler that
    // could clear it by asking twice would be a handler that can swallow a
    // stop.
    if (state.terminated) {
        return;
    }
    JS_ClearPendingException(Raw(*state.owner));
    state.caught = false;
    state.rethrow = false;
    state.exception = JS::UndefinedValue();
    state.stack = nullptr;
}

// ---------------------------------------------------------------------------
// Contexts
// ---------------------------------------------------------------------------

ContextRec* NewContext(Isolate& isolate) {
    JSContext* cx = Raw(isolate);
    JS::RealmOptions options;
    JS::RootedObject global(cx, JS_NewGlobalObject(cx, &GLOBAL_CLASS, nullptr, JS::FireOnNewGlobalHook, options));
    if (global == nullptr) {
        return nullptr;
    }
    {
        JSAutoRealm realm(cx, global);
        if (!JS::InitRealmStandardClasses(cx)) {
            return nullptr;
        }
    }
    auto* rec = new ContextRec{.owner = &isolate, .global = JS::PersistentRootedObject(cx)};
    ++isolate.impl().embedderRefs;
    rec->global = global;
    JS::SetReservedSlot(global, GLOBAL_REC_SLOT, JS::PrivateValue(rec));
    return rec;
}

void RetainContext(ContextRec* rec) noexcept {
    if (rec != nullptr) {
        ++rec->refs;
    }
}

void ReleaseContext(ContextRec* rec) noexcept {
    if (rec != nullptr && --rec->refs == 0) {
        // The global keeps a pointer to this record, and it may still be
        // reachable from a value somebody kept, so blank it before it goes.
        if (rec->global != nullptr) {
            JS::SetReservedSlot(rec->global, GLOBAL_REC_SLOT, JS::PrivateValue(nullptr));
        }
        --rec->owner->impl().embedderRefs;
        delete rec;
    }
}

Isolate& ContextIsolate(const Context& context) noexcept {
    return *context.rec()->owner;
}

Slot ContextGlobalObject(const Context& context) noexcept {
    return Push(OwnerOf(context), GlobalOf(context));
}

void ContextEnter(const Context& context, ContextScopeState& storage) noexcept {
    auto* state = ::new (static_cast<void*>(&storage)) ContextScopeState();
    ::new (static_cast<void*>(state->realm)) JSAutoRealm(Raw(context), GlobalOf(context));
}

void ContextLeave(ContextScopeState& state) noexcept {
    state.Realm().~JSAutoRealm();
    state.~ContextScopeState();
}

// ---------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------

namespace {

/// Keep the text `TryCatch::Location` will quote from, for as long as the
/// engine keeps `script`'s code; see `Isolate::Impl::sources`. Quoting is a
/// convenience, so failing to keep it costs the quote and nothing else.
void RetainSource(Isolate& isolate, JSScript* script, std::string_view source, const ScriptOrigin& origin) noexcept {
    try {
        auto kept = std::make_unique<Isolate::Impl::RetainedSource>(
            Isolate::Impl::RetainedSource{.owner = &isolate.impl(),
                                          .name = std::string(origin.resourceName),
                                          .text = std::string(source),
                                          .firstLine = std::int64_t{origin.lineOffset} + 1});
        Isolate::Impl::RetainedSource* raw = kept.get();
        isolate.impl().sources.emplace(raw, std::move(kept));
        // The engine calls the add-reference hook from inside this, which is
        // what makes the count one.
        JS::SetScriptPrivate(script, JS::PrivateValue(raw));
    } catch (const std::bad_alloc&) {
        return;
    }
}

/// Turn a stencil into the rec the API hands back, instantiating it into the
/// realm that asked, and keep `source` for `TryCatch::Location`.
ScriptRec* RecFromStencil(JSContext* cx, const Context& context, RefPtr<JS::Stencil> stencil, bool usedCache,
                          std::string_view source, const ScriptOrigin& origin) {
    if (stencil == nullptr) {
        return nullptr;
    }
    const JS::InstantiateOptions instantiateOptions;
    JS::RootedScript script(cx, JS::InstantiateGlobalStencil(cx, instantiateOptions, stencil.get()));
    if (script == nullptr) {
        return nullptr;
    }
    RetainSource(OwnerOf(context), script, source, origin);
    auto* rec = new ScriptRec{.owner = &OwnerOf(context),
                              .stencil = std::move(stencil),
                              .script = JS::PersistentRooted<JSScript*>(cx),

                              .usedCache = usedCache};
    ++rec->owner->impl().embedderRefs;
    rec->script = script;
    return rec;
}

RefPtr<JS::Stencil> CompileToStencil(JSContext* cx, std::string_view source, const JS::CompileOptions& options) {
    JS::SourceText<mozilla::Utf8Unit> text;
    if (!text.init(cx, source.data(), source.size(), JS::SourceOwnership::Borrowed)) {
        return nullptr;
    }
    return JS::CompileGlobalScriptToStencil(cx, options, text);
}

/// Where the source says it came from. The column offset moves the columns of
/// the first line only, as it does on V8 - it is where in its first line the
/// source began. A negative one is taken as none: the engine counts columns
/// from one and has no column before the first.
void ApplyOrigin(JS::CompileOptions& engine, const std::string& resourceName, const ScriptOrigin& origin) {
    engine.setFileAndLine(resourceName.c_str(), origin.lineOffset + 1);
    const auto columnOffset = static_cast<std::uint32_t>(origin.columnOffset > 0 ? origin.columnOffset : 0);
    engine.setColumn(JS::ColumnNumberOneOrigin(columnOffset + 1));
}

/// The engine's compile options for a unibind one. Eager is "parse everything
/// eagerly": every function body is parsed and given bytecode in the first
/// pass, so the stencil - which is what the code cache encodes - holds all of
/// it rather than a syntax-checked outline of each function. There is no
/// in-isolate compilation cache on this engine for it to have to be kept apart
/// from, which is the problem the other backend has.
void ApplyCompileOptions(JS::CompileOptions& engine, CompileOptions options) {
    if (options == CompileOptions::EagerCompile) {
        engine.setEagerDelazificationStrategy(JS::DelazificationOption::ParseEverythingEagerly);
    }
}

}  // namespace

ScriptRec* CompileScript(const Context& context, std::string_view source, const ScriptOrigin& origin,
                         CompileOptions compileOptions) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return nullptr;
    }

    const std::string resourceName(origin.resourceName);
    JS::CompileOptions options(cx);
    ApplyOrigin(options, resourceName, origin);
    ApplyCompileOptions(options, compileOptions);
    return RecFromStencil(cx, context, CompileToStencil(cx, source, options), false, source, origin);
}

ScriptRec* CompileScriptWithCache(const Context& context, std::string_view source, const ScriptOrigin& origin,
                                  std::span<const std::uint8_t> codeCache, CompileOptions compileOptions) {
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return nullptr;
    }

    const std::string resourceName(origin.resourceName);
    JS::CompileOptions options(cx);
    ApplyOrigin(options, resourceName, origin);
    // Set before the decode as well as the compile, harmlessly: a decoded
    // stencil is whatever was encoded, and the option only reaches the
    // fallback below, which is where a refused blob has to be compiled eagerly
    // if that is what was asked for.
    ApplyCompileOptions(options, compileOptions);

    // The blob is a hint, so every way it can be wrong ends in the same place:
    // compile the source.
    //
    // `DecodeStencil` checks the build id and answers `Failure_BadBuildId`
    // rather than handing back something a different engine built. It checks
    // *nothing* about which source the bytes came from - a blob encoded from
    // one script decodes cleanly against another and runs the first - so what
    // arrives here has already been framed and stamp-checked by `unibind/script.h`,
    // which does that keying once for every backend. Do not add a second layer
    // of it here, and do not remove the reliance on it: the failure it prevents
    // is a wrong answer that looks exactly like a right one.
    //
    // A `Throw` result leaves an exception pending that belongs to the decode
    // rather than to the source, so it is cleared before the real compile -
    // otherwise a successful compile would come back with something pending.
    if (!codeCache.empty()) {
        const JS::DecodeOptions decodeOptions(options);
        JS::Stencil* decoded = nullptr;
        const JS::TranscodeRange range(codeCache.data(), codeCache.size());
        if (JS::DecodeStencil(cx, decodeOptions, range, &decoded) == JS::TranscodeResult::Ok && decoded != nullptr) {
            ScriptRec* rec = RecFromStencil(cx, context, RefPtr<JS::Stencil>(already_AddRefed<JS::Stencil>(decoded)),
                                            true, source, origin);
            if (rec != nullptr) {
                return rec;
            }
        }
        JS_ClearPendingException(cx);
    }

    return RecFromStencil(cx, context, CompileToStencil(cx, source, options), false, source, origin);
}

bool ScriptUsedCodeCache(const ScriptRec* script) noexcept {
    return script != nullptr && script->usedCache;
}

std::optional<std::vector<std::uint8_t>> ScriptCreateCodeCache(const ScriptRec* script) {
    if (script == nullptr || script->stencil == nullptr) {
        return std::nullopt;
    }
    JSContext* cx = Raw(*script->owner);
    // Not everything can be encoded - asm.js cannot - and the engine would
    // rather be asked than fail halfway.
    if (!JS::IsStencilCacheable(script->stencil.get())) {
        return std::nullopt;
    }
    JS::TranscodeBuffer buffer;
    if (JS::EncodeStencil(cx, script->stencil.get(), buffer) != JS::TranscodeResult::Ok) {
        JS_ClearPendingException(cx);
        return std::nullopt;
    }

    return std::vector<std::uint8_t>(buffer.begin(), buffer.end());
}

void ReleaseScript(ScriptRec* script) noexcept {
    if (script == nullptr) {
        return;
    }
    --script->owner->impl().embedderRefs;
    delete script;
}

Maybe<Slot> RunScript(const Context& context, ScriptRec* script) {
    if (script == nullptr) {
        return std::nullopt;
    }
    JSContext* cx = Raw(context);
    RealmGuard realm(context);
    if (Terminating(context)) {
        return std::nullopt;
    }
    JS::RootedScript rooted(cx, script->script.get());
    JS::RootedValue result(cx);
    if (!JS_ExecuteScript(cx, rooted, &result)) {
        return std::nullopt;
    }
    return PushOrNothing(OwnerOf(context), result);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Platform, Isolate, Context: the public classes the backend implements
// ---------------------------------------------------------------------------

namespace {

bool g_initialized = false;  // NOLINT(*-avoid-non-const-global-variables)

/// The engine's background-thread count, once anything has been able to ask.
/// Process-wide and fixed by `JS_Init`, so it is cached rather than re-read.
std::optional<std::uint32_t> g_workerThreads;  // NOLINT(*-avoid-non-const-global-variables)

/// What `PlatformOptions` installed, for the life of the process. Not settable
/// afterwards, which is what lets these be read with no lock from whatever
/// thread faults - see the field's comment in unibind/isolate.h.
EngineFaultCallback g_faultHandler = nullptr;  // NOLINT(*-avoid-non-const-global-variables)
CallbackData g_faultData;                      // NOLINT(*-avoid-non-const-global-variables)

/// The vectored handler that turns `MOZ_CRASH` into `EngineFault::Fatal`, while
/// one is installed. Defined below, beside the per-thread isolate it reads.
LONG CALLBACK OnEngineCrash(EXCEPTION_POINTERS* info);
PVOID g_crashHandler = nullptr;  // NOLINT(*-avoid-non-const-global-variables)

void RemoveCrashHandler() noexcept {
    if (g_crashHandler != nullptr) {
        RemoveVectoredExceptionHandler(g_crashHandler);
        g_crashHandler = nullptr;
    }
}

/// Hand a fault to whatever the embedder installed, if anything.
///
/// **Nothing here allocates**, and that is the contract rather than an
/// implementation note: the only fault this engine raises is running out of
/// memory. The report is a stack aggregate of views into strings the engine or
/// this file already owns.
void ReportEngineFault(EngineFault fault, Isolate* isolate, std::string_view location,
                       std::string_view message) noexcept {
    if (g_faultHandler == nullptr) {
        return;
    }
    const EngineFaultReport report{.fault = fault, .isolate = isolate, .location = location, .message = message};
    g_faultHandler(report, g_faultData);
}

/// SpiderMonkey running out of memory, for one context.
///
/// Every internal out-of-memory arrives here, `JS_ReportOutOfMemory` included -
/// which is how a handle frame that could not grow (`docs/lifetimes.md` rule 9)
/// reaches an embedder on this backend without the frame code knowing anything
/// about faults. The other backend has no equivalent funnel and raises that one
/// by hand; the observable result is the same on both.
///
/// Unlike V8's, this is **not** terminal: the engine goes on to report an
/// out-of-memory condition into whatever was running, and the isolate survives.
/// The header says so rather than promising an orderly failure on both.
///
/// The engine says nothing about where or what - there is no location and no
/// message in the callback at all - so a literal stands in for the one fact
/// there is. Empty would have been the alternative and is worse: a handler that
/// logs the message would log nothing.
void OnOutOfMemory(JSContext* cx, void* /*data*/) {
    ReportEngineFault(EngineFault::OutOfMemory, static_cast<Isolate*>(JS_GetContextPrivate(cx)), "",
                      "the engine is out of memory");
}

#define UNIBIND_STRINGIFY_(x) #x
#define UNIBIND_STRINGIFY(x) UNIBIND_STRINGIFY_(x)

/// What tags everything the engine caches on disk, so that a blob from one
/// build is rejected by the next instead of being believed.
///
/// The id names the backend and the engine version it was compiled against,
/// because a change to either changes what the bytes mean. `js-config.h` is
/// where those two numbers come from, and it is the same header the library
/// itself was configured with.
///
/// Two things read it, and that they are the same string is the point:
/// `ProcessBuildId` below hands it to the engine, and `detail::BackendBuildId`
/// puts it in a cache blob's key, so unibind's own check and the engine's move
/// together and neither can accept what the other refuses.
constexpr std::string_view BUILD_ID =
    "unibind-spidermonkey-" UNIBIND_STRINGIFY(MOZJS_MAJOR_VERSION) "." UNIBIND_STRINGIFY(MOZJS_MINOR_VERSION);

/// Installed unconditionally, and it has to be: `JS::EncodeStencil` does not
/// check whether an embedder supplied one. With no build-id op set it
/// dereferences a null function pointer - not a `Failure_BadBuildId`, not a
/// throw, a crash - so the hook is process-wide state the backend owes the
/// engine before an embedder can reach anything that transcodes.
bool ProcessBuildId(JS::BuildIdCharVector* buildId) {
    return buildId->append(BUILD_ID.data(), BUILD_ID.size());
}

}  // namespace

namespace detail {

std::string_view BackendBuildId() noexcept {
    return BUILD_ID;
}

}  // namespace detail

Platform::Platform(const PlatformOptions& options) {
    // A second Platform is a precondition violation, not a failure: the
    // embedder wrote something that cannot be right, and answering it with
    // `IsInitialized() == false` would hide a bug behind the channel that
    // reports an engine which would not start. The two are kept apart
    // deliberately.
    assert(!g_initialized && "a Platform already exists");

    // First, and before anything that could fail: this is what a failure during
    // the rest of this constructor would be reported through. It is installed
    // on each isolate as it is made - the engine's hook is per-context - and
    // this is the copy those installs read.
    g_faultHandler = options.onEngineFault;
    g_faultData = options.engineFaultData;
    if (g_faultHandler != nullptr) {
        // Only when there is somewhere to send it, as on the other backend:
        // with no handler the engine's own crash is what was asked for. First
        // in the chain, so the report is written before anything else the
        // process installed decides what the exception means.
        g_crashHandler = AddVectoredExceptionHandler(1, &OnEngineCrash);
    }

    // `JS_Init` reports failure and it is not decoration - it can fail. An
    // unchecked `assert` here is worse than nothing, because it compiles out of
    // exactly the build where the failure matters, and then `IsInitialized()`
    // says true, `Isolate::New` consults it and believes it, and isolates get
    // handed out against an engine that never came up.
    //
    // So a failed bring-up leaves this object existing and having done nothing,
    // rather than existing and having done half.
    if (!JS_Init()) {
        return;
    }

    JS::SetProcessBuildIdOp(&ProcessBuildId);

    // `workerThreads` is a hint here and cannot be more than one. SpiderMonkey
    // builds its helper-thread pool inside JS_Init for the whole process, and
    // the two levers over it are both dead ends: JSGC_MAX_HELPER_THREADS is
    // silently ignored, and JS::SetHelperThreadTaskCallback with a callback
    // that runs tasks on the calling thread crashes at the first collection,
    // because the engine dispatches parallel marking tasks and then waits on
    // them. In particular a request for *none* cannot be honoured at all.
    //
    // So the request is ignored rather than approximated, and when the header
    // grows somewhere to report the count actually in effect this is where the
    // honest answer gets written. See docs/spidermonkey.md section 5.6.

    g_initialized = true;
}

Platform::~Platform() {
    // Nothing to shut down if nothing came up. `JS_ShutDown` without a
    // successful `JS_Init` is not something the engine promises to survive, and
    // the destructor of a failed bring-up is precisely when it would be called.
    if (!g_initialized) {
        RemoveCrashHandler();
        g_faultHandler = nullptr;
        g_faultData = CallbackData{};
        return;
    }
    JS_ShutDown();
    // The helper-thread count belongs to this `JS_Init`, not to the process
    // forever: a later Platform may get a different pool, and a figure cached
    // across the gap would be a wrong answer with nothing to suggest it.
    g_workerThreads.reset();
    g_initialized = false;
    // Last, because everything above could still have faulted. The handler
    // belongs to this Platform and the next one brings its own.
    RemoveCrashHandler();
    g_faultHandler = nullptr;
    g_faultData = CallbackData{};
}

bool Platform::IsInitialized() noexcept {
    return g_initialized;
}

std::string_view Platform::BackendName() noexcept {
    return "spidermonkey";
}

std::string_view Platform::BackendVersion() noexcept {
    // The engine's own words for itself - "JavaScript-C153.3.0" - and a
    // literal inside it, so there is nothing to build or own. It is not the
    // cache key: `detail::BackendBuildId` is, and it is the same string the
    // engine is handed through `JS::SetProcessBuildIdOp`.
    return JS_GetImplementationVersion();
}

namespace {

/// SpiderMonkey permits exactly one `JSContext` per thread, and a `JSContext`
/// is what a `ub::Isolate` is here. `js/Context.h` says it outright: "JSContext
/// represents a thread: there must be exactly one JSContext for each thread
/// running JS/Wasm", and `JS_NewContext` is "create a new context (and runtime)
/// for this thread". A debug engine build asserts on the second call; a release
/// one - which is what is shipped here - walks into undefined behaviour.
///
/// So the second `Isolate::New` on a thread answers empty, which
/// `unibind/isolate.h` already documents as what happens when a heap cannot be
/// made. Dying is not one of the available answers.
thread_local Isolate* g_threadIsolate = nullptr;  // NOLINT(*-avoid-non-const-global-variables)

/// SpiderMonkey ending the process: `MOZ_CRASH`, and through it every
/// `MOZ_RELEASE_ASSERT` - and every `MOZ_ASSERT` in a debug engine.
///
/// None of those has an embedder hook; they are the engine's crash-reporter
/// protocol. What they do on Windows is fixed by `mozilla/Assertions.h`: store
/// the reason in the exported `gMozCrashReason`, then `__debugbreak()`, then
/// write through null, then `TerminateProcess`. So the first of those
/// exceptions, seen with a reason set, *is* the engine's fatal path, and the
/// reason is the engine's own message for it. Nothing else in a process sets
/// that variable, which is what keeps this from claiming a breakpoint or an
/// access violation that is not the engine's.
///
/// It ends the process itself once it has reported, as the other backend does
/// for its `Fatal`: the engine is past continuing, and returning would only
/// walk into the null write and hand the same death to whatever unhandled-
/// exception filter the embedder has, as a second, less informative report.
/// Exit code 3 is the one the engine's own `TerminateProcess` would have used.
LONG CALLBACK OnEngineCrash(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const char* reason = gMozCrashReason;
    if (reason == nullptr || (code != EXCEPTION_BREAKPOINT && code != EXCEPTION_ACCESS_VIOLATION)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic_flag reported;
    if (!reported.test_and_set()) {
        ReportEngineFault(EngineFault::Fatal, g_threadIsolate, "", reason);
    }
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/// `wanted`, or as much of this thread's stack as can safely be promised.
///
/// The reserve is what the engine unwinds through after it has decided it is
/// out of stack - throwing a RangeError runs script, and that script needs
/// frames of its own.
std::size_t UsableStackBytes(std::size_t wanted) noexcept {
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (high <= low) {
        return wanted;
    }
    constexpr std::size_t RESERVE = std::size_t{128} * 1024;
    const auto total = static_cast<std::size_t>(high - low);
    const std::size_t usable = total > RESERVE ? total - RESERVE : total / 2;
    return wanted < usable ? wanted : usable;
}

/// How `Isolate::TerminateExecution` actually stops a script.
///
/// SpiderMonkey has no terminate call. Returning `false` from an interrupt
/// callback without leaving a pending exception is the engine's own idiom for
/// an *uncatchable* exception - see `JS::ReportUncatchableException` - and it
/// is what `js/ErrorReport.h` says the mechanism is mainly for. Script cannot
/// catch it and a `finally` block does not run, which is what `unibind/isolate.h`
/// promises.
///
/// The re-arm on the way out is what makes the termination sticky. The engine's
/// interrupt request is one-shot: it fires once and clears, so without this the
/// *next* thing the embedder ran would proceed normally even though nothing had
/// cancelled the termination. Re-requesting inside the callback leaves the bit
/// set, so anything that enters the interpreter again stops at its first
/// checkpoint. Once `CancelTerminateExecution` clears our flag the leftover bit
/// fires one last time and this returns true, which costs one interrupt and
/// changes nothing.
/// A script's retained text gaining a reference: the engine's source object
/// for it took the text as its private.
void AddSourceReference(const JS::Value& value) {
    if (value.isUndefined()) {
        return;
    }
    ++static_cast<Isolate::Impl::RetainedSource*>(value.toPrivate())->refs;
}

/// ... and losing one, which is the source object being finalized. The last
/// one lets the text go. Called from a finalizer, so it touches nothing the
/// collector owns.
void ReleaseSourceReference(const JS::Value& value) {
    if (value.isUndefined()) {
        return;
    }
    auto* source = static_cast<Isolate::Impl::RetainedSource*>(value.toPrivate());
    if (--source->refs == 0) {
        source->owner->sources.erase(source);
    }
}

bool OnInterrupt(JSContext* cx) {
    auto* isolate = static_cast<Isolate*>(JS_GetContextPrivate(cx));
    if (isolate == nullptr) {
        return true;
    }

    // Whatever `RequestInterrupt` asked for, once each, in the order it was
    // asked. Taken off the queue before any of it runs, so a callback that asks
    // for another interrupt gets the next pass rather than this one.
    std::vector<Isolate::Impl::PendingInterrupt> batch;
    {
        const std::lock_guard<std::mutex> lock(isolate->impl().interruptMutex);
        batch.swap(isolate->impl().interrupts);
    }
    for (const Isolate::Impl::PendingInterrupt& pending : batch) {
        // The callback may make handles and read values and may not run script.
        // SpiderMonkey would permit script here - measured, and it works - but
        // the portable rule is V8's, and it is the right rule anyway: an
        // interrupt fires between two bytecodes of unrelated code, so anything
        // a callback leaves pending is left for that code to trip over.
        pending.callback(*isolate, pending.data);
        if (JS_IsExceptionPending(cx)) {
            JS_ClearPendingException(cx);
        }
    }

    if (!isolate->impl().terminating.load(std::memory_order_acquire)) {
        return true;
    }
    JS_RequestInterruptCallback(cx);
    return false;
}

}  // namespace

void Isolate::TerminateExecution() noexcept {
    // The one entry point in this API that may be called from a thread other
    // than the isolate's own, and both halves of it are safe there:
    // `JS_RequestInterruptCallback` is documented to be callable from any
    // thread ("some time after any thread triggered the callback"), and the
    // flag is atomic.
    impl_->terminating.store(true, std::memory_order_release);
    JS_RequestInterruptCallback(impl_->cx);
}

bool Isolate::IsExecutionTerminating() const noexcept {
    return impl_->terminating.load(std::memory_order_acquire);
}

void Isolate::CancelTerminateExecution() noexcept {
    impl_->terminating.store(false, std::memory_order_release);
}

void Isolate::RequestInterrupt(InterruptCallback callback, CallbackData data) noexcept {
    if (callback == nullptr) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(impl_->interruptMutex);
        impl_->interrupts.push_back({.callback = callback, .data = data});
    }
    JS_RequestInterruptCallback(impl_->cx);
}

void Isolate::PostJob(JobCallback callback, CallbackData data) noexcept {
    if (callback == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(impl_->jobMutex);
    // Never coalesced: posting the same callback twice runs it twice, which is
    // what `unibind/isolate.h` promises and what makes a queue of work a queue
    // rather than a set of flags.
    impl_->jobs.push_back({.callback = callback, .data = data});
}

void Isolate::PostDelayedJob(JobCallback callback, CallbackData data, double delayInSeconds) noexcept {
    // `!(x > 0)` rather than `x <= 0`, so that a NaN is no delay too.
    if (!(delayInSeconds > 0)) {
        PostJob(callback, data);
        return;
    }
    if (callback == nullptr) {
        return;
    }
    // Past what the clock can count - a little under three hundred years from
    // now, in integer nanoseconds - the conversion would overflow and land in
    // the past, and a delay nothing will outlive would run at the next pump.
    // Half the room left is the cut-off, well clear of rounding, and beyond it
    // the job is due never.
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> delay(delayInSeconds);
    const std::chrono::duration<double> room(std::chrono::steady_clock::time_point::max() - now);
    const auto due = delay < room / 2 ? now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay)
                                      : std::chrono::steady_clock::time_point::max();
    const std::lock_guard<std::mutex> lock(impl_->jobMutex);
    impl_->delayedJobs.emplace(due, Impl::PostedJob{.callback = callback, .data = data});
}

void Isolate::PumpJobs() {
    // Nothing runs while a stop is in force, and the queues survive it: cancel
    // the termination and pump again. Draining here would run work the embedder
    // has just said it does not want run.
    if (impl_->terminating.load(std::memory_order_acquire)) {
        return;
    }

    // Engine jobs first, then posted work, then round again until both are
    // empty - so a posted job that settles a promise sees its continuations run
    // in the same pump, which is the behaviour that makes one drain better than
    // two.
    while (true) {
        // SpiderMonkey never drains on its own: `js::UseInternalJobQueues` in
        // `Isolate::New` makes the engine queue promise jobs instead of having
        // nowhere to put them, and this is the only thing that runs them. What
        // decision 23 costs the other backend - turning an automatic drain off
        // - costs this one nothing, because there was never one to turn off.
        js::RunJobs(impl_->cx);
        if (JS_IsExceptionPending(impl_->cx)) {
            JS_ClearPendingException(impl_->cx);
        }

        std::vector<Impl::PostedJob> batch;
        {
            const std::lock_guard<std::mutex> lock(impl_->jobMutex);
            // Whatever has fallen due joins the queue first, in the order it
            // fell due, so it runs in this pump.
            const auto now = std::chrono::steady_clock::now();
            auto& delayed = impl_->delayedJobs;
            while (!delayed.empty() && delayed.begin()->first <= now) {
                impl_->jobs.push_back(delayed.begin()->second);
                delayed.erase(delayed.begin());
            }
            batch.swap(impl_->jobs);
        }
        if (batch.empty()) {
            return;
        }
        for (const Impl::PostedJob& job : batch) {
            // The lock is not held here on purpose: a job is allowed to post
            // more work, and that work is drained by the next turn of this
            // loop.
            job.callback(*this, job.data);
            // A pump is not a call and has nowhere to put an exception, so
            // whatever a job throws stops here. A job that can fail says so to
            // the embedder itself.
            if (JS_IsExceptionPending(impl_->cx)) {
                JS_ClearPendingException(impl_->cx);
            }
            if (impl_->terminating.load(std::memory_order_acquire)) {
                return;
            }
        }
    }
}

std::unique_ptr<Isolate> Isolate::New(const IsolateOptions& options) {
    if (!Platform::IsInitialized()) {
        return nullptr;
    }
    if (g_threadIsolate != nullptr) {
        return nullptr;
    }

    // Nothrow, because the header promises an empty answer and never a crash
    // when a heap cannot be made - and running out of memory is one of the ways
    // it cannot. A `make_unique` here would throw `std::bad_alloc` out of a
    // function that says it returns empty instead, before the engine is even
    // reached.
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl());
    if (!impl) {
        return nullptr;
    }
    // `JS_NewContext` takes 32 bits and `heapLimitBytes` is a `size_t`, so on a
    // 64-bit build a ceiling above 4 GiB would be *truncated* - a 4 GiB request
    // arriving as a heap of zero. Clamping says "as much as this engine can be
    // told about", which is the nearest thing to what was asked.
    constexpr std::size_t MAX_HEAP = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t heapLimit = JS::DefaultHeapMaxBytes;
    if (options.heapLimitBytes != 0) {
        heapLimit = static_cast<std::uint32_t>(options.heapLimitBytes > MAX_HEAP ? MAX_HEAP : options.heapLimitBytes);
    }
    impl->cx = JS_NewContext(heapLimit);
    if (impl->cx == nullptr) {
        return nullptr;
    }
    // Before self-hosted code runs, so that it is compiled under the same limit
    // everything else will be.
    //
    // Clamped to the stack this thread actually has, which is the whole reason
    // the knob is worth having: a quota larger than the real stack is a limit
    // the engine never reaches, so runaway recursion walks off the end and the
    // process dies - the exact failure `stackLimitBytes` exists to turn into a
    // RangeError. Asking the OS is the only way to know; SpiderMonkey has no
    // opinion and will set whatever it is given. `ThreadStackQuotaForSize` then
    // takes off the margin the engine needs to still be able to build and throw
    // the error once it has decided it is out of stack.
    if (options.stackLimitBytes != 0) {
        JS_SetNativeStackQuota(impl->cx, JS::ThreadStackQuotaForSize(UsableStackBytes(options.stackLimitBytes)));
    }
    // Without a job queue the engine has nowhere to put a promise continuation.
    // The internal one queues them and runs nothing until `js::RunJobs`, which
    // is exactly what decision 23 asks for - `PumpJobs` the single point where
    // continuations run. On this engine that is the default rather than
    // something to switch off.
    if (!js::UseInternalJobQueues(impl->cx)) {
        JS_DestroyContext(impl->cx);
        return nullptr;
    }
    if (!JS::InitSelfHostedCode(impl->cx)) {
        JS_DestroyContext(impl->cx);
        return nullptr;
    }

    // Nothrow for the same reason, and unwound by hand for one this backend
    // feels harder than V8 does: if this allocation fails the constructor never
    // runs, so `impl` still holds the `JSContext` and nothing else does -
    // `~Isolate` is what destroys one, and there is no `Isolate`. Left alone
    // that is an entire JavaScript heap, with its self-hosted code and its
    // helper-thread registration, leaked out of a function whose documented
    // answer is "empty". The allocation function is sequenced before the
    // new-initializer, so `impl` is untouched here and still ours to unwind.
    std::unique_ptr<Isolate> isolate(new (std::nothrow) Isolate(std::move(impl)));
    // NOLINTBEGIN(bugprone-use-after-move) - on failure the ctor never ran, so the move never happened
    if (!isolate) {
        JS_DestroyContext(impl->cx);
        impl->cx = nullptr;
        return nullptr;
    }
    // NOLINTEND(bugprone-use-after-move)
    isolate->impl().self = isolate.get();
    // How a native call finds its way back to the owning ub::Isolate. There
    // is one JSContext per isolate, so the context private is free for us.
    // `OnInterrupt` below reads it, so it has to be set before the callback is
    // installed.
    JS_SetContextPrivate(isolate->impl().cx, isolate.get());
    // After the context private, which is what the callback reads to name the
    // isolate in its report. The engine's hook is per-context, so the
    // process-wide handler an embedder installed on the `Platform` is wired up
    // once per isolate here.
    if (g_faultHandler != nullptr) {
        JS::SetOutOfMemoryCallback(isolate->impl().cx, &OnOutOfMemory, nullptr);
    }
    // Checked, because this is the one failure that would be completely silent:
    // the callback is appended to a vector and the append can fail, and an
    // isolate without it looks perfectly healthy while `TerminateExecution`
    // quietly does nothing at all. An embedder would discover that when a
    // runaway script did not stop, which is the moment it can least afford to.
    // `~Isolate` is not in play yet - nothing has been handed out - so the
    // context is unwound here.
    // What keeps a script's text for `TryCatch::Location` exactly as long as the
    // engine keeps its code. See `Isolate::Impl::sources`.
    JS::SetScriptPrivateReferenceHooks(JS_GetRuntime(isolate->impl().cx), &AddSourceReference, &ReleaseSourceReference);
    if (!JS_AddInterruptCallback(isolate->impl().cx, &OnInterrupt)) {
        JS_DestroyContext(isolate->impl().cx);
        isolate->impl().cx = nullptr;
        return nullptr;
    }
    g_threadIsolate = isolate.get();
    // The figure is the process's and never changes, but reading it needs a
    // context, so the first isolate to exist is what makes `WorkerThreads()`
    // answerable without building one specially.
    if (!g_workerThreads.has_value()) {
        g_workerThreads = JS_GetGCParameter(isolate->impl().cx, JSGC_HELPER_THREAD_COUNT);
    }
    return isolate;
}

std::optional<std::uint32_t> Platform::WorkerThreads() noexcept {
    if (!g_initialized) {
        return std::nullopt;
    }
    if (g_workerThreads.has_value()) {
        return g_workerThreads;
    }
    // Asked before any isolate exists, which is exactly when an embedder that
    // needs to know wants to ask. `JSGC_HELPER_THREAD_COUNT` is a per-process
    // figure fixed by `JS_Init`, but reading it needs *a* context, so one is
    // built and thrown away rather than answering "the engine would not say"
    // for a number the engine knows perfectly well.
    //
    // Safe on a thread that has no isolate, and that is checked: one JSContext
    // per thread is the rule this whole backend is shaped around, so a probe
    // context on a thread that already has one is the thing decision 11 exists
    // to prevent.
    if (g_threadIsolate != nullptr) {
        g_workerThreads = JS_GetGCParameter(g_threadIsolate->impl().cx, JSGC_HELPER_THREAD_COUNT);
        return g_workerThreads;
    }
    JSContext* probe = JS_NewContext(JS::DefaultHeapMaxBytes);
    if (probe == nullptr) {
        return std::nullopt;
    }
    g_workerThreads = JS_GetGCParameter(probe, JSGC_HELPER_THREAD_COUNT);
    JS_DestroyContext(probe);
    return g_workerThreads;
}

Isolate::Isolate(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Isolate::~Isolate() {
    // Anything the engine might still finalise has to go before the context
    // does, and anything the engine's finalisers need - a class record, say -
    // has to go after it.
    //
    // The context may already be gone: `Isolate::New` unwinds a half-built
    // isolate through this destructor, so a null here is a bring-up that
    // stopped part way rather than something to be surprised by.
    if (impl_->cx != nullptr) {
        // The backend's own realm goes first: a `PersistentRooted` unlinks
        // itself from the runtime's root list when it is destroyed, and the
        // runtime is about to stop existing. `impl_` outlives this body.
        impl_->utility.reset();
        JS_DestroyContext(impl_->cx);
        impl_->cx = nullptr;
    }
    for (detail::NativeBox* box : impl_->liveNatives) {
        if (box != nullptr && box->destroy != nullptr) {
            box->destroy(box);
        }
    }
    impl_->liveNatives.clear();
    impl_->classes.clear();
    impl_->templates.clear();
    impl_->callbacks.clear();
    if (g_threadIsolate == this) {
        g_threadIsolate = nullptr;
    }
#if UNIBIND_HANDLE_CHECKS
    // The rule in unibind/isolate.h, diagnosed where it is broken rather than
    // at the crash it causes later: a Context, a Script or a Global<T> the
    // *embedder* is still holding is memory this isolate can never give back,
    // and the release, when it comes, unlinks a PersistentRooted from a
    // runtime that no longer exists.
    //
    // Last rather than first, because everything above legitimately gives some
    // of these back: a native the isolate destroys may itself own a realm and
    // a root, and a sandbox does exactly that. What is left at this line is
    // what nothing but the embedder holds.
    assert(impl_->embedderRefs == 0 && "a Context, Script or Global outlived its Isolate");
#endif
}

bool Isolate::HasPendingException() const noexcept {
    return detail::HasPendingException(*impl_->self);
}

void Isolate::ThrowError(ErrorKind kind, std::string_view message) {
    detail::ThrowErrorLossy(*this, kind, message);
}

HeapStatistics Isolate::GetHeapStatistics() const noexcept {
    // The collector reserves its heap in chunks, so the chunks it holds are
    // what it has reserved - used or not, which is what `totalBytes` asks.
    const std::uint64_t chunks = JS_GetGCParameter(impl_->cx, JSGC_TOTAL_CHUNKS);
    const std::uint64_t chunkBytes = JS_GetGCParameter(impl_->cx, JSGC_CHUNK_BYTES);
    return HeapStatistics{.usedBytes = JS_GetGCParameter(impl_->cx, JSGC_BYTES),
                          .totalBytes = chunks * chunkBytes,
                          .limitBytes = JS_GetGCParameter(impl_->cx, JSGC_MAX_BYTES)};
}

void Isolate::RequestGarbageCollection() noexcept {
    JS::PrepareForFullGC(impl_->cx);
    JS::NonIncrementalGC(impl_->cx, JS::GCOptions::Normal, JS::GCReason::API);
}

void Isolate::StoreEmbedderData(CallbackData data) noexcept {
    impl_->embedder = data;
}

CallbackData Isolate::LoadEmbedderData() const noexcept {
    return impl_->embedder;
}

Maybe<Context> Context::New(Isolate& isolate) {
    detail::ContextRec* rec = detail::NewContext(isolate);
    if (rec == nullptr) {
        return std::nullopt;
    }
    return Context(rec);
}

}  // namespace ub
