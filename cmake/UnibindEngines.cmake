# Getting an engine.
#
# Neither engine is in this repository and neither can be, so a clone used to
# need a hand-placed directory before it could configure at all. Both are
# published as GitHub releases of prebuilt static libraries, which is enough for
# the build to fetch the one it was configured for.
#
# Three properties the implementation is shaped by:
#
#   * Only when absent. A directory that is already there is used as it stands
#     and never re-downloaded, whether it was unpacked here, junctioned in from
#     elsewhere, or pointed at with UNIBIND_V8_DIR / UNIBIND_SPIDERMONKEY_DIR.
#   * Whole or not there. The archive is unpacked into a staging sibling and
#     renamed into place in one move, so an interrupted or failed fetch leaves
#     no half-unpacked directory that a later configure would mistake for a
#     complete one.
#   * Version-derived. The release tag, the asset name and the directory all
#     come from UNIBIND_<ENGINE>_VERSION, so changing a version repoints every one
#     of them and fetches the new library rather than reusing the old one.

# Part of every published asset name, and the toolset floor the engines were
# built against - see the floor check in the root CMakeLists.txt.
set(UNIBIND_ENGINE_TOOLSET "msvc14.44")

# sha256 and size of each asset this tree knows how to ask for, so that a
# damaged or substituted download fails here rather than at link time, and so
# the message before a quarter-gigabyte download can say how big it is.
# `gh release view <tag> --repo <repo> --json assets` prints both. An asset with
# no entry here is still fetched; it just says that it could not be verified.
set(_unibind_asset_v8-15.6.8-x86-release-msvc14.44.zip
    "df7f4f1a6b21b7093fedf252016d1514170f010097b3d324407e9a98fa2c4fba;263")
set(_unibind_asset_v8-15.6.8-x86-debug-msvc14.44.zip
    "8a6d24b1542a9b4a038ce7c3c29e63295b7dd2df28ae1f11907e37590ee14471;418")
set(_unibind_asset_v8-15.6.8-x64-release-msvc14.44.zip
    "65822dd31235025d0f9eb518fc2595f6a36967f82aa5bffa867aff61a639d1a3;277")
set(_unibind_asset_v8-15.6.8-x64-debug-msvc14.44.zip
    "b7f02de590fb2f46dd03cf08bcae2fc0ca3014eb7df6d57fd3bca4a25b69e47b;462")
set(_unibind_asset_spidermonkey-153.3.0esr-x86-release-msvc14.44.zip
    "8fb82896f9c649aef1c42c73e891c597a49f1be62c43eb5664da9481c71844b8;141")
set(_unibind_asset_spidermonkey-153.3.0esr-x86-debug-msvc14.44.zip
    "1c3e644f4818a9d49a82647a023b92dff0a42b750ba4010fcbffb60eb45109c7;227")
set(_unibind_asset_spidermonkey-153.3.0esr-x64-release-msvc14.44.zip
    "a0ca05ef96fca61ebdd576a1817792f7bb53263c0bad822aa75a1ca413c89e7f;143")
set(_unibind_asset_spidermonkey-153.3.0esr-x64-debug-msvc14.44.zip
    "105ec737ca9c65f0fcb281e4d0a25231be3e304eefc7218a364274f6e467c1c3;238")

# Everything that differs between the two engines, in one place: where it is
# published, what the directory it unpacks to is called, and the two files that
# say a directory really holds an engine rather than the remains of one.
function(_unibind_engine_facts engine)
    # Decided by the architecture check in the root CMakeLists.txt, so there is
    # one answer rather than two that can drift apart.
    set(arch "${UNIBIND_ARCH}")
    string(TOLOWER "${UNIBIND_ENGINE_FLAVOR}" flavour)
    if(NOT flavour MATCHES "^(release|debug)$")
        message(FATAL_ERROR "unibind: UNIBIND_ENGINE_FLAVOR is '${UNIBIND_ENGINE_FLAVOR}' (expected release or debug)")
    endif()

    if(engine STREQUAL "v8")
        set(version "${UNIBIND_V8_VERSION}")
        set(repo "ResurrectedTrader/v8-static-win")
        set(tag "v8-${version}")
        set(lib "v8_monolith.lib")
        set(header "include/v8.h")
        # dependencies/v8/<version>/<arch>-<flavour>/, which is one directory per
        # version holding every flavour of it.
        set(dir "${CMAKE_SOURCE_DIR}/dependencies/v8/${version}/${arch}-${flavour}")
    elseif(engine STREQUAL "spidermonkey")
        set(version "${UNIBIND_SPIDERMONKEY_VERSION}")
        set(repo "ResurrectedTrader/spidermonkey-static-win")
        set(tag "spidermonkey-${version}")
        set(lib "spidermonkey.lib")
        set(header "include/jsapi.h")
        set(dir "${CMAKE_SOURCE_DIR}/dependencies/spidermonkey/${version}-${arch}-${flavour}")
    else()
        message(FATAL_ERROR "unibind: no such engine '${engine}'")
    endif()

    set(asset "${tag}-${arch}-${flavour}-${UNIBIND_ENGINE_TOOLSET}.zip")

    set(unibindEngineVersion "${version}" PARENT_SCOPE)
    set(unibindEngineArch "${arch}" PARENT_SCOPE)
    set(unibindEngineFlavour "${flavour}" PARENT_SCOPE)
    set(unibindEngineLib "${lib}" PARENT_SCOPE)
    set(unibindEngineHeader "${header}" PARENT_SCOPE)
    set(unibindEngineDir "${dir}" PARENT_SCOPE)
    set(unibindEngineAsset "${asset}" PARENT_SCOPE)
    set(unibindEngineUrl "https://github.com/${repo}/releases/download/${tag}/${asset}" PARENT_SCOPE)
endfunction()

# Download the asset, unpack it, and move the result into place as one step.
#
# Nothing is written to `dir` until the whole archive has been unpacked and the
# library and headers have been found inside it, so a fetch that is interrupted
# - a cancelled configure, a dropped connection, a full disk - leaves `dir`
# absent and the next configure starts over. The staging directory is a sibling
# of `dir` so that the move is a rename within one volume rather than a copy.
function(_unibind_fetch_engine engine)
    _unibind_engine_facts("${engine}")

    set(sha256 "")
    set(megabytes "")
    if(DEFINED "_unibind_asset_${unibindEngineAsset}")
        list(GET "_unibind_asset_${unibindEngineAsset}" 0 sha256)
        list(GET "_unibind_asset_${unibindEngineAsset}" 1 megabytes)
        set(sizeText "${megabytes} MB")
    else()
        set(sizeText "a few hundred MB")
    endif()

    get_filename_component(parent "${unibindEngineDir}" DIRECTORY)
    get_filename_component(name "${unibindEngineDir}" NAME)
    set(staging "${parent}/.${name}.incoming")
    set(archive "${staging}/${unibindEngineAsset}")

    if(engine STREQUAL "v8")
        set(optOut "UNIBIND_V8_DIR")
    else()
        set(optOut "UNIBIND_SPIDERMONKEY_DIR")
    endif()

    message(STATUS "unibind: no ${engine} ${unibindEngineVersion} (${unibindEngineArch} ${unibindEngineFlavour}) at ${unibindEngineDir}")
    message(STATUS "unibind: fetching ${sizeText} from ${unibindEngineUrl}")
    message(STATUS "unibind: once per version, and never again while that directory is there; "
                   "-D${optOut}=<path> uses a build you already have instead")

    file(REMOVE_RECURSE "${staging}")
    file(MAKE_DIRECTORY "${staging}")

    if(sha256)
        file(DOWNLOAD "${unibindEngineUrl}" "${archive}"
             SHOW_PROGRESS TLS_VERIFY ON
             EXPECTED_HASH "SHA256=${sha256}"
             STATUS downloadStatus)
    else()
        message(STATUS "unibind: no recorded sha256 for ${unibindEngineAsset}; the download cannot be verified")
        file(DOWNLOAD "${unibindEngineUrl}" "${archive}"
             SHOW_PROGRESS TLS_VERIFY ON
             STATUS downloadStatus)
    endif()
    list(GET downloadStatus 0 downloadCode)
    if(NOT downloadCode EQUAL 0)
        list(GET downloadStatus 1 downloadMessage)
        file(REMOVE_RECURSE "${staging}")
        message(FATAL_ERROR
            "unibind: could not fetch ${unibindEngineUrl}: ${downloadMessage}\n"
            "Fetch it by hand into ${unibindEngineDir}, or point -D${optOut}=<path> at a build you have.")
    endif()

    message(STATUS "unibind: unpacking ${unibindEngineAsset}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${staging}/unpacked")
    file(REMOVE "${archive}")

    # The archive's internal layout is the publisher's business: find the library
    # and take the directory holding it, rather than assuming a top-level name.
    file(GLOB_RECURSE found LIST_DIRECTORIES false "${staging}/unpacked/${unibindEngineLib}")
    if(NOT found)
        file(REMOVE_RECURSE "${staging}")
        message(FATAL_ERROR "unibind: ${unibindEngineAsset} holds no ${unibindEngineLib}")
    endif()
    list(GET found 0 library)
    get_filename_component(root "${library}" DIRECTORY)
    if(NOT EXISTS "${root}/${unibindEngineHeader}")
        file(REMOVE_RECURSE "${staging}")
        message(FATAL_ERROR "unibind: ${unibindEngineAsset} has no ${unibindEngineHeader} beside its ${unibindEngineLib}")
    endif()

    file(MAKE_DIRECTORY "${parent}")
    file(RENAME "${root}" "${unibindEngineDir}" RESULT moved NO_REPLACE)
    file(REMOVE_RECURSE "${staging}")
    if(NOT moved STREQUAL "NO_ERROR" AND NOT EXISTS "${unibindEngineDir}/${unibindEngineLib}")
        message(FATAL_ERROR "unibind: could not move the unpacked engine into ${unibindEngineDir}: ${moved}")
    endif()
endfunction()

# Resolve where this build's engine is, fetching it if it is not anywhere yet,
# and leave the answer in UNIBIND_V8_DIR / UNIBIND_SPIDERMONKEY_DIR for the backend, the
# install rules and the parity trees to read.
#
# The knob stays what it always was: set the variable and nothing is downloaded,
# whatever is or is not under dependencies/.
function(unibind_provide_engine engine)
    if(engine STREQUAL "v8")
        set(variable "UNIBIND_V8_DIR")
    else()
        set(variable "UNIBIND_SPIDERMONKEY_DIR")
    endif()

    if(${variable})
        message(STATUS "unibind: ${variable} is set; nothing is downloaded")
        return()
    endif()

    _unibind_engine_facts("${engine}")
    if(NOT EXISTS "${unibindEngineDir}/${unibindEngineLib}")
        if(NOT UNIBIND_FETCH_ENGINES)
            message(FATAL_ERROR
                "unibind: no ${engine} at ${unibindEngineDir} and UNIBIND_FETCH_ENGINES is OFF.\n"
                "Unpack ${unibindEngineAsset} there, or point -D${variable}=<path> at a build you have.")
        endif()
        _unibind_fetch_engine("${engine}")
    endif()

    # A directory-scope variable rather than a cache entry: a cached answer would
    # outlive the version it was derived from, and the next configure after a
    # version bump would keep using the old tree. The cache entry is the knob,
    # and it stays empty unless someone sets it.
    set(${variable} "${unibindEngineDir}" PARENT_SCOPE)
endfunction()
