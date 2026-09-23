#pragma once
/// \file
/// Object and function templates: the shape of an object, declared once and
/// instantiated into any number of contexts. Plus interceptors, which are the
/// reason a sandbox or a proxy-like scope object is expressible at all.
///
/// One deliberate divergence from V8. A V8 template can hold `Local` values,
/// because a V8 template is itself a heap object in an isolate. Here a
/// template is an isolate-owned descriptor built before any context exists and
/// with no handle scope open, so what it can install directly is the closed
/// set in `ub::Constant`; anything richer is installed by a callback, at
/// instantiation time, when there is a context to install it into. The
/// restriction is what makes templates expressible on an engine whose class
/// definitions are plain C structs.
///
/// Templates live as long as their isolate. A template handle is a
/// non-owning reference, freely copied.

#include <cstdint>
#include <string_view>

#include "unibind/context.h"
#include "unibind/detail/backend.h"
#include "unibind/function.h"
#include "unibind/fwd.h"
#include "unibind/handle.h"
#include "unibind/isolate.h"
#include "unibind/types.h"
#include "unibind/value.h"

namespace ub {

// --- interceptors ----------------------------------------------------------
//
// A catch-all for *any* property on an object, named or indexed. Each hook
// says whether it handled the access; `Intercepted::No` means "carry on with
// the ordinary lookup".
//
// Three states rather than two, because "I did not handle this" has to be
// distinguishable from "I handled it and the answer is undefined". An engine
// with no interceptor of its own implements these as proxy traps - the only
// construct that both runs on every access and can hand the access back to the
// ordinary object. A resolve-style hook cannot: it fires only when a property
// is already missing, what it defines then sticks, and there is no setter at
// all, so it can never say "ask me again next time".
//
// **A hook that throws has intercepted the access, whatever it returned.**
// Declining is an instruction to carry on with the ordinary lookup, and an
// exception is an instruction to stop; a hook that does both has asked for two
// incompatible things, so the throw wins and the access fails. This is not a
// tidy-up: one engine forbids re-entering its own lookup with an exception
// pending, so "decline, with a throw left behind" is not a state a backend may
// hand it. The rule therefore belongs here, where an embedder reads it, rather
// than in whatever each engine does with the contradiction.
//
// What it means in practice: throw *or* decline. If a hook needs to report a
// failure, it throws and the return value stops mattering; if it wants the
// ordinary object to answer, it declines and leaves nothing pending.

using NamedGetterCallback = Intercepted (*)(const Local<Name>& property, const PropertyCallbackInfo& info);
using NamedSetterCallback = Intercepted (*)(const Local<Name>& property, const Local<Value>& value,
                                            const PropertyCallbackInfo& info);
/// Empty means not intercepted; a value means the property exists with those
/// attributes.
using NamedQueryCallback = std::optional<PropertyAttribute> (*)(const Local<Name>& property,
                                                                const PropertyCallbackInfo& info);
/// Empty means not intercepted; `true`/`false` is the result of `delete`.
using NamedDeleterCallback = std::optional<bool> (*)(const Local<Name>& property, const PropertyCallbackInfo& info);
/// The own keys this object claims. Empty means it claims none: an engine
/// whose enumerator has a decline path may treat that as declining, V8's has
/// none, so do not depend on the difference.
using NamedEnumeratorCallback = std::optional<Local<Array>> (*)(const PropertyCallbackInfo& info);

using IndexedGetterCallback = Intercepted (*)(std::uint32_t index, const PropertyCallbackInfo& info);
using IndexedSetterCallback = Intercepted (*)(std::uint32_t index, const Local<Value>& value,
                                              const PropertyCallbackInfo& info);
using IndexedQueryCallback = std::optional<PropertyAttribute> (*)(std::uint32_t index,
                                                                  const PropertyCallbackInfo& info);
using IndexedDeleterCallback = std::optional<bool> (*)(std::uint32_t index, const PropertyCallbackInfo& info);
using IndexedEnumeratorCallback = std::optional<Local<Array>> (*)(const PropertyCallbackInfo& info);

struct NamedPropertyHandler {
    NamedGetterCallback getter = nullptr;
    NamedSetterCallback setter = nullptr;
    NamedQueryCallback query = nullptr;
    NamedDeleterCallback deleter = nullptr;
    NamedEnumeratorCallback enumerator = nullptr;
    CallbackData data;
};

/// The half that answers for array indices - every integer key from 0 to
/// 2^32 - 2, however it was spelled in script, as long as it was spelled
/// canonically. Every other key, `4294967295` and `"01"` among them, goes to
/// the named half.
struct IndexedPropertyHandler {
    IndexedGetterCallback getter = nullptr;
    IndexedSetterCallback setter = nullptr;
    IndexedQueryCallback query = nullptr;
    IndexedDeleterCallback deleter = nullptr;
    IndexedEnumeratorCallback enumerator = nullptr;
    CallbackData data;
};

namespace detail {

// The template half of the backend interface.
TemplateRec* NewObjectTemplate(Isolate& isolate);
TemplateRec* NewFunctionTemplate(Isolate& isolate, FunctionCallback callback, CallbackData data);

void TemplateSetConstant(TemplateRec* tpl, std::string_view name, Constant value, PropertyAttribute attributes);
void TemplateSetMethod(TemplateRec* tpl, std::string_view name, FunctionCallback callback, CallbackData data,
                       PropertyAttribute attributes);
void TemplateSetSymbolMethod(TemplateRec* tpl, WellKnownSymbol key, FunctionCallback callback, CallbackData data);
void TemplateSetAccessor(TemplateRec* tpl, std::string_view name, AccessorGetterCallback getter,
                         AccessorSetterCallback setter, CallbackData data, PropertyAttribute attributes);
void TemplateSetTemplate(TemplateRec* tpl, std::string_view name, TemplateRec* value, PropertyAttribute attributes);
void TemplateSetNamedHandler(TemplateRec* tpl, const NamedPropertyHandler& handler);
void TemplateSetIndexedHandler(TemplateRec* tpl, const IndexedPropertyHandler& handler);

void TemplateSetClassName(TemplateRec* tpl, std::string_view name);
void TemplateInherit(TemplateRec* child, TemplateRec* parent);
TemplateRec* TemplatePrototype(TemplateRec* tpl);
TemplateRec* TemplateInstance(TemplateRec* tpl);

std::optional<Slot> TemplateNewInstance(const Context& context, TemplateRec* tpl);
std::optional<Slot> TemplateGetFunction(const Context& context, TemplateRec* tpl);
std::optional<bool> TemplateHasInstance(const Context& context, TemplateRec* tpl, Slot value);

}  // namespace detail

/// The shape of an object: its properties, its accessors, and optionally an
/// interceptor that answers for every property at once.
///
/// Every name declared here, on a `FunctionTemplate` or on a `Class<T>` - and a
/// string `Constant` - is UTF-8 text and reaches script as that text; bytes
/// that are not UTF-8 are decoded as `String::NewFromUtf8` decodes them.
class ObjectTemplate {
   public:
    [[nodiscard]] static ObjectTemplate New(Isolate& isolate) {
        return ObjectTemplate(detail::NewObjectTemplate(isolate));
    }

    void Set(std::string_view name, Constant value, PropertyAttribute attributes = PropertyAttribute::None) const {
        detail::TemplateSetConstant(rec_, name, value, attributes);
    }
    void Set(std::string_view name, FunctionCallback callback, CallbackData data = {},
             PropertyAttribute attributes = PropertyAttribute::DontEnum) const {
        detail::TemplateSetMethod(rec_, name, callback, data, attributes);
    }
    /// A method under a well-known symbol - `Symbol.iterator`, most usefully.
    void Set(WellKnownSymbol key, FunctionCallback callback, CallbackData data = {}) const {
        detail::TemplateSetSymbolMethod(rec_, key, callback, data);
    }
    void Set(std::string_view name, const ObjectTemplate& value,
             PropertyAttribute attributes = PropertyAttribute::None) const {
        detail::TemplateSetTemplate(rec_, name, value.rec_, attributes);
    }
    void Set(std::string_view name, const FunctionTemplate& value,
             PropertyAttribute attributes = PropertyAttribute::None) const;

    /// A native getter and optional setter on one named property. This is an
    /// ECMAScript accessor property, so it has no [[Writable]]: a getter with
    /// no setter is the read-only form and `PropertyAttribute::ReadOnly` is
    /// ignored.
    void SetAccessor(std::string_view name, AccessorGetterCallback getter, AccessorSetterCallback setter = nullptr,
                     CallbackData data = {}, PropertyAttribute attributes = PropertyAttribute::None) const {
        detail::TemplateSetAccessor(rec_, name, getter, setter, data, attributes);
    }

    /// The catch-all. Every named (respectively indexed) property access on an
    /// instance goes through these hooks first.
    void SetHandler(const NamedPropertyHandler& handler) const { detail::TemplateSetNamedHandler(rec_, handler); }
    void SetHandler(const IndexedPropertyHandler& handler) const { detail::TemplateSetIndexedHandler(rec_, handler); }

    [[nodiscard]] std::optional<Local<Object>> NewInstance(const Context& context) const {
        return detail::WrapSlot<Object>(detail::TemplateNewInstance(context, rec_));
    }

    /// Implementation detail: the backend's record.
    [[nodiscard]] constexpr detail::TemplateRec* rec() const noexcept { return rec_; }

   private:
    friend class FunctionTemplate;
    explicit constexpr ObjectTemplate(detail::TemplateRec* rec) noexcept : rec_(rec) {}

    detail::TemplateRec* rec_;
};

/// A constructor plus the prototype its instances get - and, unlike a class, a
/// function that can also just be called.
///
/// The three ways this API makes something script can invoke differ in exactly
/// one respect each, and between them they cover the whole grid:
///
///     made by              f()        new f()
///     -------------------  ---------  ---------
///     Function::New        yes        TypeError
///     FunctionTemplate     yes        yes
///     Class<T>             TypeError  yes
///
/// So a `FunctionTemplate`'s callback is the one that has to tell the two apart,
/// which is what `CallbackInfo::IsConstructCall()` is for, and it is what lets a
/// template express a built-in like `Error` that means something both ways. A
/// `FunctionTemplate` that refused a plain call would be a strictly worse
/// `Class<T>`, and the middle row would be unreachable.
///
/// In a construct call the callback's receiver is the new instance, and that
/// is what `new` evaluates to - unless the callback answers with an object,
/// which then is, as for a JavaScript constructor that returns one. A
/// primitive answer is ignored. (`Class<T>` discards the answer outright: what
/// comes out of it must carry a `T`.)
///
/// `Class<T>` is the typed wrapper over this; reach for a bare FunctionTemplate
/// when the instances carry no native state, or when the thing genuinely is a
/// function as well as a constructor.
class FunctionTemplate {
   public:
    /// `callback` is optional: a template without one is still constructable,
    /// and is how you declare a shape that only ever arrives through `new`.
    /// What a *plain call* to one does is deliberately unspecified - there is
    /// nothing to run - so give it a callback if it is meant to be called.
    [[nodiscard]] static FunctionTemplate New(Isolate& isolate, FunctionCallback callback = nullptr,
                                              CallbackData data = {}) {
        return FunctionTemplate(detail::NewFunctionTemplate(isolate, callback, data));
    }

    void SetClassName(std::string_view name) const { detail::TemplateSetClassName(rec_, name); }
    void Inherit(const FunctionTemplate& parent) const { detail::TemplateInherit(rec_, parent.rec_); }

    /// Where instance methods and accessors go.
    [[nodiscard]] ObjectTemplate PrototypeTemplate() const { return ObjectTemplate(detail::TemplatePrototype(rec_)); }
    /// Where own properties of every instance go.
    [[nodiscard]] ObjectTemplate InstanceTemplate() const { return ObjectTemplate(detail::TemplateInstance(rec_)); }

    /// Statics live on the constructor function object itself.
    void Set(std::string_view name, Constant value, PropertyAttribute attributes = PropertyAttribute::None) const {
        detail::TemplateSetConstant(rec_, name, value, attributes);
    }
    void Set(std::string_view name, FunctionCallback callback, CallbackData data = {},
             PropertyAttribute attributes = PropertyAttribute::DontEnum) const {
        detail::TemplateSetMethod(rec_, name, callback, data, attributes);
    }

    [[nodiscard]] std::optional<Local<Function>> GetFunction(const Context& context) const {
        return detail::WrapSlot<Function>(detail::TemplateGetFunction(context, rec_));
    }

    /// Whether the value was made by this template, i.e. the check to do
    /// before unwrapping.
    template <class T>
    [[nodiscard]] std::optional<bool> HasInstance(const Context& context, const Local<T>& value) const {
        return detail::TemplateHasInstance(context, rec_, value.slot());
    }

    /// Implementation detail: the backend's record.
    [[nodiscard]] constexpr detail::TemplateRec* rec() const noexcept { return rec_; }

   private:
    explicit constexpr FunctionTemplate(detail::TemplateRec* rec) noexcept : rec_(rec) {}

    detail::TemplateRec* rec_;
};

inline void ObjectTemplate::Set(std::string_view name, const FunctionTemplate& value,
                                PropertyAttribute attributes) const {
    detail::TemplateSetTemplate(rec_, name, value.rec(), attributes);
}

}  // namespace ub
