# vcpkg overlay ports

The python backend takes its engine from vcpkg (the `python` feature in
`vcpkg.json`, switched on by the root `CMakeLists.txt` when
`UNIBIND_BACKEND=python`). The ports here are used **instead of** the registry's,
and only for that backend: the root `CMakeLists.txt` adds this directory to
`VCPKG_OVERLAY_PORTS` before `project()`.

## python3

The registry's `python3` port at the baseline in `vcpkg.json` (3.12.13), plus
one patch, `0100-no-whole-program-optimization.patch`.

On a `*-windows-static` triplet the port builds CPython as a static library
against the static CRT - the shape every unibind engine has. But CPython's
`PCbuild/pyproject.props` sets `/GL` (whole-program optimisation) as `ClCompile`
item metadata, which the `/p:WholeProgramOptimization=false` that
`vcpkg_msbuild_install` passes for every static library cannot override. The
Release library then consists of LTCG objects, which:

* `lld-link` - what the ClangCL toolset links with - cannot read at all
  ("is not a native COFF file. Recompile without /GL?"), and
* MSVC's own `link.exe` reads only when it is the *exact* compiler version that
  made them, which is not a property a static library should have.

The patch makes that metadata (and the matching `/LTCG` link and lib settings)
respect the property, so vcpkg's intent takes effect. Everything else about the
port is unchanged; `port-version` is bumped so its binary-cache entry is never
confused with the registry's.

When the baseline moves, re-copy `ports/python3` from vcpkg and regenerate the
patch against the patched source tree (`buildtrees/python3/src/*.clean`).

### Built-in extension modules (in progress)

A static core cannot load a `.pyd`, so on the `*-windows-static` triplets the
port compiles extension modules into `python3X.lib` as built-in modules
(`0101-builtin-extension-modules.patch`, `python_vcpkg.props.in`, and the
`PYTHON_BUILTIN_EXTENSIONS` list in `portfile.cmake`). So far: `_asyncio`,
`_overlapped`, `_socket`, `select`. `0102-asyncio-proactor-in-subinterpreters.patch`
lets asyncio's default (proactor) loop be made in a sub-interpreter.
