# unibind (`ub::`)

One embedding API over several JavaScript engines, chosen at the link. You
write against `ub::` and compile once; whether V8 or SpiderMonkey is underneath
is decided by which library you link, and nothing in your code - or in what it
compiled to - says which.

```cpp
const ub::Platform platform;                 // process-wide, once
const auto isolate = ub::Isolate::New();     // one heap, one thread
const ub::HandleScope scope(*isolate);       // handles live here
const auto context = ub::Context::New(*isolate);
const ub::ContextScope entered(*context);

const auto result = ub::Evaluate(*context, "1 + 1");
const int sum = result->To<ub::Integer>()->Int32Value();   // 2
```

| | |
|---|---|
| Public API | complete: values, objects, accessors, interceptors, symbols, classes with native state, exceptions, realms, promises and jobs, termination, binary data, structured clone, compiled-code caching, engine-fault reporting |
| V8 15.6 | implements all of it |
| SpiderMonkey 153.3.0esr | implements all of it except the near-heap-limit hook, which its engine does not have - a call to that one does not link there, on purpose |
| Tests | one suite, written once against `ub::`: 297 cases, green on both backends, every case compared backend against backend with no divergences |
| Not here | a debugger, and cross-realm access control - see [Limits](#limits) |

> **Read [`docs/gotchas.md`](docs/gotchas.md) before you lose a day to one of
> them.** It is sixty-odd traps indexed by what you were doing when it bit you,
> and it opens with thirteen you will not diagnose from the symptom: twelve
> give a *wrong answer and no error at all* - a `TypeError` that arrives as a
> `SyntaxError`, a cached blob that runs a different script than the one you
> asked for, a promise continuation that simply never happens - and the
> thirteenth gives a loud error that blames something else entirely. Ten
> minutes there is the best-value reading in this repository.

**The two engines do not have the same rules, and the stricter one is what this
API is shaped by.** SpiderMonkey roots a GC value through `JS::Rooted`, which
must live on the stack, must be destroyed in reverse order of construction, and
cannot be moved or copied or put in a container - and its collector *moves* what
it roots, so a value copied out of a root is not merely possibly-freed but
possibly-stale. V8 has none of those rules. An abstraction only V8 could
implement honestly would be worth nothing, so what is here is what both can
implement honestly rather than what either would have designed alone - and the
handle model is where that bites hardest.

The rest of [`docs/`](docs/) is the design: [`lifetimes.md`](docs/lifetimes.md)
for the handle model everything else follows from,
[`status.md`](docs/status.md) for the twenty-eight decisions a backend author has
to know, [`testing.md`](docs/testing.md) for where the two engines differ and
what the suite asserts instead, [`spidermonkey.md`](docs/spidermonkey.md) for
what writing the second backend cost, and
[`licensing.md`](docs/licensing.md) for what you owe whom when you ship this.

---

## Contents

- [What you have to supply](#what-you-have-to-supply)
- [Building the library](#building-the-library)
- [Using it from your project](#using-it-from-your-project)
- [The API, from nothing to a working embedding](#the-api-from-nothing-to-a-working-embedding)
  - [1. Platform, isolate, context](#1-platform-isolate-context)
  - [2. Handles and scopes](#2-handles-and-scopes)
  - [3. Values and objects](#3-values-and-objects)
  - [4. Native functions](#4-native-functions)
  - [5. Binding a native class](#5-binding-a-native-class)
  - [6. Calling script, and catching what it throws](#6-calling-script-and-catching-what-it-throws)
  - [7. Interceptors: an object that answers for every property](#7-interceptors-an-object-that-answers-for-every-property)
  - [8. Promises, jobs, and the drain](#8-promises-jobs-and-the-drain)
  - [9. Stopping a runaway script](#9-stopping-a-runaway-script)
  - [10. When the engine itself is in trouble](#10-when-the-engine-itself-is-in-trouble)
- [Rules an embedder must know](#rules-an-embedder-must-know)
- [What it costs](#what-it-costs)
- [Gotchas worth knowing before you start](#gotchas-worth-knowing-before-you-start)
- [What the name claims](#what-the-name-claims)
- [Limits](#limits)
- [Layout](#layout)

---

## What you have to supply

unibind is the abstraction, not the engine. A consumer brings:

| | |
|---|---|
| **The engine** | a prebuilt static V8 (`include/` + `v8_monolith.lib`) or SpiderMonkey (`include/` + `spidermonkey.lib`), of the architecture you are building, which **you link yourself**. Building *this tree* no longer needs one - the build fetches the pinned version (see below) - but an installed prefix ships unibind's library and not the engine's, so a consumer supplies and links it. `unibind.props` and the CMake package already know the path the prefix was built against and the system libraries that go with it. See [`dependencies/README.md`](dependencies/README.md). |
| **x86 (Win32) or x64** | both, built and tested. A prefix is installed for one of them: the library, the engine and the generated `config.h` all have to agree, and `config.h` names the architecture in the ABI tag so that mixing them is LNK2038 rather than corruption. The *engine* is the choice that is not baked in - your objects link against either backend. |
| **The static CRT** | `/MT`, or `/MTd` against a debug engine tree. The engines link it; a `/MD` consumer fails at link with the MSVC STL's own `RuntimeLibrary` mismatch. |
| **MSVC toolset 14.44 or newer** | SpiderMonkey's floor, not a preference: its STL headers call helpers that ship in that toolset's `libcpmt.lib`, and an older one fails with undefined `__std_*`. The V8 tree here is built the same way. |
| **C++23, compiled by clang-cl** | `/std:c++latest` on MSBuild, `cxx_std_23` from CMake - and the **ClangCL toolset**, not MSVC's `cl.exe`. This is a constraint, not a preference: `cl.exe` instantiates `std::optional<Local<T>>` while `Local<T>` is still being defined and fails with C7637 and a cascade behind it. clang-cl accepts the headers, and the whole tree, its CI and the example are built with it. |
| **64-bit host tools** | `PreferredToolArchitecture=x64`. This is not a performance preference; see [Gotchas](#gotchas-worth-knowing-before-you-start). |

The engine's *headers* are not among them: your code includes no engine header,
which is the whole point, so the engine's own required defines
(`V8_GN_HEADER`, `STATIC_JS_API`, `XP_WIN`) applied when the backend was
compiled and do not apply to you.

## Building the library

```powershell
cmake --preset v8                                  # or --preset spidermonkey
cmake --build build/v8 --config Release --parallel 1
ctest --preset v8
```

Four presets, one per engine and architecture: `v8`, `spidermonkey`, `v8-x64`
and `spidermonkey-x64`. The unsuffixed ones are x86, which is what everything
here was first measured in.

A fresh clone builds with nothing placed by hand. Configuring fetches the
pinned engine build into `dependencies/` when one is not already there - a
little under 300 MB for V8 and a little under 150 MB for SpiderMonkey, the exact
figure depending on the architecture, and
[`dependencies/README.md`](dependencies/README.md) lists all four. It happens
once per version *and* architecture, unpacked into a staging sibling and renamed
into place in one move, so an interrupted download cannot be mistaken for a
complete one.

Point it at an engine you already have and nothing is downloaded:
`-DUNIBIND_V8_DIR=...` / `-DUNIBIND_SPIDERMONKEY_DIR=...`, or `-DUNIBIND_FETCH_ENGINES=OFF`
to refuse the fetch outright and be told what to unpack where.
`UNIBIND_V8_VERSION` / `UNIBIND_SPIDERMONKEY_VERSION` pick the version, and a version
bump repoints the tag, the asset and the directory together rather than
silently reusing the old library.

The number `ctest` prints is a little larger than 297 and depends on the tree,
because it registers the suite's cases *and* a few things that cannot be cases
among others: the whole suite again in one process, four checks that each need
a process of their own (five on V8, which adds `unibind/interop/v8.h`'s) (plus two more in a Debug build, which are the two
checked-build deaths), the benchmark, and the
cross-backend `parity` comparison (which only compares what has actually been
built). **297 cases is the figure that means the same thing everywhere** - it is
what the test binary itself reports, on either backend. The assertion count is not: a case may assert a
different number of times on each engine, so V8 counts 8906 and SpiderMonkey
8879, and neither number is the one to compare a run against.

CI pins `windows-2022` and MSVC **14.44** on purpose: that is the toolset both
engine archives were built with, and therefore the one a consumer links
against. Following `windows-latest` would test a toolchain nobody chose. A
non-blocking canary does build on `windows-latest` - Visual Studio 2026, MSVC
14.51 - and currently passes, which is how the pin will eventually be moved.

Each engine's workflow runs the suite twice, x86 and x64, as separate jobs: the
handle is a different size in the two, the backends' frames are different
sizes, and the x64 V8 archive brings a different allocator - so one of them
passing says nothing about the other.

Then install a prefix for consumers:

```powershell
cmake --install build/v8 --config Release --prefix C:\unibind
```

```
C:\unibind\
  include\unibind\*.h          the public API
  include\unibind\config.h     generated, and the same for every backend
  lib\unibind_backend_v8.lib   one engine, as a link input
  msbuild\unibind.props        for a .vcxproj
  lib\cmake\unibind\           for find_package(unibind)
```

**The backend is chosen at the link, not at the compile.** Nothing a consumer
compiles differs between engines: no public header names one, and the generated
`config.h` describes the architecture and the handle layout and says nothing
about an engine. So installing the other backend into the **same prefix** adds
one library and rewrites an identical header, and one set of your object files
links against either. `examples/embed` does exactly that - one object library,
two executables - and both run.

Installing the other *architecture* into the same prefix is not: one library
name per backend, and nothing in the layout to tell an x86 one from an x64 one.
Install a prefix per architecture. Reaching for the wrong one is caught rather
than documented - `unibind.props` and `find_package(unibind)` both check, and the
generated `config.h` carries the architecture in its ABI tag, so an object that
got past both fails to link.

## Using it from your project

You do not have to build this tree to use it. Every `vX.Y.Z` tag publishes a
prefix per architecture and flavor on the
[releases page](https://github.com/ResurrectedTrader/unibind/releases), named
`unibind-X.Y.Z-<x86|x64>-<release|debug>-msvc14.44.zip`, with a `.sha256` next
to each. One archive holds the headers, `config.h`, and **both** backend
libraries, so the same objects link either engine. `engines.txt` in the archive
names the engine versions those libraries were compiled against. The engines are
not included: fetch the matching ones from the releases that
`cmake/UnibindEngines.cmake` names, and point `UnibindV8Dir` /
`UnibindSpiderMonkeyDir` (or `UNIBIND_V8_DIR` / `UNIBIND_SPIDERMONKEY_DIR`) at
them. The `debug` flavor links the engines' debug builds and `/MTd`.

### MSBuild (`.vcxproj`)

One line, in the property-sheet slot every C++ project already has:

```xml
<ImportGroup Label="PropertySheets">
  <Import Project="C:\unibind\msbuild\unibind.props" />
</ImportGroup>
```

That sets the include directory (the public headers and the generated
`config.h` beside them), and the link inputs - `unibind_backend_<engine>.lib`,
the engine's own library, and the fourteen-odd system libraries it was built
against, which is the list nobody should be assembling by hand. Switching
`UnibindBackend` changes only the second list.

Properties you may set before the import:

| | |
|---|---|
| `UnibindBackend` | `v8` or `spidermonkey`. Defaults to whichever the prefix holds, and to `v8` when it holds both. |
| `UnibindRoot` | the prefix. Defaults to the props file's own parent, so normally unset. |
| `UnibindV8Dir`, `UnibindSpiderMonkeyDir` | where your engine lives. Defaults to what the prefix was built against. |

Your project still has to say three things for itself, because they are decided
before any property sheet is imported: the `Platform` the prefix was installed
for, `MultiThreaded` (or `MultiThreadedDebug`), and - on `Win32` -
`PreferredToolArchitecture=x64`. The props file checks all three and fails with
a sentence rather than letting the link fail with something unreadable. [`examples/embed/embed.vcxproj`](examples/embed/embed.vcxproj)
is a complete, working consumer: under a hundred lines, of which one is unibind.
[`examples/README.md`](examples/README.md) is the command that builds it, both
ways, against a prefix you installed.

### CMake

```cmake
find_package(unibind REQUIRED)            # or COMPONENTS spidermonkey
target_link_libraries(app PRIVATE unibind::unibind)
```

`UNIBIND_V8_DIR` / `UNIBIND_SPIDERMONKEY_DIR` relocate the engine if the prefix was
moved to a machine where it lives elsewhere.

Three targets, and the split is what makes one compile serve both engines:

| | |
|---|---|
| `unibind::headers` | the public headers and `config.h`. Names no engine. |
| `unibind::backend_v8`, `unibind::backend_spidermonkey` | one engine, as a link input. One per backend the prefix holds. |
| `unibind::unibind` | the headers plus one backend - what a program that links one engine wants, and the only one most consumers name. |

```cmake
find_package(unibind REQUIRED COMPONENTS v8 spidermonkey)

add_library(app_objects OBJECT app.cpp)
target_link_libraries(app_objects PRIVATE unibind::headers)   # compiled once

add_executable(app_v8 $<TARGET_OBJECTS:app_objects>)
target_link_libraries(app_v8 PRIVATE unibind::backend_v8)

add_executable(app_sm $<TARGET_OBJECTS:app_objects>)
target_link_libraries(app_sm PRIVATE unibind::backend_spidermonkey)
```

One *program* still links one engine - two of them in one executable is two
copies of every engine symbol - but building both from one set of objects is an
ordinary thing to want, and it is how `examples/embed` proves the promise.

### The config.h that goes with the library

`include/unibind/config.h` is **generated at configure time** and carries the
handle layout and the storage sizes of `HandleScope`, `TryCatch` and
`ContextScope`. A consumer compiled against a copy that disagreed with the
library it calls would lay those types out one way while the library reads them
another: no compile error, no link error, just corruption.

What prevents it is a `#pragma detect_mismatch` naming every value in the
header, so an object compiled against a different one is **LNK2038 at link
time**, which is the failure you want.

**What the tag does *not* carry is the backend**, and that is deliberate rather
than an omission. An object compiled against these headers is the same object
whichever engine it is linked with, so a tag naming the engine would refuse a
legitimate cross-backend link while catching nothing a real mismatch would not
also trip. What is left is the set a difference in which really is corruption:
the architecture, the handle layout, and the storage budgets.

The rule the tag therefore stops enforcing is enforced in the build instead: no
public header may name the backend. `tools/headers_only/backend_neutral.cmake`
reads them and fails the build if one does - it cannot be a compile error,
because `#if UNIBIND_BACKEND_V8` against a macro nobody defines is silently
false rather than wrong, which is exactly the failure worth catching. The
storage budgets carry a rule of their own for the same reason: **each is the
maximum across every backend for that architecture**, not a per-backend figure,
so a future backend with a bigger frame raises the number for everyone rather
than getting one of its own.

---

## The API, from nothing to a working embedding

Everything below compiles, and
[`examples/embed/main.cpp`](examples/embed/main.cpp) assembles most of it into
one runnable program: the platform, the isolate, the realm, a bound class, a
native function, a script, a call back into script, an exception caught, and the
pump. Escaping a handle (section 2), interceptors (section 7) and termination
(section 9) are not in it - `tests/cases/` is where those run.

```cpp
#include "unibind/unibind.h"
```

There is one header. Everything is in `namespace ub`.

Section 1 makes an isolate and a realm, so it spells them `*isolate` and
`*context` - the one is a `std::unique_ptr`, the other a `std::optional`. Every
section after it is written as the inside of a function that was handed
`ub::Isolate& isolate` and `const ub::Context& context`, which is what your own
code will look like.

### 1. Platform, isolate, context

```cpp
const ub::Platform platform;               // before the first isolate, after the last
```

One per process. It is an object rather than a pair of free functions because
that is the only shape an embedder cannot get wrong.

`Platform::BackendName()` says which engine you linked and
`Platform::BackendVersion()` which build of it - for a log line or a bug
report. Both are runtime questions because the engine is a link-time choice,
and neither is for branching on: the version string's shape is the engine's own
and this API promises nothing about it.

```cpp
const auto isolate = ub::Isolate::New();   // std::unique_ptr<Isolate>, may be null
if (isolate == nullptr) {
    return 1;                               // no heap - including: this thread already has one
}
```

An isolate is one JavaScript heap, bound to the thread that made it. **At most
one is alive per thread**; one after another is fine. That is SpiderMonkey's
rule, kept on both backends so a program cannot work on one and not the other.

```cpp
const ub::HandleScope scope(*isolate);     // see the next section
const auto context = ub::Context::New(*isolate);   // std::optional<Context>
const ub::ContextScope entered(*context);  // this realm is current for the block
```

A `Context` is a realm: its own global object, its own built-ins. It is not a
handle and does not live in a scope - it is reference counted and outlives the
call that made it, which is the point of a realm. An isolate can hold any
number.

```cpp
const auto result = ub::Evaluate(*context, "40 + 2", {.resourceName = "example.js"});
if (!result) {
    // it threw, or the source did not compile: see section 6
}
const auto asInteger = result->To<ub::Integer>();
if (asInteger) {
    std::printf("%d\n", asInteger->Int32Value());
}
```

`Evaluate` compiles and runs. To run the same source many times, keep the
compiled form:

```cpp
const auto script = ub::Script::Compile(*context, source, {.resourceName = "loop.js"});
if (script) {                                   // empty if the source did not compile
    for (int i = 0; i < 10; ++i) {
        const auto value = script->Run(*context);   // empty if it threw
    }
}
```

A `Script` is an artefact, not a handle: it holds its own root, it outlives any
scope, and it may be run **in a realm other than the one it was compiled in** -
where it sees that realm's globals. Compile once, run in every sandbox.

Both engines compile a function's body on its first call. To keep the compiled
form across runs, compile everything up front and keep the blob:

```cpp
const auto eager = ub::Script::Compile(*context, source, {.resourceName = "app.js"},
                                       ub::CompileOptions::EagerCompile);
const auto blob = eager->CreateCodeCache();          // covers every function, not only those that ran
// next run:
const auto cached = ub::Script::CompileWithCache(*context, source, *blob, {.resourceName = "app.js"},
                                                 ub::CompileOptions::EagerCompile);
```

A blob made after a lazy compile covers the top level and whatever had run.
With `EagerCompile`, `CompileWithCache` uses a good blob as it is and compiles
eagerly when the blob is stale - which is the moment a fresh one is worth
making.

### 2. Handles and scopes

This is the part that will bite first, so it is the part to read twice. The
whole model is in [`docs/lifetimes.md`](docs/lifetimes.md); the rules are these.

**A `Local<T>` names a slot in the innermost open frame.** A `HandleScope` opens
a frame. Every handle made while it is open dies when it closes.

```cpp
{
    const ub::HandleScope inner(*isolate);
    const auto text = ub::String::New(*isolate, "hello");   // lives in `inner`
}   // `text` is gone. Reading it now is undefined behaviour.
```

**A scope is a stack object and closes in reverse order of opening.** `operator
new` is deleted and copy and move are deleted, so it cannot be heap-allocated,
returned, or made a member of something that is. That is not caution: on
SpiderMonkey the frame *is* a `JS::Rooted`, which has exactly those rules.

**Within its frame, a `Local` is an ordinary value.** Copy it, pass it, put it in
a `std::vector`. It is trivially copyable, and small: a frame pointer and a
32-bit ordinal, so 8 bytes on x86 and 16 on x64.

**To return a handle, escape it.** A `HandleScope` cannot hand anything to its
caller; an `EscapableHandleScope` can:

```cpp
ub::Local<ub::Object> MakePoint(ub::Isolate& isolate, const ub::Context& context, int x, int y) {
    ub::EscapableHandleScope scope(isolate);          // NOT const: Escape is non-const

    const auto point = ub::Object::New(context);
    if (!point) {
        return {};                                     // an empty handle, not undefined
    }
    (void)point->Set(context, "x", ub::Integer::New(isolate, x));
    (void)point->Set(context, "y", ub::Integer::New(isolate, y));

    return scope.Escape(*point);                       // into the caller's frame
}
```

Whether a scope can escape is decided when it is **opened**, not when `Escape` is
called - which is why there are two types. V8 has to reserve the parent's slot up
front; SpiderMonkey does not care. Escaping more than one handle from a frame is
allowed (the second and later ones cost a global root on V8, and nothing on
SpiderMonkey).

A bare `return local;` out of a function that opened its own scope returns a
dangling handle. Nothing makes that a compile error; a checked build (`Debug`,
`UNIBIND_HANDLE_CHECKS`) diagnoses it at the point of *use*.

**To outlive every frame, use a `Global<T>`.** A cached constructor, a callback
you were handed, a value a C++ object has to remember:

```cpp
class Handler {
   public:
    Handler(ub::Isolate& isolate, const ub::Local<ub::Function>& fn) : fn_(isolate, fn) {}

    void Fire(const ub::Context& context) {
        const ub::HandleScope scope(context.GetIsolate());
        const auto fn = fn_.Get(context.GetIsolate());       // back into the current frame
        (void)fn.Call(context, context.GlobalObject());
    }

   private:
    ub::Global<ub::Function> fn_;                          // move-only, one engine root
};
```

A `Global` is move-only; copying one is spelled `Duplicate()`, because a root
costs something and that should be visible. Two `Global`s can be two roots over
one value, so compare them with `StrictEquals` / `SameValue`, never by comparing
handles - and those need no open scope.

**An empty handle is not `undefined`.** A value-making operation that could not
grow the frame hands back an empty `Local`; `IsEmpty()` asks. It is the one
failure this model cannot make impossible, and it is deliberately not
`undefined`, because running out of memory must not arrive as a plausible value.

### 3. Values and objects

Everything that can fail returns `std::optional`, and an empty one always means
the same thing: *no value was produced*. Why is a second question -
`TryCatch::HasCaught()` for a script throw, `Isolate::HasPendingException()` for
an engine failure, `TryCatch::HasTerminated()` for a stop from another thread.

```cpp
const auto object = ub::Object::New(context);                    // optional<Local<Object>>
const auto name = ub::String::New(isolate, "widget");            // optional<Local<String>>, strict UTF-8
const auto line = ub::String::NewFromUtf8(isolate, fromSocket);  // lossy: bad bytes become U+FFFD
const auto ok = object->Set(context, "name", *name);              // optional<bool>
const auto back = object->Get(context, "name");                   // optional<Local<Value>>
const std::string text = back->ToString(context)->Utf8Value();    // "widget"
```

`String::New` is strict: bytes that are not UTF-8 make no string, rather than a
row of replacement characters where the caller thought it had text.
`String::NewFromUtf8` is V8's lossy decode, for bytes that are meant to be
repaired - each maximal invalid sequence becomes one U+FFFD, by the WHATWG rule
V8 follows, and the same string comes out of both engines because the repair is
done in the header.

Widening is implicit and narrowing is checked:

```cpp
ub::Local<ub::Value> value = *name;             // Local<String> -> Local<Value>, free
const auto asString = value.To<ub::String>();    // optional: empty if it is not one
if (value.Is<ub::Function>()) { /* ... */ }
switch (value.Kind()) { case ub::ValueKind::Object: break; default: break; }
```

`ValueKind` answers *what can I do with this*, so a Date, a RegExp, a Proxy and a
typed array all report `Object`. Arrays, functions and the rest of the specific
questions are `Is<T>()` / `To<T>()`.

Arrays, keys, prototypes and attributes are where you would expect:

```cpp
const auto array = ub::Array::New(context, 3);
(void)array->Set(context, 0u, ub::Integer::New(isolate, 7));
const std::uint32_t length = array->Length();

const auto keys = object->GetOwnPropertyNames(context, {.includeSymbols = true});
(void)object->DefineOwnProperty(context, *name, *name, ub::PropertyAttribute::ReadOnly);
const auto prototype = object->GetPrototype(context);

// A property that runs native code on every read (and write, given a setter) -
// on this one object, where a template would put it on every instance.
(void)object->SetAccessor(context, "level", &ReadLevel, &WriteLevel, ub::CallbackData::For(state));
```

Bulk numeric data goes across in one crossing rather than N:

```cpp
const std::array<float, 4> samples{1.0F, 2.0F, 3.0F, 4.0F};
const auto view = ub::TypedArray::New<float>(context, samples);   // a Float32Array
```

The bytes are copied, in both directions, and `unibind/value.h` says at length why
there is no borrowing alternative.

A typed array and a `DataView` are both an `ArrayBufferView`, as in V8, and code
that only moves bytes can take either:

```cpp
const auto header = ub::DataView::New(context, *buffer, 4, 12);   // no element width of its own
if (const auto any = value.To<ub::ArrayBufferView>()) {
    std::vector<std::byte> bytes(ub::ByteLength(*any));
    (void)ub::CopyBytes(*any, bytes);                              // its own range, not the buffer's
}
```

### 4. Native functions

A callback is a plain function pointer - there is no `std::function` anywhere in
a call path. Embedder state arrives through `CallbackData`, which is typed:

```cpp
struct Counters { int calls = 0; };

void Bump(const ub::CallbackInfo& info) {
    auto* counters = info.Data<Counters>();        // null if it was declared with another type
    if (counters == nullptr) {
        info.ThrowTypeError("no counters");
        return;                                    // return promptly after throwing
    }
    ++counters->calls;

    std::int32_t by = 1;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return;                                // the coercion ran script and it threw
        }
        by = *asInt;
    }
    info.GetReturnValue().Set(counters->calls * by);
}

Counters counters;                                 // must outlive every call
const auto fn = ub::Function::New(context, &Bump, ub::CallbackData::For(counters));
(void)context.GlobalObject().Set(context, "bump", *fn);
```

`info[i]` past the end is `undefined`, as script would see. A callback that
writes nothing to `GetReturnValue()` returns `undefined`.

A function's data can instead be a *script value*, read back as `info.Data()` -
one native callback behind many functions, each closing over its own value, and
nothing for the embedder to keep alive: the value lives exactly as long as the
function.

```cpp
void Greet(const ub::CallbackInfo& info) {
    const auto greeting = info.Data().ToString(info.GetContext());  // this function's own string
    if (greeting) {
        (void)info.GetReturnValue().Set(greeting->Utf8Value() + ", world");
    }
}

for (const char* word : {"hello", "goodbye"}) {
    const auto fn = ub::Function::New(context, &Greet, *ub::String::New(isolate, word));
    (void)context.GlobalObject().Set(context, word, *fn);
}
```

**A function made this way is callable, not constructable.** `new bump()` is a
TypeError before the callback runs. Something `new`-able is asked for on purpose,
with a `FunctionTemplate` (callable *and* constructable) or a `Class<T>`
(constructable only, unless you opt into both).

### 5. Binding a native class

```cpp
struct Counter {
    explicit Counter(std::int32_t start) : value(start) {}
    std::int32_t value = 0;
};

std::unique_ptr<Counter> MakeCounter(const ub::CallbackInfo& info) {
    std::int32_t start = 0;
    if (info.Length() > 0) {
        const auto asInt = info[0].ToInt32(info.GetContext());
        if (!asInt) {
            return nullptr;
        }
        start = *asInt;
    }
    if (start < 0) {
        info.ThrowTypeError("a counter starts at zero or above");
        return nullptr;              // null after a throw declines the construction
    }
    return std::make_unique<Counter>(start);
}

void Increment(Counter& self, const ub::CallbackInfo& info) {
    info.GetReturnValue().Set(++self.value);
}

void ReadValue(Counter& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.value);
}

void WriteValue(Counter& self, const ub::Local<ub::Value>& value, const ub::PropertyCallbackInfo& info) {
    if (const auto asInt = value.ToInt32(info.GetContext())) {
        self.value = *asInt;
    }
}

void HowMany(const ub::CallbackInfo& info) { info.GetReturnValue().Set(0); }

// Declared once per isolate; instantiated into any number of realms.
const auto counterClass = ub::Class<Counter>::New(isolate, "Counter");
counterClass.Construct<&MakeCounter>();
counterClass.Method<&Increment>("increment");
counterClass.Accessor<&ReadValue, &WriteValue>("value");
counterClass.StaticMethod("howMany", &HowMany);

const auto constructor = counterClass.GetConstructor(context);
(void)context.GlobalObject().Set(context, "Counter", *constructor);
```

Callbacks are **template** arguments, not runtime ones, so a method is one
indirect call - the trampoline that unwraps `this` - with nothing per method to
store or keep alive. A method whose receiver is not a `Counter` throws a
TypeError before your code runs.

Recovering the native is checked, and cannot lie:

```cpp
Counter* self = ub::Class<Counter>::Unwrap(someValue);   // null unless it is exactly a Counter
```

**A wrapper owns a *share* of its native**, not the native. So an object the
embedder also holds is ordinary rather than a lifetime puzzle:

```cpp
const auto shared = std::make_shared<Counter>(7);
const auto wrapper = counterClass.Wrap(context, shared);        // script gets one; you keep one
const auto another = counterClass.Wrap(context, shared);        // two wrappers, one native
auto share = ub::Class<Counter>::UnwrapShared(*wrapper);       // outlive the wrapper
```

The native goes when the last share does, whoever holds it - which may be you,
after the isolate is gone. A native that must not be destroyed by the engine is
handed over with a no-op deleter, said once at the call site:
`std::shared_ptr<Counter>(&mine, [](Counter*) {})`.

### 6. Calling script, and catching what it throws

Calling a JavaScript function from native:

```cpp
const auto described = context.GlobalObject().Get(context, "describe");
const auto fn = described ? described->To<ub::Function>() : std::nullopt;
if (fn) {
    const auto argument = ub::String::New(isolate, "the answer");
    const std::array<ub::Local<ub::Value>, 1> arguments{*argument};
    const auto answer = fn->Call(context, context.GlobalObject(), arguments);
    if (!answer) {
        // it threw; see below
    }
}
```

Catching:

```cpp
{
    const ub::TryCatch caught(isolate);

    const auto result = ub::Evaluate(context, "throw new TypeError('nope')");
    if (!result && caught.HasCaught()) {
        if (caught.HasTerminated()) {
            return;                                   // a stop, not a throw: section 9
        }
        const std::string message = caught.Message(context).value_or("<none>");
        const auto where = caught.Location(context);       // script, line, column, the line's text
        const auto frames = caught.StackFrames(context);   // function, script, line
        const auto human = caught.StackTrace(context);     // the engine's own text
        const ub::Local<ub::Value> thrown = caught.Exception();
    }
}   // closing consumes what it caught
```

**Closing a `TryCatch` consumes the exception** unless `ReThrow()` was called. A
handler is a `catch` block, not an observer. The one thing it does not consume is
a termination: that continues outwards whatever you do, which is the whole point
of the facility.

Throwing from native is `info.Throw(kind, message)` inside a callback, or
`ub::Throw(isolate, kind, message)` outside one. Either way the throw takes
effect when you return to the engine, so return promptly and call nothing else
into the engine on the way out.

Two portability notes the suite had to learn: engines word their built-in
messages differently, so ask script `e.constructor.name` rather than parsing a
message; and `StackTrace` text is not parseable across engines, which is what
`StackFrames` is for. `Location` is the one to print a syntax error with: a
script that never compiled has no frame, but it does have a line.

### 7. Interceptors: an object that answers for every property

An interceptor is a catch-all for *any* property of an object - the thing a
sandbox or a scope object is made of. Each hook says whether it handled the
access; `Intercepted::No` means "carry on with the ordinary lookup", and that
third state is what keeps the object's own methods reachable.

```cpp
struct Scope {
    std::map<std::string, std::string, std::less<>> entries;
};

Scope* ScopeOf(const ub::PropertyCallbackInfo& info) {
    return ub::Class<Scope>::Unwrap(info.Holder());
}

ub::Intercepted ScopeGet(const ub::Local<ub::Name>& property, const ub::PropertyCallbackInfo& info) {
    Scope* self = ScopeOf(info);
    const auto asString = property.To<ub::String>();
    if (self == nullptr || !asString) {
        return ub::Intercepted::No;
    }
    const auto found = self->entries.find(asString->Utf8Value());
    if (found == self->entries.end()) {
        return ub::Intercepted::No;           // declining is what keeps `keys()` reachable
    }
    (void)info.GetReturnValue().Set(found->second);
    return ub::Intercepted::Yes;
}

ub::Intercepted ScopeSet(const ub::Local<ub::Name>& property, const ub::Local<ub::Value>& value,
                          const ub::PropertyCallbackInfo& info) {
    Scope* self = ScopeOf(info);
    const auto asString = property.To<ub::String>();
    const auto asText = value.ToString(info.GetContext());
    if (self == nullptr || !asString || !asText) {
        return ub::Intercepted::No;
    }
    self->entries[asString->Utf8Value()] = asText->Utf8Value();
    return ub::Intercepted::Yes;
}

std::optional<ub::PropertyAttribute> ScopeQuery(const ub::Local<ub::Name>& property,
                                                 const ub::PropertyCallbackInfo& info) {
    Scope* self = ScopeOf(info);
    const auto asString = property.To<ub::String>();
    if (self == nullptr || !asString || !self->entries.contains(asString->Utf8Value())) {
        return std::nullopt;                   // empty means: not intercepted
    }
    return ub::PropertyAttribute::None;
}

std::optional<ub::Local<ub::Array>> ScopeEnumerate(const ub::PropertyCallbackInfo& info) {
    Scope* self = ScopeOf(info);
    if (self == nullptr) {
        return std::nullopt;
    }
    const auto keys = ub::Array::New(info.GetContext(), static_cast<std::uint32_t>(self->entries.size()));
    if (!keys) {
        return std::nullopt;
    }
    std::uint32_t index = 0;
    for (const auto& [key, unused] : self->entries) {
        const auto name = ub::String::New(info.GetIsolate(), key);
        if (!name) {
            return std::nullopt;
        }
        (void)keys->Set(info.GetContext(), index++, *name);
    }
    return keys;
}

const auto scopeClass = ub::Class<Scope>::New(isolate, "Scope");
scopeClass.Construct<&MakeScope>();
scopeClass.Method<&ScopeKeys>("keys");         // reachable because the getter declines
scopeClass.SetHandler(ub::NamedPropertyHandler{.getter = &ScopeGet,
                                                .setter = &ScopeSet,
                                                .query = &ScopeQuery,
                                                .enumerator = &ScopeEnumerate});
```

Script then sees an object whose properties are yours: `s.answer = 42` lands in
the map, `'answer' in s` asks the query hook, `Object.keys(s)` asks the
enumerator, and `s.keys()` still finds the prototype's method because the getter
declined. An `IndexedPropertyHandler` is the same five hooks over `uint32_t`.

**A real sandbox adds a realm**, and one rule with it. Make a `Context` of its
own, hold its global object in a `Global<Object>`, and answer each hook out of
that realm - but **enter the realm before reading through an object that belongs
to it**:

```cpp
const auto scope = self->scope.Get(info.GetIsolate());
const ub::ContextScope inside(self->realm);       // required, not tidiness
const auto value = scope.Get(self->realm, property);
```

A value crosses realms freely, but a realm's *global object* is access-checked,
and reading a property of one while another realm is current fails - on V8 with
`TypeError: no access`, before any of your code runs. One line per hook, both
backends. [`tests/cases/sandbox_test.cpp`](tests/cases/sandbox_test.cpp) is the
whole composition: a second realm, an interceptor over every property, a `Global`
held across calls, and a prototype method that has to survive the interceptor.

### 8. Promises, jobs, and the drain

```cpp
(void)ub::Evaluate(context, "globalThis.ran = false; Promise.resolve().then(() => { ran = true });");
// ran === false
isolate.PumpJobs();
// ran === true
```

> **If you never call `PumpJobs`, promise continuations never run.** No error, no
> exception, no warning - `async` / `await` and `.then` compile and run and the
> continuation simply does not happen.

That is a documented requirement rather than a bug, and this sentence is the
difference between the two. Both engines *can* drain at moments of their own
choosing - V8's default policy does it after every call, SpiderMonkey's never
does - and unibind turns that off on both, so that a continuation runs at the same
observable moment whichever backend you linked. Call it on the isolate's own
thread with no native frame on the stack: after a `Script::Run`, at the bottom of
your loop.

An embedder settles its own promises:

```cpp
const auto promise = ub::Promise::New(context);      // hand this to script
// ... later, on the isolate's thread ...
(void)ub::Resolve(context, *promise, ub::Integer::New(isolate, 42));
isolate.PumpJobs();                                   // and its continuations run
```

`PostJob` queues work from any thread, and the same pump runs it - engine jobs
first, then posted work, then round again, so work that settles a promise sees
its continuations in the same pump. Posting does not wake anything: **posted work
runs when the script thread next pumps, and nothing accelerates that.** Whatever
is still queued when the isolate is destroyed is dropped, not run.

`PostDelayedJob(callback, data, delayInSeconds)` is the same with a floor on
when: V8's `PostDelayedTask`, for the timer you would otherwise keep beside the
isolate. It still runs only at a pump, so an embedder that sleeps between pumps
decides how late it can be.

### 9. Stopping a runaway script

```cpp
std::thread watchdog([&isolate] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    isolate.TerminateExecution();            // the one call that may come from another thread
});

{
    const ub::TryCatch caught(isolate);
    const auto result = ub::Evaluate(context, "for (;;) {}");
    if (!result && caught.HasTerminated()) {
        // stopped. There is no exception value, no message, no stack.
    }
}

watchdog.join();
isolate.CancelTerminateExecution();          // on the isolate's own thread: this is the way back
```

The unwind is not catchable from script - `try`/`finally` does not stop it - and
a `TryCatch` does not swallow it. Until it is cancelled, every operation that
would run script fails, which is settled rather than inherited: one engine would
happily run the next script.

`RequestInterrupt` is the other cross-thread call, and is narrow on purpose. It
runs your callback on the isolate's thread while script is running, where it may
make handles, read values and set flags - and **may not call a function, run a
script, or throw**. It is for a profiler tick, or for a watchdog that has to read
something on-thread before deciding to terminate.

> **Both of these reach script, not your own C++. A watchdog cannot save you
> from your own blocking callback.**

Every checkpoint either engine has is inside script. A native that spins, blocks
on a socket or waits on a lock runs to completion first; if it can block for long
enough to matter, it polls `isolate.IsExecutionTerminating()` itself and returns.

### 10. When the engine itself is in trouble

Everything above reports a failure to whoever asked for something. This is the
other direction: the engine noticing that *it* is failing, at a moment nobody
asked it anything. Without it, a long-running embedder finds out by dying.

```cpp
void OnFault(const ub::EngineFaultReport& report, ub::CallbackData data) {
    auto* log = data.As<MyLog>();                 // reserved before it was needed
    if (log != nullptr) {
        log->Record(report.fault, report.isolate, report.message);
    }
}

const ub::Platform platform({.onEngineFault = &OnFault,
                             .engineFaultData = ub::CallbackData::For(myLog)});
```

One callback with a kind, not one per kind. It lives on the `Platform` because
that is the only place that exists before the first isolate and after the last -
a failure inside `Isolate::New` has no isolate to have registered on - and
`report.isolate` names the heap when there was one. It is fixed for the life of
the `Platform` rather than settable, because it can arrive on any thread.

**What a handler may do is narrower than anywhere else in this API**: embedder
state and `TerminateExecution` yes; a handle, a value, a call into the engine,
script, a throw, and above all **an allocation** - no. The report that arrives
most often says allocation is failing, so allocating to report it is a bug in
every case and a crash in the interesting one. Reserve the buffer and open the
log file first.

Both kinds come from both engines. `EngineFault::Fatal` covers a failed engine
check of every sort - V8's `CHECK` and API misuse, SpiderMonkey's `MOZ_CRASH`
and `MOZ_RELEASE_ASSERT`, and the `DCHECK` / `MOZ_ASSERT` a debug engine adds -
and the process ends after your handler returns. It is the one hook to install
instead of each engine's own OOM, fatal and assertion handlers.

```cpp
std::size_t Rescue(ub::Isolate& isolate, std::size_t current, std::size_t initial,
                   ub::CallbackData data) {
    isolate.TerminateExecution();                 // stop the script that filled it
    return initial * 2;                           // and leave room to unwind in
}

isolate->SetHeapLimitCallback(&Rescue, ub::CallbackData::For(policy));
```

A heap about to hit `heapLimitBytes` is a *decision*, not a report, which is why
it is not one of the kinds: nothing has failed yet, and what happens next is
what you return. Raise the ceiling and terminate together - a bigger heap alone
just feeds the runaway script, and a stop alone has no room to unwind in - and
one bad script stops instead of the process ending.

**Only V8 has this hook, and a SpiderMonkey build does not link a call to it.**
That is this library's standing answer for an operation an engine cannot do: a
build error at your call site, not a field that compiles everywhere and fires in
half the builds.

---

## Rules an embedder must know

Each of these is a decision written down in a header, and each fails *silently*
if ignored - no compile error, no exception, no log line.

1. **One isolate alive per thread.** A second while the first lives is refused
   (`Isolate::New` returns null). One after another is fine. Two heaps at once is
   two threads.
2. **Scopes close in reverse order of opening**, and live on the stack.
   `HandleScope`, `EscapableHandleScope`, `ContextScope` and `TryCatch` are all
   stack-only and LIFO.
3. **A `Local` dies with its frame.** To return one, `Escape` it from an
   `EscapableHandleScope`; to keep one longer, move it into a `Global<T>`.
4. **An empty handle is not `undefined`**, and an empty `std::optional` is not a
   value. Neither is a result.
5. **If you never call `PumpJobs`, promise continuations never run.** Nothing
   else drains them.
6. **Posted work runs when the script thread next pumps**, and nothing
   accelerates that. An interrupt does not: it cannot make a running script
   yield, and it never fires while the thread is idle.
7. **A running script cannot be made to yield, only terminated.** There is no
   suspend and no resume anywhere in this API.
8. **A native that blocks cannot be interrupted.** Termination and interrupts
   land at the engine's checkpoints, all of which are inside script.
9. **A stopped isolate stays stopped until `CancelTerminateExecution`.** Every
   operation that would run script fails until then, and the failure is not an
   exception.
10. **Closing a `TryCatch` consumes what it caught** unless you called
    `ReThrow`. A termination is the exception: it continues regardless.
11. **Enter a realm before reading through an object that belongs to it.** A
    value crosses realms freely; a realm's global object is access-checked.
12. **A cache blob is opaque and belongs to the source and the engine build that
    made it.** Offer a stale one and it is refused, the source compiles normally,
    and `UsedCodeCache()` says false. The blob is keyed on the engine's own
    *build identity*, not on its name, so upgrading the engine invalidates
    every blob you kept - which is the point: a key that did not notice would
    leave you paying a full compile every run, for ever, with `UsedCodeCache()`
    quietly answering false. The same goes for a `Serialize` blob: valid for
    that engine build, in that process, and not an interchange format.
13. **A callback returns promptly after throwing** and calls nothing further into
    the engine.
14. **Whatever is still queued when an isolate is destroyed is dropped**, not
    run.
15. **Nothing an isolate handed out may outlive it.** A `Context`, a `Script`
    and a `Global<T>` are the three things that outlive a handle frame, and all
    three name that isolate's engine state: let them go before it does.
    Declaring the isolate first and everything else after it is enough. A
    checked build diagnoses the violation in `~Isolate`; a release build does
    neither check nor tolerate it.

## What it costs

Against V8's own API, the handle model costs **one extra load per value access
and one extra pointer-sized word per handle**; against SpiderMonkey's it costs
nothing, because `JS::Rooted` cannot express the operation at all. No allocation
per handle, no virtual call, no `std::function`, no RTTI. The table is in
[`docs/lifetimes.md`](docs/lifetimes.md) §6.

That table assumes link-time optimization. Without it:

> **Every backend call is a real out-of-line call.** The public headers declare
> the backend's operations and the backend library defines them, because a public
> header may not include an engine header - that is the property that keeps a
> second engine possible. With LTO the definitions inline back into your code;
> without it, reading a property is a call.

`UNIBIND_LTO` is off in this tree, which means **the benchmarks
(`tests/bench/bench_main.cpp`) were measured in the slower configuration**. They
are a floor, not a ceiling. A consumer wanting the table's numbers builds the
library with `-DUNIBIND_LTO=ON` and compiles their own code with LTO too.

Mixing is fine in the ordinary case, and was checked rather than assumed: a
consumer compiled `-flto=thin` (bitcode objects) links against this
non-LTO-compiled static library and the engine's own archives under `lld-link`,
and the result runs. What you do not get from that is inlining *across* the
boundary, which is the whole point of turning it on.

## Gotchas worth knowing before you start

[`docs/gotchas.md`](docs/gotchas.md) is the collection - sixty-odd of them,
grouped by what you were doing, and opening with the thirteen you will not
diagnose from the symptom. Four belong here because they are about *getting the
build to work at all*, which is where a new consumer meets them.

**The 32-bit linker silently loses the engine.** An x86 MSBuild project takes the
32-bit host toolchain by default (`VC\Tools\Llvm\bin`, not `...\Llvm\x64\bin`),
and a 32-bit `lld-link` cannot get through an archive the size of an engine -
V8's monolith is 1.2 GB. It does not say so. It opens the archive, loads no
member from it, and reports *every* engine symbol as undefined, exactly as though
the library had not been passed at all. Set
`<PreferredToolArchitecture>x64</PreferredToolArchitecture>` in the project's
`Globals` group, above the import of `Microsoft.Cpp.Default.props`. `unibind.props`
checks for this and stops the build with a sentence, but it cannot fix it: by the
time a property sheet is imported the toolchain has been chosen. CMake happens to
pick the 64-bit tools already, which is why this only bites MSBuild consumers.

**A `/MD` project fails at link, not at compile.** The engines link the static
CRT. The MSVC STL stamps `RuntimeLibrary` into every object with the same
mechanism unibind's `config.h` uses for its own ABI tag, so the error is LNK2038 and
names the CRT.

**A Debug build needs a debug engine tree.** `/MTd` against a `/MT` engine is the
same LNK2038 for the same reason.

**The x64 V8 monolith replaces your `operator new`.** It carries PartitionAlloc's
Windows allocator shim, which defines `malloc`, `free` and twelve of the twenty
allocation operators, in an object that every link pulls in. A program that
replaces those operators itself - a leak counter, an allocation-failure injector,
an arena - gets a duplicate symbol and does not link. The x86 monolith has no
shim, and SpiderMonkey's problem is the mirror image: including one of *its*
headers replaces your `operator new` silently (`dependencies/README.md`). This
tree's own test suite hits this; `tests/CMakeLists.txt` says what it does about
it and why.

## What the name claims

The name does not say JavaScript, and most of the binding surface really is not
JavaScript-shaped: a class with a constructor, methods, accessors, statics,
native state and a finalizer; calling native from script and script from
native; exceptions; the value set of scalars, strings, arrays and objects;
interceptors, which are `__getattr__`/`__setattr__` in Python and
`__index`/`__newindex` in Lua; iteration; and the handle model itself, since a
frame that gives back everything it took when it closes is exactly the shape a
refcounted runtime wants a borrow to have.

What would not carry is the JavaScript in the rest: realms, prototypes, symbol
keys as a concept, and the microtask and promise model. Those are sections of
this API rather than corners of it. So the name is a claim about the shape of
the binding layer, and not a claim that a non-JavaScript backend is only a
matter of writing one.

## Limits

**No debugger, and that is a conclusion rather than a deferral.** V8's debugging
surface is the inspector protocol - a C++ channel carrying CDP messages - while
SpiderMonkey's is the `Debugger` object, a JavaScript API installed into a
debuggee realm. They do not share a shape, a vocabulary or even a language. The
only common C++ surface would be "ask the backend whether it has a debugger",
which is a string, not an abstraction. A debugger belongs to a per-engine
frontend built *on* unibind - and for V8 that frontend needs the engine objects,
which [`unibind/interop/v8.h`](include/unibind/interop/v8.h) hands out
(`ub::interop::V8Isolate`, `ub::interop::V8Context`). It is the one header whose
functions only one backend defines: put the code that calls it in a library
linked only into the V8 build, and link something else in its place for
SpiderMonkey.

**Cross-realm access control is not expressible.** Two realms cannot be told to
trust each other (V8 spells that as a shared security token; SpiderMonkey has
compartments and principals, and the two do not describe the same thing), and a
realm cannot be walled off from another. The portable answer is the rule in
section 7: enter the realm that owns the object. It is a line per hook, and it is
not a workaround for something you could otherwise ask for - you cannot ask.

**No BigInt factory.** The type is recognised (`ValueKind::BigInt`,
`Is<BigInt>()`) but native cannot make one, so `BigInt64Array` and
`BigUint64Array` are absent too: an element could be read and never written.

**A heap about to hit its ceiling can only be *asked about* on one engine.**
`Isolate::SetHeapLimitCallback` is V8's near-heap-limit hook and SpiderMonkey
has nothing of the kind - not a different shape, nothing - so a call to it does
not link there. Both engines still report the failure itself as
`EngineFault::OutOfMemory`; what differs is whether you get asked first.
`EngineFault::Fatal` needs no such caveat: SpiderMonkey has no hook for its
`MOZ_CRASH`, so the backend recognises the crash itself.

**Not thread-safe, by contract.** Everything but `TerminateExecution`,
`RequestInterrupt` and `PostJob` happens on the isolate's own thread.

**Windows only.** x86 and x64 are both built and tested, on both backends, and
`CMakeLists.txt` refuses anything else rather than letting it fail later.
Nothing in the *design* is Windows-specific; nothing has been built anywhere
else.

## Layout

```
include/unibind/    the public API. No engine header, transitively, and no
                    mention of a backend either - both enforced by the
                    unibind_headers_only target, not by review
src/backends/v8/    the V8 backend: one of the two places an engine header may appear
src/backends/spidermonkey/
cmake/              UnibindEngines.cmake: fetching a published engine build, once
                    per version, whole or not at all
tools/headers_only/ compiles every public header alone, and instantiates the
                    whole template surface, with no engine on the include path;
                    backend_neutral.cmake is the half that has to be read
                    rather than compiled
tests/              one suite, written against ub:: only, run against both
                    backends and compared - see tests/README.md
examples/           a consumer, built against an installed prefix
packaging/          unibind.props and the CMake package, for consumers
docs/               decisions, including the ones that were rejected, and
                    gotchas.md, which is the one to read first
```

## License

MIT - see [`LICENSE`](LICENSE). Nothing of either engine is in this repository;
both are paths you point the build at. The engine you link has a license of its
own, and the one to read is SpiderMonkey's MPL-2.0, which permits linking into a
proprietary product and asks that the *engine's* source stay available.
[`docs/licensing.md`](docs/licensing.md) says what that means in practice.
