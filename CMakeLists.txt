cmake_minimum_required(VERSION 3.21)
project(starforge VERSION 0.2.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Two kinds of build:
#  - Windows (win64 presets): everything. The agent and injector only exist here.
#  - Linux native (linux preset): the portable parts, i.e. the launcher (which runs the game
#    through Proton) and the patch code it shares, with their tests.
add_compile_definitions(SF_VERSION="${PROJECT_VERSION}")
if(WIN32)
    # Static release CRT (/MT), like the patcher: the agent must not depend on a VC++ redist
    # inside game.exe, and xwin has no debug CRT anyway.
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    add_compile_options(/EHsc /W3 /Zi)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang-cl warns about MSVC flags it accepts but ignores; cl.exe would reject this option.
        add_compile_options(-Wno-unused-command-line-argument)
    endif()
    add_link_options(/debug /ignore:4099)
else()
    add_compile_options(-Wall -Wextra)
endif()

# The core patchsets (core-patches/*.patchset.toml), embedded as text in sf_core like Rust's
# include_str!. Re-generated whenever one of them changes.
file(GLOB CORE_PATCHSETS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/core-patches/*.patchset.toml")
set(CORE_INC "")
foreach(f IN LISTS CORE_PATCHSETS)
    get_filename_component(name "${f}" NAME)
    file(READ "${f}" text)
    string(APPEND CORE_INC "{\"core/${name}\", R\"sfcore(${text})sfcore\"},\n")
endforeach()
# Written via a temp file and copied only when different, so core.cpp only rebuilds on a real change.
file(WRITE "${CMAKE_BINARY_DIR}/generated/core_patchsets.inc.tmp" "${CORE_INC}")
file(COPY_FILE "${CMAKE_BINARY_DIR}/generated/core_patchsets.inc.tmp" "${CMAKE_BINARY_DIR}/generated/core_patchsets.inc"
     ONLY_IF_DIFFERENT)

# Patchsets (port of crates/sc-patch): portable, shared by the agent and the launcher. Static
# libraries, so each binary gets its own copy compiled in.
add_library(sf_patch STATIC
    src/patch/core.cpp
    src/patch/patchset.cpp
    src/patch/sha1.cpp
    src/patch/toml_impl.cpp
)
target_include_directories(sf_patch PUBLIC src PRIVATE "${CMAKE_BINARY_DIR}/generated")
# SYSTEM: vendored headers' own warnings aren't ours to fix.
target_include_directories(sf_patch SYSTEM PUBLIC third_party third_party/tomlplusplus)
target_compile_definitions(sf_patch PUBLIC TOML_HEADER_ONLY=0)

# The launcher's machinery (port of crates/sc-launch), portable: finding Steam, Proton and the
# game, profiles, downloads, packs. The sc-launch CLI and the GUI are thin layers over it.
add_library(sf_launch STATIC
    src/crypto/sha256.cpp
    src/launch/env.cpp
    src/launch/launch.cpp
    src/launch/net.cpp
    src/launch/paths.cpp
    src/launch/process.cpp
    src/launch/profiles.cpp
    src/launch/steam.cpp
    src/pack/pack.cpp
)
target_link_libraries(sf_launch PUBLIC sf_patch)
if(WIN32)
    target_link_libraries(sf_launch PUBLIC advapi32 winhttp)  # registry (Steam's install path), downloads
else()
    find_package(CURL REQUIRED)
    target_link_libraries(sf_launch PUBLIC CURL::libcurl)
endif()
# Development builds can read <repo>/.env at startup (machine-specific STAR_FORGE_* overrides,
# see star-forge's .env.example). Off by default, so a release never does.
option(SF_DOTENV "Read ${CMAKE_SOURCE_DIR}/.env at startup (development builds)" OFF)
if(SF_DOTENV)
    target_compile_definitions(sf_launch PRIVATE SF_DOTENV_FILE="${CMAKE_SOURCE_DIR}/.env")
endif()

if(WIN32)
# Windows code shared by the agent, the injector and their tests: profile files as the agent
# reads them (port of crates/sc-profile) and Win32 string/file helpers.
add_library(sf_core STATIC
    src/common/file.cpp
    src/common/text.cpp
    src/profile/profile.cpp
)
target_link_libraries(sf_core PUBLIC sf_patch)

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
    src/agent/file_hooks.cpp
    src/agent/patches.cpp
)
target_link_libraries(sc_agent PRIVATE sf_core minhook kernel32)

# Injector (port of crates/sc-inject): starts game.exe suspended with the agent in it. The logic
# is a library so tests can call quote() directly; WIN32 = a windowed program (no console window).
add_library(sf_inject STATIC src/inject/inject.cpp)
target_link_libraries(sf_inject PUBLIC sf_core)
add_executable(sc_inject WIN32 src/inject/main.cpp)
target_link_libraries(sc_inject PRIVATE sf_inject shell32)
set_target_properties(sc_inject PROPERTIES OUTPUT_NAME sc-inject)  # the name sc-launch looks for
endif()  # WIN32

# Tests run natively on Windows, and under wine when cross-compiling (CMAKE_CROSSCOMPILING_EMULATOR,
# set by the toolchain).
include(CTest)
if(BUILD_TESTING)
    add_executable(patch_test tests/patch_test.cpp)
    target_link_libraries(patch_test PRIVATE sf_patch)
    # Optional: the real game.exe, to check the core patches against the installed build.
    # Defaults to the Steam install on Linux (natively, or seen through wine's Z: drive); pass
    # -DSF_GAME_EXE=C:/.../game.exe on Windows. The test skips that part when it's empty.
    set(_steam_game "$ENV{HOME}/.local/share/Steam/steamapps/common/star conflict/win64/game.exe")
    if(NOT DEFINED SF_GAME_EXE AND CMAKE_HOST_UNIX AND EXISTS "${_steam_game}")
        if(WIN32)
            string(REPLACE "/" "\\" _z "${_steam_game}")
            set(SF_GAME_EXE "Z:${_z}")
        else()
            set(SF_GAME_EXE "${_steam_game}")
        endif()
    endif()
    set(SF_GAME_EXE "${SF_GAME_EXE}" CACHE STRING "game.exe for patch_test (empty: skip that part)")
    add_test(NAME patch COMMAND patch_test "${SF_GAME_EXE}")

    add_executable(launch_test tests/launch_test.cpp)
    target_link_libraries(launch_test PRIVATE sf_launch)
    add_test(NAME launch COMMAND launch_test)
    add_executable(setup_test tests/setup_test.cpp)
    target_link_libraries(setup_test PRIVATE sf_launch)
    add_test(NAME setup COMMAND setup_test)
    add_executable(proc_helper tests/proc_helper.cpp)
    if(WIN32)
        target_link_libraries(proc_helper PRIVATE shell32)
    endif()
    add_executable(process_test tests/process_test.cpp)
    target_link_libraries(process_test PRIVATE sf_launch)
    add_dependencies(process_test proc_helper)
    add_test(NAME process COMMAND process_test $<TARGET_FILE:proc_helper>)
    add_executable(pack_test tests/pack_test.cpp)
    target_link_libraries(pack_test PRIVATE sf_launch)
    # Optional: a folder of real profiles installed by the Rust launcher, to check pack.json comes
    # out byte for byte the same. Defaults to the standard Linux location (natively or via Z:).
    set(_profiles "$ENV{HOME}/.local/share/star-forge/profiles")
    if(NOT DEFINED SF_PROFILES_DIR AND CMAKE_HOST_UNIX AND IS_DIRECTORY "${_profiles}")
        if(WIN32)
            string(REPLACE "/" "\\" _z "${_profiles}")
            set(SF_PROFILES_DIR "Z:${_z}")
        else()
            set(SF_PROFILES_DIR "${_profiles}")
        endif()
    endif()
    set(SF_PROFILES_DIR "${SF_PROFILES_DIR}" CACHE STRING "Real profiles for pack_test (empty: skip that part)")
    add_test(NAME pack COMMAND pack_test ${SF_PROFILES_DIR})
    add_executable(net_test tests/net_test.cpp)
    target_link_libraries(net_test PRIVATE sf_launch)
    if(WIN32)
        target_link_libraries(net_test PRIVATE ws2_32)
    endif()
    add_test(NAME net COMMAND net_test)
    set(PORTABLE_TESTS patch launch setup process pack net)

if(WIN32)
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
    # Helpers for the patch scenarios, next to load_test: a DLL that loads after the agent, and
    # a plugin (the test copies it into the profile's patches\ folder).
    add_library(patch_target SHARED tests/patch_target.cpp)
    add_library(test_plugin SHARED tests/test_plugin.cpp)
    target_include_directories(test_plugin PRIVATE src)
    set_target_properties(patch_target test_plugin PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FAKEGAME}")
    add_dependencies(load_test patch_target test_plugin)
    # One process per scenario (see load_test.cpp).
    set(AGENT_SCENARIOS no_profile broken_profile shared_data own_data redirect listing
        patch_exe patch_fail_exit patch_fail_continue patch_wrong_build patch_dll patch_dll_fail_exit
        core_patch core_unknown plugin plugin_fail plugin_missing)
    foreach(scenario IN LISTS AGENT_SCENARIOS)
        add_test(NAME agent_${scenario} COMMAND load_test ${scenario})
        list(APPEND AGENT_TESTS agent_${scenario})
    endforeach()
    # TIMEOUT: a hung test fails after 60 s instead of blocking ctest.

    # sc-inject on a fake game that writes down how it was started.
    add_executable(fake_game WIN32 tests/fake_game.cpp)
    target_link_libraries(fake_game PRIVATE sf_core shell32)
    set_target_properties(fake_game PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FAKEGAME}")
    add_executable(inject_test tests/inject_test.cpp)
    target_link_libraries(inject_test PRIVATE sf_inject shell32)
    # A DLL that always loads at 0x200000000 (no relocations), where LoadLibraryW's result cut to
    # 32 bits is 0.
    add_library(zero_low SHARED tests/zero_low.cpp)
    target_link_options(zero_low PRIVATE /BASE:0x200000000 /FIXED /DYNAMICBASE:NO)
    add_dependencies(inject_test sc_inject fake_game sc_agent zero_low)
    set(INJECT_SCENARIOS quoting game_args profile_args unix_paths bad_args bad_profile bad_patchset bad_server
        no_agent leftover agent agent_refuses not_a_dll zero_low)
    foreach(scenario IN LISTS INJECT_SCENARIOS)
        add_test(NAME inject_${scenario} COMMAND inject_test ${scenario})
        list(APPEND INJECT_TESTS inject_${scenario})
    endforeach()

    set(WINDOWS_TESTS profile text ${AGENT_TESTS} ${INJECT_TESTS})
endif()  # WIN32

    set_tests_properties(${PORTABLE_TESTS} ${WINDOWS_TESTS} PROPERTIES ENVIRONMENT "WINEDEBUG=-all" TIMEOUT 60)
endif()
