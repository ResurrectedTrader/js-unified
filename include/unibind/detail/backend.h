#pragma once
/// \file
/// The backend interface, part one: everything expressible in terms of slots.
///
/// Every function here is DECLARED by the public headers and DEFINED by
/// exactly one backend library. That is what keeps engine headers out of the
/// public API - and it is why a release build wants LTO, which inlines these
/// definitions back into the caller (docs/lifetimes.md section 10).
///
/// Adding a backend means defining every one of these and nothing else. No
/// public header changes, no virtual interface, no dispatch table.
///
/// Conventions:
///   * Functions that allocate a value allocate it in the isolate's *current*
///     frame, so calling one without an open HandleScope is a programming
///     error (checked builds assert).
///   * An empty `std::optional<T>` means "no value". Why is a second question,
///     asked of the isolate - see "The failure convention" at the top of
///     unibind/types.h.
///   * A `Slot` parameter of the wrong kind is a programming error, not an
///     exception - the public wrappers have already narrowed the type.
///   * **A slot-allocating function that cannot grow the frame yields an
///     EMPTY `Slot`** - `Slot{}`, whose `frame` is null - and additionally
///     reports the condition the way the engine reports running out of memory,
///     as a pending exception where it can. One that returns `std::optional<Slot>`
///     yields an empty optional for the same reason.
///
///     It must NEVER hand back a slot that reads as `undefined`. Growing a
///     frame is fallible on at least one engine (SpiderMonkey's
///     `RootedVector::append` reports OOM), and a sentinel that reads as a
///     value turns running out of memory into a plausible wrong answer - the
///     one failure mode this API exists to prevent. An empty handle is not a
///     value, reading one is diagnosed, and nothing mistakes it for success.
///     See `Local<T>::IsEmpty` and docs/lifetimes.md rule 9.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "unibind/detail/slot.h"
#include "unibind/fwd.h"
#include "unibind/types.h"

namespace ub::detail {

// --- frames ---------------------------------------------------------------

/// Construct a frame in `storage` and make it the isolate's current frame.
/// `escapable` says whether this frame will hand a handle to its parent. It is
/// a construction-time flag because an engine may have to reserve the parent
/// slot up front - V8 does.
Frame& OpenFrame(Isolate& isolate, FrameStorage& storage, bool escapable) noexcept;
/// Destroy the frame, releasing every slot it owns, and restore its parent.
/// Frames on one isolate must close in reverse order of opening.
void CloseFrame(Frame& frame) noexcept;
/// Copy a slot into the *parent* of `closing` and return a handle to the copy.
/// Both frames are live roots throughout, so the value is never unrooted.
Slot EscapeSlot(Frame& closing, Slot value) noexcept;
Isolate& IsolateOf(Frame& frame) noexcept;
/// The innermost open frame, or null if no HandleScope is open.
Frame* CurrentFrame(Isolate& isolate) noexcept;

// --- inspection -----------------------------------------------------------

ValueKind KindOf(Slot value) noexcept;
bool IsType(Slot value, TypeCode type) noexcept;
bool StrictEquals(Slot lhs, Slot rhs) noexcept;
std::optional<bool> LooseEquals(const Context& context, Slot lhs, Slot rhs);
bool SameValue(Slot lhs, Slot rhs) noexcept;

// --- reading primitives ---------------------------------------------------

bool BooleanValue(Slot value) noexcept;
double NumberValue(Slot value) noexcept;
std::int32_t Int32Value(Slot value) noexcept;
/// Bytes a UTF-8 encoding of this string needs, excluding any terminator.
std::size_t Utf8Length(Slot string) noexcept;
/// Encodes into `out`, truncating at a code point boundary if it does not fit.
/// Returns bytes written.
std::size_t WriteUtf8(Slot string, std::span<char> out) noexcept;
std::string ToStdString(Slot string);
/// Description of a symbol, or an empty optional for an anonymous one.
std::optional<std::string> SymbolDescription(Slot symbol);

// --- conversion (may run user code, may throw) -----------------------------

std::optional<bool> ToBoolean(const Context& context, Slot value);
std::optional<double> ToNumber(const Context& context, Slot value);
std::optional<std::int32_t> ToInt32(const Context& context, Slot value);
std::optional<std::uint32_t> ToUint32(const Context& context, Slot value);
std::optional<Slot> ToJsString(const Context& context, Slot value);
std::optional<Slot> ToJsObject(const Context& context, Slot value);

// --- making values --------------------------------------------------------

Slot MakeUndefined(Isolate& isolate) noexcept;
Slot MakeNull(Isolate& isolate) noexcept;
Slot MakeBoolean(Isolate& isolate, bool value) noexcept;
Slot MakeNumber(Isolate& isolate, double value) noexcept;
Slot MakeInteger(Isolate& isolate, std::int32_t value) noexcept;
Slot MakeUnsigned(Isolate& isolate, std::uint32_t value) noexcept;
std::optional<Slot> MakeString(Isolate& isolate, std::string_view utf8);
std::optional<Slot> MakeSymbol(Isolate& isolate, std::optional<std::string_view> description);
/// `Symbol.for(key)` - the cross-realm registry.
std::optional<Slot> MakeSymbolFor(Isolate& isolate, std::string_view key);
std::optional<Slot> GetWellKnownSymbol(Isolate& isolate, WellKnownSymbol which);
std::optional<Slot> MakeObject(const Context& context);
std::optional<Slot> MakeArray(const Context& context, std::uint32_t length);
std::optional<Slot> MakeError(const Context& context, ErrorKind kind, std::string_view message);

// --- functions and externals ----------------------------------------------

std::optional<Slot> MakeFunction(const Context& context, FunctionCallback callback, CallbackData data);
std::optional<Slot> MakeExternal(Isolate& isolate, CallbackData data);
CallbackData ExternalData(Slot external) noexcept;
// --- objects --------------------------------------------------------------

std::optional<Slot> GetProperty(const Context& context, Slot object, Slot key);
std::optional<Slot> GetIndex(const Context& context, Slot object, std::uint32_t index);
std::optional<bool> SetProperty(const Context& context, Slot object, Slot key, Slot value);
std::optional<bool> SetIndex(const Context& context, Slot object, std::uint32_t index, Slot value);
/// Installs a data property with explicit attributes, bypassing setters.
std::optional<bool> DefineProperty(const Context& context, Slot object, Slot key, Slot value,
                                   PropertyAttribute attributes);
/// Installs an accessor property with native getter and setter functions.
std::optional<bool> SetAccessorProperty(const Context& context, Slot object, std::string_view name,
                                        AccessorGetterCallback getter, AccessorSetterCallback setter, CallbackData data,
                                        PropertyAttribute attributes);
std::optional<bool> HasProperty(const Context& context, Slot object, Slot key);
std::optional<bool> HasOwnProperty(const Context& context, Slot object, Slot key);
std::optional<bool> DeleteProperty(const Context& context, Slot object, Slot key);
std::optional<PropertyAttribute> GetPropertyAttributes(const Context& context, Slot object, Slot key);
/// Own keys as a JS array, honouring `filter`.
std::optional<Slot> GetOwnPropertyNames(const Context& context, Slot object, KeyFilter filter);
std::optional<Slot> GetPrototype(const Context& context, Slot object);
std::optional<bool> SetPrototype(const Context& context, Slot object, Slot prototype);

std::uint32_t ArrayLength(Slot array) noexcept;

// --- binary data ----------------------------------------------------------
//
// Bytes are copied in both directions; see the argument in unibind/value.h. A
// backend must never hand out a pointer into engine storage, because both
// collectors move it.

/// A buffer of `byteLength` bytes, holding a copy of `bytes` and zeroes after
/// it. `bytes` may be empty, and may not be longer than `byteLength`.
std::optional<Slot> MakeArrayBuffer(const Context& context, std::span<const std::byte> bytes, std::size_t byteLength);
std::size_t ArrayBufferByteLength(Slot buffer) noexcept;
/// Copies min(byteLength, out.size()) bytes; returns how many.
std::size_t ArrayBufferCopyOut(Slot buffer, std::span<std::byte> out) noexcept;

/// `length` is in elements. Empty if the view would not fit in the buffer.
std::optional<Slot> MakeTypedArray(const Context& context, ElementType type, Slot buffer, std::size_t byteOffset,
                                   std::size_t length);
ElementType TypedArrayElementType(Slot view) noexcept;
std::size_t TypedArrayLength(Slot view) noexcept;
std::size_t TypedArrayByteOffset(Slot view) noexcept;
std::optional<Slot> TypedArrayBuffer(const Context& context, Slot view);
/// Copies the bytes this view covers; returns how many.
std::size_t TypedArrayCopyOut(Slot view, std::span<std::byte> out) noexcept;

// --- promises -------------------------------------------------------------
//
// There is no resolver type: SpiderMonkey settles the promise object itself,
// and V8's `Promise::Resolver` is castable from the promise, so the shape both
// can keep is "hold the promise, settle the promise". `false` from a settle is
// "it was already settled", which is the language's behaviour and not a
// failure; an empty optional is a throw.

std::optional<Slot> MakePromise(const Context& context);
std::optional<bool> ResolvePromise(const Context& context, Slot promise, Slot value);
std::optional<bool> RejectPromise(const Context& context, Slot promise, Slot reason);
PromiseState PromiseStateOf(Slot promise) noexcept;

// --- structured clone -----------------------------------------------------

/// Empty if the value cannot be cloned. The bytes are the engine's own format
/// and only it can read them back.
std::optional<std::vector<std::uint8_t>> SerializeValue(const Context& context, Slot value);
/// Empty if the blob is not one this engine wrote.
std::optional<Slot> DeserializeValue(const Context& context, std::span<const std::uint8_t> blob);

// --- calling --------------------------------------------------------------

std::optional<Slot> CallFunction(const Context& context, Slot function, Slot receiver, std::span<const Slot> arguments);
std::optional<Slot> ConstructObject(const Context& context, Slot function, std::span<const Slot> arguments);

// --- native state ---------------------------------------------------------

/// Base of every embedder object handed to the engine. Type-erased without
/// `void*` and without RTTI: `type` says what the real type is, `destroy` is
/// how it goes away. See unibind/class.h.
///
/// Two obligations on a backend, and the second is not a tidiness choice:
///
///   * **Call `destroy` exactly once on every box**, and on all of them by the
///     time the isolate is gone - keep a list of the live ones and finish them
///     at teardown if the engine does not promise a finalizer first. It gives
///     back one *share* of a native, which is why a native the embedder still
///     holds correctly survives the isolate; see unibind/class.h.
///   * **Call it on the isolate's own thread.** Never a background collector or
///     helper thread. `destroy` drops a `std::shared_ptr` whose other holders
///     are the embedder's, so running it off-thread races them. An engine that
///     offers background finalization will accept the box happily and the
///     resulting failure does not reproduce - so where the choice exists
///     (SpiderMonkey's `JSCLASS_FOREGROUND_FINALIZE`), take the foreground one
///     and leave a comment saying it is required, because nothing else in the
///     code will say so.
struct NativeBox {
    TypeId type;
    void (*destroy)(NativeBox* box) noexcept = nullptr;
};

/// The native attached to this object, or null if it carries none. Does not
/// check the type; the typed wrapper does that against `NativeBox::type`.
NativeBox* GetNativeBox(Slot object) noexcept;

// --- globals (roots that outlive every frame) ------------------------------

GlobalNode* MakeGlobal(Isolate& isolate, Slot value);
/// A second root over the same value. Takes no frame and must not need one: a
/// caller duplicating a root it has held since before the current scope opened
/// may have nothing open at all. Root the value internally if the engine makes
/// you go through a handle to copy it.
GlobalNode* DuplicateGlobal(GlobalNode* node);
void ReleaseGlobal(GlobalNode* node) noexcept;
/// Materialise into the isolate's current frame.
Slot GlobalToSlot(Isolate& isolate, GlobalNode* node) noexcept;

/// Do two roots hold the same value? Two roots over one object are the normal
/// case, not the exceptional one, so this compares the *values*, never the
/// nodes.
///
/// These take no frame and must not need one: the backend roots whatever it
/// needs for the duration of the comparison, so that a caller with no
/// `HandleScope` open gets an answer rather than a failure that would read as
/// "not equal". A null node compares equal to nothing, including another null
/// one, and so does a node belonging to a different isolate.
bool GlobalStrictEquals(const GlobalNode* lhs, const GlobalNode* rhs) noexcept;
bool GlobalSameValue(const GlobalNode* lhs, const GlobalNode* rhs) noexcept;
bool GlobalStrictEqualsSlot(const GlobalNode* lhs, Slot rhs) noexcept;
bool GlobalSameValueSlot(const GlobalNode* lhs, Slot rhs) noexcept;

// --- exceptions -----------------------------------------------------------

void ThrowValue(Isolate& isolate, Slot value);
void ThrowError(Isolate& isolate, ErrorKind kind, std::string_view message);
bool HasPendingException(Isolate& isolate) noexcept;

/// The frames below the code running now, innermost first. Needs no open
/// frame: it hands back plain strings and numbers.
std::vector<StackFrame> CaptureStack(Isolate& isolate, std::uint32_t limit);

void TryCatchOpen(Isolate& isolate, TryCatchState& storage) noexcept;
void TryCatchClose(TryCatchState& state) noexcept;
bool TryCatchHasCaught(const TryCatchState& state) noexcept;
Slot TryCatchException(const TryCatchState& state, Isolate& isolate) noexcept;
std::optional<std::string> TryCatchMessage(const TryCatchState& state, const Context& context);
std::optional<std::string> TryCatchStackTrace(const TryCatchState& state, const Context& context);
std::optional<std::vector<StackFrame>> TryCatchStackFrames(const TryCatchState& state, const Context& context);
std::optional<MessageLocation> TryCatchLocation(const TryCatchState& state, const Context& context);
void TryCatchReThrow(TryCatchState& state) noexcept;
void TryCatchReset(TryCatchState& state) noexcept;

/// True when what stopped the code was `Isolate::TerminateExecution` and not a
/// throw. A handler must **not** consume one: closing it lets the unwind
/// continue whether or not `TryCatchReThrow` was called, because a swallowed
/// termination leaves the script it was told to stop running. See
/// unibind/exception.h.
bool TryCatchHasTerminated(const TryCatchState& state) noexcept;

// --- contexts -------------------------------------------------------------

ContextRec* NewContext(Isolate& isolate);
void RetainContext(ContextRec* rec) noexcept;
void ReleaseContext(ContextRec* rec) noexcept;
Isolate& ContextIsolate(const Context& context) noexcept;
Slot ContextGlobalObject(const Context& context) noexcept;
void ContextEnter(const Context& context, ContextScopeState& storage) noexcept;
void ContextLeave(ContextScopeState& state) noexcept;

// --- scripts --------------------------------------------------------------

ScriptRec* CompileScript(const Context& context, std::string_view source, const ScriptOrigin& origin);
void ReleaseScript(ScriptRec* script) noexcept;
std::optional<Slot> RunScript(const Context& context, ScriptRec* script);

/// The three code-cache operations. A backend whose engine has no compiled-code
/// cache defines **none** of them, so an embedder that reaches for one gets a
/// link error at its own call site rather than a silent no-op. Do not define a
/// stub. See the header comment in unibind/script.h.
///
/// `codeCache` is a hint: a blob this engine build does not recognise is
/// rejected and the source compiled normally, which `ScriptUsedCodeCache`
/// reports. An empty span is not an error.
///
/// **A backend does not have to key a blob to its source**: `unibind/script.h`
/// frames every blob it emits and drops one whose stamp does not match before
/// this is called, so what arrives here is either empty or a payload that
/// belongs to this source. Read the argument there before removing it from a
/// backend that has its own - it is a wrong answer that looks like a right one,
/// and neither engine checks it.
///
/// One trap for a backend author, found the hard way: SpiderMonkey's
/// `JS::EncodeStencil` **dereferences a null function pointer** - it does not
/// return failure - if `JS::SetProcessBuildIdOp` was never called. That is
/// process-wide state, so it belongs in `Platform`'s constructor, not in the
/// first call that needs it. A stencil is also realm-independent, which fits
/// decision 10 (a script sees the globals of the realm it runs in) better than
/// a per-realm compiled script does.
ScriptRec* CompileScriptWithCache(const Context& context, std::string_view source, const ScriptOrigin& origin,
                                  std::span<const std::uint8_t> codeCache);
/// Whether the compile got away without parsing. See unibind/script.h for the one
/// case where the honest answer is true for a reason that has nothing to do
/// with the blob.
bool ScriptUsedCodeCache(const ScriptRec* script) noexcept;
std::optional<std::vector<std::uint8_t>> ScriptCreateCodeCache(const ScriptRec* script);

/// What this engine build *is*, in the terms the engine keeps for itself: V8's
/// `ScriptCompiler::CachedDataVersionTag()`, SpiderMonkey's process build id.
/// Behind the backend's own name, because two backends' identities have to be
/// distinguishable and neither engine's says which library wrapped it.
///
/// This is the engine half of a cache blob's key (`unibind/script.h`), and it
/// is the engine's *build identity* rather than `Platform::BackendName()` or
/// the version string a human reads, for one reason: it is what the engine
/// itself would refuse the blob over. A key made of anything looser lets a blob
/// survive a change that invalidates it, and the embedder then pays a full
/// compile on every run while `UsedCodeCache()` quietly says false. A key made
/// of anything stricter - a build timestamp, say - throws away blobs that were
/// still good.
///
/// A backend answers with whatever its engine exposes for exactly this purpose,
/// and it may be coarser than the engine's real check without harm: the engine
/// still validates underneath, so a key that agrees when the engine would not
/// costs a rejected blob, which is the path a stale blob already takes.
///
/// **Every backend defines this, including one with no code cache.** It is not
/// one of the three optional entry points above - `Script::Compile` keys the
/// script whether or not a blob is ever asked for - and it costs a string.
std::string_view BackendBuildId() noexcept;

}  // namespace ub::detail
