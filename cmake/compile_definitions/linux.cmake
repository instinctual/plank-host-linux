# linux specific compile definitions

if(FREEBSD)
    add_compile_definitions(SUNSHINE_PLATFORM="freebsd")
    # FreeBSD installs packages to /usr/local/lib, which is not in the default linker search path.
    # link_directories() is directory-scoped and propagates to all subdirectories (including tests/),
    # so all targets (sunshine, test_sunshine) can resolve libraries found via pkg_check_modules.
    link_directories(/usr/local/lib)
else()
    add_compile_definitions(SUNSHINE_PLATFORM="linux")
endif()

# cuda
set(CUDA_FOUND OFF)
if(${SUNSHINE_ENABLE_CUDA})
    include(CheckLanguage)
    check_language(CUDA)

    if(CMAKE_CUDA_COMPILER)
        set(CUDA_FOUND ON)
        enable_language(CUDA)

        message(STATUS "CUDA Compiler Version: ${CMAKE_CUDA_COMPILER_VERSION}")
        set(CMAKE_CUDA_ARCHITECTURES "")

        # https://docs.nvidia.com/cuda/archive/12.0.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 75 80 86 87 89 90)
        else()
            message(FATAL_ERROR
                    "Sunshine requires a minimum CUDA Compiler version of 12.0.
                    Found version: ${CMAKE_CUDA_COMPILER_VERSION}"
            )
        endif()

        # https://docs.nvidia.com/cuda/archive/12.8.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.8)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 100 101 120)
        endif()

        # https://docs.nvidia.com/cuda/archive/12.9.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.9)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 103 121)
        endif()

        # https://docs.nvidia.com/cuda/archive/13.0.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
            list(REMOVE_ITEM CMAKE_CUDA_ARCHITECTURES 101)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 110)
        else()
            list(APPEND CMAKE_CUDA_ARCHITECTURES 50 52 53 60 61 62 70 72)
        endif()

        # sort the architectures
        list(SORT CMAKE_CUDA_ARCHITECTURES COMPARE NATURAL)

        # message(STATUS "CUDA NVCC Flags: ${CUDA_NVCC_FLAGS}")
        message(STATUS "CUDA Architectures: ${CMAKE_CUDA_ARCHITECTURES}")
    elseif(${CUDA_FAIL_ON_MISSING})
        message(FATAL_ERROR
                "CUDA not found.
                If this is intentional, set '-DSUNSHINE_ENABLE_CUDA=OFF' or '-DCUDA_FAIL_ON_MISSING=OFF'"
        )
    endif()
endif()
if(CUDA_FOUND)
    include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvfbc")
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.cu"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.cpp"
            "${CMAKE_SOURCE_DIR}/third-party/nvfbc/NvFBC.h")

    add_compile_definitions(SUNSHINE_BUILD_CUDA)
endif()

# DRM/KMS helpers share the capture device with CUDA.
if(${SUNSHINE_ENABLE_DRM})
    find_package(LIBDRM REQUIRED)
else()
    set(LIBDRM_FOUND OFF)
endif()
if(LIBDRM_FOUND)
    include_directories(SYSTEM ${LIBDRM_INCLUDE_DIRS})
    if(${SUNSHINE_ENABLE_DRM})
        list(APPEND PLATFORM_LIBRARIES ${LIBDRM_LIBRARIES})
        add_compile_definitions(SUNSHINE_BUILD_DRM)
        list(APPEND PLATFORM_TARGET_FILES
                "${CMAKE_SOURCE_DIR}/src/platform/linux/kmsgrab.cpp")
        list(APPEND SUNSHINE_DEFINITIONS EGL_NO_X11=1)
    endif()
endif()

# Capabilities
if(LINUX)
    find_package(LIBCAP REQUIRED)
    include_directories(SYSTEM ${LIBCAP_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${LIBCAP_LIBRARIES})
endif()

# evdev
include(dependencies/libevdev_Sunshine)

# x11
if(${SUNSHINE_ENABLE_X11})
    find_package(X11 REQUIRED)
else()
    set(X11_FOUND OFF)
endif()
if(X11_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_X11)
    include_directories(SYSTEM ${X11_INCLUDE_DIR})
    list(APPEND PLATFORM_LIBRARIES ${X11_LIBRARIES})
    # A dedicated XCB connection isolates clipboard protocol errors from Xlib
    # capture threads and their process-wide error handler.
    pkg_check_modules(PLANK_XCB REQUIRED xcb)
    include_directories(SYSTEM ${PLANK_XCB_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${PLANK_XCB_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11grab.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11grab.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11_clipboard.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11_clipboard.cpp")
endif()

# GIO
pkg_check_modules(GIO gio-2.0 gio-unix-2.0 REQUIRED)
if(GIO_FOUND)
    include_directories(SYSTEM ${GIO_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${GIO_LIBRARIES})
endif()

if(NOT CUDA_FOUND AND NOT LIBDRM_FOUND AND NOT X11_FOUND)
    message(FATAL_ERROR "No supported Linux capture backend found (CUDA, DRM or X11)")
endif()

# These need to be set before common.cmake adds the libvirtualhid subdirectory in order for them to be picked up
set(LIBEVDEV_CUSTOM_INCLUDE_DIR "${EVDEV_INCLUDE_DIR}")
set(LIBEVDEV_CUSTOM_LIBRARY "${EVDEV_LIBRARY}")

list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/linux/input/virtualhid.cpp")

include_directories(SYSTEM
        ${CMAKE_BINARY_DIR}/generated-src)

list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/linux/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/graphics.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/graphics.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/audio.cpp")

list(APPEND PLATFORM_LIBRARIES
        dl
        pulse
        pulse-simple)

list(APPEND SUNSHINE_EXTERNAL_LIBRARIES glad)
