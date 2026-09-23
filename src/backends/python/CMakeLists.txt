# The CPython backend. This is the only directory in the tree that may include
# a CPython header; `unibind_headers_only` exists to keep it that way.
#
# The engine is a static /MT CPython from vcpkg - see _unibind_provide_python in
# cmake/UnibindEngines.cmake - so, like the other two, it is a link input and
# nothing to deploy beside the program except the pure-Python standard library.

add_library(unibind_backend_python STATIC
    core.cpp
    values.cpp
    objects.cpp
    bindings.cpp
    runtime.cpp
    data.cpp
    module.cpp
    inspector.cpp)
add_library(unibind::backend_python ALIAS unibind_backend_python)

target_link_libraries(unibind_backend_python PUBLIC unibind_headers)
target_include_directories(unibind_backend_python SYSTEM PRIVATE "${UNIBIND_PYTHON_INCLUDE_DIR}")
target_include_directories(unibind_backend_python PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Py_NO_LINK_LIB: vcpkg's pyconfig.h otherwise names the import library in a
# `#pragma comment(lib)`, and the static library is named here instead.
#
# UNIBIND_PYTHON_DEFAULT_STDLIB is where the standard library was when this was
# built. It is the last place the backend looks, after UNIBIND_PYTHON_HOME and
# a `python-stdlib` directory beside the program - see docs/python.md.
target_compile_definitions(unibind_backend_python PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN Py_NO_LINK_LIB
    "UNIBIND_PYTHON_DEFAULT_STDLIB=\"${UNIBIND_PYTHON_STDLIB}\"")

target_link_libraries(unibind_backend_python PUBLIC ${UNIBIND_PYTHON_LIBS} ${UNIBIND_PYTHON_SYSTEM_LIBS})

message(STATUS "unibind: CPython ${UNIBIND_PYTHON_VERSION}, standard library at ${UNIBIND_PYTHON_STDLIB}")
