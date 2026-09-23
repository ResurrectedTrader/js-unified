# The CPython backend's suite.
#
# The shared suite in tests/cases is written in JavaScript source as much as in
# C++, so it has nothing to say to an engine whose scripts are Python - and the
# parity comparison, which lines one backend's results up against another's,
# has nothing to compare. This suite is the same kind of thing written for this
# engine: C++ against the public `ub::` API only, with Python as the script
# language. See tests/python/README.md.

add_executable(unibind_python_tests
    main.cpp
    support.cpp
    smoke_test.cpp
    binary_test.cpp
    serialization_test.cpp
    codecache_test.cpp

    objects_test.cpp
    functions_test.cpp
    templates_test.cpp
    interceptors_test.cpp
    classes_test.cpp)

target_include_directories(unibind_python_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(unibind_python_tests PRIVATE unibind::unibind doctest::doctest)

if(EXISTS "${doctest_DIR}/doctest.cmake")
    include("${doctest_DIR}/doctest.cmake")
    doctest_discover_tests(unibind_python_tests
        TEST_PREFIX "python."
        PROPERTIES TIMEOUT 120 LABELS "suite")
else()
    add_test(NAME python.suite COMMAND unibind_python_tests)
    set_tests_properties(python.suite PROPERTIES TIMEOUT 300 LABELS "suite")
endif()

add_test(NAME python.whole-suite-in-one-process COMMAND unibind_python_tests)
set_tests_properties(python.whole-suite-in-one-process PROPERTIES TIMEOUT 300 LABELS "suite")
