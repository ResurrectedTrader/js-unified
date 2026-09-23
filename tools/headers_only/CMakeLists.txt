# Proof, by compilation, that the public headers reach no engine header - and,
# beside it, the one rule about them that compilation cannot prove.
#
# Every header under include/unibind is compiled on its own, in a target that links
# only unibind_headers - so no engine include directory is on the command line. A
# public header that includes <v8.h> or <jsapi.h>, directly or through six
# levels of transitive include, fails to compile here. Nothing is linked, so
# the backend's undefined symbols do not matter.
#
# `backend_neutral.cmake` is the other half: a public header may not name the
# backend either, because a consumer compiles these headers once and picks an
# engine at the link. That one cannot be a compile error - `#if
# UNIBIND_BACKEND_V8` against a macro nobody defines is silently false rather
# than wrong - so it is read rather than compiled. The script says why at
# length.
#
# It builds two ways:
#
#   * as part of the project, where it uses the build tree's own generated
#     unibind/config.h and is one more target of an ordinary build;
#   * on its own - `cmake -S tools/headers_only -B build/headers` - with no
#     engine, no vcpkg, no backend and no architecture requirement, which is
#     what CI runs on every push. Nobody can check out this repository and
#     build the library (neither engine is fetchable), so this is the one check
#     a stranger, and a CI runner, can always run.

cmake_minimum_required(VERSION 3.25)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(unibindStandalone ON)
    project(unibind_headers_only LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    get_filename_component(unibindRoot "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
else()
    set(unibindStandalone OFF)
    set(unibindRoot "${CMAKE_SOURCE_DIR}")
endif()

set(unibindIncludeDir "${unibindRoot}/include")

# ---------------------------------------------------------------------------
# Standalone: a config.h of its own, and an interface target to carry it.
#
# The numbers below are not this build's ABI and are not read by anything that
# runs - the inline storage sizes only matter to a backend, and there is no
# backend here. What they have to be is *present*, because the public headers
# declare real storage with them. The root CMakeLists.txt is where the values
# that matter are chosen.
#
# Both settings of UNIBIND_HANDLE_CHECKS are compiled, because that macro changes
# the layout of Local<T> and is the only thing in the public headers that is
# conditional at all.
# ---------------------------------------------------------------------------
function(unibind_headers_only_target name handleChecks)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/config-${name}")

    set(UNIBIND_HANDLE_CHECKS ${handleChecks})
    set(UNIBIND_FRAME_STORAGE_SIZE 256)
    set(UNIBIND_FRAME_STORAGE_ALIGN 8)
    set(UNIBIND_TRY_CATCH_STORAGE_SIZE 128)
    set(UNIBIND_TRY_CATCH_STORAGE_ALIGN 8)
    set(UNIBIND_CONTEXT_SCOPE_STORAGE_SIZE 64)
    set(UNIBIND_CONTEXT_SCOPE_STORAGE_ALIGN 8)
    set(UNIBIND_FRAME_INLINE_SLOTS 8)

    configure_file("${unibindIncludeDir}/unibind/config.h.in" "${dir}/unibind/config.h" @ONLY)

    add_library(${name} INTERFACE)
    target_include_directories(${name} INTERFACE "${unibindIncludeDir}" "${dir}")
    target_compile_features(${name} INTERFACE cxx_std_23)
endfunction()

if(unibindStandalone)
    unibind_headers_only_target(unibind_headers_checked 1)
    unibind_headers_only_target(unibind_headers_unchecked 0)
endif()

# ---------------------------------------------------------------------------
# One translation unit per header, plus the instantiation of the template
# surface that a bare #include would barely parse.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE UNIBIND_PUBLIC_HEADERS
     RELATIVE "${unibindIncludeDir}"
     CONFIGURE_DEPENDS
     "${unibindIncludeDir}/unibind/*.h")

set(generated "")
foreach(header IN LISTS UNIBIND_PUBLIC_HEADERS)
    string(MAKE_C_IDENTIFIER "${header}" stem)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/alone_${stem}.cpp")
    # Included twice, which also proves the include guard works - so the second
    # include is the point of the file rather than a mistake, and clang-tidy is
    # told so in the generated source itself.
    file(CONFIGURE OUTPUT "${source}"
         CONTENT "#include \"@header@\"\n// NOLINTNEXTLINE(readability-duplicate-include) - twice on purpose: it proves the include guard\n#include \"@header@\"\n"
         @ONLY)
    list(APPEND generated "${source}")
endforeach()

if(unibindStandalone)
    add_library(unibind_headers_only STATIC ${generated} instantiate.cpp)
    target_link_libraries(unibind_headers_only PRIVATE unibind_headers_checked)

    add_library(unibind_headers_only_unchecked STATIC ${generated} instantiate.cpp)
    target_link_libraries(unibind_headers_only_unchecked PRIVATE unibind_headers_unchecked)
else()
    add_library(unibind_headers_only STATIC ${generated} instantiate.cpp)
    target_link_libraries(unibind_headers_only PRIVATE unibind_headers)
endif()

# ---------------------------------------------------------------------------
# And the rule compilation cannot check: no public header names the backend.
#
# Attached to the headers-only library so it runs in both ways this directory
# is built - as part of the project, and standalone, which is what CI runs on
# every push and what a stranger with no engine can run.
# ---------------------------------------------------------------------------
add_custom_command(TARGET unibind_headers_only POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DUNIBIND_INCLUDE_DIR=${unibindIncludeDir}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/backend_neutral.cmake"
    COMMENT "unibind: checking that no public header names the backend"
    VERBATIM)
