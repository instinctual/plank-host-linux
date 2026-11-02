# common target definitions
# this file will also load platform specific macros

add_executable(sunshine ${SUNSHINE_TARGET_FILES})
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(sunshine ${dep})  # compile these before sunshine
endforeach()

# platform specific target definitions
include(${CMAKE_MODULE_PATH}/targets/linux.cmake)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(PLANK_SYSTEMD_LIBRARY NAMES systemd REQUIRED)
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES ${PLANK_SYSTEMD_LIBRARY})
endif()

target_link_libraries(sunshine ${SUNSHINE_EXTERNAL_LIBRARIES} ${EXTRA_LIBS})
target_compile_definitions(sunshine PUBLIC ${SUNSHINE_DEFINITIONS})
if(PLANK_PRODUCT_BUILD)
    target_compile_definitions(sunshine PRIVATE PLANK_PRODUCT_BUILD=1)
endif()

if(PLANK_ENABLE_TRANSPORT)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The PLANK transport is Linux-only")
    endif()
    if(NOT PLANK_TRANSPORT_DIR)
        cmake_path(ABSOLUTE_PATH CMAKE_SOURCE_DIR
                BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
                NORMALIZE
                OUTPUT_VARIABLE PLANK_HOST_SOURCE_DIR)
        cmake_path(GET PLANK_HOST_SOURCE_DIR PARENT_PATH PLANK_HOST_PARENT_DIR)
        cmake_path(GET PLANK_HOST_PARENT_DIR PARENT_PATH PLANK_ROOT_DIR)
        set(PLANK_TRANSPORT_DIR
                "${PLANK_ROOT_DIR}/protocol/plank-transport")
    endif()
    if(NOT EXISTS "${PLANK_TRANSPORT_DIR}/Cargo.toml" OR
       NOT EXISTS "${PLANK_TRANSPORT_DIR}/include/plank_transport.h" OR
       NOT EXISTS "${PLANK_TRANSPORT_DIR}/include/plank_transport_control.h" OR
       NOT EXISTS "${PLANK_TRANSPORT_DIR}/include/plank_transport_event.h" OR
       NOT EXISTS "${PLANK_TRANSPORT_DIR}/include/plank_transport_input.h")
        message(FATAL_ERROR
                "PLANK native transport source is incomplete: "
                "${PLANK_TRANSPORT_DIR}")
    endif()

    find_program(PLANK_CARGO_EXECUTABLE NAMES cargo REQUIRED)
    set(PLANK_TRANSPORT_CARGO_TARGET_DIR
            "${CMAKE_BINARY_DIR}/plank-transport-cargo")
    set(PLANK_TRANSPORT_LIBRARY
            "${PLANK_TRANSPORT_CARGO_TARGET_DIR}/release/libplank_transport.a")
    set(PLANK_TRANSPORT_CARGO_FEATURES "" CACHE STRING
            "Comma-separated PLANK transport Cargo features")
    set(PLANK_TRANSPORT_CARGO_FEATURE_ARGS)
    if(PLANK_TRANSPORT_CARGO_FEATURES)
        list(APPEND PLANK_TRANSPORT_CARGO_FEATURE_ARGS
                --features "${PLANK_TRANSPORT_CARGO_FEATURES}")
        message(STATUS
                "PLANK transport Cargo features: ${PLANK_TRANSPORT_CARGO_FEATURES}")
    endif()
    add_custom_target(plank-transport-build
            COMMAND ${CMAKE_COMMAND} -E env
                    "CARGO_TARGET_DIR=${PLANK_TRANSPORT_CARGO_TARGET_DIR}"
                    "${PLANK_CARGO_EXECUTABLE}" build
                    --locked --offline --release
                    ${PLANK_TRANSPORT_CARGO_FEATURE_ARGS}
                    --manifest-path "${PLANK_TRANSPORT_DIR}/Cargo.toml"
            WORKING_DIRECTORY "${PLANK_TRANSPORT_DIR}"
            COMMENT "Building PLANK native transport"
            VERBATIM)
    add_dependencies(sunshine plank-transport-build)
    target_include_directories(sunshine PRIVATE
            "${PLANK_TRANSPORT_DIR}/include")
    target_compile_definitions(sunshine PRIVATE PLANK_TRANSPORT=1)
    target_link_libraries(sunshine
            "${PLANK_TRANSPORT_LIBRARY}"
            ${CMAKE_DL_LIBS}
            Threads::Threads
            m
            rt)
    set_property(TARGET sunshine APPEND PROPERTY LINK_DEPENDS
            "${PLANK_TRANSPORT_LIBRARY}")
endif()

# CLion complains about unknown flags after running cmake, and cannot add symbols to the index for cuda files
if(CUDA_INHERIT_COMPILE_OPTIONS)
    foreach(flag IN LISTS SUNSHINE_COMPILE_OPTIONS)
        list(APPEND SUNSHINE_COMPILE_OPTIONS_CUDA "$<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=${flag}>")
    endforeach()
endif()

target_compile_options(sunshine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(sunshine PRIVATE ${SUNSHINE_LINK_OPTIONS})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Retain the upstream CMake target name to minimize divergence while
    # exposing the PLANK product identity to procfs and operators.
    set_target_properties(sunshine PROPERTIES OUTPUT_NAME "plank-host")

    find_library(PLANK_PAM_LIBRARY NAMES pam REQUIRED)
    add_executable(plank-pam-broker
            "${CMAKE_SOURCE_DIR}/src/auth/pam_broker.cpp"
            "${CMAKE_SOURCE_DIR}/src/auth/pam_broker_protocol.h")
    target_link_libraries(plank-pam-broker PRIVATE ${PLANK_PAM_LIBRARY})
    target_compile_options(plank-pam-broker PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
    target_link_options(plank-pam-broker PRIVATE ${SUNSHINE_LINK_OPTIONS})
    install(TARGETS plank-pam-broker
            RUNTIME DESTINATION bin
            COMPONENT sunshine)

    add_executable(plank-host-supervisor
            "${CMAKE_SOURCE_DIR}/src/session/host_supervisor.cpp"
            "${CMAKE_SOURCE_DIR}/src/session/session_context.cpp"
            "${CMAKE_SOURCE_DIR}/src/session/session_context.h")
    target_link_libraries(plank-host-supervisor PRIVATE ${PLANK_SYSTEMD_LIBRARY})
    target_compile_options(plank-host-supervisor PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
    target_link_options(plank-host-supervisor PRIVATE ${SUNSHINE_LINK_OPTIONS})
    install(TARGETS plank-host-supervisor
            RUNTIME DESTINATION bin
            COMPONENT sunshine)
endif()

# docs
if(BUILD_DOCS)
    add_subdirectory(third-party/doxyconfig docs)
endif()

# tests
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Release assertions are disabled independently of test compilation.
string(TOUPPER "x${CMAKE_BUILD_TYPE}" BUILD_TYPE)
if(NOT "${BUILD_TYPE}" STREQUAL "XDEBUG")
    add_definitions(-DNDEBUG)
endif()
