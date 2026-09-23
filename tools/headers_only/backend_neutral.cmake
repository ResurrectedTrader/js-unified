# The second rule a public header has to obey, checked the same way as the
# first: it may not name the backend.
#
# The first rule - no engine header, ever - is enforced by compiling every
# public header with no engine include directory on the command line, which is
# what the target beside this script does. This one cannot be enforced that
# way, because the mistake compiles perfectly: a header that writes
# `#if UNIBIND_BACKEND_V8` against a macro nobody defines any more is not an
# error, it is silently false, in every build, including the V8 one.
#
# It matters because a consumer compiles their own objects **once** and chooses
# `unibind_backend_v8` or `unibind_backend_spidermonkey` at the final link. A
# public header that laid a type out differently per engine would break that
# with nothing to catch it: the ABI tag in the generated `config.h` deliberately
# no longer names the backend, precisely so that a cross-backend link is
# allowed, so it cannot be the thing that notices either.
#
# Which backend is linked is a **runtime** question and has a runtime answer,
# `ub::Platform::BackendName()`. Nothing in a public header needs it earlier
# than that, and `unibind/script.h` - which keys a cache blob to the engine that
# wrote it - is the proof: it asks at runtime, and the same compiled object
# gets the right key under either backend.
#
#   cmake -DUNIBIND_INCLUDE_DIR=<include> -P backend_neutral.cmake

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED UNIBIND_INCLUDE_DIR)
    message(FATAL_ERROR "backend_neutral: UNIBIND_INCLUDE_DIR is not set")
endif()

file(GLOB_RECURSE headers "${UNIBIND_INCLUDE_DIR}/unibind/*.h" "${UNIBIND_INCLUDE_DIR}/unibind/*.h.in")

set(offenders "")
foreach(header IN LISTS headers)
    file(STRINGS "${header}" hits REGEX "UNIBIND_BACKEND")
    if(hits)
        file(RELATIVE_PATH shown "${UNIBIND_INCLUDE_DIR}" "${header}")
        foreach(hit IN LISTS hits)
            string(STRIP "${hit}" hit)
            list(APPEND offenders "${shown}: ${hit}")
        endforeach()
    endif()
endforeach()

if(offenders)
    list(JOIN offenders "\n  " shownOffenders)
    message(FATAL_ERROR
        "unibind: a public header names the backend, which it may not:\n  ${shownOffenders}\n"
        "A consumer compiles these headers once and picks a backend at the link, so nothing "
        "here may differ by engine. Ask ub::Platform::BackendName() at runtime instead.")
endif()

message(STATUS "unibind: no public header names the backend")
