# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: LGPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)

file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" VERZETA_VERSION LIMIT_COUNT 1)
string(STRIP "${VERZETA_VERSION}" VERZETA_VERSION)
if(NOT VERZETA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "Malformed VERSION file at ${CMAKE_SOURCE_DIR}/VERSION — "
        "expected MAJOR.MINOR.PATCH, got: '${VERZETA_VERSION}'")
endif()

project(VerzetaStudio VERSION ${VERZETA_VERSION} LANGUAGES CXX)

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/verzeta-version.h.in"
    "${CMAKE_BINARY_DIR}/generated/include/verzeta-version.h"
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(VERZETA_BUILD_TESTS "Build unit and integration tests" ON)
option(VERZETA_ENABLE_LLAMACPP "Build with llama.cpp local inference" ON)

set(VERZETA_LLAMA_BACKEND "auto" CACHE STRING
    "llama.cpp acceleration backend: auto|vulkan|cuda|openblas|metal|cpu")
set_property(CACHE VERZETA_LLAMA_BACKEND PROPERTY STRINGS
             auto vulkan cuda openblas metal cpu)

function(_verzeta_apply_llama_backend_flags)
    set(backend "${VERZETA_LLAMA_BACKEND}")

    if(backend STREQUAL "auto")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(backend "vulkan")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(backend "metal")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            set(backend "vulkan")
        else()
            set(backend "cpu")
        endif()
    endif()

    set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
    set(GGML_CUDA   OFF CACHE BOOL "" FORCE)
    set(GGML_BLAS   OFF CACHE BOOL "" FORCE)
    set(GGML_METAL  OFF CACHE BOOL "" FORCE)

    if(backend STREQUAL "vulkan")
        find_package(Vulkan REQUIRED)
        set(GGML_VULKAN ON CACHE BOOL "" FORCE)
    elseif(backend STREQUAL "cuda")
        enable_language(CUDA)
        set(GGML_CUDA ON CACHE BOOL "" FORCE)
    elseif(backend STREQUAL "openblas")
        find_package(BLAS REQUIRED)
        set(GGML_BLAS ON CACHE BOOL "" FORCE)
        set(GGML_BLAS_VENDOR "OpenBLAS" CACHE STRING "" FORCE)
    elseif(backend STREQUAL "metal")
        set(GGML_METAL ON CACHE BOOL "" FORCE)
    elseif(backend STREQUAL "cpu")
    else()
        message(FATAL_ERROR
            "Unknown VERZETA_LLAMA_BACKEND='${VERZETA_LLAMA_BACKEND}'; "
            "expected one of: auto vulkan cuda openblas metal cpu")
    endif()

    set(VERZETA_LLAMA_BACKEND_RESOLVED "${backend}" PARENT_SCOPE)
    message(STATUS "llama.cpp acceleration: ${backend}")
endfunction()

find_package(Qt6 REQUIRED COMPONENTS
    Core Quick QuickControls2 Qml Widgets Network Sql Multimedia Concurrent
    WebSockets)
find_package(KF6Kirigami REQUIRED)
find_package(SQLite3 REQUIRED)

set(VERZETA_HAS_LLAMA FALSE)
if(VERZETA_ENABLE_LLAMACPP)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llama.cpp/CMakeLists.txt")
        set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)

        _verzeta_apply_llama_backend_flags()

        add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
        set(VERZETA_HAS_LLAMA TRUE)
        message(STATUS
            "llama.cpp: building from submodule (static, backend="
            "${VERZETA_LLAMA_BACKEND_RESOLVED})")
    else()
        message(WARNING
            "llama.cpp submodule not found at third_party/llama.cpp — "
            "disabling VERZETA_ENABLE_LLAMACPP. Run "
            "'git submodule update --init --recursive' to enable.")
        set(VERZETA_ENABLE_LLAMACPP OFF)
    endif()
endif()

find_package(KF6SyntaxHighlighting REQUIRED)

find_package(KF6Notifications REQUIRED)

find_package(KF6Archive REQUIRED)

if(WIN32)
    find_package(KF6BreezeIcons REQUIRED)
    find_package(KF6IconThemes REQUIRED)
    find_package(KF6ColorScheme REQUIRED)
    find_package(KF6Config REQUIRED)
endif()

include(cmake/SqliteVec.cmake)

add_subdirectory(backend)
add_subdirectory(frontend)
add_subdirectory(resources)

if(VERZETA_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

include(cmake/packaging/appimage.cmake OPTIONAL)
include(cmake/packaging/flatpak.cmake OPTIONAL)
include(cmake/packaging/snap.cmake OPTIONAL)
include(cmake/packaging/click.cmake OPTIONAL)
