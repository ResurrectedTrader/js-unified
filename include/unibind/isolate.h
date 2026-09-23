#pragma once
/// \file
/// Process-wide engine setup and the isolate: one JavaScript heap, one thread.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "unibind/fwd.h"
#include "unibind/types.h"

namespace ub {

// ---------------------------------------------------------------------------
// When the engine itself is in trouble
//
// Everything else in this API reports a failure to the caller that asked for
// something. This is the other direction: the engine noticing that *it* is in
// trouble, at a moment nobody asked it anything. A long-running embedder - the
// case this library is for - otherwise finds out that its engine is failing by
// dying.
// ---------------------------------------------------------------------------

/// What went wrong inside the engine, as opposed to inside a script.
///
/// **This is not `ErrorKind`** (`unibind/types.h`), and the two are kept
/// deliberately far apart in name because they are nearly opposite things.
/// `ErrorKind` names a JavaScript `Error` constructor and is what native code
/// *throws*: a value, catchable by script, part of the program's normal
/// behaviour. Nothing here is a value, nothing here is catchable, and nothing
/// here is normal - these are conditions the engine reports about itself, on
/// its way to failing or to dying.
///
/// **A backend raises only the kinds its engine has, and which those are is
/// written down rather than discovered** - the same treatment every other
/// capability gets here:
///
/// | kind | V8 | SpiderMonkey |
/// |---|---|---|
/// | `OutOfMemory` | yes | yes |
/// | `Fatal` | yes | yes |
///
/// **An engine assertion is a `Fatal`, not a kind of its own.** V8's `DCHECK`
/// and SpiderMonkey's `MOZ_ASSERT` exist only in a debug engine, and when one
/// fails the engine is exactly as finished as after a check that survives into
/// a release build - there is nothing an embedder would do differently, so a
/// separate kind would be a distinction with no use. What the report says in
/// `message` is where the two differ, and that is diagnostic text anyway.
enum class EngineFault : std::uint8_t {
    /// The engine could not get memory it needed, and said so rather than
    /// waiting to be asked.
    ///
    /// Raised for the engine's own heap, and also where the *library* runs
    /// out of memory doing the engine's work - a handle frame that cannot grow
    /// (`docs/lifetimes.md` rule 9) is the same condition arriving through a
    /// different allocator, and an embedder has no use for the distinction.
    ///
    /// **Whether anything survives it is not promised, and cannot be.** The
    /// library's own failures are recoverable on both engines: the operation
    /// answers empty, the isolate carries on. The *engine's* are not
    /// comparable - SpiderMonkey reports an out-of-memory condition into
    /// whatever was running and keeps going, while V8 treats its own heap
    /// giving out as fatal and ends the process once the callback returns. And
    /// the report does not reliably say which it is: V8's says a little about
    /// where, SpiderMonkey's says nothing at all.
    ///
    /// So the rule is the same one either way: **write a handler that is
    /// correct if the next line never runs**, and it is correct on both. Do
    /// not write one that tries to tell recovery from death and act
    /// differently.
    OutOfMemory,

    /// The engine hit a condition it does not continue from.
    ///
    /// On V8: an API misuse V8 itself detects, a failed `CHECK`, and in a
    /// debug engine a failed `DCHECK`. On SpiderMonkey: `MOZ_CRASH`, which is
    /// what every `MOZ_RELEASE_ASSERT` - and in a debug engine every
    /// `MOZ_ASSERT` - comes down to. SpiderMonkey has no hook for that; the
    /// backend recognises the crash sequence the engine's own header defines
    /// (its reason stored in `gMozCrashReason`, then a breakpoint) with a
    /// vectored exception handler, installed only when a handler is. `message`
    /// is the engine's reason, `location` is empty.
    ///
    /// **The process ends, and installing a handler does not change that.**
    /// That is deliberate and is the only defensible answer: V8 with no
    /// handler prints and aborts, while V8 with one would hand the failed
    /// check back to whatever was running - so a hook that merely reported
    /// would be a hook that decided whether the program survives, which no
    /// diagnostic should do. The backend reports first and ends the process
    /// second. So this is a place to write a line to a log that is already
    /// open, and nothing else.
    Fatal,
};

/// What the engine said, as it said it.
///
/// Everything here is a borrowed view that is valid for the duration of the
/// call and no longer. **Nothing in delivering this report allocates**, which
/// is the point: the commonest fault is running out of memory, and a report
/// that had to allocate to be made would be a report that could not be made
/// exactly when it is needed. Copy what you want to keep - into storage you
/// reserved earlier, because you cannot allocate either.
struct EngineFaultReport {
    EngineFault fault = EngineFault::OutOfMemory;

    /// The isolate it happened in, or null when there was none - a fault
    /// during bring-up, or on a thread of the engine's own.
    ///
    /// It is populated more often than the engines would manage alone: only
    /// some of their hooks name a heap, and one isolate per thread (decision
    /// 11) means the rest can be resolved from the thread they arrived on.
    /// Null is therefore a real answer - "no heap was involved" - rather than
    /// "the backend could not be bothered".
    Isolate* isolate = nullptr;

    /// Where the engine says it happened, in the engine's own words, or empty.
    /// A function name, a source position, whatever that engine puts there.
    std::string_view location;

    /// What the engine says happened, in the engine's own words, or empty.
    ///
    /// **Diagnostic only and not parseable**, exactly like
    /// `Platform::BackendVersion()`: log it, put it in a crash report, and do
    /// not branch on it. The two engines share no vocabulary here and one of
    /// them frequently says nothing at all.
    std::string_view message;
};

/// Told that the engine is in trouble. Installed once, on `PlatformOptions`.
///
/// **What you may do inside it is narrower than anywhere else in this API**,
/// and narrower than an interrupt callback (`Isolate::RequestInterrupt`),
/// which is the nearest precedent:
///
/// | inside an engine-fault callback | |
/// |---|---|
/// | read and write embedder state, set a flag, write to a buffer you already have | **yes** |
/// | `Isolate::TerminateExecution()` on the reported isolate | **yes** |
/// | **allocate anything at all** | **no** |
/// | make a handle, read a value, call into the engine | **no** |
/// | call a JavaScript function, run a script, throw | **no** |
///
/// The allocation ban is not caution, it is the definition of the situation:
/// the report that arrives most often is `OutOfMemory`, and allocating in
/// order to report that allocation failed is a bug in every case and a crash
/// in the interesting one. That is the rule the backends already hold
/// themselves to - the message a frame-exhaustion failure carries is a string
/// literal precisely so that saying it costs nothing - and it is handed on
/// here unchanged. Reserve your buffer, open your log file, and size your
/// strings *before* you need them.
///
/// Handles are refused for a second reason on top of that one. An interrupt
/// may make them because the engine sets a scope up for exactly that; nothing
/// sets one up here, the fault may have arrived with no isolate at all, and on
/// V8 a `Fatal` report is running inside machinery that has already decided
/// the process is over.
///
/// **It may be called on any thread**, including one that has never had an
/// isolate, and including while another thread is inside the engine. Whatever
/// state it touches has to be safe for that; a flag and a pre-sized buffer
/// are, and anything that takes a lock the failing thread might already hold
/// is not.
///
/// It must not throw. An exception crossing back into the engine is
/// `std::terminate` at best, and the backends make it exactly that rather than
/// something less defined.
using EngineFaultCallback = void (*)(const EngineFaultReport& report, CallbackData data);

/// What is configurable about the engine *process*, as opposed to one heap.
///
/// Both settings here look at first like they belong on an isolate, and on
/// neither engine do they: V8's worker threads belong to the `v8::Platform`
/// and its flags are parsed into process-global variables, while
/// SpiderMonkey's helper threads and its JIT switches are set up by `JS_Init`
/// for the process. An `IsolateOptions` field for either would be a promise the
/// second isolate silently could not keep, which is why they are here.
struct PlatformOptions {
    /// Background threads the engine may use for parsing, compilation and
    /// collection. Empty asks for the engine's own default.
    ///
    /// **A hint, at every value including zero**, and
    /// `Platform::WorkerThreads()` reports what is actually in effect. The
    /// honest reason is that one of the two engines cannot comply: SpiderMonkey
    /// builds its pool inside `JS_Init`, ignores
    /// `JSGC_MAX_HELPER_THREADS = 0` outright, and the one hook that does
    /// replace the pool - `JS::SetHelperThreadTaskCallback` - accepts the
    /// callback and then crashes on the first collection, because the engine
    /// dispatches parallel marking tasks and waits on them, so running one
    /// inline is a re-entrancy it does not support. Of "ignore it silently",
    /// "crash later" and "refuse", only the first is survivable, and a hint
    /// nobody can observe is how `UsedCodeCache` would have gone wrong. So it
    /// is a hint *plus* an answer.
    ///
    /// **Why this is a hint while a second isolate is refused outright**
    /// (decision 11), which looks like the opposite policy: that rule is about
    /// semantics a program can observe - an isolate either exists or does not,
    /// and code that depends on one existing breaks on the other backend.
    /// Worker threads change no observable behaviour of any script; they change
    /// timing and where work happens. An ignored hint is not a portability
    /// cliff, whereas making V8 refuse a mode it supports perfectly well would
    /// throw away a real capability to buy symmetry. An embedder that genuinely
    /// *requires* no background threads - because of where it is embedded -
    /// asks, checks, and fails on its own terms.
    std::optional<std::uint32_t> workerThreads;

    /// Engine-specific tuning, passed through verbatim - V8's command-line
    /// flags, say. A backend with no such concept ignores it.
    ///
    /// **Nothing in this API's behaviour may depend on it**, and it is here
    /// rather than on an isolate because on both engines a flag string is the
    /// process's, not the heap's: setting one per isolate would silently apply
    /// it to every isolate that came before.
    std::string_view engineFlags;

    /// Told when the engine is in trouble. Null - the default - means the
    /// engine's own behaviour, which is to print something to stderr and, for
    /// the fatal kinds, end the process.
    ///
    /// **Here rather than on `Isolate`, and set here rather than by a setter.**
    /// Both halves of that were decided rather than assumed (decision 28), and
    /// both are about the moments this exists to cover:
    ///
    ///   * The kind both engines raise, `OutOfMemory`, is per-isolate on both
    ///     - which argues for `Isolate` and is not the whole argument. A fault
    ///     during `Isolate::New` has no isolate to have been registered on,
    ///     and that is the single moment an embedder most wants to hear about;
    ///     so is a failure on a thread of the engine's own, and so is V8's
    ///     process-wide internal check, which names no heap. A handler that
    ///     can only be installed on an isolate is deaf at all three. A
    ///     `Platform` exists before the first isolate and after the last, so
    ///     one installed here is armed for the whole life of the engine - and
    ///     `EngineFaultReport::isolate` gives back everything putting it on
    ///     `Isolate` would have bought.
    ///   * It cannot be changed afterwards because it can arrive **on any
    ///     thread**. A settable handler is a function pointer and a data
    ///     pointer that another thread may be part-way through reading when a
    ///     third one faults, and the obvious fix - a lock - is a lock taken at
    ///     out-of-memory, possibly by the thread that already holds it. Fixed
    ///     for the life of the `Platform`, it needs neither. It is also what
    ///     the engines want: V8's process-level handlers are documented as
    ///     process-wide state to install before the engine comes up, and
    ///     SpiderMonkey's process hook may be set at most once.
    ///
    /// An embedder whose policy changes over time changes it behind one fixed
    /// handler, through `engineFaultData`, which is the same indirection this
    /// API asks for everywhere else.
    EngineFaultCallback onEngineFault = nullptr;

    /// Handed to `onEngineFault`. The pointee is the embedder's and must
    /// outlive the `Platform`, which is the whole program.
    CallbackData engineFaultData;
};

/// Process-wide engine initialisation. Construct one before the first isolate
/// and keep it alive until after the last one; constructing a second while one
/// exists is an error.
///
/// V8 has no such class (it is a pair of free functions); SpiderMonkey's
/// `JS_Init`/`JS_ShutDown` are the same idea. Making it an object is the only
/// way to give both a shape an embedder cannot get wrong.
class Platform {
   public:
    explicit Platform(const PlatformOptions& options = {});
    ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) = delete;
    Platform& operator=(Platform&&) = delete;

    /// Whether the engine actually came up. **Check it.**
    ///
    /// A constructor cannot return a failure, and bringing an engine up can
    /// fail for reasons that are nobody's mistake - `JS_Init` reports it,
    /// V8's `Initialize` reports it, and a platform may decline to be made at
    /// all. So this is the failure channel, and a `Platform` that answers
    /// false is an object that exists and did nothing: every `Isolate::New`
    /// against it will answer empty, which is the same refusal arriving one
    /// step later for a caller who did not ask here.
    ///
    /// Constructing a *second* `Platform` while one is alive is a different
    /// thing - a programming error, not a failure - and it is a precondition
    /// rather than something to check for.
    ///
    /// **Nor is a second one after the first has gone.** Bring the engine up
    /// once per process and leave it up: "destroy and remake" is not a portable
    /// lifecycle, and it fails in the worse direction. SpiderMonkey survives it
    /// - `JS_Init` after `JS_ShutDown` works, and the new platform hands out
    /// isolates - while V8 **dies**, because `V8::Initialize` after
    /// `DisposePlatform` trips a fatal check on its own startup state. So this
    /// is not a capability one backend has and the other lacks; it is one
    /// backend being harmlessly permissive about something the other treats as
    /// unrecoverable, which is exactly the shape that gets discovered by
    /// porting. A `Platform` is a scope around the whole program's use of the
    /// engine.
    [[nodiscard]] static bool IsInitialized() noexcept;

    /// Which backend this build links, e.g. "v8" or "spidermonkey". For
    /// diagnostics and test reporting only - branching on it in library or
    /// embedder code means the abstraction has failed somewhere else.
    ///
    /// **It is a runtime question on purpose.** A consumer compiles against
    /// these headers once and chooses `unibind_backend_v8` or
    /// `unibind_backend_spidermonkey` at the final link, so there is no
    /// compile-time answer to give and no macro that offers one.
    [[nodiscard]] static std::string_view BackendName() noexcept;

    /// Which *build* of that engine, in whatever words the engine uses for
    /// itself - "15.6.8" from V8, "JavaScript-C153.3.0" from SpiderMonkey.
    ///
    /// **Diagnostic only, and not parseable.** The shape is the engine's and
    /// differs between them, so log it, print it in a bug report, put it in a
    /// crash dump - and do not split it, order it, or compare it against a
    /// number you wrote down. There is deliberately no structured version here:
    /// two engines' version schemes have nothing in common to promise, and an
    /// embedder that needs to branch on an engine's version is asking for
    /// something this API does not do.
    ///
    /// It exists because "which engine" turned out not to be enough to ask. A
    /// cache blob belongs to an engine *build* rather than to an engine, so
    /// upgrading the engine has to invalidate every blob an embedder kept -
    /// and before this, nothing in the API could tell that anything had
    /// changed. That key is not this string, though: it is the engine's own
    /// build identity, which is stricter and is what the engine itself checks.
    /// See the top of `unibind/script.h`. This one is for a human.
    [[nodiscard]] static std::string_view BackendVersion() noexcept;

    /// The number of engine background threads actually in effect, or empty
    /// when the backend cannot get the figure out of the engine.
    ///
    /// This is the observable half of the `workerThreads` hint, and it has to
    /// be a count rather than a yes/no because the three outcomes an embedder
    /// must tell apart - honoured, clamped, ignored - are otherwise
    /// indistinguishable from outside. Compare it with what you asked for:
    /// equal is honoured, different is clamped or ignored, and **empty is not
    /// zero** - it means the engine would not say, so assume it has threads.
    ///
    /// An embedder that merely wants fewer threads sets the option and moves
    /// on. One that requires none, because of where it is embedded, asks,
    /// compares, and fails on its own terms rather than discovering later that
    /// this backend was never going to comply.
    [[nodiscard]] static std::optional<std::uint32_t> WorkerThreads() noexcept;
};

/// Asked what to do about a heap that is about to hit its ceiling, **before**
/// anything has failed. See `Isolate::SetHeapLimitCallback`.
///
/// Return the ceiling the heap should have from now on, in bytes. Returning
/// `currentLimitBytes` - or anything below it, which is clamped up to it -
/// leaves the ceiling where it is and lets the engine do whatever it does when
/// it reaches one, which on the engine that has this hook is to abort.
///
/// `initialLimitBytes` is the ceiling the isolate was made with, so a handler
/// that has raised the limit before can tell how far it has already gone
/// without keeping a count of its own.
///
/// **What you may do inside it** is the engine-fault contract
/// (`EngineFaultCallback`) with one addition and one relaxation. The addition
/// is that you are being asked a question and your answer is acted on. The
/// relaxation is that nothing has failed yet, so ordinary embedder work is
/// fine - but the JavaScript heap is at its limit and the engine is inside a
/// collection, so: no handles, no values, no script, no throw, and nothing
/// that would allocate on the heap you are being asked about. Called on the
/// isolate's own thread, always.
///
/// `Isolate::TerminateExecution()` is callable here, and pairing it with a
/// raised ceiling is the reason this hook is worth having: see the setter.
using HeapLimitCallback = std::size_t (*)(Isolate& isolate, std::size_t currentLimitBytes,
                                          std::size_t initialLimitBytes, CallbackData data);

/// What is configurable about one heap.
///
/// Deliberately short. A knob only belongs here if it is genuinely per-isolate
/// on both engines *and* means the same thing on both; the rest is in
/// `PlatformOptions`, belongs to a realm, or is refused. Refused, with the
/// reason, because an embedder is entitled to know that the answer is no
/// rather than discover it by porting:
///
///   * **Interpreter-only / no JIT.** V8 spells it as a process-global flag
///     (`--jitless`), SpiderMonkey as per-context JIT switches whose
///     granularity does not match. Portable only as `engineFlags`.
///   * **Collector tuning** - nursery size, growth factors, incremental
///     slices. The two collectors do not have the same knobs, and a number
///     that means one thing on one heap means nothing on the other.
///   * **`eval` and `new Function`.** Both engines control this, and both do it
///     per *realm*, not per heap. If it is ever added it goes on `Context`.
///   * **Microtask / job queue policy.** Real and per-isolate on both, and
///     deliberately not a knob: `Isolate::PumpJobs` *is* the policy, and it is
///     fixed rather than configurable so that a promise continuation runs at
///     the same observable moment on both backends. See decision 23.
///   * **Locale, ICU data, random seed.** Process-wide on both engines.
struct IsolateOptions {
    /// Ceiling on the JavaScript heap. 0 means the engine's default.
    ///
    /// What an engine does when it reaches the ceiling is its own business -
    /// both may collect harder, and both may abort rather than report - so this
    /// is a budget, not a promise of an orderly failure.
    ///
    /// Where a backend can be asked first, `Isolate::SetHeapLimitCallback` is
    /// how; where it cannot, that call does not link. Either way the failure
    /// itself arrives as an `EngineFault` if a handler was installed on the
    /// `Platform`.
    std::size_t heapLimitBytes = 0;

    /// Native stack the engine may use before it stops recursing, in bytes,
    /// measured from wherever `Isolate::New` was called. 0 means the engine's
    /// default.
    ///
    /// This is the knob that turns runaway recursion in script into an
    /// exception the script can catch instead of a stack overflow in the host
    /// process, which is why it is worth promising: it is per-isolate on both
    /// engines, both measure it the same way, and the failure it prevents is not
    /// one an embedder can catch by other means. Set it comfortably below the
    /// thread's real stack - the engine needs headroom to build and throw.
    ///
    /// **Which** error is the engine's business and the two do not agree -
    /// `RangeError` on one, `InternalError` on the other - so a portable script
    /// catches it rather than asking what it is.
    std::size_t stackLimitBytes = 0;
};

/// One JavaScript heap. Owns its contexts, its handle frames, and its pending
/// exception. Not thread-safe and not thread-movable: everything done with an
/// isolate happens on the thread that created it.
///
/// **At most one isolate is alive per thread.** One after another is fine -
/// destroy one and make the next - but a second while the first is alive is
/// refused. The restriction is the stricter engine's rather than an invention:
/// SpiderMonkey keeps the running `JSContext` in a single thread-local slot, so
/// a second one on a thread is not a heap it can make at all. V8 would allow
/// it, and refuses here anyway, because a program that works on one backend and
/// not on the other is the failure this library exists to prevent.
///
/// Two heaps at once is therefore two threads, which costs nothing this design
/// had not already charged: handles, scopes and contexts are thread-bound
/// anyway, so nothing could have passed between two isolates on one thread
/// either.
///
/// **Nothing an isolate handed out may outlive it.** A `Context`, a `Script`
/// and a `Global<T>` are the three things an embedder can hold across frames,
/// and all three name engine state that belongs to this isolate: destroy them,
/// or let them go out of scope, before the isolate goes. Declaring the isolate
/// first and everything else after it is enough - C++ destroys them in the
/// reverse order - and that is what every example here does.
///
/// This is a rule rather than something the library can survive, and it is
/// stated because an embedder could not otherwise know. One of these still
/// alive when `~Isolate` runs is **two** faults at once: its own memory is
/// never given back, because it is released by the call the embedder has not
/// made yet, and that call - `~Context`, `~Script`, `Global::Reset` - then
/// resets an engine handle against an isolate that has been disposed. The
/// first is a leak; the second is undefined behaviour, and it is the kind
/// that works on a good day.
///
/// A checked build (`UNIBIND_HANDLE_CHECKS`, i.e. Debug) counts them and
/// diagnoses the violation in `~Isolate`, where it happens, rather than
/// leaving it to be discovered at the crash. A release build does neither
/// check nor tolerate it.
///
/// What an isolate destroys at teardown is not a violation, even where it
/// releases one of these: a native the isolate owns may itself hold a realm
/// and a root, and giving them back is what its destruction is for. The rule
/// is about what is still outstanding once the isolate has emptied everything
/// it owns - which is whatever the embedder is holding.
///
/// The types an isolate owns outright - templates, classes and their records -
/// are the other half of this and need nothing from an embedder: they are
/// isolate-lifetime by design, they hand out raw pointers, and decision 13 in
/// `docs/status.md` says so.
class Isolate {
   public:
    /// Empty if a heap could not be made - including because this thread
    /// already has a live one. Never a crash: a caller that does not control
    /// its host thread is entitled to ask and be told.
    ///
    /// **Why a pointer, when `Global<T>` is handed back by value.** The
    /// dereference is the first thing a reader notices and the reason is not
    /// visible from the signature, so: an `Isolate` must not move, and the
    /// pointer is what enforces it.
    ///
    /// Its *address* is what everything else remembers it by. A backend stores
    /// it in the engine's own embedder slot, so that a native callback handed
    /// nothing but an engine context can find its way back; a thread-local
    /// holds it to enforce one isolate per thread (decision 11); and every
    /// frame, context, script, template, class, root and handler carries an
    /// `Isolate*` to its owner. Moving the object dangles all of them at once.
    ///
    /// That is not merely an implementation detail to route around by storing
    /// the *implementation's* address instead - which would work, at one extra
    /// load per recovery. **Immovability is worth having on its own**, because
    /// `HandleScope`, `ContextScope` and `TryCatch` are stack objects that point
    /// at the isolate, and a movable isolate could be moved out from under an
    /// open one with nothing to catch it. Today the type system refuses.
    ///
    /// The only shape that would remove the arrow is a plain `Isolate` carrying
    /// its own empty state - `std::optional<Isolate>` needs the same `operator*`
    /// and buys nothing. And that shape trades a null pointer, which cannot be
    /// used by accident, for a valid-looking object whose methods are undefined:
    /// the very failure this API exists to prevent. `Local` accepts that trade
    /// and says so, because handles are made constantly and an unwrap at every
    /// one of them would be intolerable. An isolate is made *once*, so the check
    /// costs nothing and the trade is not worth making twice.
    [[nodiscard]] static std::unique_ptr<Isolate> New(const IsolateOptions& options = {});

    ~Isolate();

    Isolate(const Isolate&) = delete;
    Isolate& operator=(const Isolate&) = delete;
    Isolate(Isolate&&) = delete;
    Isolate& operator=(Isolate&&) = delete;

    /// True if a native or script throw is pending and has not been caught.
    /// After any operation that returned an empty optional, this says whether
    /// the cause was a JavaScript exception.
    [[nodiscard]] bool HasPendingException() const noexcept;

    /// Throw from native code. The throw takes effect when control returns to
    /// the engine; the calling code should stop and return promptly. `message`
    /// is decoded as `ub::Throw` decodes it: bytes that are not UTF-8 become
    /// U+FFFD rather than costing the error.
    void ThrowError(ErrorKind kind, std::string_view message);

    [[nodiscard]] HeapStatistics GetHeapStatistics() const noexcept;

    /// Ask for a full collection. Engines are free to ignore the request; it
    /// exists so lifetime tests can be written at all, and is not a tuning
    /// knob.
    void RequestGarbageCollection() noexcept;

    /// Be asked what to do when this heap is about to hit `heapLimitBytes`,
    /// instead of finding out afterwards. Pass a null callback to stop being
    /// asked.
    ///
    /// **Not every backend defines this, and a call to it in a build whose
    /// backend does not is a link error at your own call site** - this API's
    /// standing answer for an operation an engine cannot do (decision 19). It
    /// is V8's `AddNearHeapLimitCallback` and SpiderMonkey has no equivalent:
    /// not a hook with a different shape, but nothing at all - no notification
    /// as the heap fills, and no moment at which anyone is asked. A field on
    /// `IsolateOptions` would have compiled there and never fired, which is
    /// the silent failure this library exists to prevent; a link error is the
    /// loud one.
    ///
    /// **This is a decision point, not a notification**, which is why it is
    /// not one of the `EngineFault` kinds. Nothing has failed when it runs:
    /// the heap is near its ceiling, the engine has collected and it did not
    /// help, and the next thing to happen is decided by what you return. An
    /// embedder that must not be killed by one runaway script raises the
    /// ceiling far enough to unwind in and asks the script to stop:
    ///
    /// ```cpp
    /// std::size_t Rescue(ub::Isolate& isolate, std::size_t current, std::size_t initial,
    ///                    ub::CallbackData data) {
    ///     auto* state = data.As<MyState>();
    ///     if (state == nullptr || state->rescued) {
    ///         return current;                 // out of second chances: let it end
    ///     }
    ///     state->rescued = true;
    ///     isolate.TerminateExecution();       // unwind the script that did this
    ///     return initial * 2;                 // room to unwind in
    /// }
    /// ```
    ///
    /// That pairing is the whole point. Raising the ceiling alone buys a
    /// runaway script a bigger heap to fill; terminating alone leaves no room
    /// for the unwind itself. Together they turn "the process dies" into "that
    /// script stopped", which is the difference between an embedder that
    /// survives a bad script and one that does not.
    ///
    /// **The ceiling you return stands.** The engine does not put the old one
    /// back when the heap shrinks, so a handler that raises without limit has
    /// removed the limit; that is why the example counts.
    ///
    /// Set it on the isolate's own thread, and it is called on that thread.
    /// Only the most recently set callback is asked.
    void SetHeapLimitCallback(HeapLimitCallback callback, CallbackData data) noexcept;

    // --- stopping a script from outside ------------------------------------
    //
    // An embedder that runs script on its own thread needs a way to stop a
    // runaway loop, or to wind one down at shutdown, from a thread that is not
    // the one blocked inside the engine. Both engines have the facility and
    // both shape it the same way: a request from anywhere, noticed by the
    // running code at the next check, and an unwind that no script `catch` can
    // stop.

    /// Stop whatever this isolate is running, as soon as it notices.
    ///
    /// **The only operation in this API that may be called from a thread other
    /// than the isolate's own**, and the reason is that it is the only one that
    /// is useful from one. Everything else - handles, scopes, contexts, values
    /// - stays thread-bound.
    ///
    /// What it does:
    ///
    ///   * Running script unwinds at the next check the engine makes. The
    ///     unwind is **not catchable from script**: a `try`/`finally` does not
    ///     stop it, and neither does a native `TryCatch` (see
    ///     `TryCatch::HasTerminated`).
    ///   * Every operation that would run script fails - an empty
    ///     `std::optional`, an empty `Local` - until the termination is
    ///     cancelled. The failure is not a JavaScript exception: there is no
    ///     value, no message and no stack to read, which is what distinguishes
    ///     it from a throw.
    ///   * Requested while nothing is running, it is **remembered** and stops
    ///     the next thing that runs. It stays armed until it fires or
    ///     `CancelTerminateExecution` disarms it. There is no way to observe
    ///     the race between "already running" and "about to run", so both
    ///     engines make the same promise and so does this.
    ///   * **A native callback is not interrupted.** The unwind happens at the
    ///     next JavaScript checkpoint, so a native that is spinning, blocking on
    ///     a socket, or waiting on a lock runs to completion first - on both
    ///     engines, measured, and with no facility on either to do otherwise.
    ///
    /// Which is the sentence to read before building a timeout, not after:
    ///
    /// > **This reaches script, not your own C++. A watchdog cannot save you
    /// > from your own blocking callback.**
    ///
    /// A native that can block for long enough to matter has to poll
    /// `IsExecutionTerminating()` itself and return; nothing else will stop it,
    /// and a stop requested while it blocks simply waits for it.
    ///
    /// **A stopped isolate stays stopped until it is cancelled, whatever the
    /// engine would allow.** This is settled rather than inherited, because the
    /// engines answer differently: SpiderMonkey's context is usable the instant
    /// the unwind finishes, with nothing to reset, while V8 keeps its
    /// termination pending. Taking the permissive answer would mean a stop
    /// requested from another thread races the very next `Script::Run` and
    /// sometimes loses, silently, on one backend only - so the strict answer is
    /// the contract and a backend that runs on a permissive engine enforces it.
    /// `CancelTerminateExecution` is not optional book-keeping; it is how an
    /// isolate is made usable again.
    ///
    /// What is safe on a terminating isolate, on its own thread: returning
    /// promptly out of every native frame, reading `IsExecutionTerminating()`,
    /// calling `CancelTerminateExecution()`, closing scopes, releasing globals,
    /// and destroying the isolate. Nothing else - and in particular an
    /// operation whose result is checked but whose failure is ignored will
    /// silently do nothing, over and over.
    ///
    /// The isolate must outlive the call, which is the embedder's to arrange:
    /// join the script thread before destroying the isolate, not the other way
    /// round.
    void TerminateExecution() noexcept;

    /// True from the moment a stop is requested until it is cancelled. Ask it on
    /// the isolate's own thread - and in particular ask it in a long-running
    /// native callback, which should then return promptly without calling
    /// further into the engine, because nothing else is going to stop it.
    ///
    /// **This is the library's answer, not the engine's**, on both backends.
    /// Neither engine has a bit that means "a stop has been requested": V8's own
    /// `IsExecutionTerminating` is true only while the termination exception is
    /// actually pending, which is essentially never while a native is running,
    /// and SpiderMonkey has nothing sticky at all - there the unwind happens and
    /// the context is immediately live again. So a backend holds the flag itself
    /// and gates every entry point that would run script. The promise is
    /// keepable, but it is *ours to keep*: reach around this API to the engine
    /// while a stop is pending and the engine will happily run script.
    [[nodiscard]] bool IsExecutionTerminating() const noexcept;

    /// Disarm a termination, so the isolate can be used again. On the isolate's
    /// own thread, and after every native frame the termination unwound has
    /// returned - cancelling from inside one resumes a script that was told to
    /// stop.
    void CancelTerminateExecution() noexcept;

    // --- pending work --------------------------------------------------------
    //
    // Two things queue up behind a script and run later: the engine's own jobs
    // (promise continuations - the spec calls them microtasks) and whatever an
    // embedder posted from another thread. `PumpJobs` runs both, and it is the
    // only thing that does.
    //
    // **Why one drain and not two.** A promise needs a pump whatever else
    // happens - a settled promise queues a continuation and nothing else in
    // this API would run it - and an embedder's own work has to run at a point
    // the script thread chose, which is the same point. Two pumps would mean
    // two rules, and the second one an embedder forgot would fail the same
    // silent way the first does. The ordering below is what keeps them honest
    // when they interleave.
    //
    // **Why posting is not the engine's interrupt.** The interrupt runs inside
    // the interrupted execution, between two bytecodes, and cannot run script
    // (see `RequestInterrupt` below for exactly what it can do). Work that
    // wants to call a function or run a script therefore cannot happen there,
    // and a queue the script thread drains at a point of its own choosing has
    // no re-entrancy question to get wrong.
    //
    // The two do **not** compose into "post, interrupt, drain sooner": an
    // interrupt cannot make a running script yield and cannot fire at all while
    // the thread is idle, so it moves nothing forward. `PostJob` says so at
    // length, because it is the first thing an embedder will try.

    /// Run `callback` on this isolate's thread promptly, *even while script is
    /// running*, at the next point the engine checks.
    ///
    /// **Callable from any thread.** This is the engine's own interrupt - V8's
    /// `RequestInterrupt`, SpiderMonkey's `JS_RequestInterruptCallback` - and
    /// it is the only way to reach the isolate's thread while a script is
    /// running. It is *not* how work is posted: what it can do is narrow, and
    /// the narrowness is the whole point.
    ///
    /// It also only reaches a thread that is *in* script. There is no
    /// checkpoint while the thread is idle between scripts, so an interrupt
    /// requested then waits for the next one.
    ///
    /// | inside an interrupt callback | |
    /// |---|---|
    /// | make and read handles, inspect values | **yes** |
    /// | read and write embedder state, set a flag, queue a job | **yes** |
    /// | call a JavaScript function, run a script, throw | **no** |
    ///
    /// Handles are fine because the engine sets up a scope for exactly this -
    /// V8 opens a fresh `HandleScope` and an external VM state around the call,
    /// deliberately unsealing the handles the stack guard had sealed.
    ///
    /// **Running script is not, and one engine would let you.** Measured, on
    /// SpiderMonkey, from inside a live interrupt fired into a `while (true)`
    /// loop: a callback there can make an object, set a property, call a
    /// JavaScript function that really runs, compile and run a fresh script,
    /// and capture a stack, after which the loop terminates normally and the
    /// isolate is fine. V8 refuses the same thing. The rule follows V8 anyway,
    /// and not for symmetry's sake:
    ///
    /// An interrupt fires *between two bytecodes of whatever was running*. Script
    /// run there runs at an arbitrary point inside unrelated code, and anything
    /// it leaves behind - an exception above all - is left for the interrupted
    /// frame to trip over. The probe above survived only because it cleared its
    /// own exceptions at every stage; a callback that threw and returned
    /// normally would hand the interrupted code an exception it never threw. The
    /// same hazard is visible in V8's dispatch, where a termination is also
    /// processed *earlier in the same interrupt pass*, so script run here can run
    /// after one was already handled.
    ///
    /// "You may, but you must clean up perfectly afterwards" is exactly the kind
    /// of rule that is better written as "you may not". Obeying it costs no
    /// capability and nothing slower - it is a restriction on the embedder, not
    /// on a backend, which is the right place for it.
    ///
    /// **What this is actually for** is narrow, and both uses share the one
    /// property a foreign thread cannot arrange any other way - being *on the
    /// isolate's thread while script is running*:
    ///
    ///   * **A profiler tick**: capture a stack at an arbitrary point inside
    ///     running script.
    ///   * **A watchdog that samples before deciding**: read state on-thread,
    ///     then decide whether to call `TerminateExecution`. Note that
    ///     `TerminateExecution` is itself callable from any thread, so the
    ///     interrupt earns its place here only when the decision depends on
    ///     reading something first.
    ///
    /// Both read; neither runs.
    ///
    /// **It is not a way to deliver work.** A posted job runs at the drain
    /// point and never inside an interrupt, and an interrupt does not bring the
    /// drain point closer: it cannot make a running script yield, and it never
    /// fires while the thread is idle. `PostJob` has the trace.
    ///
    /// And the same warning `TerminateExecution` carries, for the same reason:
    ///
    /// > **This reaches script, not your own C++. A watchdog cannot save you
    /// > from your own blocking callback.**
    ///
    /// The engine checks for an interrupt at its own checkpoints, all of which
    /// are inside script. A native that spins, blocks on a socket or waits on a
    /// lock is not one of them, and runs to completion first.
    ///
    /// Requests are ordered and each one runs once. What has not fired when the
    /// isolate is destroyed is dropped.
    void RequestInterrupt(InterruptCallback callback, CallbackData data) noexcept;

    /// Queue `callback` to run on this isolate's thread the next time it pumps.
    ///
    /// **Callable from any thread**, and it does not wake the isolate. Posted
    /// work runs when the script thread next calls `PumpJobs`, and **nothing
    /// accelerates that** short of terminating whatever is running.
    ///
    /// That is worth stating flatly, because the obvious way to try is
    /// `RequestInterrupt` and it does not work. Trace it: if the thread is
    /// inside a long-running script, the interrupt fires, the callback cannot
    /// run the work and cannot make the script yield - there is no yield,
    /// suspend or resume in this API - so the script resumes and runs to
    /// completion and the thread pumps afterwards, which is what would have
    /// happened anyway. If the thread is idle, no script is running, no
    /// checkpoint is reached, and the interrupt never fires at all. In neither
    /// state does it make posted work run sooner.
    ///
    /// So the bounds on "promptly", for anyone building an event-driven
    /// embedding: **a running script cannot be made to give the thread back,
    /// only terminated**, and a native that blocks is not interruptible either
    /// (see `TerminateExecution`). An embedder that needs to interleave work
    /// with script keeps its scripts short and pumps between them; one that must
    /// bound the wait terminates and restarts.
    ///
    ///   * **Ordered, never coalesced.** Work runs in the order it was posted,
    ///     once each; posting the same callback twice runs it twice.
    ///   * **Whatever is still queued when the isolate is destroyed is dropped,
    ///     not run.** Said out loud because "it will run eventually" is what a
    ///     caller would otherwise assume. An embedder that must know its work
    ///     happened acknowledges from inside the callback.
    ///   * The callback gets a live isolate and a full call frame's worth of
    ///     API - open your own `HandleScope` and `ContextScope`, because it is
    ///     not inside a call and no realm is current. Let nothing escape: it
    ///     runs between two things the script thread was doing, so wrap engine
    ///     calls in a `TryCatch` and report failure to the embedder.
    void PostJob(JobCallback callback, CallbackData data) noexcept;

    /// `PostJob`, but not before `delayInSeconds` have passed - V8's
    /// `TaskRunner::PostDelayedTask`, for the timer an embedder would otherwise
    /// keep beside the isolate.
    ///
    /// Everything `PostJob` says holds, and the delay adds exactly one thing: a
    /// floor, measured on a steady clock from this call. Once it has passed the
    /// job is posted work like any other, and runs at the first `PumpJobs`
    /// after that - **nothing wakes the thread when it falls due**, any more
    /// than posted work wakes it. An embedder that sleeps between pumps bounds
    /// how late a job can be by how long it sleeps.
    ///
    ///   * Delayed jobs run in the order they fall due, and two that fall due
    ///     at the same moment in the order they were posted. Relative to
    ///     undelayed work, one joins the back of the queue at the first pump
    ///     after it falls due - so work posted before that pump runs first,
    ///     whenever it was posted.
    ///   * A delay of zero, a negative one, or a NaN is no delay: the job is
    ///     posted as it would be by `PostJob`.
    ///   * Callable from any thread. Still waiting when the isolate is
    ///     destroyed means dropped, as for any posted work.
    void PostDelayedJob(JobCallback callback, CallbackData data, double delayInSeconds) noexcept;

    /// Run everything pending - promise continuations and posted work - until
    /// there is nothing left.
    ///
    /// **Nothing else runs any of it.** Script containing `async`/`await` or
    /// any `.then` compiles and runs, and its continuations do not execute
    /// until this is called: no error, no exception, the work simply does not
    /// happen. That is a documented requirement rather than a bug, and this
    /// sentence is the difference. Call it on the isolate's own thread with no
    /// native frame on the stack - after a `Script::Run`, at the bottom of your
    /// event loop.
    ///
    /// Order: engine jobs first, then posted work, then round again, until both
    /// are empty. Posted work that settles a promise therefore sees its
    /// continuations run in the same pump, which is the behaviour that makes
    /// one drain better than two.
    ///
    /// Both engines *can* drain their own job queue at moments of their
    /// choosing, and V8's default policy does exactly that when a call returns.
    /// This API turns that off, so that a promise continuation runs at the same
    /// observable moment on both backends rather than "after each script" on
    /// one and "never" on the other. Unlike a worker-thread count, when a
    /// continuation runs is something a script can see, so it is made uniform
    /// rather than left as a hint.
    ///
    /// Anything a job throws is caught and discarded here: a pump is not a call
    /// and has nowhere to put an exception. A job that can fail should say so
    /// to the embedder. Nothing runs while a termination is pending, and the
    /// queues survive it - cancel the termination and pump again.
    void PumpJobs();

    /// One typed embedder pointer per isolate, recovered as its real type or
    /// not at all. The pointee is the embedder's and must outlive the isolate.
    template <class D>
        requires(!std::is_const_v<D>)
    void SetEmbedderData(D& data) noexcept {
        StoreEmbedderData(CallbackData::For(data));
    }
    template <class D>
    [[nodiscard]] D* GetEmbedderData() const noexcept {
        return LoadEmbedderData().template As<D>();
    }

    void StoreEmbedderData(CallbackData data) noexcept;
    [[nodiscard]] CallbackData LoadEmbedderData() const noexcept;

    /// Implementation detail: the backend's per-isolate state.
    struct Impl;
    [[nodiscard]] Impl& impl() const noexcept { return *impl_; }

   private:
    explicit Isolate(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ub
