# load common dependencies
# this file will also load platform specific dependencies

# Resolve OpenSSL before subprojects run their own find_package(OpenSSL) calls.
# This ensures a user-provided OPENSSL_ROOT_DIR is honored consistently.
find_package(OpenSSL REQUIRED)

# boost, this should be before Simple-Web-Server as it also depends on boost
include(dependencies/Boost_Sunshine)

# web server
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/Simple-Web-Server")

# Common helpers and the Host-owned test framework.
include("${CMAKE_CURRENT_LIST_DIR}/lizardbyte_common.cmake")

# libdisplaydevice
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/libdisplaydevice")

# common dependencies
include("${CMAKE_MODULE_PATH}/dependencies/nv_codec_headers.cmake")
include("${CMAKE_MODULE_PATH}/dependencies/nlohmann_json.cmake")
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# ffmpeg pre-compiled binaries
include("${CMAKE_MODULE_PATH}/dependencies/ffmpeg.cmake")

# Opus
set(OPUS_USE_STATIC ON CACHE BOOL "Static linking for libopus")
include("${CMAKE_MODULE_PATH}/dependencies/FindOpus.cmake")

# platform specific dependencies
if(WIN32)
    include("${CMAKE_MODULE_PATH}/dependencies/windows.cmake")
elseif(UNIX)
    include("${CMAKE_MODULE_PATH}/dependencies/unix.cmake")

    include("${CMAKE_MODULE_PATH}/dependencies/linux.cmake")
endif()
