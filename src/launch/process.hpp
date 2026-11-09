#pragma once

// Starting programs, portably: the part of Rust's std::process::Command the launcher uses.
// fork/exec on Linux, CreateProcessW on Windows.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "launch/paths.hpp"

namespace sf::launch {

// What to run. Built up like Rust's Command: program, args, environment changes, folder.
struct Command {
    fs::path program;  // a path, or a bare name looked up on PATH ("xdg-open")
    std::vector<std::string> args;
    // Changes to this process's environment for the child, applied in order (a later change to
    // the same variable wins): a value sets it, nullopt removes it.
    std::vector<std::pair<std::string, std::optional<std::string>>> env;
    std::optional<fs::path> cwd;  // working folder; unset: ours

    Command& arg(std::string a) {
        args.push_back(std::move(a));
        return *this;
    }
    Command& set_env(std::string k, std::optional<std::string> v) {
        env.emplace_back(std::move(k), std::move(v));
        return *this;
    }
    // The environment value the child will see for `key` (after the changes).
    std::optional<std::string> env_value(const std::string& key) const;
};

// Where the child's stdout and stderr go.
struct Output {
    enum class Kind { Inherit, Null, File } kind = Kind::Inherit;
    fs::path file;  // Kind::File: created (replaced) and gets both streams
};

// A started program.
class Process {
public:
    Process() = default;
    Process(Process&& o) noexcept;
    Process& operator=(Process&& o) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process();

    // Waits for it to exit. Its exit code, or nullopt when it was killed by a signal (Linux).
    std::optional<int> wait();

private:
    friend Process spawn(const Command&, const Output&, bool);
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int pid_ = -1;
#endif
};

// Starts `cmd` (stdin: none). Throws std::runtime_error("could not start <program>: <why>").
// no_window: on Windows, don't open a console window for a console program (the GUI's way).
Process spawn(const Command& cmd, const Output& out = {}, bool no_window = false);

// Windows command-line quoting (CommandLineToArgvW's rules), as sc-inject's quote(); UTF-8.
std::string quote_arg(const std::string& arg);

// This program's own file.
fs::path current_exe();

}  // namespace sf::launch
