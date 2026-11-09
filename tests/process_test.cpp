// Tests sf::launch::spawn (process.hpp) by running proc_helper, which prints how it was started.
//
// Usage: process_test <path to proc_helper>

#include "launch/process.hpp"

#include "check.hpp"

using namespace sf::launch;
using test::check;
using test::contains;

namespace {

fs::path g_helper;

// Runs the helper with `cmd` (program filled in) and its output in a file; returns its output.
std::string run(Command cmd, std::optional<int>* code = nullptr) {
    if (cmd.program.empty()) cmd.program = g_helper;
    const fs::path out = fs::temp_directory_path() / "sf_process_test.out";
    std::optional<int> c = spawn(cmd, Output{Output::Kind::File, out}).wait();
    if (code) *code = c;
    return test::read_file(out);
}

}  // namespace

int main(int argc, char** argv) {
    test::exit_on_crash();
    if (argc < 2) {
        std::printf("usage: process_test <proc_helper>\n");
        return 2;
    }
    g_helper = fs::absolute(fs::u8path(argv[1]));
    const fs::path tmp = test::fresh_dir(fs::temp_directory_path() / "sf_process_test");

    // Arguments arrive exactly, including the ones Windows needs quoted.
    {
        const std::vector<std::string> tricky{"plain", "with space", "say \"hi\"", "trail\\", "sp trail\\", "", "\xc3\xa9t\xc3\xa9 \xe2\x98\x85"};
        Command cmd;
        cmd.args = tricky;
        std::optional<int> code;
        std::string got = run(cmd, &code);
        std::string want;
        for (const auto& a : tricky) want += "arg=" + a + "\n";
        if (!contains(got, want)) std::printf("got:\n%s\n", got.c_str());
        check(contains(got, want), "every argument arrives unchanged");
        check(code == 0, "exit code 0");
    }

    // Environment: set, override, remove; the exit code comes back; the working folder is used.
    {
        set_env("SF_PARENT_ONLY", "from parent");
        set_env("SF_REMOVED", "should go");
        Command cmd;
        cmd.set_env("SF_SHOW", "SF_PARENT_ONLY,SF_REMOVED,SF_NEW,SF_TWICE")
            .set_env("SF_REMOVED", std::nullopt)
            .set_env("SF_NEW", "\xc3\xa9")
            .set_env("SF_TWICE", "first")
            .set_env("SF_TWICE", "second")
            .set_env("SF_EXIT", "7");
        fs::create_directories(tmp / "work dir");
        cmd.cwd = tmp / "work dir";
        std::optional<int> code;
        std::string got = run(cmd, &code);
        check(contains(got, "env:SF_PARENT_ONLY=from parent\n"), "the parent's environment is inherited");
        check(contains(got, "env:SF_REMOVED=(unset)\n"), "a removed variable is gone");
        check(contains(got, "env:SF_NEW=\xc3\xa9\n"), "a new variable is set (UTF-8)");
        check(contains(got, "env:SF_TWICE=second\n"), "the later change wins");
        check(contains(got, "cwd=" + ustr(fs::canonical(tmp / "work dir")) + "\n"), "runs in the given folder");
        check(code == 7, "the exit code comes back");
        check(cmd.env_value("SF_PARENT_ONLY") == std::string("from parent") && !cmd.env_value("SF_REMOVED") &&
                  cmd.env_value("SF_TWICE") == std::string("second"),
              "env_value: what the child will see");
    }

    // Failures to start are errors, not exit codes.
    try {
        Command cmd;
        cmd.program = tmp / "no-such-program";
        spawn(cmd).wait();
        check(false, "a missing program is an error");
    } catch (const std::exception& e) {
        check(contains(e.what(), "could not start "), "a missing program is an error");
    }
    try {
        Command cmd;
        cmd.program = g_helper;
        cmd.cwd = tmp / "no-such-folder";
        spawn(cmd, Output{Output::Kind::Null, {}}).wait();
        check(false, "a missing working folder is an error");
    } catch (const std::exception& e) {
        check(contains(e.what(), "could not start "), "a missing working folder is an error");
    }

#ifndef _WIN32
    // A bare name is found on the child's PATH (how the GUI starts xdg-open).
    {
        Command cmd;
        cmd.program = g_helper.filename();
        cmd.set_env("PATH", ustr(tmp) + ":" + ustr(g_helper.parent_path()));
        std::optional<int> code;
        run(cmd, &code);
        check(code == 0, "a bare program name is found on PATH");
    }
    // Killed by a signal: no exit code (Rust's status.code() is None).
    {
        Command cmd;
        cmd.program = "/bin/sh";
        cmd.arg("-c").arg("kill -9 $$");
        check(!spawn(cmd).wait(), "killed by a signal: no exit code");
    }
#endif

    check(quote_arg("a b\\") == "\"a b\\\\\"" && quote_arg("x") == "x" && quote_arg("") == "\"\"", "quote_arg");
    check(!current_exe().empty() && current_exe().filename().u8string().rfind("process_test", 0) == 0, "current_exe");
    return test::failures ? 1 : 0;
}
