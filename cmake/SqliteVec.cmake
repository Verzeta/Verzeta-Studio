# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: LGPL-3.0-or-later

set(SQLITE_VEC_VERSION "0.1.10-alpha.4")
set(SQLITE_VEC_BASE_URL
    "https://github.com/asg017/sqlite-vec/releases/download/v${SQLITE_VEC_VERSION}/sqlite-vec-${SQLITE_VEC_VERSION}-loadable")

set(SQLITE_VEC_LOADABLE "" CACHE INTERNAL "Path to the downloaded sqlite-vec loadable, or empty")

set(_svec_target "")
set(_svec_sha "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_svec_target "linux-aarch64")
        set(_svec_sha "32b2333e0b14d318036320c729cab35655b32e8ac3edeed1d048369e6b0b6095")
    else()
        set(_svec_target "linux-x86_64")
        set(_svec_sha "0349d1cffea2da145eb195ee0fa0e7803544a9491a458c58aeacfb434517abf1")
    endif()
elseif(WIN32)
    set(_svec_target "windows-x86_64")
    set(_svec_sha "39d29bd26252b61daf7e1d3d0b538336e5364e1022c61084bba91f6570941017")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_svec_target "macos-aarch64")
        set(_svec_sha "9c4c3c9fee1cd68d07028f90c9e31b67f13ca1a1737435ae569e8fe7a17b5a91")
    else()
        set(_svec_target "macos-x86_64")
        set(_svec_sha "eaa956fa7f145260c7f607ae5e094e48ce6366bb8d0b284266504405b62c7d17")
    endif()
endif()

if(_svec_target STREQUAL "")
    message(WARNING "sqlite-vec: no prebuilt loadable for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}; "
                    "vector search will fall back to brute-force at runtime.")
    return()
endif()

set(_svec_dir "${CMAKE_BINARY_DIR}/sqlite-vec")
file(MAKE_DIRECTORY "${_svec_dir}")
set(_svec_tgz "${_svec_dir}/sqlite-vec-${_svec_target}.tar.gz")

if(NOT EXISTS "${_svec_tgz}")
    message(STATUS "sqlite-vec: downloading ${_svec_target} (v${SQLITE_VEC_VERSION}) ...")
    file(DOWNLOAD
        "${SQLITE_VEC_BASE_URL}-${_svec_target}.tar.gz"
        "${_svec_tgz}"
        EXPECTED_HASH SHA256=${_svec_sha}
        TIMEOUT 120
        STATUS _svec_dl)
    list(GET _svec_dl 0 _svec_rc)
    if(NOT _svec_rc EQUAL 0)
        file(REMOVE "${_svec_tgz}")
        message(WARNING "sqlite-vec: download failed (${_svec_dl}); "
                        "vector search will fall back to brute-force at runtime.")
        return()
    endif()
endif()

file(ARCHIVE_EXTRACT INPUT "${_svec_tgz}" DESTINATION "${_svec_dir}")
file(GLOB _svec_libs "${_svec_dir}/vec0.*")
if(_svec_libs)
    list(GET _svec_libs 0 _svec_lib)
    set(SQLITE_VEC_LOADABLE "${_svec_lib}" CACHE INTERNAL "Path to the downloaded sqlite-vec loadable" FORCE)
    message(STATUS "sqlite-vec: loadable ready at ${SQLITE_VEC_LOADABLE}")
else()
    message(WARNING "sqlite-vec: archive extracted but no vec0.* found; brute-force fallback at runtime.")
endif()
