# Does the engine's own library replace the program's allocation operators?
#
# `tests/support/allocations.cpp` replaces global `operator new` and `operator
# delete` for the whole test binary, which is how two things the engines cannot
# be asked get measured: whether a frame gives back the storage it took, and
# what a frame does when an allocation fails (docs/lifetimes.md rule 9).
#
# That works everywhere except against a library that has replaced them too -
# and the x64 V8 monolith has. It carries PartitionAlloc's Windows allocator
# shim, which defines `malloc`, `free`, `_aligned_malloc` *and* twelve of the
# twenty allocation operators. The shim's object is pulled into any link that
# mentions `malloc`, which every C++ program does, so its definitions are
# always present and the test binary's own are a duplicate symbol: LNK2005 from
# lld-link, and nothing builds. The x86 monolith has no shim (PartitionAlloc is
# not used as malloc on 32-bit Windows), which is why this only appears now.
#
# The archive's symbol index says so without a link attempt and without a tool -
# see ArchiveIndex.cmake for why the scan is bounded to that member. The probe
# looks for nothrow `operator new`, which is in the shim's object and is not
# something the C++ runtime's own archives put in an *engine* library.
#
#   MSVC x64  ??2@YAPEAX_KAEBUnothrow_t@std@@@Z
#   MSVC x86  ??2@YAPAXIABUnothrow_t@std@@@Z

include("${CMAKE_CURRENT_LIST_DIR}/ArchiveIndex.cmake")

function(unibind_engine_replaces_operators archive out)
    set(${out} FALSE PARENT_SCOPE)
    if(NOT EXISTS "${archive}")
        return()
    endif()

    unibind_archive_index_end("${archive}" indexEnd)
    if(indexEnd EQUAL 0)
        return()
    endif()

    file(STRINGS "${archive}" found LIMIT_INPUT ${indexEnd} LIMIT_COUNT 1
         REGEX "[?][?]2@YAP[AE]+AX.*nothrow_t@std@@@Z")
    if(found)
        set(${out} TRUE PARENT_SCOPE)
    endif()
endfunction()
