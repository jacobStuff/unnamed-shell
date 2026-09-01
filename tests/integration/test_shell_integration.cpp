// Integration tests: run the actual built `ush` binary (via fork+execve,
// not popen, so there's no double shell-quoting to worry about) and check
// its combined stdout+stderr output and exit status. These exercise real
// process execution (fork/exec/wait/pipes/redirects) that the unit tests
// for the lexer/parser/expander can't - see docs/DESIGN.md.

#include <catch2/catch_test_macros.hpp>

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>  // forkpty()
#else
#include <pty.h>  // forkpty()
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef USH_BINARY_PATH
#error "USH_BINARY_PATH must be defined by the build (see tests/CMakeLists.txt)"
#endif

using namespace std::string_literals;
namespace fs = std::filesystem;

namespace {

struct RunResult {
    std::string output;
    int status;
};

// Every test process inherits the real $HOME (and possibly $ENV) - none
// of this file's spawn points ever wants that: without overriding it, an
// interactive run would default HISTFILE to the *actual* user's
// ~/.ush_history (load/saving it for real) and, now that main.cpp
// sources ~/.ushrc and $ENV at startup, would try to run whatever's in
// the *real* developer's actual ~/.ushrc too. "/dev/null" (histFile's
// default) makes history purely in-memory for the test; a nonexistent
// homeDir (its default) makes ~/.ushrc "not found" - both harmless no-ops
// - and $ENV is unset outright so nothing inherited from the host
// environment gets sourced either. A test that specifically wants to
// exercise persistence or startup-file sourcing passes a real
// (temp-directory) path for the relevant one.
RunResult runUshWithArgv(const std::vector<std::string>& args, const std::string& stdinContent,
                          const std::string& histFile = "/dev/null",
                          const std::string& homeDir = "/nonexistent-ush-test-home",
                          const std::string& envPath = "") {
    int outPipe[2];
    REQUIRE(::pipe(outPipe) == 0);
    int inPipe[2];
    REQUIRE(::pipe(inPipe) == 0);

    pid_t pid = ::fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        ::dup2(inPipe[0], 0);
        ::dup2(outPipe[1], 1);
        ::dup2(outPipe[1], 2);
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::setenv("HISTFILE", histFile.c_str(), 1);
        ::setenv("HOME", homeDir.c_str(), 1);
        if (envPath.empty()) {
            ::unsetenv("ENV");
        } else {
            ::setenv("ENV", envPath.c_str(), 1);
        }

        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(USH_BINARY_PATH, argv.data());
        _exit(127);
    }

    ::close(inPipe[0]);
    ::close(outPipe[1]);

    // Write stdin content (assumed small enough not to deadlock on the
    // pipe buffer - true for every test here) before reading output.
    if (!stdinContent.empty()) {
        ::write(inPipe[1], stdinContent.data(), stdinContent.size());
    }
    ::close(inPipe[1]);

    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(outPipe[0], buf, sizeof(buf))) > 0) output.append(buf, static_cast<std::size_t>(n));
    ::close(outPipe[0]);

    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);
    int status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : (128 + WTERMSIG(wstatus));
    return {output, status};
}

RunResult runUsh(const std::string& script, const std::vector<std::string>& extraArgs = {},
                  const std::string& stdinContent = "") {
    std::vector<std::string> args = {USH_BINARY_PATH, "-c", script};
    for (const auto& a : extraArgs) args.push_back(a);
    return runUshWithArgv(args, stdinContent);
}

// Runs ush in forced-interactive mode (`-i` - see main.cpp; a real tty
// isn't available/needed here, since only the parse-incrementally-and-
// prompt logic is under test, not terminal line editing) with
// `stdinScript` fed to it a line at a time, and returns the combined
// output (prompts interleaved with whatever the commands themselves
// print) and exit status.
RunResult runUshInteractive(const std::string& stdinScript) {
    return runUshWithArgv({USH_BINARY_PATH, "-i"}, stdinScript);
}

RunResult runUshInteractive(const std::string& stdinScript, const std::string& histFile) {
    return runUshWithArgv({USH_BINARY_PATH, "-i"}, stdinScript, histFile);
}

// As above, but overriding $HOME instead of $HISTFILE - for tests
// exercising ~/.ushrc sourcing specifically.
RunResult runUshInteractiveWithHome(const std::string& stdinScript, const std::string& homeDir) {
    return runUshWithArgv({USH_BINARY_PATH, "-i"}, stdinScript, "/dev/null", homeDir);
}

// As above, but setting $ENV - for tests exercising POSIX's $ENV startup
// file specifically.
RunResult runUshInteractiveWithEnv(const std::string& stdinScript, const std::string& envPath) {
    return runUshWithArgv({USH_BINARY_PATH, "-i"}, stdinScript, "/dev/null",
                           "/nonexistent-ush-test-home", envPath);
}

// A live `ush -i` session with its stdin/stdout as pipes the test can
// drive incrementally - needed for job-control tests, which have to read
// a job's announced pid from mid-session output before they can send it
// a real signal, unlike every other interactive test here (which can
// just feed the whole script upfront and check the combined output at
// the end).
class InteractiveSession {
public:
    // `histFile`/`homeDir`: see runUshWithArgv()'s identical parameters -
    // default to never touching the real user's ~/.ush_history or
    // ~/.ushrc.
    explicit InteractiveSession(const std::string& histFile = "/dev/null",
                                 const std::string& homeDir = "/nonexistent-ush-test-home") {
        int inPipe[2], outPipe[2];
        REQUIRE(::pipe(inPipe) == 0);
        REQUIRE(::pipe(outPipe) == 0);
        pid_ = ::fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            ::dup2(inPipe[0], 0);
            ::dup2(outPipe[1], 1);
            ::dup2(outPipe[1], 2);
            ::close(inPipe[0]);
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            ::setenv("HISTFILE", histFile.c_str(), 1);
            ::setenv("HOME", homeDir.c_str(), 1);
            ::unsetenv("ENV");
            std::vector<std::string> args = {USH_BINARY_PATH, "-i"};
            std::vector<char*> argv;
            for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            ::execv(USH_BINARY_PATH, argv.data());
            _exit(127);
        }
        ::close(inPipe[0]);
        ::close(outPipe[1]);
        stdinFd_ = inPipe[1];
        stdoutFd_ = outPipe[0];
    }
    ~InteractiveSession() {
        if (stdinFd_ >= 0) ::close(stdinFd_);
        if (stdoutFd_ >= 0) ::close(stdoutFd_);
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int wstatus = 0;
            ::waitpid(pid_, &wstatus, 0);
        }
    }

    pid_t pid() const { return pid_; }

    void send(const std::string& line) {
        std::string s = line + "\n";
        ::write(stdinFd_, s.data(), s.size());
    }

    // Reads whatever output arrives within `timeoutMs` of the last byte
    // received (not a fixed total deadline): keeps reading as long as
    // more shows up, so a slow-but-eventually-responsive command doesn't
    // race a fixed budget, while a truly-finished burst of output doesn't
    // force the full timeout to elapse either.
    std::string readAvailable(int timeoutMs = 2000) {
        std::string result;
        char buf[4096];
        while (true) {
            struct pollfd pfd {
                stdoutFd_, POLLIN, 0
            };
            int r = ::poll(&pfd, 1, timeoutMs);
            if (r <= 0 || !(pfd.revents & POLLIN)) break;
            ssize_t n = ::read(stdoutFd_, buf, sizeof(buf));
            if (n <= 0) break;
            result.append(buf, static_cast<std::size_t>(n));
        }
        return result;
    }

    int finish() {
        if (stdinFd_ >= 0) {
            ::close(stdinFd_);
            stdinFd_ = -1;
        }
        int wstatus = 0;
        pid_t r;
        while ((r = ::waitpid(pid_, &wstatus, 0)) < 0 && errno == EINTR) {
        }
        pid_t donePid = pid_;
        pid_ = -1;  // finished (or failed) - the destructor shouldn't try again
        return r == donePid && WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : (128 + WTERMSIG(wstatus));
    }

private:
    pid_t pid_ = -1;
    int stdinFd_ = -1;
    int stdoutFd_ = -1;
};

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("ush_integration_" + std::to_string(++counter) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// A live `ush -i` session connected to a real pseudo-terminal rather than
// pipes - needed to exercise the raw-mode line editor at all
// (LineEditor::isUsable() requires stdin/stdout to actually be a tty;
// InteractiveSession's pipes deliberately fall back to plain line
// reading, like `ush -i < script`). sendRaw() writes bytes with no
// automatic newline, so a test can send arrow keys/control characters,
// not just whole lines.
class PtySession {
public:
    explicit PtySession(const std::string& histFile = "/dev/null",
                         const std::string& homeDir = "/nonexistent-ush-test-home") {
        pid_ = ::forkpty(&fd_, nullptr, nullptr, nullptr);
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            ::setenv("HISTFILE", histFile.c_str(), 1);
            ::setenv("HOME", homeDir.c_str(), 1);
            ::unsetenv("ENV");
            ::setenv("TERM", "xterm", 1);
            std::vector<std::string> args = {USH_BINARY_PATH, "-i"};
            std::vector<char*> argv;
            for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            ::execv(USH_BINARY_PATH, argv.data());
            _exit(127);
        }
    }
    ~PtySession() {
        if (fd_ >= 0) ::close(fd_);
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int wstatus = 0;
            ::waitpid(pid_, &wstatus, 0);
        }
    }

    void sendRaw(const std::string& bytes) { ::write(fd_, bytes.data(), bytes.size()); }
    // A whole line of plain text, terminated with Enter - '\r' (not
    // '\n') is what a real terminal in cbreak/canonical mode sends for
    // the Enter key, and what the line editor's readLine() looks for.
    void sendLine(const std::string& text) { sendRaw(text + "\r"); }

    std::string readAvailable(int timeoutMs = 2000) {
        std::string result;
        char buf[4096];
        while (true) {
            struct pollfd pfd {
                fd_, POLLIN, 0
            };
            int r = ::poll(&pfd, 1, timeoutMs);
            if (r <= 0 || !(pfd.revents & POLLIN)) break;
            ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n <= 0) break;
            result.append(buf, static_cast<std::size_t>(n));
        }
        return result;
    }

    int finish() {
        sendLine("exit");
        // Keep draining until the child actually exits: its own
        // terminal-mode-restore can (harmlessly, in production - see
        // RawMode's TCSANOW comment in line_editor.cpp) still have
        // output queued that nothing has read yet, and some ptys/kernels
        // are happy to let queued-but-unread output pile up indefinitely
        // regardless. readAvailable()'s short timeout keeps this from
        // blocking once the child is actually gone.
        int wstatus = 0;
        pid_t r;
        do {
            readAvailable(100);
            r = ::waitpid(pid_, &wstatus, WNOHANG);
        } while (r == 0);
        while (r < 0 && errno == EINTR) {
            r = ::waitpid(pid_, &wstatus, 0);
        }
        pid_t donePid = pid_;
        pid_ = -1;
        return r == donePid && WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : (128 + WTERMSIG(wstatus));
    }

private:
    pid_t pid_ = -1;
    int fd_ = -1;
};

}  // namespace

TEST_CASE("simple external command", "[integration]") {
    auto r = runUsh("echo hello world");
    CHECK(r.output == "hello world\n");
    CHECK(r.status == 0);
}

TEST_CASE("exit status of the last command is returned", "[integration]") {
    CHECK(runUsh("true").status == 0);
    CHECK(runUsh("false").status == 1);
    CHECK(runUsh("exit 42").status == 42);
}

TEST_CASE("a pipeline of real external processes", "[integration]") {
    auto r = runUsh("printf 'b\\na\\nc\\n' | sort | tr 'a-z' 'A-Z'");
    CHECK(r.output == "A\nB\nC\n");
    CHECK(r.status == 0);
}

TEST_CASE("$? reflects the previous pipeline's exit status", "[integration]") {
    auto r = runUsh("false; echo $?");
    CHECK(r.output == "1\n");
}

TEST_CASE("&& and || short-circuit", "[integration]") {
    CHECK(runUsh("true && echo yes").output == "yes\n");
    CHECK(runUsh("false && echo yes").output == "");
    CHECK(runUsh("false || echo yes").output == "yes\n");
    CHECK(runUsh("true || echo yes").output == "");
}

TEST_CASE("negated pipeline", "[integration]") {
    auto r = runUsh("if ! false; then echo works; fi");
    CHECK(r.output == "works\n");
}

TEST_CASE("if/elif/else", "[integration]") {
    CHECK(runUsh("x=2; if [ $x = 1 ]; then echo one; elif [ $x = 2 ]; then echo two; "
                  "else echo other; fi")
              .output == "two\n");
}

TEST_CASE("for loop over a word list and over $@", "[integration]") {
    CHECK(runUsh("for i in a b c; do echo $i; done").output == "a\nb\nc\n");
    // extraArgs[0] becomes $0 (as in real `sh -c script name arg...`);
    // the rest become $1, $2, ...
    auto r = runUsh("for i; do echo $i; done", {"argv0", "x", "y"});
    CHECK(r.output == "x\ny\n");
}

TEST_CASE("while loop with arithmetic expansion", "[integration]") {
    auto r = runUsh("i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done");
    CHECK(r.output == "0\n1\n2\n");
}

TEST_CASE("until loop", "[integration]") {
    auto r = runUsh("i=0; until [ $i -ge 3 ]; do echo $i; i=$((i+1)); done");
    CHECK(r.output == "0\n1\n2\n");
}

TEST_CASE("case statement", "[integration]") {
    CHECK(runUsh("case foo in f*) echo matched;; *) echo nope;; esac").output == "matched\n");
    CHECK(runUsh("case zzz in f*) echo matched;; *) echo nope;; esac").output == "nope\n");
}

TEST_CASE("break and continue, including multi-level", "[integration]") {
    auto r1 = runUsh("for i in 1 2 3 4 5; do "
                      "if [ $i -eq 3 ]; then continue; fi; "
                      "if [ $i -eq 5 ]; then break; fi; echo $i; done");
    CHECK(r1.output == "1\n2\n4\n");

    auto r2 = runUsh("for i in 1 2; do for j in a b c; do "
                      "if [ $j = b ]; then break 2; fi; echo $i-$j; done; done");
    CHECK(r2.output == "1-a\n");
}

TEST_CASE("function definition, arguments, and return status", "[integration]") {
    auto r = runUsh("f() { echo \"n=$# first=$1\"; return 3; }; f a b c; echo \"status=$?\"");
    CHECK(r.output == "n=3 first=a\nstatus=3\n");
}

TEST_CASE("command substitution runs a real command, both $() and backticks", "[integration]") {
    CHECK(runUsh("echo $(echo inner)").output == "inner\n");
    CHECK(runUsh("echo `echo inner`").output == "inner\n");
}

TEST_CASE("nested command substitution and arithmetic", "[integration]") {
    auto r = runUsh("echo $(( $(echo 3) * $(echo 4) ))");
    CHECK(r.output == "12\n");
}

TEST_CASE("output redirection, append, and input redirection", "[integration]") {
    TempDir dir;
    std::string file = (dir.path() / "out.txt").string();
    auto r = runUsh("echo line1 > " + file + "; echo line2 >> " + file + "; cat < " + file);
    CHECK(r.output == "line1\nline2\n");
    CHECK(r.status == 0);
}

TEST_CASE("here-document with and without expansion", "[integration]") {
    auto r1 = runUsh("cat <<EOF\nvalue: $((2+2))\nEOF\n");
    CHECK(r1.output == "value: 4\n");

    auto r2 = runUsh("cat <<'EOF'\nvalue: $((2+2))\nEOF\n");
    CHECK(r2.output == "value: $((2+2))\n");
}

TEST_CASE("subshells are isolated; brace groups share the current shell", "[integration]") {
    auto sub = runUsh("(x=set); echo \"[${x:-unset}]\"");
    CHECK(sub.output == "[unset]\n");

    auto brace = runUsh("{ x=set; }; echo \"[$x]\"");
    CHECK(brace.output == "[set]\n");
}

TEST_CASE("cd and pwd change and report the working directory", "[integration]") {
    TempDir dir;
    auto r = runUsh("cd " + dir.path().string() + " && pwd");
    // Resolve both sides through the filesystem so a symlinked temp dir
    // (e.g. macOS's /var -> /private/var) doesn't cause a false mismatch.
    std::string want = fs::canonical(dir.path()).string() + "\n";
    CHECK(r.output == want);
}

TEST_CASE("export makes a variable visible to child processes", "[integration]") {
    // Single-quote the binary path: it may itself contain spaces (e.g. a
    // project directory named "unnamed shell"), which would otherwise
    // split into multiple words when ush lexes this script.
    auto r = runUsh("export FOO=bar; '" + std::string(USH_BINARY_PATH) + "' -c 'echo $FOO'");
    CHECK(r.output == "bar\n");
}

TEST_CASE("readonly rejects reassignment without crashing the shell", "[integration]") {
    // Check $? right after the failed assignment - echo afterward would
    // otherwise be the last command and set its own (successful) status.
    auto r = runUsh("readonly X=5; X=6 2>/dev/null; echo \"X=$X status=$?\"");
    CHECK(r.output == "X=5 status=1\n");
}

TEST_CASE("unset removes a variable", "[integration]") {
    auto r = runUsh("Y=hi; unset Y; echo \"[${Y:-unset}]\"");
    CHECK(r.output == "[unset]\n");
}

TEST_CASE("shift removes leading positional parameters", "[integration]") {
    auto r = runUsh("echo $1; shift; echo $1", {"argv0", "a", "b", "c"});
    CHECK(r.output == "a\nb\n");
}

TEST_CASE("the test and bracket builtins", "[integration]") {
    CHECK(runUsh("[ 1 -lt 2 ] && echo yes").output == "yes\n");
    CHECK(runUsh("[ abc = abc ] && echo yes").output == "yes\n");
    CHECK(runUsh("[ -z '' ] && echo yes").output == "yes\n");
    CHECK(runUsh("test 5 -gt 3 && echo yes").output == "yes\n");
}

TEST_CASE("a command that doesn't exist exits 127", "[integration]") {
    auto r = runUsh("this_command_definitely_does_not_exist_xyz");
    CHECK(r.status == 127);
}

TEST_CASE("colon is a true no-op", "[integration]") {
    CHECK(runUsh(": ; echo after").output == "after\n");
}

TEST_CASE("eval runs a constructed command", "[integration]") {
    auto r = runUsh("cmd='echo hi there'; eval $cmd");
    CHECK(r.output == "hi there\n");
}

TEST_CASE("printf: basic conversions, width/precision, and cycling", "[integration]") {
    CHECK(runUsh("printf '%s is %d\\n' Alice 30").output == "Alice is 30\n");
    CHECK(runUsh("printf '%-6s|%5d|\\n' hi 42").output == "hi    |   42|\n");
    CHECK(runUsh("printf '%x %o\\n' 255 8").output == "ff 10\n");
    // More arguments than conversions -> the format is reused.
    CHECK(runUsh("printf '%d-%d\\n' 1 2 3 4 5").output == "1-2\n3-4\n5-0\n");
    // A literal '%%' and backslash escapes in the format text itself.
    CHECK(runUsh("printf '100%%\\tdone\\n'").output == "100%\tdone\n");
}

TEST_CASE("read splits a line on IFS, with the last variable absorbing extra fields",
          "[integration]") {
    auto r = runUsh("read a b rest; echo \"[$a][$b][$rest]\"", {}, "hello world  extra1 extra2\n");
    CHECK(r.output == "[hello][world][extra1 extra2]\n");
}

TEST_CASE("read without -r processes a trailing backslash-newline as a line continuation",
          "[integration]") {
    auto r = runUsh("read line; echo \"[$line]\"", {}, "one\\\ntwo\n");
    CHECK(r.output == "[onetwo]\n");
}

TEST_CASE("read -r treats backslash literally", "[integration]") {
    auto r = runUsh("read -r line; echo \"[$line]\"", {}, "a\\nb\n");
    CHECK(r.output == "[a\\nb]\n");
}

TEST_CASE("read reports failure at end of input", "[integration]") {
    auto r = runUsh("read x; echo \"status=$?\"", {}, "");
    CHECK(r.output == "status=1\n");
}

TEST_CASE("command -v and command bypassing a function", "[integration]") {
    CHECK(runUsh("command -v echo").output == "echo\n");
    CHECK(runUsh("command -v this_command_definitely_does_not_exist_xyz").status == 1);
    // `command` skips function lookup, so a function named after a
    // builtin doesn't shadow it.
    auto r = runUsh("echo() { printf 'shadowed\\n'; }; command echo real");
    CHECK(r.output == "real\n");
}

TEST_CASE("type identifies builtins, functions, and external commands", "[integration]") {
    CHECK(runUsh("type cd").output == "cd is a shell builtin\n");
    CHECK(runUsh("f() { :; }; type f").output == "f is a function\n");
    CHECK(runUsh("type :").output == ": is a special shell builtin\n");
    CHECK(runUsh("type this_command_definitely_does_not_exist_xyz").status == 1);
}

TEST_CASE("getopts parses bundled short options and an option with an argument",
          "[integration]") {
    auto r = runUsh(
        "while getopts \"ab:\" opt; do case $opt in "
        "a) echo flag-a ;; b) echo \"arg=$OPTARG\" ;; esac; done; "
        "shift $((OPTIND - 1)); echo \"rest=$*\"",
        {"argv0", "-ab", "val", "extra"});
    CHECK(r.output == "flag-a\narg=val\nrest=extra\n");
}

TEST_CASE("getopts reports an unknown option via '?'", "[integration]") {
    auto r = runUsh(
        "getopts \"a\" opt 2>/dev/null; echo \"opt=$opt\"",
        {"argv0", "-z"});
    CHECK(r.output == "opt=?\n");
}

TEST_CASE("umask reports and sets the current mask", "[integration]") {
    auto r = runUsh("umask 022; umask");
    CHECK(r.output == "0022\n");
}

TEST_CASE("wait reaps a background job and returns its exit status", "[integration]") {
    auto r = runUsh("(exit 7) & wait; echo \"status=$?\"");
    CHECK(r.output == "status=7\n");
}

// --- interactive mode (-i) ----------------------------------------------

TEST_CASE("interactive mode prompts with PS1 and runs each complete line", "[integration]") {
    auto r = runUshInteractive("echo hello\necho world\n");
    CHECK(r.output == "$ hello\n$ world\n$ \n");
    CHECK(r.status == 0);
}

TEST_CASE("an incomplete compound command continues with PS2 until it closes",
          "[integration]") {
    auto r = runUshInteractive("if true\nthen\necho yes\nfi\n");
    CHECK(r.output == "$ > > > yes\n$ \n");
}

TEST_CASE("an unterminated quote continues with PS2, preserving the embedded newline",
          "[integration]") {
    auto r = runUshInteractive("echo \"a\nb\"\n");
    CHECK(r.output == "$ > a\nb\n$ \n");
}

TEST_CASE("a function defined on one line is callable on a later one", "[integration]") {
    auto r = runUshInteractive("greet() { echo \"hi $1\"; }\ngreet world\n");
    CHECK(r.output == "$ $ hi world\n$ \n");
}

TEST_CASE("exit ends the interactive session immediately, skipping later lines",
          "[integration]") {
    auto r = runUshInteractive("echo before\nexit 5\necho after\n");
    CHECK(r.output == "$ before\n$ ");
    CHECK(r.status == 5);
}

TEST_CASE("a genuine syntax error is reported and the session recovers", "[integration]") {
    auto r = runUshInteractive("echo hi &&\n&& echo bad\necho recovered\n");
    CHECK(r.output.find("hi") == std::string::npos);  // the first line never completed/ran
    CHECK(r.output.find("syntax error") != std::string::npos);
    CHECK(r.output.find("recovered\n") != std::string::npos);
}

TEST_CASE("unexpected end of file inside an unfinished command is an error, exit status 2",
          "[integration]") {
    auto r = runUshInteractive("if true\nthen\necho yes\n");
    CHECK(r.output.find("syntax error") != std::string::npos);
    CHECK(r.status == 2);
}

TEST_CASE("a plain end of file at a fresh prompt exits cleanly with status 0",
          "[integration]") {
    auto r = runUshInteractive("");
    CHECK(r.output == "$ \n");
    CHECK(r.status == 0);
}

TEST_CASE("setting PS1 takes effect starting with the next prompt", "[integration]") {
    auto r = runUshInteractive("PS1='myprompt> '\necho after\n");
    // Trailing "\n" is the cosmetic newline printed on EOF at a fresh
    // prompt (real shells do the same on Ctrl-D, since EOF doesn't echo
    // one itself).
    CHECK(r.output == "$ myprompt> after\nmyprompt> \n");
}

TEST_CASE("PS1's '!' is replaced with the next command's history number, '!!' with a literal '!'",
          "[integration]") {
    auto r = runUshInteractive("PS1='[!] '\necho one\necho two\nPS1='literal !! bang !$ '\necho three\n");
    // The very first prompt is shown before any input has been read, so
    // it still uses the default PS1 ("$ ") - the same off-by-one as
    // "setting PS1 takes effect starting with the next prompt" above.
    // From there: "PS1='[!] '" becomes history entry #1 -> next prompt
    // "[2] " -> "echo one" (#2) -> "[3] " -> "echo two" (#3) -> "[4] " ->
    // "PS1='literal !! bang !$ '" (#4) -> next prompt, with the new PS1
    // and next number 5: "literal ! bang 5$ " ("!!" -> "!", "!$" -> "5"
    // then literal "$ ") -> "echo three" (#5) -> "literal ! bang 6$ ".
    CHECK(r.output ==
          "$ [2] one\n[3] two\n[4] literal ! bang 5$ three\nliteral ! bang 6$ \n");
}

// --- startup files (~/.ushrc, $ENV) ---------------------------------------

TEST_CASE("the ushrc startup file is sourced before the first interactive prompt", "[integration]") {
    TempDir home;
    {
        std::ofstream rc(home.path() / ".ushrc");
        rc << "echo ushrc-loaded\n";
        rc << "greet() { echo \"hi $1\"; }\n";
        rc << "GREETING=set-in-ushrc\n";
    }
    auto r = runUshInteractiveWithHome("greet world\necho $GREETING\n", home.path().string());
    // All three prove the rc file ran *in the current environment* (not
    // a subshell) before the first prompt: its own echo appears, the
    // function it defined is callable on a later line, and the variable
    // it set is visible on a later line too.
    CHECK(r.output.find("ushrc-loaded") != std::string::npos);
    CHECK(r.output.find("hi world") != std::string::npos);
    CHECK(r.output.find("set-in-ushrc") != std::string::npos);
}

TEST_CASE("the ushrc startup file is NOT sourced for a non-interactive run", "[integration]") {
    TempDir home;
    {
        std::ofstream rc(home.path() / ".ushrc");
        rc << "echo ushrc-loaded\n";
    }
    auto r = runUshWithArgv({USH_BINARY_PATH, "-c", "echo hi"}, "", "/dev/null", home.path().string());
    CHECK(r.output == "hi\n");
    CHECK(r.output.find("ushrc-loaded") == std::string::npos);
}

TEST_CASE("a missing ~/.ushrc is not an error", "[integration]") {
    // Every other interactive test in this file already relies on this
    // (the default homeDir doesn't exist) - this just makes the
    // guarantee explicit.
    auto r = runUshInteractive("echo hi\n");
    CHECK(r.status == 0);
    CHECK(r.output.find("hi") != std::string::npos);
}

TEST_CASE("exit inside ~/.ushrc ends the session immediately, without reading further input",
          "[integration]") {
    TempDir home;
    {
        std::ofstream rc(home.path() / ".ushrc");
        rc << "echo before-exit\n";
        rc << "exit 7\n";
        rc << "echo should-not-run\n";
    }
    auto r = runUshInteractiveWithHome("echo also-should-not-run\n", home.path().string());
    CHECK(r.output == "before-exit\n");
    CHECK(r.status == 7);
}

TEST_CASE("a syntax error in ~/.ushrc is reported but doesn't stop the shell from starting",
          "[integration]") {
    TempDir home;
    {
        std::ofstream rc(home.path() / ".ushrc");
        rc << "if true\n";  // unterminated compound command
    }
    auto r = runUshInteractiveWithHome("echo still-alive\n", home.path().string());
    CHECK(r.output.find("syntax error") != std::string::npos);
    CHECK(r.output.find("still-alive") != std::string::npos);
}

TEST_CASE("$ENV is sourced for interactive shells before ~/.ushrc", "[integration]") {
    TempDir dir;
    fs::path envFile = dir.path() / "envfile.sh";
    {
        std::ofstream f(envFile);
        f << "echo env-loaded\n";
        f << "WHO=env\n";
    }
    auto r = runUshInteractiveWithEnv("echo $WHO\n", envFile.string());
    CHECK(r.output.find("env-loaded") != std::string::npos);
    CHECK(r.output.find("env\n") != std::string::npos);
}

TEST_CASE("passing -i together with -c just runs non-interactively (documented simplification)",
          "[integration]") {
    auto r = runUshWithArgv({USH_BINARY_PATH, "-i", "-c", "echo hi"}, "");
    CHECK(r.output == "hi\n");
}

// --- trap / times / kill -------------------------------------------------

TEST_CASE("an EXIT trap runs once, after the script's own last command",
          "[integration]") {
    auto r = runUsh("trap 'echo cleanup' EXIT; echo body");
    CHECK(r.output == "body\ncleanup\n");
}

TEST_CASE("an EXIT trap runs (and the original status is preserved) when exit is called",
          "[integration]") {
    auto r = runUsh("trap 'echo cleanup' EXIT; echo body; exit 3");
    CHECK(r.output == "body\ncleanup\n");
    CHECK(r.status == 3);
}

TEST_CASE("an EXIT trap calling exit itself overrides the final status", "[integration]") {
    auto r = runUsh("trap 'exit 9' EXIT; exit 3");
    CHECK(r.status == 9);
}

TEST_CASE("trap with no operands lists currently-trapped conditions", "[integration]") {
    auto r = runUsh("trap 'echo hi' TERM; trap");
    CHECK(r.output == "trap -- 'echo hi' TERM\n");
}

TEST_CASE("trap - resets a condition to its default and removes it from the listing",
          "[integration]") {
    auto r = runUsh("trap 'echo hi' TERM; trap - TERM; trap");
    CHECK(r.output == "");
}

TEST_CASE("times runs successfully and reports two lines of CPU time", "[integration]") {
    auto r = runUsh("times");
    CHECK(r.status == 0);
    CHECK(std::count(r.output.begin(), r.output.end(), '\n') == 2);
    CHECK(r.output.find('m') != std::string::npos);
    CHECK(r.output.find('s') != std::string::npos);
}

TEST_CASE("kill sends a signal that terminates the target process", "[integration]") {
    auto r = runUsh("sleep 30 & pid=$!; kill $pid; wait $pid; echo \"status=$?\"");
    CHECK(r.output == "status=143\n");  // 128 + SIGTERM(15)
}

TEST_CASE("a signal trap interrupts a blocked wait on a foreground child promptly",
          "[integration]") {
    // Regression test: signal(2) on at least macOS installs handlers with
    // SA_RESTART implied, which would make a blocked waitpid(2)
    // transparently resume instead of returning EINTR - the trap would
    // still eventually run, but only once the foreground child happened
    // to exit on its own (here, after the full 30-second sleep), which
    // defeats the entire point of trapping a signal for prompt shutdown.
    // Fixed via sigaction(2) with SA_RESTART deliberately not set - see
    // docs/DESIGN.md. The huge margin between the sleep (30s) and this
    // test's deadline (~5s) is deliberate: it needs to be generous enough
    // to never flake under CI load while still failing hard if the old,
    // "wait for the child no matter what" behavior ever comes back.
    pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        std::vector<std::string> args = {USH_BINARY_PATH, "-c",
                                          "trap 'exit 7' TERM; sleep 30; exit 1"};
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(USH_BINARY_PATH, argv.data());
        _exit(127);
    }

    ::usleep(300000);  // give the trap time to install before signaling
    ::kill(pid, SIGTERM);

    int wstatus = 0;
    bool exited = false;
    for (int i = 0; i < 50; ++i) {  // up to ~5s total, far under the 30s sleep
        pid_t r = ::waitpid(pid, &wstatus, WNOHANG);
        if (r == pid) {
            exited = true;
            break;
        }
        ::usleep(100000);
    }
    if (!exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &wstatus, 0);
    }
    REQUIRE(exited);
    CHECK(WIFEXITED(wstatus));
    CHECK(WEXITSTATUS(wstatus) == 7);
}

// --- job control ---------------------------------------------------------

namespace {
// Parses the pid out of a "[<jobId>] <pid>" announcement line (printed
// when a job is backgrounded, or echoed back by `bg`/`fg` style commands
// in some of these tests) - returns -1 if not found.
long extractAnnouncedPid(const std::string& output, int jobId) {
    std::string marker = "[" + std::to_string(jobId) + "] ";
    auto pos = output.find(marker);
    if (pos == std::string::npos) return -1;
    pos += marker.size();
    std::size_t end = pos;
    while (end < output.size() && std::isdigit(static_cast<unsigned char>(output[end]))) ++end;
    if (end == pos) return -1;
    try {
        return std::stol(output.substr(pos, end - pos));
    } catch (...) {
        return -1;
    }
}
}  // namespace

TEST_CASE("non-interactive mode never announces or tracks jobs", "[integration]") {
    auto r = runUsh("sleep 0.1 & echo main; wait; echo done");
    CHECK(r.output == "main\ndone\n");  // no "[1] <pid>" line
}

TEST_CASE("backgrounding a job announces it and jobs lists it as Running",
          "[integration]") {
    InteractiveSession s;
    std::string out = s.readAvailable();  // initial prompt
    s.send("sleep 30 &");
    out = s.readAvailable();
    CHECK(extractAnnouncedPid(out, 1) > 0);

    s.send("jobs");
    out = s.readAvailable();
    CHECK(out.find("Running") != std::string::npos);
    CHECK(out.find("sleep 30") != std::string::npos);

    s.send("kill %1");
    s.readAvailable();
    s.send("wait");
    s.finish();
}

TEST_CASE("a completed background job is reported Done before the next prompt",
          "[integration]") {
    InteractiveSession s;
    s.readAvailable();
    s.send("sleep 0.2 &");
    s.readAvailable();
    s.send("sleep 0.5");  // foreground: gives the background job time to finish
    std::string out = s.readAvailable(3000);
    CHECK(out.find("Done") != std::string::npos);
    CHECK(out.find("sleep 0.2") != std::string::npos);
    s.finish();
}

TEST_CASE("a job stopped by SIGTSTP is reported, then resumable via bg and reachable via jobs",
          "[integration]") {
    InteractiveSession s;
    s.readAvailable();
    s.send("sleep 30 &");
    std::string out = s.readAvailable();
    long pid = extractAnnouncedPid(out, 1);
    REQUIRE(pid > 0);

    // A single backgrounded external command is its own process group
    // leader (pgid == its own pid) - see runList()'s async handling in
    // executor.cpp. A real terminal's Ctrl-Z signals the whole foreground
    // *group*, not one process - kill(2) needs the negated pid for that.
    REQUIRE(::kill(-static_cast<pid_t>(pid), SIGTSTP) == 0);
    // kill(2) returning just means the signal was queued, not that the
    // target has actually finished transitioning to the stopped state
    // yet - give it a moment before asking the shell to notice.
    ::usleep(200000);

    s.send("jobs");
    out = s.readAvailable();
    CHECK(out.find("Stopped") != std::string::npos);

    s.send("bg %1");
    out = s.readAvailable();
    CHECK(out.find("&") != std::string::npos);  // bg's "[1]+  sleep 30 &" echo

    s.send("jobs");
    out = s.readAvailable();
    CHECK(out.find("Running") != std::string::npos);

    s.send("kill %1");
    s.readAvailable();
    s.send("wait");
    s.finish();
}

TEST_CASE("fg brings a background job to the foreground and waits for it",
          "[integration]") {
    InteractiveSession s;
    s.readAvailable();
    s.send("sleep 0.3 &");
    s.readAvailable();
    s.send("fg");
    std::string out = s.readAvailable(3000);
    CHECK(out.find("sleep 0.3") != std::string::npos);  // fg's own echo of the command

    s.send("echo after-fg");
    out = s.readAvailable();
    CHECK(out.find("after-fg") != std::string::npos);
    s.send("jobs");
    out = s.readAvailable();
    CHECK(out.find("Running") == std::string::npos);  // fg's job is gone, not still listed
    CHECK(out.find("Stopped") == std::string::npos);

    s.finish();
}

// --- history / fc --------------------------------------------------------

TEST_CASE("a non-interactive run never builds a history list", "[integration]") {
    auto r = runUsh("echo one; echo two; fc -l");
    CHECK(r.output.find("one") != std::string::npos);
    CHECK(r.output.find("two") != std::string::npos);
    // fc -l with nothing recorded reports an empty history, not a crash
    // or a listing of "echo one"/"echo two" themselves.
    CHECK(r.output.find("history is empty") != std::string::npos);
}

TEST_CASE("fc -l lists previously run interactive commands, numbered", "[integration]") {
    InteractiveSession s;
    s.readAvailable();
    s.send("echo one");
    s.readAvailable();
    s.send("echo two");
    s.readAvailable();
    s.send("fc -l");
    std::string out = s.readAvailable();
    s.finish();
    CHECK(out.find("1  echo one") != std::string::npos);
    CHECK(out.find("2  echo two") != std::string::npos);
}

TEST_CASE("history builtin lists the same entries as fc -l", "[integration]") {
    InteractiveSession s;
    s.readAvailable();
    s.send("echo hi");
    s.readAvailable();
    s.send("history");
    std::string out = s.readAvailable();
    s.finish();
    CHECK(out.find("1  echo hi") != std::string::npos);
}

TEST_CASE("fc -s re-executes the previous command", "[integration]") {
    InteractiveSession s;
    s.readAvailable();
    s.send("echo original");
    s.readAvailable();
    s.send("fc -s");
    std::string out = s.readAvailable();
    s.finish();
    // fc -s echoes the command before running it, then runs it - "original"
    // should appear twice: once from fc's echo, once from actually running it.
    std::size_t first = out.find("original");
    REQUIRE(first != std::string::npos);
    CHECK(out.find("original", first + 1) != std::string::npos);
}

TEST_CASE("fc -s applies an old=new substitution", "[integration]") {
    InteractiveSession s;
    s.readAvailable();
    s.send("echo hello");
    s.readAvailable();
    s.send("fc -s hello=goodbye");
    std::string out = s.readAvailable();
    s.finish();
    CHECK(out.find("goodbye") != std::string::npos);
}

TEST_CASE("history persists across sessions via HISTFILE", "[integration]") {
    TempDir dir;
    std::string histFile = (dir.path() / "hist").string();

    {
        InteractiveSession s(histFile);
        s.readAvailable();
        s.send("echo from-session-one");
        s.readAvailable();
        s.finish();
    }

    auto r = runUshInteractive("fc -l\n", histFile);
    CHECK(r.output.find("echo from-session-one") != std::string::npos);
}

// --- interactive line editing (real pty) ----------------------------------

TEST_CASE("line editor: left arrow plus backspace edit a command before it runs",
          "[integration]") {
    PtySession s;
    s.readAvailable();
    // Type "echo XY", move left one (before the Y), backspace (removes
    // the X) - "echo Y" should be what actually runs.
    s.sendRaw("echo XY");
    s.readAvailable(300);
    s.sendRaw("\x1b[D");
    s.readAvailable(300);
    s.sendRaw("\x7f");
    s.readAvailable(300);
    s.sendRaw("\r");
    std::string out = s.readAvailable();
    s.finish();
    CHECK(out.find("\nY") != std::string::npos);
    CHECK(out.find("XY") == std::string::npos);
}

TEST_CASE("line editor: up arrow recalls the previous command", "[integration]") {
    PtySession s;
    std::string out = s.readAvailable();
    s.sendLine("echo first");
    out += s.readAvailable();
    s.sendRaw("\x1b[A");  // up arrow: recall "echo first"
    out += s.readAvailable(300);
    s.sendRaw("\r");
    out += s.readAvailable();
    s.finish();
    // "first" should appear twice across the whole transcript: once from
    // actually running it, once more from the recalled re-run.
    std::size_t at = out.find("first");
    REQUIRE(at != std::string::npos);
    CHECK(out.find("first", at + 1) != std::string::npos);
}

TEST_CASE("line editor: Ctrl-A then Ctrl-K clears a typed line before it runs", "[integration]") {
    PtySession s;
    s.readAvailable();
    s.sendRaw("echo REMOVE_ME");
    s.readAvailable(300);
    s.sendRaw("\x01");  // Ctrl-A: start of line
    s.readAvailable(300);
    s.sendRaw("\x0b");  // Ctrl-K: kill to end
    s.readAvailable(300);
    s.sendLine("echo kept");
    std::string out = s.readAvailable();
    s.finish();
    CHECK(out.find("kept") != std::string::npos);
    CHECK(out.find("REMOVE_ME") == std::string::npos);
}

TEST_CASE("line editor: Ctrl-C aborts the current line without exiting the shell",
          "[integration]") {
    PtySession s;
    s.readAvailable();
    s.sendRaw("echo not_run");
    s.readAvailable(300);
    s.sendRaw("\x03");  // Ctrl-C
    s.readAvailable(500);
    s.sendLine("echo still_alive");
    std::string out = s.readAvailable();
    int status = s.finish();
    CHECK(out.find("not_run") == std::string::npos);
    CHECK(out.find("still_alive") != std::string::npos);
    CHECK(status == 0);
}
