cmake_minimum_required(VERSION 3.21)
# MSVC debug info as CMAKE_MSVC_DEBUG_INFORMATION_FORMAT says, not a /Zi in the default flags.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
project(starforge VERSION 0.3.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Two kinds of build:
#  - Windows (win64 presets): everything. The agent and injector only exist here.
#  - Linux native (linux preset): the portable parts, i.e. the launcher (which runs the game
#    through Proton) and the patch code it shares, with their tests.
# A pre-release build (release.yml, for a tag like v0.3.1-rc.1) adds its label: 0.3.1-rc.1.
set(SF_PRERELEASE "" CACHE STRING "pre-release label (rc.1 makes the version x.y.z-rc.1); empty for a release")
set(SF_VERSION "${PROJECT_VERSION}")
if(SF_PRERELEASE)
    set(SF_VERSION "${PROJECT_VERSION}-${SF_PRERELEASE}")
endif()
add_compile_definitions(SF_VERSION="${SF_VERSION}")
if(WIN32)
    # Static release CRT (/MT), like the patcher: the agent must not depend on a VC++ redist
    # inside game.exe, and xwin has no debug CRT anyway.
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    # Debug info in each object (/Z7) rather than a shared .pdb (/Zi): CI's compiler cache
    # (sccache) can only cache the former. The linker still writes a .pdb.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
    add_compile_options(/EHsc /W3)
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
file(GLOB CORE_PATCHSETS "${CMAKE_SOURCE_DIR}/core-patches/*.patchset.toml")

# Create custom command that runs at build time
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/generated/core_patchsets.inc"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND ${CMAKE_COMMAND}
        -D "CORE_PATCHSETS=${CORE_PATCHSETS}"
        -D "OUTPUT_FILE=${CMAKE_BINARY_DIR}/generated/core_patchsets.inc"
        -P "${CMAKE_SOURCE_DIR}/cmake/generate_core_patchsets.cmake"
    DEPENDS ${CORE_PATCHSETS}
    COMMENT "Generating core_patchsets.inc"
    VERBATIM
)

# Create a custom target
add_custom_target(generate_core_patchsets ALL
    DEPENDS "${CMAKE_BINARY_DIR}/generated/core_patchsets.inc"
)

# Patchsets (port of crates/sc-patch): portable, shared by the agent and the launcher. Static
# libraries, so each binary gets its own copy compiled in.
add_library(sf_patch STATIC
    src/patch/core.cpp
    src/patch/patchset.cpp
    src/patch/server_config.cpp
    src/patch/sha1.cpp
    src/patch/toml_impl.cpp
)
target_include_directories(sf_patch PUBLIC src PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(sf_patch generate_core_patchsets)
# SYSTEM: vendored headers' own warnings aren't ours to fix.
target_include_directories(sf_patch SYSTEM PUBLIC third_party third_party/tomlplusplus)
target_compile_definitions(sf_patch PUBLIC TOML_HEADER_ONLY=0)

if(WIN32)
# Windows code shared by the agent, the injector and their tests: profile files as the agent
# reads them (port of crates/sc-profile) and Win32 string/file helpers.
add_library(sf_core STATIC
    src/common/file.cpp
    src/common/text.cpp
    src/profile/profile.cpp
)
target_link_libraries(sf_core PUBLIC sf_patch)

# MinHook (C): function hooking for the agent and the proxy. Same sources the Rust minhook crate builds.
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

# Hooking (MinHook wrapper) and file redirects, shared by the agent (x64) and the server proxy (x86).
add_library(sf_redirect STATIC
    src/hook/hook.cpp
    src/redirect/redirect.cpp
)
target_link_libraries(sf_redirect PUBLIC sf_core minhook)

endif()  # WIN32

# The x86 build (win32 preset) is the server side only: the proxy DLL every cloud\*.exe loads
# (the servers are 32-bit) and its test. Everything else is built x64 or for Linux.
if(SF_ARCH STREQUAL "x86")
    include(CTest)
    # Midhooks (C++ patches, see src/proxy/sfp_hook.hpp) are compiled into the proxy: every .cpp in
    # this folder, e.g. a patch repo's src/dll/hooks.
    set(SF_PROXY_HOOKS "" CACHE PATH "Folder of midhook .cpp files to build into the server proxy")
    set(proxy_hooks "")
    if(SF_PROXY_HOOKS)
        file(GLOB proxy_hooks CONFIGURE_DEPENDS "${SF_PROXY_HOOKS}/*.cpp")
    endif()
    if(BUILD_TESTING)
        list(APPEND proxy_hooks tests/proxy_test_hooks.cpp)  # for proxy_test's midhook_run
    endif()

    # Stand-in for the servers' libcrypto-1_1_tg.dll; the .def forwards every export to the original.
    add_library(sc_proxy SHARED src/proxy/proxy.cpp src/proxy/midhook.cpp src/proxy/libcrypto-1_1_tg.def ${proxy_hooks})
    target_link_libraries(sc_proxy PRIVATE sf_redirect kernel32)
    # Hook files include "sfp_hook.hpp" (and their own headers, beside them).
    target_include_directories(sc_proxy PRIVATE src/proxy ${SF_PROXY_HOOKS})
    set_target_properties(sc_proxy PROPERTIES OUTPUT_NAME libcrypto-1_1_tg)

    if(BUILD_TESTING)
        # The test sits in <build>/fakeserver/cloud/ with the proxy, like a server exe.
        set(FAKESERVER "${CMAKE_BINARY_DIR}/fakeserver/cloud")
        add_executable(proxy_test tests/proxy_test.cpp)
        target_link_libraries(proxy_test PRIVATE sf_core)
        set_target_properties(proxy_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FAKESERVER}")
        add_custom_command(OUTPUT "${FAKESERVER}/libcrypto-1_1_tg.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sc_proxy> "${FAKESERVER}/libcrypto-1_1_tg.dll"
            DEPENDS sc_proxy)
        add_custom_target(fakeserver_proxy ALL DEPENDS "${FAKESERVER}/libcrypto-1_1_tg.dll")
        # A stand-in server exe that imports the proxy, for the [stack] scenarios.
        add_executable(stack_test tests/stack_test.cpp)
        target_link_libraries(stack_test PRIVATE sc_proxy)
        set_target_properties(stack_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FAKESERVER}")
        add_dependencies(proxy_test fakeserver_proxy stack_test)
        set(PROXY_SCENARIOS plain patch patch_off wrong_build wrong_build_continue bad_config midhook midhook_run files files_elsewhere
            stack stack_off)
        foreach(scenario IN LISTS PROXY_SCENARIOS)
            add_test(NAME proxy_${scenario} COMMAND proxy_test ${scenario})
            list(APPEND PROXY_TESTS proxy_${scenario})
        endforeach()
        # RUN_SERIAL: the scenarios share fakeserver/cloud/starforge/.
        set_tests_properties(${PROXY_TESTS} PROPERTIES ENVIRONMENT "WINEDEBUG=-all" TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
    return()
endif()

# The launcher's machinery (port of crates/sc-launch), portable: finding Steam, Proton and the
# game, profiles, downloads, packs. The sc-launch CLI and the GUI are thin layers over it.
# Monocypher (C): ed25519 for pack signatures (RFC 8032, with its optional SHA-512 part).
add_library(monocypher STATIC
    third_party/monocypher/monocypher.c
    third_party/monocypher/monocypher-ed25519.c
)
target_include_directories(monocypher SYSTEM PUBLIC third_party/monocypher)

# miniz (C): reading pack zips (and writing them, in tests).
add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz SYSTEM PUBLIC third_party/miniz)
# Vendored C, kept unchanged: its warnings aren't ours to fix.
foreach(lib monocypher miniz)
    target_compile_options(${lib} PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)
endforeach()

add_library(sf_launch STATIC
    src/crypto/base64.cpp
    src/crypto/random.cpp
    src/crypto/sha256.cpp
    src/launch/env.cpp
    src/launch/gamescope.cpp
    src/launch/launch.cpp
    src/launch/mods.cpp
    src/launch/net.cpp
    src/launch/packs.cpp
    src/launch/patching.cpp
    src/launch/prepare.cpp
    src/launch/paths.cpp
    src/launch/process.cpp
    src/launch/profiles.cpp
    src/launch/remote.cpp
    src/launch/servers.cpp
    src/launch/signing.cpp
    src/launch/trust.cpp
    src/launch/steam.cpp
    src/pack/pack.cpp
)
target_link_libraries(sf_launch PUBLIC sf_patch PRIVATE monocypher miniz)
if(WIN32)
    target_link_libraries(sf_launch PUBLIC advapi32 winhttp bcrypt)  # registry (Steam's install path), downloads, random
else()
    find_package(CURL REQUIRED)
    target_link_libraries(sf_launch PUBLIC CURL::libcurl ${CMAKE_DL_LIBS})  # dl: gamescope.cpp loads libxcb
endif()
# Development builds can read <repo>/.env at startup (machine-specific STAR_FORGE_* overrides,
# see star-forge's .env.example). Off by default, so a release never does.
option(SF_DOTENV "Read ${CMAKE_SOURCE_DIR}/.env at startup (development builds)" OFF)
if(SF_DOTENV)
    target_compile_definitions(sf_launch PRIVATE SF_DOTENV_FILE="${CMAKE_SOURCE_DIR}/.env")
endif()

# The command-line launcher (port of crates/sc-launch/src/main.rs).
add_executable(sc_launch src/cli/main.cpp)
target_link_libraries(sc_launch PRIVATE sf_launch)
if(WIN32)
    target_link_libraries(sc_launch PRIVATE shell32)
endif()
set_target_properties(sc_launch PROPERTIES OUTPUT_NAME sc-launch)

# The launcher's backend (port of star-forge gui/src-tauri): the commands its HTML/JS interface
# invokes, without the window, so it builds and tests anywhere.
file(READ samples/sc-classic-europe/pack.json SF_SAMPLE_SC_CLASSIC_EUROPE)
configure_file(src/gui/samples.hpp.in ${CMAKE_BINARY_DIR}/generated/gui/samples.hpp @ONLY)
add_library(sf_gui STATIC
    src/gui/backend.cpp
    src/gui/game.cpp
    src/gui/mods.cpp
    src/gui/prefs.cpp
    src/gui/profiles.cpp
    src/gui/servers.cpp
    src/gui/setup.cpp
    src/gui/updater.cpp
)
target_include_directories(sf_gui PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(sf_gui PUBLIC sf_launch PRIVATE monocypher)

# The launcher itself (port of gui/src-tauri's main.rs and lifecycle.rs): the unchanged HTML/JS
# interface in gui/ui on webview/webview (WebKitGTK on Linux, WebView2 on Windows), built into the
# program with its icons.
set(SF_ASSETS_CPP ${CMAKE_BINARY_DIR}/generated/app/assets.cpp)
file(GLOB_RECURSE SF_UI_FILES CONFIGURE_DEPENDS gui/ui/* gui/icons/*)
add_custom_command(OUTPUT ${SF_ASSETS_CPP}
    COMMAND ${CMAKE_COMMAND} -DOUT=${SF_ASSETS_CPP} -DUI_DIR=${CMAKE_SOURCE_DIR}/gui/ui
            -DICONS_DIR=${CMAKE_SOURCE_DIR}/gui/icons -P ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
    DEPENDS ${SF_UI_FILES} cmake/embed_assets.cmake
    COMMENT "Building the interface into the launcher"
    VERBATIM)
if(WIN32)
    configure_file(src/app/app.rc.in ${CMAKE_BINARY_DIR}/generated/app/app.rc @ONLY)
    add_executable(star_forge WIN32 src/app/app.cpp src/app/native_win.cpp ${SF_ASSETS_CPP}
                   ${CMAKE_BINARY_DIR}/generated/app/app.rc)
    target_include_directories(star_forge SYSTEM PRIVATE third_party/webview third_party/webview2)
    target_link_libraries(star_forge PRIVATE sf_gui advapi32 dwmapi ole32 shell32 shlwapi user32 version)
    set(SF_HAVE_GUI ON)
else()
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(WEBKIT IMPORTED_TARGET gtk+-3.0 webkit2gtk-4.1)
    endif()
    if(WEBKIT_FOUND)
        add_executable(star_forge src/app/app.cpp src/app/native_gtk.cpp src/app/tray_sni.cpp ${SF_ASSETS_CPP})
        target_include_directories(star_forge SYSTEM PRIVATE third_party/webview)
        target_link_libraries(star_forge PRIVATE sf_gui PkgConfig::WEBKIT)
        set(SF_HAVE_GUI ON)
    else()
        message(STATUS "GTK 3 / WebKitGTK 4.1 not found: building without the launcher window (star-forge)")
    endif()
endif()
# The launcher's self-update: latest.json's URL (empty: this build doesn't update itself) and the
# minisign key its packages are signed with (tauri.conf.json's).
set(SF_UPDATE_ENDPOINT "" CACHE STRING "URL of the latest.json the launcher updates from")
# With "pre-release updates" on: a GitHub API release list (api.github.com/repos/<repo>/releases);
# the newest release in it, pre-release or not, that carries a latest.json.
set(SF_PRERELEASE_ENDPOINT "" CACHE STRING "URL of the GitHub release list pre-release updates come from")
set(SF_UPDATE_PUBKEY "dW50cnVzdGVkIGNvbW1lbnQ6IG1pbmlzaWduIHB1YmxpYyBrZXk6IEY5OEY4NUE4NjFFMTJGQUIKUldTckwrRmhxSVdQK2NKaTQ2Y0c5T2U0UGc2MTU4V3JIS2kyVlNlWnJCNXlnMkF2RWZteEhZVGYK"
    CACHE STRING "base64 minisign public key for launcher updates")
if(SF_HAVE_GUI)
    target_include_directories(star_forge PRIVATE src)
    target_compile_definitions(star_forge PRIVATE SF_UPDATE_ENDPOINT="${SF_UPDATE_ENDPOINT}"
        SF_PRERELEASE_ENDPOINT="${SF_PRERELEASE_ENDPOINT}" SF_UPDATE_PUBKEY="${SF_UPDATE_PUBKEY}")
    set_target_properties(star_forge PROPERTIES OUTPUT_NAME star-forge)
endif()

if(WIN32)
# Agent DLL injected into game.exe (port of crates/sc-agent).
add_library(sc_agent SHARED
    src/agent/dllmain.cpp
    src/agent/agent.cpp
    src/agent/folders.cpp
    src/agent/patches.cpp
)
target_link_libraries(sc_agent PRIVATE sf_redirect kernel32)

# Injector (port of crates/sc-inject): starts game.exe suspended with the agent in it. The logic
# is a library so tests can call quote() directly; WIN32 = a windowed program (no console window).
add_library(sf_inject STATIC src/inject/inject.cpp)
target_link_libraries(sf_inject PUBLIC sf_core)
add_executable(sc_inject WIN32 src/inject/main.cpp)
target_link_libraries(sc_inject PRIVATE sf_inject shell32)
set_target_properties(sc_inject PROPERTIES OUTPUT_NAME sc-inject)  # the name sc-launch looks for

# Server supervisor: starts and stops a server install's MongoDB and Hub (see server.hpp). A
# console program, so it can reach the servers with Ctrl+C.
add_library(sf_server STATIC src/server/server.cpp)
target_include_directories(sf_server PUBLIC src)
target_link_libraries(sf_server PUBLIC ws2_32)
add_executable(sc_server src/server/main.cpp)
target_link_libraries(sc_server PRIVATE sf_server)
set_target_properties(sc_server PROPERTIES OUTPUT_NAME sc-server)
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
    add_executable(signing_test tests/signing_test.cpp)
    target_link_libraries(signing_test PRIVATE sf_launch)
    add_test(NAME signing COMMAND signing_test ${SF_PROFILES_DIR})
    add_executable(remote_test tests/remote_test.cpp)
    target_link_libraries(remote_test PRIVATE sf_launch)
    if(WIN32)
        target_link_libraries(remote_test PRIVATE ws2_32)
    endif()
    add_test(NAME remote COMMAND remote_test)
    add_executable(trust_test tests/trust_test.cpp)
    target_link_libraries(trust_test PRIVATE sf_launch)
    add_test(NAME trust COMMAND trust_test)
    add_executable(packs_test tests/packs_test.cpp)
    target_link_libraries(packs_test PRIVATE sf_launch miniz)
    if(WIN32)
        target_link_libraries(packs_test PRIVATE ws2_32)
    endif()
    add_test(NAME packs COMMAND packs_test)
    add_executable(prepare_test tests/prepare_test.cpp)
    target_link_libraries(prepare_test PRIVATE sf_launch)
    if(WIN32)
        target_link_libraries(prepare_test PRIVATE ws2_32)
    endif()
    add_test(NAME prepare COMMAND prepare_test)
    add_executable(mods_test tests/mods_test.cpp)
    target_link_libraries(mods_test PRIVATE sf_launch)
    add_test(NAME mods COMMAND mods_test)
    add_executable(gui_test tests/gui_test.cpp)
    target_link_libraries(gui_test PRIVATE sf_gui monocypher)
    if(WIN32)
        target_link_libraries(gui_test PRIVATE ws2_32)
    endif()
    target_compile_definitions(gui_test PRIVATE SF_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME gui COMMAND gui_test $<TARGET_FILE:proc_helper>)
    add_executable(cli_test tests/cli_test.cpp)
    target_link_libraries(cli_test PRIVATE sf_launch)
    add_dependencies(cli_test sc_launch)
    add_test(NAME cli COMMAND cli_test $<TARGET_FILE:sc_launch>)
    set(PORTABLE_TESTS patch launch setup process pack net signing remote trust packs prepare cli gui)

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

    add_executable(fake_server tests/fake_server.cpp)
    target_link_libraries(fake_server PRIVATE ws2_32)
    add_executable(server_test tests/server_test.cpp)
    target_link_libraries(server_test PRIVATE sf_server)
    add_dependencies(server_test sc_server fake_server)
    add_test(NAME server COMMAND server_test $<TARGET_FILE:sc_server> $<TARGET_FILE:fake_server>)
    set_tests_properties(server PROPERTIES TIMEOUT 120)

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
        core_patch core_by_player core_unknown plugin plugin_fail plugin_missing)
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
