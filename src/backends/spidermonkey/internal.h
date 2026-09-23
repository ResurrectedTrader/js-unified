#pragma once
// Shared by the SpiderMonkey backend's translation units. Not a public header,
// and the only place outside this directory that may see a SpiderMonkey type
// is nowhere: nothing includes this but the backend itself.
//
// The one thing worth reading here is `Frame`. The whole of docs/lifetimes.md
// stands or falls on a frame being a genuine stack root, and on this engine
// that means a `JS::RootedVector<JS::Value>` constructed in the caller's
// `HandleScope` storage and destroyed in strict LIFO order. It is not a
// `PersistentRooted`, not a heap node, not a hand-rolled trace hook: it is the
// engine's own stack-rooting type, linked onto the context's rooted list in
// its constructor and unlinked in its destructor, with the engine's own
// assertion that the unlink is in order.

#include <js/AllocPolicy.h>
#include <js/Array.h>
#include <js/CallAndConstruct.h>
#include <js/CharacterEncoding.h>
#include <js/CompilationAndEvaluation.h>
#include <js/CompileOptions.h>
#include <js/Conversions.h>
#include <js/ErrorReport.h>
#include <js/Exception.h>
#include <js/GlobalObject.h>
#include <js/Initialization.h>
#include <js/Object.h>
#include <js/Promise.h>
#include <js/PropertyAndElement.h>
#include <js/PropertyDescriptor.h>
#include <js/PropertySpec.h>
#include <js/Realm.h>
#include <js/RootingAPI.h>
#include <js/SourceText.h>
#include <js/Symbol.h>
#include <js/Transcoding.h>
#include <js/Value.h>
#include <js/Warnings.h>
#include <js/Wrapper.h>
#include <js/experimental/JSStencil.h>
#include <jsapi.h>
#include <jsfriendapi.h>
#include <mozilla/Maybe.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frame_alloc.h"
#include "unibind/class.h"
#include "unibind/context.h"
#include "unibind/exception.h"
#include "unibind/function.h"
#include "unibind/handle.h"
#include "unibind/isolate.h"
#include "unibind/script.h"
#include "unibind/template.h"
#include "unibind/value.h"

namespace ub {

namespace detail {
struct TemplateRec;
struct ClassRec;
struct CallbackRecord;
struct TryCatchState;

void DestroyTemplate(TemplateRec* tpl) noexcept;
void DestroyClass(ClassRec* rec) noexcept;
void DestroyCallbackRecord(CallbackRecord* record) noexcept;

/// So `Isolate::Impl` can own these by value without seeing their definitions.
struct TemplateDeleter {
    void operator()(TemplateRec* tpl) const noexcept { DestroyTemplate(tpl); }
};
struct ClassDeleter {
    void operator()(ClassRec* rec) const noexcept { DestroyClass(rec); }
};
struct CallbackRecordDeleter {
    void operator()(CallbackRecord* record) const noexcept { DestroyCallbackRecord(record); }
};

using CallbackRecordPtr = std::unique_ptr<CallbackRecord, CallbackRecordDeleter>;
}  // namespace detail

struct Isolate::Impl {
    JSContext* cx = nullptr;
    detail::Frame* current = nullptr;
    std::uint32_t nextEpoch = 1;
    std::uint32_t nextTemplateId = 1;
    CallbackData embedder;
    Isolate* self = nullptr;
    detail::TryCatchState* tryCatch = nullptr;

    /// Whether `Isolate::TerminateExecution` is in force.
    ///
    /// **This is unibind bookkeeping, not engine state.** SpiderMonkey has no
    /// sticky terminating condition: the interrupt callback returns false, the
    /// script unwinds, and on the very next call the `JSContext` is live again
    /// as though nothing had happened. `unibind/isolate.h` promises something
    /// stronger - that everything which would run script keeps failing until
    /// `CancelTerminateExecution` - so this flag is what keeps it, and every
    /// path into the engine has to consult it. A path that forgets will
    /// cheerfully run script that was told to stop, and the engine will not
    /// object. On V8 the same question is the engine's own.
    ///
    /// Atomic because the whole point of it is to be set from another thread;
    /// it is the only field here that is.
    std::atomic<bool> terminating{false};

    /// Work posted with `Isolate::PostJob`, run by `Isolate::PumpJobs`.
    ///
    /// Guarded because posting is explicitly allowed from any thread while
    /// draining happens on the isolate's own. Only the queue is shared; a job
    /// runs with the lock released, because a job may post more work and the
    /// header promises that work is drained too.
    struct PostedJob {
        JobCallback callback = nullptr;
        CallbackData data;
    };
    std::mutex jobMutex;
    std::vector<PostedJob> jobs;
    /// `PostDelayedJob`'s work, keyed on when it falls due. A multimap keeps
    /// jobs that fall due together in the order they were posted, which is
    /// the order the header promises. Moved into `jobs` by `PumpJobs`.
    std::multimap<std::chrono::steady_clock::time_point, PostedJob> delayedJobs;

    /// Interrupt callbacks asked for with `Isolate::RequestInterrupt`, run once
    /// each the next time the engine checks. Same sharing story as the jobs.
    struct PendingInterrupt {
        InterruptCallback callback = nullptr;
        CallbackData data;
    };
    std::mutex interruptMutex;
    std::vector<PendingInterrupt> interrupts;

    /// Stable homes for things the engine is handed a bare pointer to.
    ///
    /// The templates and the classes are isolate-lifetime by design (see
    /// docs/spidermonkey.md). The callback records are not all of them: one
    /// behind a value is taken out again when that value is finalised, which
    /// is why this is keyed by the record's address rather than being a list.
    std::unordered_map<detail::CallbackRecord*, detail::CallbackRecordPtr> callbacks;
    std::vector<std::unique_ptr<detail::TemplateRec, detail::TemplateDeleter>> templates;
    std::vector<std::unique_ptr<detail::ClassRec, detail::ClassDeleter>> classes;
    /// Natives whose object was never finalised, destroyed at teardown so the
    /// two backends agree on when embedder state goes away.
    ///
    /// A set rather than a list, and the difference is not tidiness: a
    /// finalizer has to find its own entry, and finding it by walking makes
    /// the cost of collecting one instance a function of how many the isolate
    /// has *ever* made. Measured before it was one: six windows of twenty
    /// thousand instances went from 46 ms to 442 ms, dead linear, while the
    /// other backend - which has always kept a map - stayed flat.
    std::unordered_set<detail::NativeBox*> liveNatives;

    /// A realm of the backend's own, made on demand.
    ///
    /// SpiderMonkey has no "no realm" mode: every allocation needs one, and
    /// the API has operations that take an `Isolate&` and nothing else -
    /// `String::New`, `External::New`, `Throw`, `CaptureStackFrames`. An
    /// embedder is entitled to call those with no `ContextScope` open, and
    /// `Isolate::PostJob` says in as many words that a job runs with no realm
    /// current, so "enter one yourself" is not an answer this backend may
    /// give. It is built the first time one of those needs it and never
    /// otherwise, and nothing ever runs script in it.
    JS::PersistentRootedObject utility;

    /// Contexts, scripts and `Global<T>` roots the embedder is still holding.
    ///
    /// These three are `new`ed on demand and `delete`d by the call that gives
    /// them back, which is the embedder's to make - so unlike everything else
    /// here they are not in a container this isolate can empty. One still alive
    /// in `~Isolate` leaks its own memory *and* leaves a later `Reset`
    /// unlinking a `PersistentRooted` from a runtime that has been destroyed,
    /// so `unibind/isolate.h` makes it a rule and this counter is what
    /// diagnoses breaking it, in a checked build, where it happens.
    std::int32_t embedderRefs = 0;

    /// The text of every live script compiled through `Script`, which
    /// `TryCatch::Location` quotes a runtime error's line from. SpiderMonkey
    /// keeps a copy of its own but offers no way to read a line of it back, and
    /// V8 gives the line.
    ///
    /// Each entry is the script private of the engine's source object for one
    /// compile, and lives exactly as long as that object: the reference hooks
    /// installed in `Isolate::New` count it up and down, and the last release
    /// removes it. So a script's text goes when its code does - its `Script`
    /// and every function it made - and not before, which is what lets an
    /// error raised by a function outlive the `Script` that defined it and
    /// still be quoted. Keyed by the entry's own address.
    struct RetainedSource {
        Impl* owner = nullptr;
        std::string name;
        std::string text;
        std::int64_t firstLine = 1;
        std::uint32_t refs = 0;
    };
    std::unordered_map<const RetainedSource*, std::unique_ptr<RetainedSource>> sources;
};

namespace detail {

/// What a native function or accessor was declared with. SpiderMonkey hands a
/// `JSNative` no closure pointer of its own, so the pair lives here and the
/// function object carries the address in a reserved slot.
///
/// A record a *template* declared lives as long as the isolate (decision 13).
/// A record behind a `Function::New` or an `External::New` lives as long as
/// the value, because that is what the value is: an ordinary thing script
/// drops and the collector takes. `owner` is how the finalizer that notices
/// finds the map to take it out of.
struct CallbackRecord {
    Isolate* owner = nullptr;
    FunctionCallback callback = nullptr;
    AccessorGetterCallback getter = nullptr;
    AccessorSetterCallback setter = nullptr;
    CallbackData data;
    /// The function carries a script value for its data, in its holder's
    /// second reserved slot. A flag here rather than a look at that slot on
    /// every call, so that a function without one pays nothing for the
    /// question.
    bool hasValue = false;
};

/// Reserved slot 0 of a `NewFunctionWithReserved` function holds the
/// `CallbackRecord*`.
inline constexpr std::size_t FUNCTION_RECORD_SLOT = 0;
/// Slot 1 of a function made by `Function::New` holds the object whose
/// finalizer gives that record back - and, for a function made with a script
/// value as its data, the value too, in the holder's second slot. An
/// accessor's function uses the same slot for the property name instead, and
/// needs no holder: a record a template declared lives as long as the isolate
/// anyway (decision 13).
///
/// Those are the only two: `js::NewFunctionWithReserved` gives a function
/// exactly two reserved slots, so anything a function has to carry beyond
/// them goes in the holder.
inline constexpr std::size_t FUNCTION_HOLDER_SLOT = 1;

/// Where a frame's spill buffer comes from: the C++ global allocator, not the
/// engine's.
///
/// `JS::RootedVector` defaults to `js::TempAllocPolicy`, which allocates
/// through `js_malloc`. Two things follow from moving off it, and neither is
/// taste:
///
///   * Rule 9 becomes *testable*. The suite provokes frame exhaustion by
///     replacing global `operator new`; that reaches the V8 backend's overflow
///     vector and would never reach this one, so the case could only report a
///     skip. Now the same injection reaches both engines and the case asserts
///     on both. Nothing new is exposed for a test to call: the lever the suite
///     already has simply works here.
///   * "Frames give back everything they took" starts measuring something.
///     That test counts outstanding `operator new`s, and a spill through
///     `js_malloc` is invisible to it.
///
/// It changes nothing about rooting. The frame is still a `JS::Rooted` living
/// in the caller's `HandleScope`, still linked onto the context's rooted list,
/// still strictly LIFO; only the address of the buffer it traces moves. The
/// first `UNIBIND_FRAME_INLINE_SLOTS` handles do not touch any allocator at all.
///
/// It goes through `frame_alloc.h` rather than writing `::operator new` here,
/// and that indirection is load-bearing: this header has seen `jsapi.h`, which
/// pulls in `mozilla/cxxalloc.h`, which defines `operator new` as an
/// always-inline forward to `moz_xmalloc`. A `::operator new` written in this
/// file would not be the program's replaceable one.
class FrameAllocPolicy : public js::AllocPolicyBase {
   public:
    template <class T>
    T* maybe_pod_malloc(std::size_t count) {
        if (count > (static_cast<std::size_t>(-1) / sizeof(T))) {
            return nullptr;
        }
        return static_cast<T*>(FrameAllocate(count * sizeof(T)));
    }

    template <class T>
    T* maybe_pod_calloc(std::size_t count) {
        T* memory = maybe_pod_malloc<T>(count);
        if (memory != nullptr) {
            std::memset(static_cast<void*>(memory), 0, count * sizeof(T));
        }
        return memory;
    }

    template <class T>
    T* maybe_pod_realloc(T* old, std::size_t oldCount, std::size_t newCount) {
        T* memory = maybe_pod_malloc<T>(newCount);
        if (memory == nullptr) {
            return nullptr;
        }
        if (old != nullptr) {
            std::memcpy(static_cast<void*>(memory), static_cast<const void*>(old),
                        (oldCount < newCount ? oldCount : newCount) * sizeof(T));
            FrameRelease(static_cast<void*>(old));
        }
        return memory;
    }

    template <class T>
    T* pod_malloc(std::size_t count) {
        return maybe_pod_malloc<T>(count);
    }
    template <class T>
    T* pod_calloc(std::size_t count) {
        return maybe_pod_calloc<T>(count);
    }
    template <class T>
    T* pod_realloc(T* old, std::size_t oldCount, std::size_t newCount) {
        return maybe_pod_realloc<T>(old, oldCount, newCount);
    }

    template <class T>
    void free_(T* memory, std::size_t /*count*/ = 0) {
        FrameRelease(static_cast<void*>(memory));
    }
};

/// The frame's rooted slot array. `StackGCVector` fixes its inline capacity at
/// eight, which is where `UNIBIND_FRAME_INLINE_SLOTS` came from; the assert below
/// keeps the two from drifting apart.
using FrameSlots = JS::StackGCVector<JS::Value, FrameAllocPolicy>;
static_assert(FrameSlots::InlineLength == UNIBIND_FRAME_INLINE_SLOTS,
              "UNIBIND_FRAME_INLINE_SLOTS must match StackGCVector's inline capacity");

/// A handle frame.
///
/// Slots `[0, argCount)` are borrowed from the `JS::CallArgs` of the call this
/// frame belongs to - the interpreter has already rooted those on the VM
/// stack, so reading one copies nothing and the frame roots nothing extra.
/// Slots above that live in `slots`, which is the stack root.
struct Frame {
    Frame(Isolate& owner, Frame* parent, const JS::CallArgs* args)
        : slots(owner.impl().cx),
          owner(&owner),
          parent(parent),
          epoch(owner.impl().nextEpoch++),
          args(args),
          argCount(args == nullptr ? 0U : static_cast<std::uint32_t>(args->length())) {}

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&) = delete;
    Frame& operator=(Frame&&) = delete;
    /// A closed frame's epoch is one no handle of it carries, so a handle that
    /// outlived it is diagnosed whether or not a later frame reused the storage.
    /// An optimised build usually does reuse it and an unoptimised one usually
    /// does not, and without this the check caught only the first. Volatile,
    /// because a store in a destructor to an object that is about to end is
    /// exactly the store an optimiser may drop.
    ~Frame() {
#if UNIBIND_HANDLE_CHECKS
        *static_cast<volatile std::uint32_t*>(&epoch) = ~epoch;
#endif
    }

    /// `NO_SLOT` when the frame could not grow. The caller turns that into an
    /// *empty* handle - never into a slot that reads as a value, which would
    /// make running out of memory indistinguishable from a property that was
    /// not there. See rule 9 in docs/lifetimes.md.
    static constexpr SlotIndex NO_SLOT = ~SlotIndex{0};

    [[nodiscard]] SlotIndex Push(const JS::Value& value) noexcept {
        if (!slots.append(value)) {
            return NO_SLOT;
        }
        return argCount + static_cast<SlotIndex>(slots.length() - 1);
    }

    // Both returns below hand back a `JS::Value` the engine already holds - a
    // rooted argument, or one of this frame's own slots. The check reads the
    // copy as a conversion and wants `return {handle};`, which names nothing
    // the reader does not already see.
    // NOLINTBEGIN(modernize-return-braced-init-list)
    [[nodiscard]] JS::Value At(SlotIndex index) const {
        if (index == NO_SLOT) {
            return JS::UndefinedValue();
        }
        if (index < argCount) {
            return args->get(static_cast<unsigned>(index));
        }
        const SlotIndex own = index - argCount;
        assert(own < slots.length() && "slot index out of range for its frame");
        return slots[own];
    }
    // NOLINTEND(modernize-return-braced-init-list)

    JS::Rooted<FrameSlots> slots;
    Isolate* owner;
    Frame* parent;
    std::uint32_t epoch;
    const JS::CallArgs* args;
    std::uint32_t argCount;
};

static_assert(sizeof(Frame) <= UNIBIND_FRAME_STORAGE_SIZE,
              "UNIBIND_FRAME_STORAGE_SIZE in CMakeLists.txt is too small for the SpiderMonkey frame");
static_assert(alignof(Frame) <= UNIBIND_FRAME_STORAGE_ALIGN, "UNIBIND_FRAME_STORAGE_ALIGN is too small");

struct ContextRec {
    Isolate* owner = nullptr;
    JS::PersistentRootedObject global;
    int refs = 1;
};

/// Compiled source.
///
/// The stencil is the unit here, and the `JSScript` is what one realm made of
/// it. A stencil is the parser's output before anything reaches the GC heap: it
/// belongs to no realm, it is reference-counted rather than rooted, and it is
/// the only thing on this engine that can be turned into bytes and back. So it
/// is what the code cache encodes, and encoding it is free of a re-parse
/// because the rec is already holding it.
///
/// `script` is instantiated in the realm that compiled it and runs only there:
/// a `JSScript` belongs to one realm, and executing it with another one
/// entered is a realm mismatch - a debug engine asserts, a release one runs it
/// on borrowed invariants. `global` is that realm, so that a run elsewhere
/// can tell and instantiate the stencil into the realm it is run in instead.
struct ScriptRec {
    Isolate* owner = nullptr;
    RefPtr<JS::Stencil> stencil;
    JS::PersistentRooted<JSScript*> script;
    JS::PersistentRootedObject global;

    /// Whether the blob handed to `CompileScriptWithCache` was actually used.
    /// False for a compile with no blob, and false for a blob the engine
    /// rejected - to a caller those are the same thing, which is that this
    /// compile paid full price.
    bool usedCache = false;
};

struct GlobalNode {
    Isolate* owner = nullptr;
    JS::PersistentRooted<JS::Value> value;
};

/// SpiderMonkey has no `v8::TryCatch`: an exception is simply pending on the
/// context until something takes it. So this is a stack of handlers the
/// backend keeps itself, and "catching" is taking the pending exception off
/// the context the first time anyone asks.
struct TryCatchState {
    Isolate* owner = nullptr;
    TryCatchState* prev = nullptr;
    bool caught = false;
    bool rethrow = false;
    /// What stopped the code was `Isolate::TerminateExecution`, not a throw.
    /// There is no exception value, no message and no stack, and closing this
    /// handler does not consume it - see `TryCatchClose`.
    bool terminated = false;
    JS::PersistentRooted<JS::Value> exception;
    JS::PersistentRootedObject stack;
    /// An exception that was already pending when this handler opened. It is
    /// not ours to catch, so it is parked here and put back on close.
    bool hadOuter = false;
    JS::PersistentRooted<JS::Value> outer;
};

static_assert(sizeof(TryCatchState) <= UNIBIND_TRY_CATCH_STORAGE_SIZE, "UNIBIND_TRY_CATCH_STORAGE_SIZE is too small");
static_assert(alignof(TryCatchState) <= UNIBIND_TRY_CATCH_STORAGE_ALIGN,
              "UNIBIND_TRY_CATCH_STORAGE_ALIGN is too small");

/// `JSAutoRealm` is stack-only and LIFO, which is exactly what `ContextScope`
/// promises, so the mapping is one-to-one.
struct ContextScopeState {
    alignas(alignof(JSAutoRealm)) unsigned char realm[sizeof(JSAutoRealm)];

    [[nodiscard]] JSAutoRealm& Realm() noexcept { return *reinterpret_cast<JSAutoRealm*>(realm); }
};

static_assert(sizeof(ContextScopeState) <= UNIBIND_CONTEXT_SCOPE_STORAGE_SIZE,
              "UNIBIND_CONTEXT_SCOPE_STORAGE_SIZE is too small");

/// One call into native code, whatever shape it was called in.
///
/// SpiderMonkey needs no type erasure here, which is the one place its model
/// is kinder than V8's: every hook writes its result into a
/// `JS::MutableHandleValue`, so the return sink is one pointer and not one
/// function pointer per hook shape.
struct CallbackState {
    Isolate* owner = nullptr;
    Frame* frame = nullptr;
    Context context;
    const JS::CallArgs* call = nullptr;
    /// Where a result goes. Never null, and always an address the engine has
    /// already rooted - the call's `rval()` slot, or a local for a hook whose
    /// result the language does not read.
    JS::Value* result = nullptr;
    /// The receiver and the holder, as slots of `frame` rather than as copied
    /// values: the collector moves objects, so a `JS::Value` sitting in this
    /// struct would be stale the moment the callback allocated anything. The
    /// frame is a root; this struct is not.
    SlotIndex thisSlot = Frame::NO_SLOT;
    SlotIndex holderSlot = Frame::NO_SLOT;
    CallbackData data;
    bool isConstruct = false;
    /// The script value a `Function::New` with one carries, as a slot of
    /// `frame` for the same reason as the two above. `hasValue` is false for
    /// every other callback, whose data value is `undefined`; true with
    /// `NO_SLOT` is a frame that could not grow, which is an empty handle.
    SlotIndex valueSlot = Frame::NO_SLOT;
    bool hasValue = false;
};

// --- slot plumbing, shared by both translation units ------------------------

[[nodiscard]] inline Slot MakeSlot(Frame& frame, SlotIndex index) noexcept {
#if UNIBIND_HANDLE_CHECKS
    return Slot{.frame = &frame, .index = index, .epoch = frame.epoch};
#else
    return Slot{.frame = &frame, .index = index};
#endif
}

/// A slot a trampoline reserved when a call began, or an empty handle if the
/// frame could not grow then. Never a slot that reads as a value.
[[nodiscard]] inline Slot SlotOrEmpty(Frame& frame, SlotIndex index) noexcept {
    if (index == Frame::NO_SLOT) {
        return Slot{};
    }
    return MakeSlot(frame, index);
}

[[nodiscard]] inline JS::Value Resolve(Slot slot) noexcept {
    assert(slot.frame != nullptr && "handle used while empty");
#if UNIBIND_HANDLE_CHECKS
    assert(slot.epoch == slot.frame->epoch && "handle used after its HandleScope closed");
#endif
    return slot.frame->At(slot.index);
}

[[nodiscard]] inline Isolate& IsolateFor(Slot slot) noexcept {
    return *slot.frame->owner;
}

/// The object a value names, seen through any cross-compartment wrapper.
/// A question about what an object *is* - its class, its reserved slots - has
/// to be asked of the object, not of whatever stands in for it in the realm
/// that happens to be current.
[[nodiscard]] inline JSObject* Unwrapped(const JS::Value& value) noexcept {
    return value.isObject() ? js::UncheckedUnwrap(&value.toObject()) : nullptr;
}

[[nodiscard]] inline JSContext* Raw(Isolate& isolate) noexcept {
    return isolate.impl().cx;
}

/// An empty handle, plus the condition reported the way this engine reports
/// running out of memory. Nothing is allocated saying so, which matters,
/// because the reason we are here is that an allocation failed.
[[nodiscard]] inline Slot NoSlot(Isolate& isolate) noexcept {
    JS_ReportOutOfMemory(Raw(isolate));
    return Slot{};
}

[[nodiscard]] inline Slot Push(Isolate& isolate, const JS::Value& value) noexcept {
    Frame* frame = isolate.impl().current;
    assert(frame != nullptr && "a value was created with no HandleScope open");
    const SlotIndex index = frame->Push(value);
    if (index == Frame::NO_SLOT) {
        return NoSlot(isolate);
    }
    return MakeSlot(*frame, index);
}

[[nodiscard]] inline Slot Push(Isolate& isolate, JSObject* object) noexcept {
    return Push(isolate, JS::ObjectValue(*object));
}

/// The `Maybe<Slot>` form: a frame that could not grow is an empty answer,
/// exactly as a call that threw is.
[[nodiscard]] inline Maybe<Slot> PushOrNothing(Isolate& isolate, const JS::Value& value) noexcept {
    const Slot slot = Push(isolate, value);
    if (slot.IsEmpty()) {
        return std::nullopt;
    }
    return slot;
}

[[nodiscard]] inline Maybe<Slot> PushOrNothing(Isolate& isolate, JSObject* object) noexcept {
    return PushOrNothing(isolate, JS::ObjectValue(*object));
}

[[nodiscard]] inline Isolate& OwnerOf(const Context& context) noexcept {
    return *context.rec()->owner;
}

[[nodiscard]] inline JSContext* Raw(const Context& context) noexcept {
    return Raw(OwnerOf(context));
}

[[nodiscard]] inline JSObject* GlobalOf(const Context& context) noexcept {
    return context.rec()->global;
}

/// Resolve a handle into the realm that is current *now*.
///
/// A value belongs to an isolate, not to a realm (docs/lifetimes.md rule 7),
/// so any handle may be used with any realm of its isolate. V8 needs nothing
/// for that; SpiderMonkey compartmentalises, so a value from another
/// compartment has to be wrapped before it can be operated on here.
/// `JS_WrapValue` is a no-op when the value is already in this compartment, so
/// the same-realm path - which is every path in practice - pays a branch.
///
/// The wrapper is a distinct object, which is why identity as seen from a
/// foreign realm is explicitly not guaranteed.
[[nodiscard]] inline bool ResolveHere(JSContext* cx, Slot slot, JS::MutableHandleValue out) {
    out.set(Resolve(slot));
    return JS_WrapValue(cx, out);
}

/// The same, for a handle the caller has already established is an object.
/// Null if it is not one, or if it could not be wrapped.
[[nodiscard]] inline JSObject* ResolveObjectHere(JSContext* cx, Slot slot, JS::MutableHandleValue scratch) {
    if (!ResolveHere(cx, slot, scratch) || !scratch.isObject()) {
        return nullptr;
    }
    return &scratch.toObject();
}

/// How every global an isolate makes is created: a realm in a compartment of
/// its own, and every one of them in the one zone.
///
/// The compartment is what keeps realms apart, and it stays per realm. The
/// zone is what strings and atoms belong to, and a handle belongs to the
/// isolate rather than to a realm, so a string made in one realm is read, keyed
/// on and passed from any other. Across zones that is not allowed: a string is
/// a zone's cell, and an atom is only kept alive in a zone that has marked it.
/// A debug engine asserts on the first read ("atom is marked white for zone",
/// a zone mismatch); a release one reads it, and its collector may later free
/// an atom a realm of another zone still names. One zone per isolate is what
/// makes "a value belongs to the isolate" true of strings without wrapping
/// each one at every use.
[[nodiscard]] JS::RealmOptions IsolateRealmOptions() noexcept;

/// The isolate's own realm, made the first time something needs one. Null if
/// it could not be made, which is the only failure this has.
[[nodiscard]] JSObject* UtilityGlobal(Isolate& isolate) noexcept;

/// A realm for an operation that was given an `Isolate&` and nothing else.
///
/// On V8 those need no realm at all; here every allocation does, and an
/// embedder is entitled to call `String::New`, `External::New`, `Throw` or
/// `CaptureStackFrames` with no `ContextScope` open - `Isolate::PostJob` says
/// outright that a job runs with no realm current. So: if a realm is current,
/// use it, because a value made in the caller's realm is what the caller
/// expects; otherwise enter the isolate's own.
///
/// `Usable()` is false only when there was no realm and one could not be made,
/// which is an out-of-memory the caller reports as an empty answer.
class IsolateRealm {
   public:
    explicit IsolateRealm(Isolate& isolate) noexcept {
        JSContext* cx = isolate.impl().cx;
        if (JS::CurrentGlobalOrNull(cx) != nullptr) {
            usable_ = true;
            return;
        }
        JSObject* global = UtilityGlobal(isolate);
        if (global == nullptr) {
            return;
        }
        entered_.emplace(cx, global);
        usable_ = true;
    }

    IsolateRealm(const IsolateRealm&) = delete;
    IsolateRealm& operator=(const IsolateRealm&) = delete;
    IsolateRealm(IsolateRealm&&) = delete;
    IsolateRealm& operator=(IsolateRealm&&) = delete;
    ~IsolateRealm() = default;

    [[nodiscard]] bool Usable() const noexcept { return usable_; }

   private:
    mozilla::Maybe<JSAutoRealm> entered_;
    bool usable_ = false;
};

/// Every operation that takes a `Context` runs in that realm, which is what
/// makes the context parameter mean the same thing it means on V8.
class RealmGuard {
   public:
    explicit RealmGuard(const Context& context) noexcept : realm_(Raw(context), GlobalOf(context)) {}

    RealmGuard(const RealmGuard&) = delete;
    RealmGuard& operator=(const RealmGuard&) = delete;
    RealmGuard(RealmGuard&&) = delete;
    RealmGuard& operator=(RealmGuard&&) = delete;
    ~RealmGuard() = default;

   private:
    JSAutoRealm realm_;
};

/// Whether a termination is in force, asked at every gate into the engine.
///
/// **Read the comment on `Isolate::Impl::terminating` before adding a call
/// into the engine that does not go through this.** On V8 a terminating
/// isolate refuses at the engine's own API entry points; on SpiderMonkey
/// nothing refuses anything - the unwind ends and the `JSContext` is
/// immediately live again - so this check *is* the enforcement.
/// `unibind/isolate.h` promises that everything which would run script keeps
/// failing until `CancelTerminateExecution`, and it is kept here or not at
/// all: a new entry point that forgets to ask will run script the embedder
/// told the isolate to stop, on this backend only, and no test written
/// against the other backend will catch it.
[[nodiscard]] inline bool Terminating(const Isolate& isolate) noexcept {
    return isolate.impl().terminating.load(std::memory_order_acquire);
}
[[nodiscard]] bool Terminating(const Context& context) noexcept;

/// Wrap what a native wrote into `args.rval()` back into the realm the call is
/// returning to, and say whether the call may still be reported as a success.
///
/// Every trampoline in this backend ends with this. A callback is entitled to
/// enter another realm of the same isolate and answer with something it found
/// there - `unibind/context.h` promises a value may be used with any realm of its
/// isolate, and a sandbox object is that promise being used. But the value it
/// wrote is then a stranger to the realm the caller is in, and returning it
/// unwrapped puts a cross-compartment object where the engine expects a local
/// one. `JS_WrapValue` is a no-op whenever there is nothing to do, which is
/// almost always.
[[nodiscard]] inline bool FinishNativeCall(JSContext* cx, JS::CallArgs& args) noexcept {
    if (JS_IsExceptionPending(cx)) {
        // What it threw needs wrapping for exactly the same reason its result
        // would have: a callback that entered another realm and let something
        // throw there leaves an exception belonging to that compartment, and
        // the handler about to catch it is in this one. Left unwrapped it is
        // not catchable from where the call was made. The saved stack goes back
        // with it, so a `TryCatch` still has frames to report.
        JS::ExceptionStack caught(cx);
        if (JS::StealPendingExceptionStack(cx, &caught)) {
            JS::RootedValue value(cx, caught.exception());
            JS::RootedObject stack(cx, caught.stack());
            if (JS_WrapValue(cx, &value)) {
                JS::ExceptionStack rewrapped(cx, value, stack);
                JS::SetPendingExceptionStack(cx, rewrapped);
            } else {
                // Not the unwrapped value: putting a cross-compartment object
                // back as the pending exception hands the catching realm
                // something it cannot name, which is the very thing the wrap
                // above exists to prevent. `JS_WrapValue` fails by running out
                // of memory, so that is what is reported instead.
                JS_ReportOutOfMemory(cx);
            }
        } else if (!JS_IsExceptionPending(cx)) {
            // The steal failed and took the exception with it. Something has
            // to be pending: returning false with *nothing* pending is this
            // engine's uncatchable idiom - the one `TerminateExecution` uses -
            // so leaving it that way would turn an ordinary throw into an
            // unwind no `try`/`catch` can see and `HasTerminated()` cannot
            // explain.
            JS_ReportOutOfMemory(cx);
        }
        return false;
    }
    JS::RootedValue result(cx, args.rval());
    if (!JS_WrapValue(cx, &result)) {
        return false;
    }
    args.rval().set(result);
    return true;
}

[[nodiscard]] JSString* MakeRawString(JSContext* cx, std::string_view utf8) noexcept;
/// A string of text the embedder declared - a property or function name, a
/// template's string constant, an error message - decoded as
/// `String::NewFromUtf8` decodes: bytes that are not UTF-8 become U+FFFD.
/// `MakeRawString` is the strict form, for `String::New`.
[[nodiscard]] JSString* MakeTextString(JSContext* cx, std::string_view utf8);
/// The property key for a declared name, decoded as `MakeTextString` decodes.
/// The engine's `const char*` name overloads read their argument as Latin-1,
/// which turns every non-ASCII UTF-8 name into different text, and stop at a
/// NUL; nothing a template or a class declares goes through them.
[[nodiscard]] bool NameToId(JSContext* cx, std::string_view name, JS::MutableHandleId out);
/// A native function with reserved slots, named by a declared name - see
/// `NameToId` for why not `js::NewFunctionWithReserved`.
[[nodiscard]] JSFunction* NewNamedFunction(JSContext* cx, JSNative native, unsigned nargs, unsigned flags,
                                           std::string_view name);
[[nodiscard]] bool ToPropertyKey(JSContext* cx, Slot key, JS::MutableHandleId out) noexcept;
[[nodiscard]] unsigned ToNativeAttributes(PropertyAttribute attributes) noexcept;
[[nodiscard]] JSExnType ToExnType(ErrorKind kind) noexcept;
[[nodiscard]] bool MakeErrorValue(JSContext* cx, ErrorKind kind, std::string_view message,
                                  JS::MutableHandleValue out) noexcept;

/// Records the callback pair for the isolate and hands back a stable address.
[[nodiscard]] CallbackRecord* StoreCallback(Isolate& isolate, CallbackRecord record);

/// Define `name` on `target` as an accessor property whose getter and setter -
/// whichever `record` has - are native functions carrying `record`. What a
/// template's accessor declaration becomes on each instance, and what
/// `Object::SetAccessor` makes on one object.
///
/// With `result` null a refusal - a frozen target, say - throws a TypeError, as
/// it should for an instance being built. With one, a refusal is written there
/// and nothing is thrown, which is `Object::SetAccessor`'s answer.
[[nodiscard]] bool DefineAccessor(JSContext* cx, JS::HandleObject target, const std::string& name,
                                  CallbackRecord* record, PropertyAttribute attributes,
                                  JS::ObjectOpResult* result = nullptr);

/// A call's receiver as a callback sees it: converted the way a sloppy-mode
/// function's is, and the way V8 converts one for every native it calls -
/// undefined and null become the current realm's global object, and a
/// primitive becomes its wrapper. `This()` is a `Local<Object>`, so handing a
/// callback the primitive itself would be a number where an object is
/// promised. False, with an exception pending, only if the wrapper could not be
/// made.
[[nodiscard]] bool ReceiverObject(JSContext* cx, JS::MutableHandleValue receiver);

/// The trampoline every native function declared through this API goes
/// through. Recovers its `CallbackRecord` from the callee's reserved slot.
bool FunctionTrampoline(JSContext* cx, unsigned argc, JS::Value* vp);

/// A function object carrying `record`, for installing on an object or a
/// template.
[[nodiscard]] JSObject* NewNativeFunction(JSContext* cx, CallbackRecord* record, std::string_view name);

/// The realm's `ContextRec`, recovered from the global object. Null if the
/// global was not made by this API.
[[nodiscard]] ContextRec* RecOfGlobal(JSObject* global) noexcept;

/// Reserved slots of a global object: its `ContextRec*`, and the object that
/// caches this realm's materialised templates (see templates.cpp).
inline constexpr std::uint32_t GLOBAL_REC_SLOT = JSCLASS_GLOBAL_SLOT_COUNT;
inline constexpr std::uint32_t GLOBAL_TEMPLATE_CACHE_SLOT = JSCLASS_GLOBAL_SLOT_COUNT + 1;

/// Opens a frame for the duration of a native call, borrowing the engine's
/// already-rooted argument array.
class CallFrame {
   public:
    CallFrame(Isolate& isolate, const JS::CallArgs* args) : isolate_(&isolate) {
        frame_ = ::new (static_cast<void*>(storage_.bytes)) Frame(isolate, isolate.impl().current, args);
        isolate.impl().current = frame_;
    }
    ~CallFrame() {
        isolate_->impl().current = frame_->parent;
        frame_->~Frame();
    }

    CallFrame(const CallFrame&) = delete;
    CallFrame& operator=(const CallFrame&) = delete;
    CallFrame(CallFrame&&) = delete;
    CallFrame& operator=(CallFrame&&) = delete;

    [[nodiscard]] Frame& frame() const noexcept { return *frame_; }

   private:
    Isolate* isolate_;
    Frame* frame_ = nullptr;
    FrameStorage storage_{};
};

/// The `Context` for the realm a native call arrived in.
[[nodiscard]] Context CurrentContext(JSContext* cx) noexcept;

}  // namespace detail
}  // namespace ub
