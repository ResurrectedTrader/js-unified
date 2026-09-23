# Engine builds

This is where an engine lands. Both are prebuilt static libraries, static CRT
(`/MT`), and neither is in git — see `.gitignore`.

**You do not have to put one here.** Configuring fetches the engine the build is
pinned to, when there is not already one in the directory its version and
architecture name:

```
dependencies/
  v8/15.6.8/x86-release/                 include/  v8_monolith.lib   (1.2 GB)
  v8/15.6.8/x64-release/                 include/  v8_monolith.lib   (1.2 GB)
  spidermonkey/153.3.0esr-x86-release/   include/  spidermonkey.lib  (629 MB)
  spidermonkey/153.3.0esr-x64-release/   include/  spidermonkey.lib  (630 MB)
```

The architecture is the build's, not a choice: an x64 configure fetches the x64
archive, and the two sit side by side.

Each comes from a public GitHub release of the matching repository —
`ResurrectedTrader/v8-static-win` and `ResurrectedTrader/spidermonkey-static-win`
— whose x86, x64, release and debug archives all carry `include/`, the library,
a `README.txt` and a `toolset.txt` at the archive root. `cmake/UnibindEngines.cmake`
is the whole of it, and it holds the sha256 of every archive it knows how to ask
for, so a damaged or substituted download fails at configure time rather than at
link time.

| | |
|---|---|
| when | at configure time, only when the directory the version and architecture name is absent |
| how much | x86: 263 MB for V8, 141 MB for SpiderMonkey. x64: 277 MB and 143 MB. About 40 seconds each on a fast link |
| again | never, while that directory is there — including on a reconfigure, a fresh build tree, or a second backend |
| partial | not possible: the archive unpacks into a staging sibling and is renamed into place in one move, so the directory is whole or it is not there |

Four cache variables decide what happens, and the first two are the ones anyone
touches:

| | |
|---|---|
| `UNIBIND_V8_DIR`, `UNIBIND_SPIDERMONKEY_DIR` | a build you already have. Set either and **nothing is downloaded**: the path is used as it stands, wherever it is. This is also how a junction or a shared engine tree on the machine is used instead of a copy per checkout. |
| `UNIBIND_V8_VERSION`, `UNIBIND_SPIDERMONKEY_VERSION` | which release to fetch. The tag, the asset name and the directory are all derived from it, so changing one repoints every path and fetches the new library rather than quietly reusing the old one. |
| `UNIBIND_ENGINE_FLAVOR` | `release` (the default) or `debug`. A `Debug` build of this tree links `/MTd` and needs the debug archive; see the note under each engine below. |
| `UNIBIND_FETCH_ENGINES` | `OFF` forbids downloading, and configuring then fails with what to unpack where, instead of reaching for the network. |

## SpiderMonkey 153.3.0esr — `spidermonkey/153.3.0esr-<arch>-release/`

| | |
|---|---|
| link | `spidermonkey.lib` (629 MB x86, 630 MB x64) |
| include | `include/` (`jsapi.h`, `js/`, `js-config.h`, …) |
| required defines | `STATIC_JS_API`, `MOZ_STATIC_JS`, `XP_WIN`, and `ENABLE_EXPLICIT_RESOURCE_MANAGEMENT` — see below for why the last one is not in `js-config.h` |
| built with | MSVC 14.44.35207, `/MT`, `RUSTFLAGS=-Ctarget-feature=+crt-static` |
| extra system libs | `mincore ws2_32 advapi32 user32 ole32 oleaut32 shell32 userenv bcrypt ntdll dbghelp psapi winmm shlwapi` |

**Toolset floor: MSVC 14.44 or newer.** The STL headers it was compiled against
call helpers that ship in that toolset's own `libcpmt.lib`; an older toolset
fails with undefined `__std_*` symbols. `CMakeLists.txt` checks this at configure
time — but it is a floor, not a preference.

### Including an engine header replaces your `operator new`

`mozilla/cxxalloc.h`, which `jsapi.h` pulls in transitively, defines
`operator new` and `operator delete` as always-inline forwards to `moz_xmalloc`.

So **a `::operator new` written in a translation unit that has seen a
SpiderMonkey header is not the program's replaceable `operator new`.** Anything
relying on replacing it — a leak counter, an allocation-failure injector, an
arena — silently stops working in that TU, and the only outward sign is
`LNK4217` warnings about `moz_xmalloc` in the build log.

The fix is to put such code in a file that includes no engine header. This
repository does exactly that in `src/backends/spidermonkey/frame_alloc.cpp`;
adding an engine include to it would quietly un-test the frame-exhaustion rule.

### `js-config.h` is incomplete, not authoritative

The bundle's `js-config.h` does **not** define `ENABLE_EXPLICIT_RESOURCE_MANAGEMENT`,
but the library was built with it. That macro gates `JSEXN_SUPPRESSEDERR` in the
*middle* of `JSExnType`, and entries in `JSProtoKey` and `JS::SymbolCode` — so
every enumerator after it means one thing inside the library and another thing
in your translation unit.

Nothing warns. It compiles, links, and runs, and then asking for a `TypeError`
hands back a `SyntaxError`.

If an enum-valued answer from SpiderMonkey is off by one position, suspect a
build flag missing from `js-config.h` before suspecting your own code. Define
the missing macro yourself to match how the library was actually built.

**`mozglue` defines `DllMain`** (`WindowsDllMain.obj`). Irrelevant for a static
library and test executables, but a host *DLL* that defines its own `DllMain`
will fail to link with a duplicate symbol. If that ever comes up: either let
mozglue's run, or exclude that object.

Upstream: `ResurrectedTrader/spidermonkey-static-win`, release
`spidermonkey-153.3.0esr`, asset
`spidermonkey-153.3.0esr-<arch>-<flavour>-msvc14.44.zip`. All four are published
in the same release; the architecture follows the build and
`-DUNIBIND_ENGINE_FLAVOR=debug` picks the debug one, which is what a `Debug` build
of this tree wants — it links `/MTd`, and mixing CRTs is LNK2038.

## V8 15.6.8 — `v8/15.6.8/<arch>-release/`

| | |
|---|---|
| link | `v8_monolith.lib` (1.2 GB) |
| include | `include/` (`v8.h`, `v8-*.h`, `v8-gn.h`, …) |
| required defines | `V8_GN_HEADER` |
| built with | MSVC 14.44.35207, `/MT`, `v8_monolithic=true`, `is_component_build=false` |
| extra system libs | `winmm dbghelp advapi32 shlwapi ws2_32 user32 kernel32 ole32 oleaut32 psapi version ntdll userenv bcrypt` |

`V8_GN_HEADER` is not optional: it makes `v8config.h` pull in the bundled
`include/v8-gn.h`, which carries the exact define set the library was built
with. Without it the public headers fall back to their defaults — a different
internal field count, no pointer compression — and lay objects out differently
from the library. `V8::Initialize()` catches the subset it can see and aborts;
the rest corrupts silently.

**A 32-bit linker cannot get through this archive**, and does not say so: it
opens it, loads no member, and reports every V8 symbol as undefined. CMake picks
the 64-bit host tools; an MSBuild consumer sets `PreferredToolArchitecture=x64`
itself, and `unibind.props` stops the build if it did not. (That is about the
*host* toolchain and applies to an x86 target; an x64 target gets the 64-bit
tools anyway.)

### The x64 build brings its own allocator

The x64 monolith carries PartitionAlloc's Windows allocator shim —
`allocator_shim_win_static.obj` — which defines `malloc`, `free`,
`_aligned_malloc` and twelve of the twenty allocation operators: nothrow `new`,
aligned `new`, sized `delete`, aligned `delete`. Every link mentions `malloc`,
so that object is always pulled in.

So **a program linking x64 V8 does not own its allocator.** Replacing those
operators is LNK2005 on all twelve, not a silent override — which is the
opposite failure to SpiderMonkey's above, and the more merciful one. The x86
build has no shim: PartitionAlloc is not used as malloc on 32-bit Windows.

A program that really must replace them links with `/FORCE:MULTIPLE` and relies
on the order — an object on the command line is seen before an archive scanned
after it — so its own definitions win and the shim's are dropped. Memory stays
consistent because those operators still call the shim's `malloc`. That is what
this tree's test suite does, and only where the probe in
`tests/cmake/EngineAllocator.cmake` finds the operators in the archive.

Upstream: `ResurrectedTrader/v8-static-win`, release `v8-15.6.8`, asset
`v8-15.6.8-<arch>-<flavour>-msvc14.44.zip`. All four are published together, and
the version directory holds one subdirectory per architecture and flavour
(`x86-release/`, `x64-release/`, `x86-debug/`, …), so several sit side by side.
