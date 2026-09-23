# The SpiderMonkey backend. This is the only directory in the tree that may
# include a SpiderMonkey header; `unibind_headers_only` exists to keep it that way.

if(NOT UNIBIND_SPIDERMONKEY_DIR)
    message(FATAL_ERROR
        "unibind: UNIBIND_SPIDERMONKEY_DIR is not set. Point it at a prebuilt SpiderMonkey "
        "directory holding include/ and spidermonkey.lib, e.g. "
        "-DUNIBIND_SPIDERMONKEY_DIR=${CMAKE_SOURCE_DIR}/dependencies/spidermonkey/153.3.0esr-${UNIBIND_ARCH}-release")
endif()

if(NOT EXISTS "${UNIBIND_SPIDERMONKEY_DIR}/include/jsapi.h")
    message(FATAL_ERROR "unibind: no include/jsapi.h under UNIBIND_SPIDERMONKEY_DIR='${UNIBIND_SPIDERMONKEY_DIR}'")
endif()
if(NOT EXISTS "${UNIBIND_SPIDERMONKEY_DIR}/spidermonkey.lib")
    message(FATAL_ERROR "unibind: no spidermonkey.lib under UNIBIND_SPIDERMONKEY_DIR='${UNIBIND_SPIDERMONKEY_DIR}'")
endif()

add_library(unibind_backend_spidermonkey STATIC
    backend.cpp
    frame_alloc.cpp
    templates.cpp)
add_library(unibind::backend_spidermonkey ALIAS unibind_backend_spidermonkey)

target_link_libraries(unibind_backend_spidermonkey PUBLIC unibind_headers)
# SYSTEM for the engine tree, and only for it: these headers are third-party,
# they are enormous, and neither the compiler nor clang-tidy has any business
# reporting on them. It is also what keeps a lint run over this backend from
# walking into SpiderMonkey through every `#include`.
target_include_directories(unibind_backend_spidermonkey SYSTEM PRIVATE
    "${UNIBIND_SPIDERMONKEY_DIR}/include")
target_include_directories(unibind_backend_spidermonkey PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# STATIC_JS_API and XP_WIN are not optional: the headers lay types out
# differently without them, and the mismatch mostly does not show up as a
# compile error. MOZ_STATIC_JS is the same switch under mozilla-config's name.
#
# ENABLE_EXPLICIT_RESOURCE_MANAGEMENT is the trap. The prebuilt library was
# compiled with it, but js-config.h - which is the header bundle's record of
# what the build enabled - does not mention it. It gates an enumerator in the
# middle of JSExnType (JSEXN_SUPPRESSEDERR) and entries in JSProtoKey and
# JS::SymbolCode, so without it every enumerator after that point means
# something different here from what it means inside the library: asking for a
# TypeError produces a SyntaxError. It compiles cleanly either way. See
# docs/spidermonkey.md.
target_compile_definitions(unibind_backend_spidermonkey PRIVATE
    STATIC_JS_API
    MOZ_STATIC_JS
    XP_WIN
    ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

# The engine and the system libraries it was built against, as listed in
# dependencies/README.md.
target_link_libraries(unibind_backend_spidermonkey PUBLIC
    "${UNIBIND_SPIDERMONKEY_DIR}/spidermonkey.lib"
    mincore ws2_32 advapi32 user32 ole32 oleaut32 shell32 userenv
    bcrypt ntdll dbghelp psapi winmm shlwapi)

# The prebuilt library links the static CRT, so the CRT flavour has to match: a
# Debug build of this tree wants /MTd and therefore a debug SpiderMonkey tree.
message(STATUS "unibind: SpiderMonkey at ${UNIBIND_SPIDERMONKEY_DIR} (a Debug build needs a debug tree: the static CRT must match)")
