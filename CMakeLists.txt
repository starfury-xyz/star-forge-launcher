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
add_compile_options(/EHsc /W3 /Zi)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # clang-cl warns about MSVC flags it accepts but ignores; cl.exe would reject this option.
    add_compile_options(-Wno-unused-command-line-argument)
endif()
add_link_options(/debug /ignore:4099)

# Code shared by the agent, the tests and (later) the launcher: profile files (port of
# crates/sc-profile) and string helpers. Static, so it's compiled into each binary.
add_library(sf_core STATIC
    src/common/text.cpp
    src/profile/profile.cpp
)
target_include_directories(sf_core PUBLIC src)
# SYSTEM: vendored headers' own warnings aren't ours to fix.
target_include_directories(sf_core SYSTEM PUBLIC third_party)

# MinHook (C): function hooking for the agent. Same sources the Rust minhook crate builds.
# hde is its instruction decoder, one per CPU architecture.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MINHOOK_HDE third_party/minhook/src/hde/hde64.c)
else()
    set(MINHOOK_HDE third_party/minhook/src/hde/hde32.c)
endif()
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    ${MINHOOK_HDE}
)
target_include_directories(minhook SYSTEM PUBLIC third_party/minhook/include)

# Agent DLL injected into game.exe (port of crates/sc-agent).
add_library(sc_agent SHARED
    src/agent/dllmain.cpp
    src/agent/agent.cpp
    src/agent/folders.cpp
    src/agent/hook.cpp
    src/agent/hooks.cpp
)
target_link_libraries(sc_agent PRIVATE sf_core minhook kernel32)

# Tests run natively on Windows, and under wine when cross-compiling (CMAKE_CROSSCOMPILING_EMULATOR,
# set by the toolchain).
include(CTest)
if(BUILD_TESTING)
    set(FAKEGAME "${CMAKE_BINARY_DIR}/fakegame/win64")
    add_executable(profile_test tests/profile_test.cpp)
    target_link_libraries(profile_test PRIVATE sf_core)
    # Sample packs the test reads, next to the exe.
    add_custom_command(TARGET profile_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/tests/data" "$<TARGET_FILE_DIR:profile_test>/data")
    add_test(NAME profile COMMAND profile_test)

    add_executable(text_test tests/text_test.cpp)
    target_link_libraries(text_test PRIVATE sf_core)
    add_test(NAME text COMMAND text_test)

    add_executable(load_test tests/load_test.cpp)
    target_link_libraries(load_test PRIVATE sf_core shell32)
    set_target_properties(load_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FAKEGAME}")
    # Copy the agent next to the test on every build where it changed (a POST_BUILD step on
    # load_test would only run when the test itself relinks, leaving a stale DLL).
    add_custom_command(OUTPUT "${FAKEGAME}/sc_agent.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sc_agent> "${FAKEGAME}/sc_agent.dll"
        DEPENDS sc_agent)
    add_custom_target(fakegame_agent ALL DEPENDS "${FAKEGAME}/sc_agent.dll")
    add_dependencies(load_test fakegame_agent)
    # One process per scenario (see load_test.cpp).
    set(AGENT_SCENARIOS no_profile broken_profile shared_data own_data redirect)
    foreach(scenario IN LISTS AGENT_SCENARIOS)
        add_test(NAME agent_${scenario} COMMAND load_test ${scenario})
        list(APPEND AGENT_TESTS agent_${scenario})
    endforeach()
    # TIMEOUT: a hung test fails after 60 s instead of blocking ctest.
    set_tests_properties(profile text ${AGENT_TESTS} PROPERTIES ENVIRONMENT "WINEDEBUG=-all" TIMEOUT 60)
endif()
