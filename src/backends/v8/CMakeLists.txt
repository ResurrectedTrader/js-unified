# The V8 backend. This is the only directory in the tree that may include a V8
# header; `unibind_headers_only` exists to keep it that way.

if(NOT UNIBIND_V8_DIR)
    message(FATAL_ERROR
        "unibind: UNIBIND_V8_DIR is not set. Point it at a prebuilt V8 directory holding "
        "include/ and v8_monolith.lib, e.g. "
        "-DUNIBIND_V8_DIR=C:/.../dependencies/v8/15.6.8/${UNIBIND_ARCH}-release")
endif()

if(NOT EXISTS "${UNIBIND_V8_DIR}/include/v8.h")
    message(FATAL_ERROR "unibind: no include/v8.h under UNIBIND_V8_DIR='${UNIBIND_V8_DIR}'")
endif()
if(NOT EXISTS "${UNIBIND_V8_DIR}/v8_monolith.lib")
    message(FATAL_ERROR "unibind: no v8_monolith.lib under UNIBIND_V8_DIR='${UNIBIND_V8_DIR}'")
endif()

add_library(unibind_backend_v8 STATIC backend.cpp)
add_library(unibind::backend_v8 ALIAS unibind_backend_v8)

target_link_libraries(unibind_backend_v8 PUBLIC unibind_headers)
# SYSTEM for the engine tree: these headers are third-party and enormous, and
# neither the compiler nor clang-tidy has any business reporting on them. It is
# also what keeps a lint run over this backend from walking into V8 through
# every `#include`.
target_include_directories(unibind_backend_v8 SYSTEM PRIVATE "${UNIBIND_V8_DIR}/include")

# V8_GN_HEADER makes v8config.h pull in the bundled v8-gn.h, which carries the
# exact define set the library was built with. Without it the public headers
# fall back to their defaults and lay objects out differently from the .lib -
# an ABI mismatch that mostly does not show up as a compile error.
target_compile_definitions(unibind_backend_v8 PRIVATE V8_GN_HEADER NOMINMAX WIN32_LEAN_AND_MEAN)

# The monolith and the system libraries it was built against.
target_link_libraries(unibind_backend_v8 PUBLIC
    "${UNIBIND_V8_DIR}/v8_monolith.lib"
    winmm dbghelp advapi32 shlwapi ws2_32 user32 kernel32
    ole32 oleaut32 psapi version ntdll userenv bcrypt)

# The prebuilt monolith links the static CRT, so the CRT flavour has to match:
# a Debug build of this tree wants /MTd and therefore a debug V8 tree.
message(STATUS "unibind: V8 at ${UNIBIND_V8_DIR} (a Debug build needs a debug tree: the static CRT must match)")
