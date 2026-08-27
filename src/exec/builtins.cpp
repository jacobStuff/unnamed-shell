#include "exec/builtins.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "exec/control_signals.hpp"
#include "exec/executor.hpp"
#include "runtime/environment.hpp"

namespace ush {

namespace {

constexpr std::size_t kPathBufSize = 4096;

int parseIntArg(const std::string& s, int fallback) {
    try {
        std::size_t consumed = 0;
        int v = std::stoi(s, &consumed);
        if (consumed != s.size()) return fallback;
        return v;
    } catch (...) {
        return fallback;
    }
}

std::string pathSearch(const std::string& env_path, const std::string& name, int mode) {
    std::size_t start = 0;
    while (true) {
        std::size_t colon = env_path.find(':', start);
        std::string dir =
            env_path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (dir.empty()) dir = ".";
        std::string candidate = dir + "/" + name;
        if (::access(candidate.c_str(), mode) == 0) return candidate;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}

// --- special builtins -------------------------------------------------

int biColon(Executor&, const std::vector<std::string>&) { return 0; }

int biExit(Executor& ex, const std::vector<std::string>& args) {
    int status = ex.env().lastExitStatus;
    if (args.size() >= 2) status = parseIntArg(args[1], 2);
    throw ExitSignal{status};
}

int biReturn(Executor& ex, const std::vector<std::string>& args) {
    int status = ex.env().lastExitStatus;
    if (args.size() >= 2) status = parseIntArg(args[1], 0);
    throw ReturnSignal{status};
}

int biBreak(Executor&, const std::vector<std::string>& args) {
    int n = args.size() >= 2 ? parseIntArg(args[1], 1) : 1;
    throw BreakSignal{n < 1 ? 1 : n};
}

int biContinue(Executor&, const std::vector<std::string>& args) {
    int n = args.size() >= 2 ? parseIntArg(args[1], 1) : 1;
    throw ContinueSignal{n < 1 ? 1 : n};
}

int biExport(Executor& ex, const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-p") continue;  // listing not implemented
        auto eq = a.find('=');
        std::string name = (eq == std::string::npos) ? a : a.substr(0, eq);
        if (eq != std::string::npos) {
            try {
                ex.env().set(name, a.substr(eq + 1));
            } catch (const ReadonlyVariableError& e) {
                std::fprintf(stderr, "ush: export: %s\n", e.what());
                return 1;
            }
        }
        ex.env().setExported(name, true);
    }
    return 0;
}

int biReadonly(Executor& ex, const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-p") continue;
        auto eq = a.find('=');
        std::string name = (eq == std::string::npos) ? a : a.substr(0, eq);
        if (eq != std::string::npos) {
            try {
                ex.env().set(name, a.substr(eq + 1));
            } catch (const ReadonlyVariableError& e) {
                std::fprintf(stderr, "ush: readonly: %s\n", e.what());
                return 1;
            }
        }
        ex.env().setReadonly(name, true);
    }
    return 0;
}

int biUnset(Executor& ex, const std::vector<std::string>& args) {
    bool functionsOnly = false;
    std::size_t i = 1;
    if (i < args.size() && (args[i] == "-f" || args[i] == "-v")) {
        functionsOnly = (args[i] == "-f");
        ++i;
    }
    for (; i < args.size(); ++i) {
        if (functionsOnly) {
            ex.eraseFunction(args[i]);
        } else {
            ex.env().unset(args[i]);
        }
    }
    return 0;
}

int biShift(Executor& ex, const std::vector<std::string>& args) {
    int n = args.size() >= 2 ? parseIntArg(args[1], -1) : 1;
    auto params = ex.env().positionalParams();
    if (n < 0 || static_cast<std::size_t>(n) > params.size()) {
        std::fprintf(stderr, "ush: shift: shift count out of range\n");
        return 1;
    }
    params.erase(params.begin(), params.begin() + n);
    ex.env().setPositionalParams(std::move(params));
    return 0;
}

int biSet(Executor& ex, const std::vector<std::string>& args) {
    std::size_t i = 1;
    bool sawDashDash = false;
    for (; i < args.size(); ++i) {
        if (args[i] == "--") {
            sawDashDash = true;
            ++i;
            break;
        }
        if (args[i].size() >= 2 && (args[i][0] == '-' || args[i][0] == '+')) {
            continue;  // option flag - not implemented, silently accepted
        }
        break;  // first operand
    }
    if (sawDashDash || i < args.size()) {
        ex.env().setPositionalParams(std::vector<std::string>(args.begin() + i, args.end()));
    }
    return 0;
}

int biEval(Executor& ex, const std::vector<std::string>& args) {
    std::string src;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (i > 1) src += ' ';
        src += args[i];
    }
    return ex.runSourceInCurrentContext(src);
}

int biDot(Executor& ex, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::fprintf(stderr, "ush: .: filename argument required\n");
        return 2;
    }
    std::string filename = args[1];
    std::string resolved = filename;
    if (filename.find('/') == std::string::npos) {
        resolved = pathSearch(ex.env().get("PATH").value_or(""), filename, R_OK);
        if (resolved.empty()) {
            std::fprintf(stderr, "ush: .: %s: not found\n", filename.c_str());
            return 1;
        }
    }
    std::ifstream f(resolved);
    if (!f) {
        std::fprintf(stderr, "ush: .: %s: cannot open\n", resolved.c_str());
        return 1;
    }
    std::ostringstream ss;
    ss << f.rdbuf();

    bool overrideParams = args.size() > 2;
    std::vector<std::string> saved;
    if (overrideParams) {
        saved = ex.env().positionalParams();
        ex.env().setPositionalParams(std::vector<std::string>(args.begin() + 2, args.end()));
    }
    int status;
    try {
        status = ex.runSourceInCurrentContext(ss.str());
    } catch (...) {
        if (overrideParams) ex.env().setPositionalParams(std::move(saved));
        throw;
    }
    if (overrideParams) ex.env().setPositionalParams(std::move(saved));
    return status;
}

int biExec(Executor& ex, const std::vector<std::string>& args) {
    if (args.size() < 2) return 0;  // just redirects, already applied permanently by the caller

    std::vector<std::string> cmdArgs(args.begin() + 1, args.end());
    std::vector<std::string> envStrings = ex.env().exportedEnviron();
    std::vector<char*> argv, envp;
    for (auto& s : cmdArgs) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    for (auto& s : envStrings) envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);

    const std::string& name = cmdArgs[0];
    bool foundExecutable = false;
    if (name.find('/') != std::string::npos) {
        foundExecutable = true;
        ::execve(name.c_str(), argv.data(), envp.data());
    } else {
        std::string candidate = pathSearch(ex.env().get("PATH").value_or("/bin:/usr/bin"), name, X_OK);
        if (!candidate.empty()) {
            foundExecutable = true;
            ::execve(candidate.c_str(), argv.data(), envp.data());
        }
    }
    std::fprintf(stderr, "ush: exec: %s: %s\n", name.c_str(),
                 foundExecutable ? std::strerror(errno) : "command not found");
    // §2.14 exec: failing to execute causes a non-interactive shell to
    // exit (127 if not found, 126 if found but not executable).
    throw ExitSignal{foundExecutable ? 126 : 127};
}

// --- regular builtins ---------------------------------------------------

int biTrueBuiltin(Executor&, const std::vector<std::string>&) { return 0; }
int biFalseBuiltin(Executor&, const std::vector<std::string>&) { return 1; }

int biEcho(Executor&, const std::vector<std::string>& args) {
    std::size_t i = 1;
    bool newline = true;
    if (i < args.size() && args[i] == "-n") {
        newline = false;
        ++i;
    }
    for (; i < args.size(); ++i) {
        std::fputs(args[i].c_str(), stdout);
        if (i + 1 < args.size()) std::fputc(' ', stdout);
    }
    if (newline) std::fputc('\n', stdout);
    return 0;
}

int biCd(Executor& ex, const std::vector<std::string>& args) {
    std::size_t i = 1;
    while (i < args.size() && args[i].size() > 1 && args[i][0] == '-') ++i;  // skip -L/-P (unimplemented)

    std::string target;
    bool printTarget = false;
    if (i < args.size()) {
        if (args[i] == "-") {
            auto oldpwd = ex.env().get("OLDPWD");
            if (!oldpwd) {
                std::fprintf(stderr, "ush: cd: OLDPWD not set\n");
                return 1;
            }
            target = *oldpwd;
            printTarget = true;
        } else {
            target = args[i];
        }
    } else {
        auto home = ex.env().get("HOME");
        if (!home) {
            std::fprintf(stderr, "ush: cd: HOME not set\n");
            return 1;
        }
        target = *home;
    }

    char oldCwd[kPathBufSize];
    if (!::getcwd(oldCwd, sizeof(oldCwd))) oldCwd[0] = '\0';
    if (::chdir(target.c_str()) != 0) {
        std::fprintf(stderr, "ush: cd: %s: %s\n", target.c_str(), std::strerror(errno));
        return 1;
    }
    char newCwd[kPathBufSize];
    if (::getcwd(newCwd, sizeof(newCwd))) {
        ex.env().set("OLDPWD", oldCwd);
        ex.env().set("PWD", newCwd);
        if (printTarget) std::printf("%s\n", newCwd);
    }
    return 0;
}

int biPwd(Executor&, const std::vector<std::string>&) {
    char buf[kPathBufSize];
    if (!::getcwd(buf, sizeof(buf))) {
        std::perror("ush: pwd");
        return 1;
    }
    std::printf("%s\n", buf);
    return 0;
}

// `test`/`[`: a practical subset of §2.14's [/test - unary file/string
// tests, string equality, numeric comparison, and negation. Does NOT
// implement -a/-o/parenthesized combinations (POSIX itself calls their
// behavior "undefined" beyond 4 arguments, and they're rarely needed - a
// documented simplification, see docs/DESIGN.md).
bool unaryFileTest(const std::string& op, const std::string& path) {
    struct stat st;
    bool exists = ::stat(path.c_str(), &st) == 0;
    if (op == "-e") return exists;
    if (op == "-f") return exists && S_ISREG(st.st_mode);
    if (op == "-d") return exists && S_ISDIR(st.st_mode);
    if (op == "-r") return ::access(path.c_str(), R_OK) == 0;
    if (op == "-w") return ::access(path.c_str(), W_OK) == 0;
    if (op == "-x") return ::access(path.c_str(), X_OK) == 0;
    if (op == "-s") return exists && st.st_size > 0;
    return false;
}

int runTest(const std::vector<std::string>& a) {
    if (a.empty()) return 1;
    if (a.size() == 1) return a[0].empty() ? 1 : 0;
    if (a.size() == 2) {
        if (a[0] == "!") return a[1].empty() ? 0 : 1;
        if (a[0] == "-z") return a[1].empty() ? 0 : 1;
        if (a[0] == "-n") return a[1].empty() ? 1 : 0;
        if (a[0].size() == 2 && a[0][0] == '-') return unaryFileTest(a[0], a[1]) ? 0 : 1;
        return 2;
    }
    if (a.size() == 3) {
        const std::string& op = a[1];
        if (op == "=") return a[0] == a[2] ? 0 : 1;
        if (op == "!=") return a[0] != a[2] ? 0 : 1;
        auto toInt = [](const std::string& s) -> std::intmax_t {
            try {
                return std::stoll(s);
            } catch (...) {
                return 0;
            }
        };
        if (op == "-eq") return toInt(a[0]) == toInt(a[2]) ? 0 : 1;
        if (op == "-ne") return toInt(a[0]) != toInt(a[2]) ? 0 : 1;
        if (op == "-lt") return toInt(a[0]) < toInt(a[2]) ? 0 : 1;
        if (op == "-le") return toInt(a[0]) <= toInt(a[2]) ? 0 : 1;
        if (op == "-gt") return toInt(a[0]) > toInt(a[2]) ? 0 : 1;
        if (op == "-ge") return toInt(a[0]) >= toInt(a[2]) ? 0 : 1;
        return 2;
    }
    return 2;
}

int biTest(Executor&, const std::vector<std::string>& args) {
    return runTest(std::vector<std::string>(args.begin() + 1, args.end()));
}

int biBracket(Executor&, const std::vector<std::string>& args) {
    std::vector<std::string> a(args.begin() + 1, args.end());
    if (a.empty() || a.back() != "]") {
        std::fprintf(stderr, "ush: [: missing ']'\n");
        return 2;
    }
    a.pop_back();
    return runTest(a);
}

// --- registry -----------------------------------------------------------

const std::unordered_set<std::string>& specialBuiltinNames() {
    static const std::unordered_set<std::string> names = {
        ":", ".", "break", "continue", "eval", "exec", "exit",
        "export", "readonly", "return", "set", "shift", "unset",
    };
    return names;
}

using BuiltinFunc = int (*)(Executor&, const std::vector<std::string>&);

const std::unordered_map<std::string, BuiltinFunc>& builtinTable() {
    static const std::unordered_map<std::string, BuiltinFunc> table = {
        {":", biColon},         {".", biDot},           {"break", biBreak},
        {"continue", biContinue}, {"eval", biEval},     {"exec", biExec},
        {"exit", biExit},       {"export", biExport},   {"readonly", biReadonly},
        {"return", biReturn},   {"set", biSet},         {"shift", biShift},
        {"unset", biUnset},

        {"cd", biCd},           {"pwd", biPwd},         {"echo", biEcho},
        {"true", biTrueBuiltin}, {"false", biFalseBuiltin},
        {"test", biTest},       {"[", biBracket},
    };
    return table;
}

}  // namespace

bool isSpecialBuiltin(const std::string& name) { return specialBuiltinNames().count(name) != 0; }

bool isBuiltin(const std::string& name) { return builtinTable().count(name) != 0; }

int callBuiltin(const std::string& name, Executor& ex, const std::vector<std::string>& args) {
    auto it = builtinTable().find(name);
    if (it == builtinTable().end()) {
        std::fprintf(stderr, "ush: %s: not a builtin\n", name.c_str());
        return 127;
    }
    return it->second(ex, args);
}

}  // namespace ush
