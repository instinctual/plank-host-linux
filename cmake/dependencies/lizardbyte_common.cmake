# Own the GoogleTest version instead of inheriting the helper library's nested
# pin. Define its targets first so lizardbyte-common uses this same framework.
# Release builds must not configure GoogleTest or the test-support library.
include_guard(GLOBAL)

if(BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest" FORCE)
    set(INSTALL_GMOCK OFF CACHE BOOL "Install GoogleMock" FORCE)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../third-party/googletest"
                     "${CMAKE_BINARY_DIR}/third-party/googletest")
endif()

set(LIZARDBYTE_COMMON_BUILD_TEST_SUPPORT ${BUILD_TESTS}
        CACHE BOOL "Build lizardbyte-common GoogleTest support helpers" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../third-party/lizardbyte-common"
                 "${CMAKE_BINARY_DIR}/third-party/lizardbyte-common")
