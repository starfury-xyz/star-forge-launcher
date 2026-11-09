cmake_minimum_required(VERSION 3.21)
project(starforge VERSION 0.2.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static release CRT (/MT), like the patcher: the agent must not depend on a VC++ redist inside
# game.exe, and xwin has no debug CRT anyway.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT WIN32)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake (or a preset)")
endif()

add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
                        SF_VERSION="${PROJECT_VERSION}")
add_compile_options(/EHsc /W3 /Zi -Wno-unused-command-line-argument)
add_link_options(/debug /ignore:4099)

# Agent DLL injected into game.exe (port of crates/sc-agent).
add_library(sc_agent SHARED
    src/agent/dllmain.cpp
    src/agent/agent.cpp
)
target_include_directories(sc_agent PRIVATE src)
target_link_libraries(sc_agent PRIVATE kernel32)
