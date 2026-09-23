#pragma once
/// \file
/// Forward declarations for the whole public API.
///
/// The value tag types (`Value`, `Object`, ...) are only declared here; they
/// are defined in `unibind/value.h`, which is where their static factories live.
/// `Local<T>` never needs `T` to be complete, so a header that only passes
/// handles around can include this one.

#include "unibind/config.h"
#include "unibind/types.h"

namespace ub {

// Value tag types. These are never instantiated; they exist to give handles
// distinct static types. The inheritance among them is the narrowing lattice:
// Local<Function> converts to Local<Object> implicitly, the other way round
// only through a checked cast.
struct Value;
struct Primitive;
struct Boolean;
struct Number;
struct Integer;
struct Name;
struct String;
struct Symbol;
struct BigInt;
struct Object;
struct Array;
struct Function;
struct ArrayBuffer;
struct TypedArray;
struct Promise;
struct External;

template <class T>
class Local;
template <class T>
class Global;
template <class T>
class Class;

class Platform;
class Isolate;
class Context;
class ContextScope;
class HandleScope;
class EscapableHandleScope;
class TryCatch;
class Script;
class ObjectTemplate;
class FunctionTemplate;
class CallbackInfo;
class PropertyCallbackInfo;
class ReturnValue;
class CallbackData;

namespace detail {

// Backend-defined types. Declared, never defined in a public header - the
// definitions live in whichever backend library is linked.
struct Frame;
struct FrameStorage;
struct GlobalNode;
struct ContextRec;
struct ScriptRec;
struct TemplateRec;
struct ClassRec;
struct TryCatchState;
struct ContextScopeState;
struct CallbackState;
struct CallbackRecord;
}  // namespace detail

/// A native function callable from script. A plain function pointer: no
/// std::function anywhere in a call path, per the cost rules. Embedder state
/// comes in through `CallbackData`, not through a capture.
using FunctionCallback = void (*)(const CallbackInfo& info);

/// Work posted to an isolate's thread with `Isolate::PostJob` and run by
/// `Isolate::PumpJobs`. It is not inside a call, so it gets the isolate and its
/// embedder pointer and nothing else - no receiver, no arguments, no return
/// slot, because there is no call to have them.
using JobCallback = void (*)(Isolate& isolate, CallbackData data);

/// Run promptly on the isolate's thread by `Isolate::RequestInterrupt`, in the
/// middle of whatever script was doing. Same signature as a `JobCallback` and a
/// deliberately different name, because the contract is not the same one: this
/// may read and make values but must not call a function or run a script. See
/// `Isolate::RequestInterrupt`.
using InterruptCallback = void (*)(Isolate& isolate, CallbackData data);

/// The static type of `T` as a runtime question. Specialised in `unibind/value.h`
/// for every tag type.
template <class T>
struct TypeCodeOfTag;

template <class T>
inline constexpr TypeCode TypeCodeOf = TypeCodeOfTag<T>::value;

}  // namespace ub
