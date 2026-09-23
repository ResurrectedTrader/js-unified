# What each area of the suite needs the backend to define.
#
# An operation that a backend declares but does not define is a *link error at
# the call site* (docs/status.md), not a runtime failure - so a suite that
# simply called it would not build. Areas are therefore gated at compile time,
# and the gate is decided by looking at what the built backend library actually
# defines.
#
# Each entry is  <CAPABILITY>|<symbol>[,<symbol>...]  where a symbol is either
# `Name` (a free function in ub::detail) or `Name@Scope` (a member of
# ub::Scope). A capability is present when every one of its symbols is.
#
# Adding a test area means adding a row here and a UNIBIND_TEST_CASE(<CAPABILITY>,
# ...) in the sources. Nothing else knows the list.

set(UNIBIND_CAPABILITIES
    # A row no backend can satisfy, so the suite always carries one skipped case
    # and the skip path - compiled out, listed, reported - is exercised on every
    # backend rather than only on the day one falls behind.
    "NOTHING|NoBackendDefinesThis"
    "VALUES|MakeUndefined,MakeNull,MakeBoolean,MakeNumber,MakeInteger,MakeUnsigned,MakeString,KindOf,IsType"
    "FRAMES|OpenFrame,CloseFrame,EscapeSlot,CurrentFrame,IsolateOf"
    "EQUALITY|StrictEquals,SameValue,LooseEquals"
    "COERCION|ToBoolean,ToNumber,ToInt32,ToUint32,ToJsString,ToJsObject"
    "STRINGS|Utf8Length,WriteUtf8,ToStdString"
    "OBJECTS|MakeObject,GetProperty,SetProperty,GetIndex,SetIndex,HasProperty,HasOwnProperty,DeleteProperty,DefineProperty,GetOwnPropertyNames,GetPrototype,SetPrototype"
    "ARRAYS|MakeArray,ArrayLength"
    "FUNCTIONS|MakeFunction,CallFunction,ConstructObject,CallbackArgument,CallbackArgumentCount,CallbackThis,CallbackHolder,CallbackIsConstruct,CallbackDataOf,SetReturnSlot,SetReturnUndefined,SetReturnNull,SetReturnBoolean,SetReturnNumber,SetReturnInteger"
    "EXTERNALS|MakeExternal,ExternalData"
    "GLOBALS|MakeGlobal,DuplicateGlobal,ReleaseGlobal,GlobalToSlot"
    "EXCEPTIONS|ThrowValue,ThrowError,MakeError,TryCatchOpen,TryCatchClose,TryCatchHasCaught,TryCatchException,TryCatchMessage,TryCatchReThrow,TryCatchReset"
    "STACK_TRACE|TryCatchStackTrace"
    "SCRIPTS|CompileScript,RunScript,ReleaseScript"
    "REALMS|NewContext,RetainContext,ReleaseContext,ContextIsolate,ContextGlobalObject,ContextEnter,ContextLeave"
    "HEAP|RequestGarbageCollection@Isolate,GetHeapStatistics@Isolate"
    "SYMBOLS|MakeSymbol,MakeSymbolFor,GetWellKnownSymbol,SymbolDescription"
    "PROPERTY_ATTRIBUTES|GetPropertyAttributes"
    "OBJECT_ACCESSORS|SetAccessorProperty"
    "TEMPLATES|NewObjectTemplate,NewFunctionTemplate,TemplateSetConstant,TemplateSetMethod,TemplateSetAccessor,TemplateSetTemplate,TemplateSetClassName,TemplateInherit,TemplatePrototype,TemplateInstance,TemplateNewInstance,TemplateGetFunction,TemplateHasInstance"
    "SYMBOL_METHODS|TemplateSetSymbolMethod,GetWellKnownSymbol"
    "INTERCEPTORS|TemplateSetNamedHandler,TemplateSetIndexedHandler,NewObjectTemplate,TemplateNewInstance"
    "CLASSES|NewClass,ClassSetConstructor,ClassPrototypeTemplate,ClassConstructorTemplate,ClassInstanceTemplate,ClassGetConstructor,ClassInstantiate,ClassHasInstance,GetNativeBox"

    # --- decisions 14-25 (docs/status.md) ---------------------------------
    #
    # OWNERSHIP and CALLABLE_CLASS name no new symbol, because neither decision
    # added one: shared ownership is entirely in the header templates, and
    # construct-without-`new` only widened `ClassSetConstructor`. They are rows
    # so that the areas are listed and skippable like every other - a backend
    # that has the class machinery has these by construction.
    "OWNERSHIP|ClassInstantiate,GetNativeBox"
    "CALLABLE_CLASS|ClassSetConstructor,ClassInstantiate"

    # Two roots over one value, asked of the engine rather than of the roots.
    "GLOBAL_IDENTITY|GlobalStrictEquals,GlobalSameValue,GlobalStrictEqualsSlot,GlobalSameValueSlot"

    # Stopping a running script from another thread, and telling that stop apart
    # from an ordinary throw.
    "TERMINATION|TerminateExecution@Isolate,IsExecutionTerminating@Isolate,CancelTerminateExecution@Isolate,TryCatchHasTerminated"

    # A stack read frame by frame, rather than as the engine's own text. The
    # text form is STACK_TRACE above and is older.
    "STACK_FRAMES|CaptureStack,TryCatchStackFrames"

    # Where a caught exception was raised, with the line's text - including a
    # syntax error, which has no stack frame to read it from.
    "MESSAGE_LOCATION|TryCatchLocation"

    # Bytes out of one compile, into the next.
    "CODE_CACHE|CompileScriptWithCache,ScriptUsedCodeCache,ScriptCreateCodeCache"

    # A native stack ceiling that turns runaway recursion into an exception.
    # Like the two above it names no new entry point - the option rides on
    # `Isolate::New` - and is a row so the area is listed.
    "STACK_LIMIT|New@Isolate"

    # Bulk data in and out, and moving a value between isolates.
    "BINARY_DATA|MakeArrayBuffer,ArrayBufferByteLength,ArrayBufferCopyOut,MakeTypedArray,TypedArrayElementType,TypedArrayLength,TypedArrayByteOffset,TypedArrayBuffer,TypedArrayCopyOut"
    "SERIALIZATION|SerializeValue,DeserializeValue"

    # Work that is not a call, in three parts: a promise the embedder settles,
    # the queue a foreign thread posts to, and the interrupt it uses to say
    # "look now". Separate rows because a backend can arrive at them separately.
    "PROMISES|MakePromise,ResolvePromise,RejectPromise,PromiseStateOf,PumpJobs@Isolate"
    "JOBS|PostJob@Isolate,PumpJobs@Isolate"
    "DELAYED_JOBS|PostDelayedJob@Isolate,PumpJobs@Isolate"
    "INTERRUPTS|RequestInterrupt@Isolate"

    # The observable half of the workerThreads hint.
    "WORKER_THREADS|WorkerThreads@Platform"

    # Being asked what to do about a heap that is about to hit its ceiling,
    # rather than being told afterwards that it did. One engine has the hook and
    # the other has nothing of the kind, so this is the row that gates a genuine
    # "this engine cannot" rather than a backend that has not caught up. Being
    # told about the *failure* needs no row: `PlatformOptions::onEngineFault` is
    # a field rather than an entry point and every backend honours it.
    "HEAP_LIMIT|SetHeapLimitCallback@Isolate"
)
