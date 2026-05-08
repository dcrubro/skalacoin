cmake_minimum_required(VERSION 3.16)

project(skalacoin
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)
include(FetchContent)

# OpenSSL
find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
    if(APPLE)
        execute_process(
            COMMAND brew --prefix openssl@3
            OUTPUT_VARIABLE HOMEBREW_OPENSSL_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(HOMEBREW_OPENSSL_PREFIX)
            set(OPENSSL_ROOT_DIR "${HOMEBREW_OPENSSL_PREFIX}")
        endif()
    endif()
    find_package(OpenSSL REQUIRED)
endif()

# libcurl (required by autolykos2 vendored code)
find_package(CURL QUIET)
if(NOT CURL_FOUND)
    message(STATUS "libcurl not found on system; attempting to fetch/build via FetchContent")
    FetchContent_Declare(
        curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG curl-8_4_0
        GIT_SHALLOW TRUE
    )
    # Try to make it available (this will add_subdirectory if curl provides CMake)
    FetchContent_MakeAvailable(curl)
endif()

# secp256k1 (Bitcoin Core library)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SECP256K1 QUIET IMPORTED_TARGET libsecp256k1)
endif()

# Try pkg-config / system first, then fall back to find_*(). If still not found
# attempt to fetch and build a bundled copy of libsecp256k1 using FetchContent.
if(NOT SECP256K1_FOUND)
    find_path(SECP256K1_INCLUDE_DIR NAMES secp256k1.h)
    find_library(SECP256K1_LIBRARY NAMES secp256k1)
    if(SECP256K1_INCLUDE_DIR AND SECP256K1_LIBRARY)
        set(SECP256K1_FOUND TRUE)
    endif()
endif()

if(NOT SECP256K1_FOUND)
    message(STATUS "secp256k1 not found on system; fetching and building a vendored copy")
    include(FetchContent)
    FetchContent_Declare(
        secp256k1
        GIT_REPOSITORY https://github.com/bitcoin-core/secp256k1.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(secp256k1)
    if(NOT secp256k1_POPULATED)
        FetchContent_Populate(secp256k1)
        # Prefer a CMake build if present
        if(EXISTS "${secp256k1_SOURCE_DIR}/CMakeLists.txt")
            add_subdirectory(${secp256k1_SOURCE_DIR} ${secp256k1_BINARY_DIR})
            if(TARGET secp256k1)
                set(SECP256K1_FOUND TRUE)
                set(SECP256K1_TARGET secp256k1)
            elseif(TARGET libsecp256k1)
                set(SECP256K1_FOUND TRUE)
                set(SECP256K1_TARGET libsecp256k1)
            endif()
        else()
            # Fall back to the autotools build path. Install into a private prefix
            set(SECP256K1_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/secp256k1/install")
            file(MAKE_DIRECTORY ${SECP256K1_INSTALL_DIR})
            execute_process(COMMAND ./autogen.sh
                WORKING_DIRECTORY ${secp256k1_SOURCE_DIR}
                RESULT_VARIABLE _secp_autogen_result
                OUTPUT_QUIET ERROR_QUIET)
            if(NOT _secp_autogen_result EQUAL 0)
                message(FATAL_ERROR "Failed to run autogen.sh for secp256k1")
            endif()
            execute_process(COMMAND ./configure --enable-module-ecdh --enable-experimental --prefix=${SECP256K1_INSTALL_DIR}
                WORKING_DIRECTORY ${secp256k1_SOURCE_DIR}
                RESULT_VARIABLE _secp_configure_result
                OUTPUT_QUIET ERROR_QUIET)
            if(NOT _secp_configure_result EQUAL 0)
                message(FATAL_ERROR "Failed to configure secp256k1")
            endif()
            execute_process(COMMAND make
                WORKING_DIRECTORY ${secp256k1_SOURCE_DIR}
                RESULT_VARIABLE _secp_make_result
                OUTPUT_QUIET ERROR_QUIET)
            if(NOT _secp_make_result EQUAL 0)
                message(FATAL_ERROR "Failed to build secp256k1")
            endif()
            execute_process(COMMAND make install
                WORKING_DIRECTORY ${secp256k1_SOURCE_DIR}
                RESULT_VARIABLE _secp_make_install_result
                OUTPUT_QUIET ERROR_QUIET)
            set(SECP256K1_INCLUDE_DIR ${SECP256K1_INSTALL_DIR}/include)
            set(SECP256K1_LIBRARY ${SECP256K1_INSTALL_DIR}/lib/libsecp256k1.a)
            if(EXISTS ${SECP256K1_LIBRARY})
                set(SECP256K1_FOUND TRUE)
            endif()
        endif()
    endif()
endif()

# Autolykos2 CPU reference backend (optional)
option(SKALACOIN_ENABLE_AUTOLYKOS2_REF "Enable Autolykos2 CPU reference backend" ON)
set(SKALACOIN_AUTOLYKOS2_REF_AVAILABLE OFF)

if(SKALACOIN_ENABLE_AUTOLYKOS2_REF)
    FetchContent_Declare(
        autolykos2_ref_src
        GIT_REPOSITORY https://github.com/mhssamadani/Autolykos2_NV_Miner.git
        GIT_TAG main
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(autolykos2_ref_src)

    set(AUTOLYKOS2_REF_BASE ${autolykos2_ref_src_SOURCE_DIR}/secp256k1)
    set(AUTOLYKOS2_REF_SOURCES
        ${AUTOLYKOS2_REF_BASE}/src/cpuAutolykos.cc
        ${AUTOLYKOS2_REF_BASE}/src/conversion.cc
        ${AUTOLYKOS2_REF_BASE}/src/cryptography.cc
        ${AUTOLYKOS2_REF_BASE}/src/definitions.cc
        ${AUTOLYKOS2_REF_BASE}/src/easylogging++.cc
        ${AUTOLYKOS2_REF_BASE}/src/jsmn.c
        ${PROJECT_SOURCE_DIR}/src/autolykos2/easylogging_init.cpp
        ${PROJECT_SOURCE_DIR}/src/autolykos2/autolykos2_ref_wrapper.cpp
    )

    add_library(autolykos2_ref STATIC ${AUTOLYKOS2_REF_SOURCES})
    target_include_directories(autolykos2_ref PRIVATE ${AUTOLYKOS2_REF_BASE}/include)
    # Upstream source uses `malloc/free/exit/EXIT_FAILURE` without including
    # stdlib headers in some C++ translation units. AppleClang can compile this,
    # while Linux Clang fails. Force-include stdlib.h for C++ in this vendored lib.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(autolykos2_ref PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-include>
            $<$<COMPILE_LANGUAGE:CXX>:stdlib.h>
        )
    elseif(MSVC)
        target_compile_options(autolykos2_ref PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/FIstdlib.h>
        )
    endif()
    if(TARGET CURL::libcurl)
        set(_AUTOLYKOS2_CURL_LIB CURL::libcurl)
    elseif(DEFINED CURL_LIBRARIES AND CURL_LIBRARIES)
        set(_AUTOLYKOS2_CURL_LIB ${CURL_LIBRARIES})
    else()
        set(_AUTOLYKOS2_CURL_LIB "")
    endif()

    target_link_libraries(autolykos2_ref PRIVATE
        ${CMAKE_THREAD_LIBS_INIT}
        OpenSSL::SSL
        OpenSSL::Crypto
        $<$<BOOL:$_AUTOLYKOS2_CURL_LIB>:$_AUTOLYKOS2_CURL_LIB>
    )

    if(NOT _AUTOLYKOS2_CURL_LIB)
        message(FATAL_ERROR "autolykos2_ref requires libcurl (curl/curl.h). Install libcurl devel package or allow FetchContent to build it.")
    endif()
    set(SKALACOIN_AUTOLYKOS2_REF_AVAILABLE ON)
endif()

# ---------------------------------------------------------
# Output directories
# ---------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

foreach(OUTPUTCONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib)
endforeach()

# Node
file(GLOB_RECURSE NODE_SRC CONFIGURE_DEPENDS src/*.c)
add_executable(node ${NODE_SRC})
target_link_libraries(node PRIVATE
    ${CMAKE_THREAD_LIBS_INIT}
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(TARGET PkgConfig::SECP256K1)
    target_link_libraries(node PRIVATE PkgConfig::SECP256K1)
elseif(DEFINED SECP256K1_TARGET AND TARGET ${SECP256K1_TARGET})
    target_link_libraries(node PRIVATE ${SECP256K1_TARGET})
elseif(SECP256K1_FOUND AND SECP256K1_LIBRARY)
    target_include_directories(node PRIVATE ${SECP256K1_INCLUDE_DIR})
    target_link_libraries(node PRIVATE ${SECP256K1_LIBRARY})
else()
    message(FATAL_ERROR "secp256k1 not found and no vendored target available. Install libsecp256k1 or enable FetchContent builds.")
endif()

if(SKALACOIN_AUTOLYKOS2_REF_AVAILABLE)
    target_link_libraries(node PRIVATE autolykos2_ref)
endif()

target_include_directories(node PRIVATE 
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(node PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -g
)
target_compile_definitions(node PRIVATE
    CHAIN_DATA_DIR="${CMAKE_BINARY_DIR}/chain_data"
    $<$<BOOL:${SKALACOIN_AUTOLYKOS2_REF_AVAILABLE}>:SKALACOIN_AUTOLYKOS2_REF_AVAILABLE>
)
set_target_properties(node PROPERTIES OUTPUT_NAME "skalacoin_node")
