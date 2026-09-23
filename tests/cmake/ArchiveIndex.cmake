# Where a static library's symbol index ends.
#
# The index is an archive's first member and lists exactly the symbols the
# archive *defines*. Scanning the whole file instead would also pick up the
# undefined references and the debug information, which name every declaration
# whether or not anyone defined it - so a probe reading the whole file would
# answer "yes, it has that" about a symbol nobody defined, which is the one
# answer such a probe must never give by accident.
#
# It is read by hand rather than with dumpbin or llvm-nm, so a probe needs
# nothing on the machine that is not already building the tree.
#
#   8 bytes   "!<arch>\n"
#   60 bytes  member header, whose size field is 10 ASCII digits at offset 48
#   <size>    the index itself, ending in the NUL-separated symbol names
#
# `out` is set to 0 for anything that is not an archive with an index.

function(unibind_archive_index_end file out)
    file(READ "${file}" magic LIMIT 8 HEX)
    file(READ "${file}" nameByte OFFSET 8 LIMIT 1 HEX)
    if(NOT magic STREQUAL "213c617263683e0a" OR NOT nameByte STREQUAL "2f")
        set(${out} 0 PARENT_SCOPE)  # not an archive we recognise
        return()
    endif()

    file(READ "${file}" sizeHex OFFSET 56 LIMIT 10 HEX)
    set(digits "")
    foreach(index RANGE 9)
        math(EXPR at "${index} * 2")
        string(SUBSTRING "${sizeHex}" ${at} 2 pair)
        if(pair MATCHES "^3([0-9])$")
            string(APPEND digits "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(digits STREQUAL "")
        set(${out} 0 PARENT_SCOPE)
        return()
    endif()
    math(EXPR end "68 + ${digits}")
    set(${out} ${end} PARENT_SCOPE)
endfunction()
