#pragma once

// Port of star-forge crates/sc-inject: starts game.exe suspended, injects sc_agent.dll, then
// resumes it and waits for it to exit.
//
// Normally started by sc-launch. Usage (Windows, or inside Proton/wine):
//   sc-inject.exe --game <path\to\win64\game.exe> --profile <profile dir> [--dll <sc_agent.dll>] [-- game args]

#include <string>
#include <vector>

namespace sf::inject {

// Runs the injector with its arguments (without the program name). Returns its exit code: the
// game's, or 1 if it couldn't start the game (after saying why, in inject.log and on stdout).
int run(const std::vector<std::wstring>& args);

// Quotes one argument so CommandLineToArgvW (how the game reads its command line) gives it back
// unchanged: plain words stay as they are; anything with a space, tab or quote, or empty, gets
// "..." with quotes and the backslashes before them escaped.
std::wstring quote(const std::wstring& arg);

}  // namespace sf::inject
