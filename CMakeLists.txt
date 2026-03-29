cmake_minimum_required(VERSION 3.16)

project(miniboinc
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

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

# RandomX
set(RANDOMX_ROOT "" CACHE PATH "Root directory of a RandomX installation")
set(RANDOMX_USING_FETCHCONTENT OFF)
if(PkgConfig_FOUND)
    pkg_check_modules(RANDOMX QUIET IMPORTED_TARGET randomx)
endif()

if(NOT RANDOMX_FOUND)
    find_path(RANDOMX_INCLUDE_DIR
        NAMES randomx.h
        HINTS ${RANDOMX_ROOT}
        PATH_SUFFIXES include
    )
    find_library(RANDOMX_LIBRARY
        NAMES randomx
        HINTS ${RANDOMX_ROOT}
        PATH_SUFFIXES lib
    )

    if(NOT RANDOMX_INCLUDE_DIR OR NOT RANDOMX_LIBRARY)
        include(FetchContent)
        FetchContent_Declare(
            randomx_src
            GIT_REPOSITORY https://github.com/tevador/RandomX.git
            GIT_TAG master
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(randomx_src)
        set(RANDOMX_USING_FETCHCONTENT ON)
        set(RANDOMX_INCLUDE_DIR ${randomx_src_SOURCE_DIR}/src)
    endif()
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

if(RANDOMX_FOUND)
    target_link_libraries(node PRIVATE PkgConfig::RANDOMX)
elseif(RANDOMX_USING_FETCHCONTENT)
    target_link_libraries(node PRIVATE randomx)
    target_include_directories(node PRIVATE ${RANDOMX_INCLUDE_DIR})
else()
    target_include_directories(node PRIVATE ${RANDOMX_INCLUDE_DIR})
    target_link_libraries(node PRIVATE ${RANDOMX_LIBRARY})
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
target_compile_definitions(node PRIVATE)
set_target_properties(node PROPERTIES OUTPUT_NAME "minicoin_node")
