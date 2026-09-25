# vcpkg overlay ports

The python backend takes its engine from vcpkg (the `python` feature in
`vcpkg.json`, switched on by the root `CMakeLists.txt` when
`UNIBIND_BACKEND=python`). The ports here are used **instead of** the registry's,
and only for that backend: the root `CMakeLists.txt` adds this directory to
`VCPKG_OVERLAY_PORTS` before `project()`.

## python3

The registry's `python3` port at the baseline in `vcpkg.json` (3.12.13), with
three patches of ours, a longer `python_vcpkg.props.in`, and a `portfile.cmake`
that builds the standard library's C extension modules **into** the static
library. `port-version` is bumped past the registry's so that a binary-cache
entry of one is never taken for the other.

On a `*-windows-static` triplet the port builds CPython as a static library
against the static CRT - the shape every unibind engine has. The registry port
then gives up on extension modules altogether ("a static python core cannot load
extension modules": every `.pyd` links `python3X.dll`, which does not exist).
This one compiles them in as built-in modules instead, so nothing ever looks for
a `.pyd`, and `DLLs/` does not exist.

### What each change is for

**`0100-no-whole-program-optimization.patch`.** CPython's
`PCbuild/pyproject.props` sets `/GL` (whole-program optimisation) as
`ClCompile` item metadata, which the `/p:WholeProgramOptimization=false` that
`vcpkg_msbuild_install` passes for every static library cannot override. The
Release library then consists of LTCG objects, which

* `lld-link` - what the ClangCL toolset links with - cannot read at all
  ("is not a native COFF file. Recompile without /GL?"), and
* MSVC's own `link.exe` reads only when it is the *exact* compiler version that
  made them, which is not a property a static library should have.

The patch makes that metadata (and the matching `/LTCG` link and lib settings)
respect the property, so vcpkg's intent takes effect.

**`0101-builtin-extension-modules.patch`.** What building extension modules
into the core needs of the sources:

* `PC/config.c` includes `vcpkg_builtin_modules.h`, which the portfile
  generates: a `PyInit_*` declaration per module, and `_PyImport_Inittab`
  entries for them.
* `Python/import.c`: `create_builtin` refuses a *single-phase* built-in module
  in a sub-interpreter that has `check_multi_interp_extensions` set, before its
  init function runs. 3.12 applies that check to a `.pyd` (in
  `_imp.create_dynamic`), but to a built-in only once the module is in the
  extensions cache - so the *first* isolate to import `_ctypes`, `_decimal` or
  the core's `_datetime` got it, sharing C globals with every other
  interpreter, and every later isolate was refused. Now they are refused
  always. The portfile works out which modules are single-phase (a `PyInit_`
  whose body does not `return PyModuleDef_Init(...)`), both among the built-in
  extension modules and in `pythoncore` itself, and the header names them:
  `_ctypes`, `_decimal`, `_msi`, `_datetime`, `_tracemalloc`.
* `Modules/_zoneinfo.c`: `_zoneinfo` needs `_datetime`'s C API. Where
  `_datetime` is refused, `datetime` is `_pydatetime`, which has none, and
  `PyDateTime_IMPORT` fails with an `AttributeError` that `zoneinfo` does not
  expect - `import zoneinfo` itself failed. The module now reports that as an
  `ImportError`, and `zoneinfo` uses its pure-Python `ZoneInfo`.
* `PC/_wmimodule.cpp` passes `bstr_t` a wide literal. A narrow one calls
  `comsupp.lib`'s `ConvertStringToBSTR`, and the `wchar_t` form a native-wchar
  consumer needs is missing from older toolsets' `comsupp.lib` (14.44's, which
  this repository's ClangCL build links against): an unresolved symbol, only
  in the consumer.
* `Modules/_ctypes/callbacks.c`: a built-in `_ctypes` does not define
  `DllGetClassObject` / `DllCanUnloadNow`. In a `.pyd` they are its COM entry
  points; in a static library they would become the embedding program's, and
  collide with a COM server's own.
* `PCbuild/_elementtree.vcxproj` stops compiling a second, bundled copy of
  expat. `_elementtree` reaches expat only through `pyexpat`'s capsule, and
  `pyexpat` already uses vcpkg's (patch 0004); it now compiles against vcpkg's
  `expat.h` too, so the version check between them compares like with like.

**`0102-asyncio-proactor-in-subinterpreters.patch`.** asyncio's default loop on
Windows, `ProactorEventLoop`, calls `signal.set_wakeup_fd` whenever it is made
on the "main thread". To `threading`, a sub-interpreter's first thread *is* its
main thread, and `set_wakeup_fd` raises outside the main interpreter - so
`asyncio.run` failed in every isolate. The loop now does that only in the main
interpreter, the only one signals are delivered to.

**`0103-no-allocator-swap-in-subinterpreter-init.patch`.** Every new
interpreter's `init_sys_streams` ends with `_Py_ClearStandardStreamEncoding()`,
which frees what `Py_SetStandardStreamEncoding` kept - and to do so switches the
process-wide `PYMEM_DOMAIN_RAW` allocator to the "default" one with
`_PyMem_SetDefaultAllocator` and back, whether or not there is anything to free.
The switch holds the allocators' mutex, but `PyMem_RawMalloc`/`PyMem_RawFree`
read the allocator without it, and with an own-GIL sub-interpreter other
interpreters are running on other threads while one starts. Under `Py_DEBUG` the
default is `malloc` wrapped in the debug hooks, installed in two steps, so for a
moment the RAW allocator is plain `malloc`: a block another thread allocates or
frees in that moment (pymalloc sends every request over 512 bytes to RAW, so the
compiler's arrays are the usual victims) goes through the wrong allocator. That
is the "debug heap reports a block freed by an interpreter that did not allocate
it" crash - `_CrtIsValidHeapPointer`, `RtlValidateHeap`, or a debug block whose
pad bytes are not `FORBIDDENBYTE` - that concurrent isolates hit in Debug.
Release has the same race, but its default RAW allocator is the one already
installed, so the switch writes identical values and nothing can go wrong -
unless anything has replaced or wrapped the RAW allocator (debug hooks via
`PyMem_SetupDebugHooks`/`PYTHONMALLOC=debug`, tracemalloc, an embedder's own),
and then Release corrupts its heap the same way. The patch returns before the
switch when there is nothing to free, which is always after the main
interpreter's first start: `Py_SetStandardStreamEncoding` refuses once Python is
initialised. 3.12 has no upstream fix; 3.13 removed `Py_SetStandardStreamEncoding`
and this clean-up with it.

**`portfile.cmake` and `python_vcpkg.props.in`.** For a static Windows build the
portfile lists the modules to build in (`PYTHON_BUILTIN_EXTENSIONS`), rewrites
each one's `.vcxproj` from `DynamicLibrary` to `StaticLibrary`, and generates
the header above. `python_vcpkg.props` - force-imported into every project just
before `Microsoft.Cpp.targets` - then, for those projects, defines
`Py_BUILD_CORE_BUILTIN` (so `PyMODINIT_FUNC` is not `dllexport` and the module
compiles as part of the core), drops their version resource and their
`ProjectReference` to `pythoncore`, and empties their librarian inputs. It gives
`pythoncore` the reverse references, so the modules build first, and hands their
`.lib`s to its librarian step, which merges their objects into `python3X.lib`.
`python.exe` links the libraries below. `vcpkg.json` makes the third-party
libraries dependencies for `windows & static` (the `extensions` feature stays
what it was, for the DLL build).

`python3X.lib` holds CPython and the built-in modules and nothing else: zlib is
no longer merged into it, and neither is anything newer. The third-party
libraries stay vcpkg's own, so a program that uses openssl or sqlite itself
links one copy of each.

### The built-in modules

Built in, in addition to what the core always has (`zlib` among them):

| module | needs | in an isolate |
|---|---|---|
| `_asyncio` | | yes |
| `_overlapped` | ws2_32 | yes |
| `_socket` | ws2_32, iphlpapi, rpcrt4 | yes |
| `select` | ws2_32 | yes |
| `_ssl` | openssl (libssl, libcrypto), crypt32, ws2_32 | yes |
| `_hashlib` | openssl (libcrypto) | yes |
| `_sqlite3` | sqlite3 | yes |
| `_bz2` | bzip2 | yes |
| `_lzma` | liblzma | yes |
| `_queue` | | yes |
| `_multiprocessing` | ws2_32 | yes |
| `_uuid` | rpcrt4 | yes |
| `unicodedata` | | yes |
| `winsound` | winmm | yes |
| `_ctypes` | libffi | **no** - single-phase init |
| `_decimal` | (bundled libmpdec) | **no** - single-phase init; `decimal` falls back to `_pydecimal` |
| `_msi` | msi, cabinet, rpcrt4 | **no** - single-phase init |
| `pyexpat` | expat | **no** - declares `Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED` (gh-103092) |
| `_elementtree` | (pyexpat) | **no** - same; `xml.etree` imports but cannot parse |
| `_wmi` | wbemuuid, propsys, ole32 | **no** - multi-phase, but does not declare per-interpreter-GIL support |
| `_zoneinfo` | | **no** - needs `_datetime`'s C API; `zoneinfo` uses its pure-Python `ZoneInfo` |

and of the core's own, `_datetime` (so `datetime` is `_pydatetime`) and
`_tracemalloc` are refused in an isolate too, by the same patch.

"In an isolate" is an own-GIL sub-interpreter with
`check_multi_interp_extensions` on - what `ub::Isolate` is. The refusals are
CPython's own judgement of the module (or, for the single-phase ones, the
check a `.pyd` gets, applied by 0101); each import fails with `ImportError:
module X does not support loading in subinterpreters` and leaves nothing
behind. So in an isolate there is **no ctypes and no XML parsing** (`pyexpat`
is what `xml.etree`, `xml.dom.minidom` and `xml.sax` parse with); `decimal`,
`datetime` and `zoneinfo` work through their pure-Python twins. Everything
works in the main interpreter (the static `tools/python3/python.exe`).
`tests/python/stdlib_test.cpp` exercises every row.

Left out: `_tkinter` (needs Tcl/Tk), and the test modules (`_testcapi`,
`_testinternalcapi`, `_testbuffer`, `_testimportmultiple`, `_testmultiphase`,
`_testsinglephase`, `_testconsole`, `_testclinic`, `_ctypes_test`, `xxlimited`,
`xxlimited_35`).

`zoneinfo` works, but Windows has no tz database: `ZoneInfo('Europe/London')`
needs the `tzdata` package on the path. `ssl` finds the Windows certificate
stores by itself.

### What a program links

Beside `python3X.lib` (`python3X_d.lib` for Debug), from the same vcpkg prefix
- release names in `lib/`, debug names in `debug/lib/`:

| library | release | debug |
|---|---|---|
| zlib | `zs.lib` | `zsd.lib` |
| openssl | `libssl.lib`, `libcrypto.lib` | same names |
| libffi | `ffi.lib` | `ffi.lib` |
| sqlite3 | `sqlite3.lib` | `sqlite3.lib` |
| expat | `libexpatMT.lib` | `libexpatdMT.lib` |
| liblzma | `lzma.lib` | `lzma.lib` |
| bzip2 | `bz2.lib` | `bz2d.lib` |

and from Windows: `version ws2_32 shlwapi pathcch bcrypt advapi32 user32
kernel32 ole32 oleaut32 iphlpapi rpcrt4 crypt32 winmm msi cabinet wbemuuid
propsys`.

`_unibind_provide_python` in `cmake/UnibindEngines.cmake` finds all of these
(`UNIBIND_PYTHON_LIBS`, per configuration; `UNIBIND_PYTHON_SYSTEM_LIBS`), and the
install tree's `unibind-backend-python.cmake` names them too.

### Cost

Building the modules in adds openssl, libffi, sqlite3, expat, liblzma and bzip2
to the first vcpkg install of a triplet - about 20 minutes on a 32-thread
machine, nearly all of it openssl and libffi (autotools under msys), once, then
binary-cached. CPython's own build hardly changes (about 1.5 minutes for Release
and Debug together there). `python312.lib` (Release x64) grows from 66 MB (with
zlib merged in) to 81 MB; `python312_d.lib` from 71 MB to 87 MB. None of the
third-party libraries is in there - they are the link inputs below.

### When the baseline moves

Re-copy `ports/python3` from vcpkg, then re-apply ours: the `unibind:` blocks in
`portfile.cmake`, the tail of `python_vcpkg.props.in`, the `windows & static`
dependencies in `vcpkg.json`, and a `port-version` above the registry's. To
regenerate a patch, run the port once (a failed build is fine), take the source
tree it extracted - `buildtrees/python3/src/*.clean` has every earlier patch
applied - make it a git repository, redo the change, and `git diff` it into the
patch file. Check the module list against `PCbuild/pcbuild.proj`'s
`ExtensionModules`/`ExternalModules`; the portfile works out the single-phase
ones itself. In 3.13 `_ctypes` and `_decimal` become multi-phase and `_msi` is
gone.
