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

RunResult runUsh(const std::string& script, const std::vector<std::string>& extraArgs = {}) {
    int pipefd[2];
    REQUIRE(::pipe(pipefd) == 0);

    pid_t pid = ::fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        ::dup2(pipefd[1], 1);
        ::dup2(pipefd[1], 2);
        ::close(pipefd[0]);
        ::close(pipefd[1]);

        std::vector<std::string> args = {USH_BINARY_PATH, "-c", script};
        for (const auto& a : extraArgs) args.push_back(a);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(USH_BINARY_PATH, argv.data());
        _exit(127);
    }

    ::close(pipefd[1]);
    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) output.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);

    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);
    int status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : (128 + WTERMSIG(wstatus));
    return {output, status};
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
