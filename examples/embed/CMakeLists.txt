# The same program, consumed from CMake instead of MSBuild - and the proof of
# the thing the install layout exists for.
#
# A standalone project on purpose: it is configured against an *installed*
# prefix, not added to the library's build tree, so what it proves is that the
# install is complete.
#
#   cmake -S examples/embed -B build/example-cmake -G "Visual Studio 17 2022" \
#         -A Win32 -T ClangCL -DCMAKE_PREFIX_PATH=<prefix>
#   cmake --build build/example-cmake --config Release --parallel 1
#
# `main.cpp` is compiled **once**, into an object library that links
# `unibind::headers` and names no engine at all. Every backend the prefix holds
# then gets an executable of its own, built from those same objects. That is
# the whole claim: the backend is a link input, so a consumer who ships both
# engines compiles their code once and links it twice.
#
# A prefix with one backend produces one executable and the loop reads the same.

cmake_minimum_required(VERSION 3.25)
project(unibind_example_embed LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The engines link the static CRT, and so must anything that links them.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Ask what this prefix holds, then ask for all of it. A single-backend consumer
# writes `find_package(unibind REQUIRED)` and links `unibind::unibind`; this one
# wants every engine in the prefix, so it names them.
find_package(unibind REQUIRED)
find_package(unibind REQUIRED COMPONENTS ${UNIBIND_AVAILABLE_BACKENDS})

add_library(embed_objects OBJECT main.cpp)
target_link_libraries(embed_objects PRIVATE unibind::headers)

foreach(backend IN LISTS UNIBIND_AVAILABLE_BACKENDS)
    add_executable(embed_${backend} $<TARGET_OBJECTS:embed_objects>)
    target_link_libraries(embed_${backend} PRIVATE unibind::backend_${backend})
endforeach()

message(STATUS "example: one compile of main.cpp, linked against: ${UNIBIND_AVAILABLE_BACKENDS}")
