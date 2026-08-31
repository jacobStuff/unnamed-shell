// Integration tests: run the actual built `ush` binary (via fork+execve,
// not popen, so there's no double shell-quoting to worry about) and check
// its combined stdout+stderr output and exit status. These exercise real
// process execution (fork/exec/wait/pipes/redirects) that the unit tests
// for the lexer/parser/expander can't - see docs/DESIGN.md.

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
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

RunResult runUshWithArgv(const std::vector<std::string>& args, const std::string& stdinContent) {
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

TEST_CASE("-i combined with -c just runs non-interactively (documented simplification)",
          "[integration]") {
    auto r = runUshWithArgv({USH_BINARY_PATH, "-i", "-c", "echo hi"}, "");
    CHECK(r.output == "hi\n");
}
