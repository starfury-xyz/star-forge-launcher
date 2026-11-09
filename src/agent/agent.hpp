#pragma once

namespace sf::agent {

// Sets the agent up. Returns false when the game must not start.
bool init();

// Appends one line to <profile>\<agent log>; no-op before init() opened it.
void log(const char* fmt, ...);

}  // namespace sf::agent
