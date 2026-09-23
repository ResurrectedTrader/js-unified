#pragma once
/// \file
/// Contexts (realms) and entering them.
///
/// An isolate can hold any number of contexts. Each has its own global object
/// and its own set of built-ins; a value from one is still a value in the
/// other, which is what makes a sandbox useful and what makes cross-realm
/// identity checks (`instanceof` across realms) fail the way they do in every
/// engine.

#include <cstddef>
#include <utility>

#include "unibind/detail/backend.h"
#include "unibind/fwd.h"
#include "unibind/handle.h"
#include "unibind/types.h"

namespace ub {

/// One realm. Reference counted: the last handle to go drops the embedder's
/// root on the realm's global object, after which the collector may reclaim it.
///
/// V8 spells this `Local<Context>`, i.e. a context is a value. This API does
/// not, because a context is not a value on every engine, and making it one
/// would tie a realm's lifetime to a handle scope - which is exactly wrong for
/// something whose whole purpose is to outlive the call that made it.
///
/// It outlives a scope, and it **does not outlive its isolate**: drop the last
/// one before the isolate goes. `unibind/isolate.h` says what happens if you do
/// not, and a checked build diagnoses it.
///
/// **A value may be used with any realm of its isolate.** A handle belongs to
/// an isolate and a thread, not to a realm (docs/lifetimes.md rule 7), so an
/// object made in one realm can be read, written, called and handed to script
/// in another. That is what makes a sandbox usable at all: the point of a
/// second realm is to put values into it.
///
/// Making a backend deliver that is the backend's problem, and the two engines
/// charge differently for it: V8 needs nothing, while SpiderMonkey must wrap an
/// object into the target compartment first (`JS_WrapValue`). Guaranteeing it
/// here rather than leaving it undefined is deliberate - the alternative would
/// have been a restriction that contradicts the handle model and costs the API
/// its most useful realm feature, to save one backend a call.
///
/// The one thing that is *not* guaranteed is **identity as seen from a foreign
/// realm**. Where an engine wraps, the wrapper is a distinct object, so an
/// engine-level "is this the same object" asked across a realm boundary can
/// answer differently on different backends. Compare values within one realm,
/// or compare something the values carry. A portable program does not ask.
///
/// ---------------------------------------------------------------------------
/// Read an object in the realm that owns it
/// ---------------------------------------------------------------------------
///
/// **Enter a realm before reading through an object that belongs to it.** That
/// is a rule, not a matter of taste, and it is the second boundary on the
/// guarantee above: a value *crosses* freely, but a realm's **global object** is
/// access-checked, and reading a property of one while a different realm is
/// current fails - on V8 with a `TypeError: no access`, before any embedder code
/// runs at all.
///
/// It is the canonical sandbox that meets this: a property hook on an outer
/// object that answers by reading the inner realm's `globalThis`. The hook is
/// invoked with the outer realm current, so it must open a `ContextScope` on the
/// inner realm before it touches anything - which is one line, works on both
/// backends, and is the whole fix.
///
/// There is deliberately no way to say "these two realms trust each other". V8
/// spells that as a security token shared between contexts; SpiderMonkey has no
/// token, only compartments and principals, and the two do not describe the same
/// thing closely enough to promise one. A knob that meant something different on
/// each backend would be worse than the rule, and the rule costs a line.
class Context {
   public:
    /// Empty if a realm could not be made: the engine refused, or there was
    /// not the memory for one.
    [[nodiscard]] static std::optional<Context> New(Isolate& isolate);

    constexpr Context() noexcept = default;
    ~Context() { Reset(); }

    /// Copyable, because a realm outlives the call that made it and more than
    /// one place legitimately refers to the same one - a callback is handed the
    /// context it was called in, and it must not be the owner of it. The record
    /// is reference counted; the realm goes when the last reference does.
    Context(const Context& other) noexcept : rec_(other.rec_) { detail::RetainContext(rec_); }
    Context& operator=(const Context& other) noexcept {
        if (this != &other) {
            detail::RetainContext(other.rec_);
            Reset();
            rec_ = other.rec_;
        }
        return *this;
    }

    Context(Context&& other) noexcept : rec_(std::exchange(other.rec_, nullptr)) {}
    Context& operator=(Context&& other) noexcept {
        if (this != &other) {
            Reset();
            rec_ = std::exchange(other.rec_, nullptr);
        }
        return *this;
    }

    /// Implementation detail: how a backend makes a Context from its record.
    [[nodiscard]] static Context FromRec(detail::ContextRec* rec) noexcept {
        detail::RetainContext(rec);
        return Context(rec);
    }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return rec_ == nullptr; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return rec_ != nullptr; }

    [[nodiscard]] Isolate& GetIsolate() const noexcept { return detail::ContextIsolate(*this); }

    /// The realm's global object. V8 calls this `Global()`; here that name is
    /// taken by the `Global<T>` handle type.
    [[nodiscard]] Local<Object> GlobalObject() const noexcept {
        return Local<Object>::FromSlot(detail::ContextGlobalObject(*this));
    }

    /// Implementation detail: the backend's realm record.
    [[nodiscard]] constexpr detail::ContextRec* rec() const noexcept { return rec_; }

    void Reset() noexcept {
        if (rec_ != nullptr) {
            detail::ReleaseContext(rec_);
            rec_ = nullptr;
        }
    }

   private:
    explicit constexpr Context(detail::ContextRec* rec) noexcept : rec_(rec) {}

    detail::ContextRec* rec_ = nullptr;
};

/// Makes a context current for the duration of a block: compiled code runs in
/// it, and the engine's "current realm" is it.
///
/// Stack only and LIFO, like every scope in this API - on SpiderMonkey it is a
/// `JSAutoRealm`, which has those rules natively.
class ContextScope {
   public:
    explicit ContextScope(const Context& context) noexcept {
        detail::ContextEnter(context, *reinterpret_cast<detail::ContextScopeState*>(storage_));
    }
    ~ContextScope() { detail::ContextLeave(*reinterpret_cast<detail::ContextScopeState*>(storage_)); }

    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
    ContextScope(ContextScope&&) = delete;
    ContextScope& operator=(ContextScope&&) = delete;

    void* operator new(std::size_t) = delete;
    void* operator new[](std::size_t) = delete;

   private:
    alignas(UNIBIND_CONTEXT_SCOPE_STORAGE_ALIGN) unsigned char storage_[UNIBIND_CONTEXT_SCOPE_STORAGE_SIZE]{};
};

}  // namespace ub
