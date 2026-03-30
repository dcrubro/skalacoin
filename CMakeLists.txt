cmake_minimum_required(VERSION 3.16)

project(miniboinc
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

# secp256k1 (Bitcoin Core library)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SECP256K1 QUIET IMPORTED_TARGET libsecp256k1)
endif()

if(NOT SECP256K1_FOUND)
    find_path(SECP256K1_INCLUDE_DIR NAMES secp256k1.h)
    find_library(SECP256K1_LIBRARY NAMES secp256k1)
    if(NOT SECP256K1_INCLUDE_DIR OR NOT SECP256K1_LIBRARY)
        message(FATAL_ERROR "secp256k1 not found. Install with: brew install secp256k1")
    endif()
endif()

# Autolykos2 CPU reference backend (optional)
option(MINICOIN_ENABLE_AUTOLYKOS2_REF "Enable Autolykos2 CPU reference backend" ON)
set(MINICOIN_AUTOLYKOS2_REF_AVAILABLE OFF)

if(MINICOIN_ENABLE_AUTOLYKOS2_REF)
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
    target_link_libraries(autolykos2_ref PRIVATE
        ${CMAKE_THREAD_LIBS_INIT}
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    set(MINICOIN_AUTOLYKOS2_REF_AVAILABLE ON)
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

if(SECP256K1_FOUND)
    target_link_libraries(node PRIVATE PkgConfig::SECP256K1)
else()
    target_include_directories(node PRIVATE ${SECP256K1_INCLUDE_DIR})
    target_link_libraries(node PRIVATE ${SECP256K1_LIBRARY})
endif()

if(MINICOIN_AUTOLYKOS2_REF_AVAILABLE)
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
    $<$<BOOL:${MINICOIN_AUTOLYKOS2_REF_AVAILABLE}>:MINICOIN_AUTOLYKOS2_REF_AVAILABLE>
)
set_target_properties(node PROPERTIES OUTPUT_NAME "minicoin_node")
