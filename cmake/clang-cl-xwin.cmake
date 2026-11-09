# Cross-compiles Windows binaries from Linux with clang-cl + lld-link against the MSVC CRT and
# Windows SDK fetched by cargo-xwin (same toolchain as star-forge/patcher).
#   SF_ARCH = x86_64 (default; game.exe and the launcher) or x86 (cloud/*.exe servers)
#   XWIN    = xwin root; defaults to cargo-xwin's cache for the chosen arch

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)

if(NOT SF_ARCH)
    set(SF_ARCH x86_64)
endif()
set(SF_ARCH "${SF_ARCH}" CACHE STRING "Target arch: x86_64 or x86")

if(SF_ARCH STREQUAL "x86_64")
    set(CMAKE_SYSTEM_PROCESSOR AMD64)
    set(_triple x86_64-pc-windows-msvc)
    set(_machine x64)
    set(_default_xwin "$ENV{HOME}/.cache/cargo-xwin/xwin")
elseif(SF_ARCH STREQUAL "x86")
    set(CMAKE_SYSTEM_PROCESSOR X86)
    set(_triple i686-pc-windows-msvc)
    set(_machine x86)
    set(_default_xwin "$ENV{HOME}/.cache/star-forge-xwin/xwin")
else()
    message(FATAL_ERROR "SF_ARCH must be x86_64 or x86, got '${SF_ARCH}'")
endif()

if(NOT XWIN)
    if(DEFINED ENV{XWIN})
        set(XWIN "$ENV{XWIN}")
    else()
        set(XWIN "${_default_xwin}")
    endif()
endif()
set(XWIN "${XWIN}" CACHE PATH "xwin root (crt/ + sdk/)")
if(NOT EXISTS "${XWIN}/crt/lib/${SF_ARCH}")
    message(FATAL_ERROR "No MSVC CRT for ${SF_ARCH} under ${XWIN}; see README \"Toolchain\"")
endif()

# Toolchain files run once per try_compile too; forward our settings there.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SF_ARCH XWIN)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})

set(_inc "/imsvc \"${XWIN}/crt/include\" /imsvc \"${XWIN}/sdk/include/ucrt\" /imsvc \"${XWIN}/sdk/include/um\" /imsvc \"${XWIN}/sdk/include/shared\" /imsvc \"${XWIN}/sdk/include/winrt\"")
set(CMAKE_C_FLAGS_INIT "${_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_inc}")

set(_libs "/libpath:\"${XWIN}/crt/lib/${SF_ARCH}\" /libpath:\"${XWIN}/sdk/lib/um/${SF_ARCH}\" /libpath:\"${XWIN}/sdk/lib/ucrt/${SF_ARCH}\" /machine:${_machine}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_libs}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_libs}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_libs}")

set(CMAKE_FIND_ROOT_PATH "${XWIN}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# xwin ships only the release CRT (no msvcrtd/libcmtd), so every config, try_compile
# included, links the static release one.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

# ctest runs the built .exe files under wine.
find_program(WINE_EXECUTABLE wine)
if(WINE_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${WINE_EXECUTABLE}")
endif()
