cmake_minimum_required(VERSION 3.16)

project(skalacoin
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------
# Build configuration
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug     # -Og -g3, ASan+UBSan, GCC -fanalyzer
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # -O3, _FORTIFY_SOURCE, no instrumentation
#
# Debug is tuned to *find* memory bugs, Release to *survive* them. The warning
# set is the same in both; only the instrumentation differs.
# ---------------------------------------------------------
get_property(SKALACOIN_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT SKALACOIN_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    message(STATUS "No CMAKE_BUILD_TYPE specified; defaulting to Debug")
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

# OFF until src/ is clean under the warning set below; flip it on afterwards so
# it stays clean.
option(SKALACOIN_WERROR "Debug: treat warnings as errors" OFF)
option(SKALACOIN_ENABLE_SANITIZERS "Debug: build with AddressSanitizer + UndefinedBehaviorSanitizer" ON)
option(SKALACOIN_ENABLE_ANALYZER "Debug: run the compiler's static analyzer (GCC -fanalyzer)" ON)
option(SKALACOIN_ENABLE_HARDENING "All configs: stack protector, _FORTIFY_SOURCE, RELRO/NOW, CFI" ON)
option(SKALACOIN_ENABLE_LTO "Optimized configs: link-time optimization" OFF)

include(CheckCCompilerFlag)
include(CheckLinkerFlag)

# Most of the interesting memory diagnostics below only exist from a certain
# GCC/Clang version onwards, and several are architecture specific. Probe every
# flag instead of gating on compiler version, so an older or foreign toolchain
# silently gets the subset it understands rather than failing to configure.
#
# A flag the driver merely tolerates is not a flag that does anything (Clang
# accepts -fstack-clash-protection on arm64 and then ignores it), so the probe
# promotes "argument unused" to an error where the compiler supports that.
check_c_compiler_flag(-Werror=unused-command-line-argument SKALACOIN_HAS_WERROR_UNUSED_ARG)
if(SKALACOIN_HAS_WERROR_UNUSED_ARG)
    set(SKALACOIN_FLAG_PROBE_STRICT "-Werror=unused-command-line-argument")
else()
    set(SKALACOIN_FLAG_PROBE_STRICT "")
endif()

function(skalacoin_append_supported_c_flags out_var)
    set(_accepted ${${out_var}})
    set(CMAKE_REQUIRED_FLAGS "${SKALACOIN_FLAG_PROBE_STRICT}")
    foreach(_flag IN LISTS ARGN)
        string(MAKE_C_IDENTIFIER "SKALACOIN_HAS_CFLAG_${_flag}" _cache_var)
        check_c_compiler_flag("${_flag}" ${_cache_var})
        if(${_cache_var})
            list(APPEND _accepted "${_flag}")
        endif()
    endforeach()
    set(${out_var} "${_accepted}" PARENT_SCOPE)
endfunction()

# Instrumentation such as -fsanitize=address has to reach the linker as well,
# and is only usable if the matching runtime library actually exists, so the
# probe hands the flag to both halves of the try-compile.
function(skalacoin_append_supported_instrument_flags out_var)
    set(_accepted ${${out_var}})
    foreach(_flag IN LISTS ARGN)
        string(MAKE_C_IDENTIFIER "SKALACOIN_HAS_INSTRUMENT_${_flag}" _cache_var)
        set(CMAKE_REQUIRED_FLAGS "${SKALACOIN_FLAG_PROBE_STRICT}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "${_flag}")
        check_c_compiler_flag("${_flag}" ${_cache_var})
        if(${_cache_var})
            list(APPEND _accepted "${_flag}")
        endif()
    endforeach()
    set(${out_var} "${_accepted}" PARENT_SCOPE)
endfunction()

function(skalacoin_append_supported_link_flags out_var)
    set(_accepted ${${out_var}})
    foreach(_flag IN LISTS ARGN)
        string(MAKE_C_IDENTIFIER "SKALACOIN_HAS_LDFLAG_${_flag}" _cache_var)
        check_linker_flag(C "${_flag}" ${_cache_var})
        if(${_cache_var})
            list(APPEND _accepted "${_flag}")
        endif()
    endforeach()
    set(${out_var} "${_accepted}" PARENT_SCOPE)
endfunction()

# Warnings for our own C code; the per-config flag sets are applied through
# generator expressions so multi-config generators (Xcode, VS) work too.
set(SKALACOIN_C_WARNINGS "")
set(SKALACOIN_C_FLAGS_DEBUG "")
set(SKALACOIN_C_FLAGS_OPTIMIZED "")
# Instrumentation that must be handed to the linker as well, and that also
# covers the vendored code we link in.
set(SKALACOIN_INSTRUMENT_COMPILE "")
set(SKALACOIN_INSTRUMENT_LINK "")

if(MSVC)
    list(APPEND SKALACOIN_C_WARNINGS /W4 /permissive- /sdl)
    list(APPEND SKALACOIN_C_FLAGS_DEBUG /Od /RTC1 /GS)
    list(APPEND SKALACOIN_C_FLAGS_OPTIMIZED /O2 /GS /guard:cf)
    if(SKALACOIN_WERROR)
        list(APPEND SKALACOIN_C_FLAGS_DEBUG /WX)
    endif()
    if(SKALACOIN_ENABLE_ANALYZER)
        list(APPEND SKALACOIN_C_FLAGS_DEBUG /analyze)
    endif()
    if(SKALACOIN_ENABLE_SANITIZERS)
        list(APPEND SKALACOIN_INSTRUMENT_COMPILE /fsanitize=address)
    endif()
else()
    # Portable warning set, memory-safety first.
    skalacoin_append_supported_c_flags(SKALACOIN_C_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wcast-qual
        -Wcast-align
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wold-style-definition
        -Wbad-function-cast
        -Wwrite-strings
        -Wformat=2
        -Wnull-dereference
        -Wdouble-promotion
        -Wfloat-equal          # consensus code must stay integer-only
        -Wvla
        -Wstack-protector
        -Wpointer-arith
        -Wundef
        -Winit-self
        -Wmissing-include-dirs
    )

    if(CMAKE_C_COMPILER_ID MATCHES "GNU")
        # GCC-only diagnostics. The -W*=N forms ask for the strictest level:
        # more false positives, but they catch out-of-bounds writes, dangling
        # pointers and double frees that -Wall/-Wextra walk straight past.
        skalacoin_append_supported_c_flags(SKALACOIN_C_WARNINGS
            -Warray-bounds=2
            -Wstringop-overflow=4
            -Wstringop-truncation
            -Wformat-overflow=2
            -Wformat-truncation=2
            -Wuse-after-free=3
            -Wdangling-pointer=2
            -Wfree-nonheap-object
            -Walloc-zero
            -Walloca
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wjump-misses-init
            -Wtrampolines
            -Wflex-array-member-not-at-end
        )
        # -fanalyzer is a whole-path symbolic execution pass (leaks, double
        # free, use-after-free, NULL derefs across function boundaries). It is
        # slow, so Debug only.
        if(SKALACOIN_ENABLE_ANALYZER)
            skalacoin_append_supported_c_flags(SKALACOIN_C_FLAGS_DEBUG -fanalyzer)
        endif()
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
        # Clang-only diagnostics. Clang has no in-compiler equivalent of
        # -fanalyzer; run `scan-build cmake --build build` for that.
        skalacoin_append_supported_c_flags(SKALACOIN_C_WARNINGS
            -Warray-bounds-pointer-arithmetic
            -Wconditional-uninitialized
            -Wshift-sign-overflow
            -Wassign-enum
            -Wcomma
            -Wloop-analysis
            -Wthread-safety
            -Wover-aligned
        )
    endif()

    # -Og keeps the code steppable while still running the optimizer passes
    # that -Wmaybe-uninitialized / -Wstringop-* rely on; at -O0 those warnings
    # go quiet. -fno-omit-frame-pointer buys readable sanitizer backtraces.
    skalacoin_append_supported_c_flags(SKALACOIN_C_FLAGS_DEBUG
        -Og
        -g3
        -fno-omit-frame-pointer
    )
    if(SKALACOIN_WERROR)
        list(APPEND SKALACOIN_C_FLAGS_DEBUG -Werror)
    endif()

    if(SKALACOIN_ENABLE_HARDENING)
        skalacoin_append_supported_c_flags(SKALACOIN_C_FLAGS_DEBUG
            -fstack-protector-strong
        )
        # _FORTIFY_SOURCE needs an optimized build to see through the buffer
        # sizes, and it fights ASan's interceptors, so it is Release-only.
        skalacoin_append_supported_c_flags(SKALACOIN_C_FLAGS_OPTIMIZED
            -U_FORTIFY_SOURCE
            -D_FORTIFY_SOURCE=3
            -fstack-protector-strong
            -fstack-clash-protection
        )
        skalacoin_append_supported_instrument_flags(SKALACOIN_INSTRUMENT_COMPILE
            -fcf-protection=full         # x86_64 CET
            -mbranch-protection=standard # aarch64 BTI/PAC
        )
        skalacoin_append_supported_link_flags(SKALACOIN_INSTRUMENT_LINK
            "LINKER:-z,relro"
            "LINKER:-z,now"
            "LINKER:-z,noexecstack"
        )
    endif()

    if(SKALACOIN_ENABLE_SANITIZERS)
        # -fno-sanitize-recover makes UB abort instead of printing and
        # continuing, so a bad shift or overflow cannot be ignored in CI.
        set(SKALACOIN_SANITIZER_FLAGS "")
        skalacoin_append_supported_instrument_flags(SKALACOIN_SANITIZER_FLAGS
            -fsanitize=address
            -fsanitize=undefined
            -fno-sanitize-recover=undefined
            -fno-omit-frame-pointer
        )
        list(APPEND SKALACOIN_INSTRUMENT_COMPILE "$<$<CONFIG:Debug>:${SKALACOIN_SANITIZER_FLAGS}>")
        list(APPEND SKALACOIN_INSTRUMENT_LINK "$<$<CONFIG:Debug>:${SKALACOIN_SANITIZER_FLAGS}>")
    endif()
endif()

if(SKALACOIN_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT SKALACOIN_IPO_SUPPORTED OUTPUT SKALACOIN_IPO_ERROR)
    if(NOT SKALACOIN_IPO_SUPPORTED)
        message(WARNING "LTO requested but unsupported by this toolchain: ${SKALACOIN_IPO_ERROR}")
    endif()
endif()

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
    # Vendored code gets the instrumentation but not our warning set: sanitizers
    # only see a bug if the translation unit that owns the memory is compiled
    # with them, and this library allocates buffers that our code touches.
    target_compile_options(autolykos2_ref PRIVATE "${SKALACOIN_INSTRUMENT_COMPILE}")
    target_link_options(autolykos2_ref PRIVATE "${SKALACOIN_INSTRUMENT_LINK}")
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
        target_link_libraries(autolykos2_ref PRIVATE
            ${CMAKE_THREAD_LIBS_INIT}
            OpenSSL::SSL
            OpenSSL::Crypto
            CURL::libcurl
        )
    elseif(DEFINED CURL_LIBRARIES AND CURL_LIBRARIES)
        target_link_libraries(autolykos2_ref PRIVATE
            ${CMAKE_THREAD_LIBS_INIT}
            OpenSSL::SSL
            OpenSSL::Crypto
            ${CURL_LIBRARIES}
        )
    else()
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
    "${SKALACOIN_C_WARNINGS}"
    "${SKALACOIN_INSTRUMENT_COMPILE}"
    "$<$<CONFIG:Debug>:${SKALACOIN_C_FLAGS_DEBUG}>"
    "$<$<NOT:$<CONFIG:Debug>>:${SKALACOIN_C_FLAGS_OPTIMIZED}>"
)
target_link_options(node PRIVATE "${SKALACOIN_INSTRUMENT_LINK}")

if(SKALACOIN_ENABLE_LTO AND SKALACOIN_IPO_SUPPORTED)
    set_target_properties(node PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
        INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON
    )
endif()

target_compile_definitions(node PRIVATE
    CHAIN_DATA_DIR="${CMAKE_BINARY_DIR}/chain_data"
    $<$<BOOL:${SKALACOIN_AUTOLYKOS2_REF_AVAILABLE}>:SKALACOIN_AUTOLYKOS2_REF_AVAILABLE>
    $<$<BOOL:1>:_POSIX_C_SOURCE=200809L>
    # getifaddrs() (used to learn our own addresses) is a BSD extension, not POSIX; glibc hides it
    # unless the default set is requested as well.
    $<$<BOOL:1>:_DEFAULT_SOURCE>
)
set_target_properties(node PROPERTIES OUTPUT_NAME "skalacoin_node")

# ---------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------
message(STATUS "skalacoin: build type      ${CMAKE_BUILD_TYPE}")
message(STATUS "skalacoin: compiler        ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "skalacoin: warnings-as-err ${SKALACOIN_WERROR}")
message(STATUS "skalacoin: sanitizers      ${SKALACOIN_ENABLE_SANITIZERS} (Debug only) [${SKALACOIN_SANITIZER_FLAGS}]")
message(STATUS "skalacoin: static analyzer ${SKALACOIN_ENABLE_ANALYZER} (Debug only, GCC)")
message(STATUS "skalacoin: hardening       ${SKALACOIN_ENABLE_HARDENING}")
message(STATUS "skalacoin: LTO             ${SKALACOIN_ENABLE_LTO}")
