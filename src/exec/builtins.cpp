#include "exec/builtins.hpp"

#include <signal.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "exec/control_signals.hpp"
#include "exec/executor.hpp"
#include "expand/expander.hpp"
#include "expand/field_split.hpp"
#include "runtime/environment.hpp"
#include "runtime/history.hpp"

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

// Resolves a `fg`/`bg`/`wait` job operand: "%n" for job n, "%%"/"%+"/"%-"
// (ush doesn't distinguish "current" from "previous", unlike real shells
// with fuller job-control fidelity) or a bare empty string for the
// current (most-recently-referenced) job. Returns nullptr if it doesn't
// look like a job spec at all, or names a job that doesn't exist.
Executor::Job* resolveJobSpec(Executor& ex, const std::string& spec) {
    if (spec.empty()) return ex.currentJob();
    if (spec[0] != '%') return nullptr;
    std::string rest = spec.substr(1);
    if (rest.empty() || rest == "%" || rest == "+" || rest == "-") return ex.currentJob();
    try {
        std::size_t consumed = 0;
        int id = std::stoi(rest, &consumed);
        if (consumed == rest.size()) return ex.findJob(id);
    } catch (...) {
    }
    return nullptr;
}

// Resolves an `fc` history operand to a concrete history number: a plain
// positive integer is that history number directly; a negative integer
// is relative to the most recent command (-1 == the previous command);
// anything else is taken as a string and matched against the most recent
// history entry that starts with it (POSIX's "reference by string" form).
// Returns nullopt if it doesn't resolve to anything currently retained.
std::optional<std::size_t> resolveHistRef(const History& h, const std::string& ref) {
    if (ref.empty() || h.empty()) return std::nullopt;
    bool neg = ref[0] == '-';
    std::size_t digitsStart = neg ? 1 : 0;
    bool allDigits = digitsStart < ref.size();
    for (std::size_t k = digitsStart; k < ref.size(); ++k) {
        if (!std::isdigit(static_cast<unsigned char>(ref[k]))) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        long n;
        try {
            n = std::stol(ref);
        } catch (...) {
            return std::nullopt;
        }
        if (n < 0) {
            long resolved = static_cast<long>(h.lastNumber()) + n;
            if (resolved < static_cast<long>(h.firstNumber())) return std::nullopt;
            return static_cast<std::size_t>(resolved);
        }
        if (n == 0) return std::nullopt;
        return h.byNumber(static_cast<std::size_t>(n)) ? std::optional<std::size_t>(n) : std::nullopt;
    }
    for (std::size_t num = h.lastNumber(); num >= h.firstNumber(); --num) {
        const std::string* s = h.byNumber(num);
        if (s && s->rfind(ref, 0) == 0) return num;
        if (num == h.firstNumber()) break;  // avoid size_t underflow past 0
    }
    return std::nullopt;
}

// The history number of "the previous command" for `fc`'s default operand
// (used whenever `first`/`last` - or `fc -s`'s command reference - is
// omitted): NOT simply the newest entry in the list, because by the time
// this runs, `fc`'s own invocation has already been recorded as that
// newest entry (history recording happens before a parsed command is
// executed - see main.cpp). "The previous command" means the one before
// that, i.e. lastNumber() - 1. Returns nullopt if there isn't one (an
// empty history, or the `fc` invocation itself is the only entry).
std::optional<std::size_t> previousCommandNumber(const History& h) {
    if (h.lastNumber() <= h.firstNumber()) return std::nullopt;
    return h.lastNumber() - 1;
}

// Shared by `trap` and `kill`. Covers the signals standardized by POSIX
// and available under the same name on both macOS and Linux.
struct SignalSpec {
    int number;
    const char* name;
};
const SignalSpec kSignalTable[] = {
    {SIGHUP, "HUP"},     {SIGINT, "INT"},       {SIGQUIT, "QUIT"},   {SIGILL, "ILL"},
    {SIGTRAP, "TRAP"},   {SIGABRT, "ABRT"},     {SIGFPE, "FPE"},     {SIGKILL, "KILL"},
    {SIGBUS, "BUS"},     {SIGSEGV, "SEGV"},     {SIGSYS, "SYS"},     {SIGPIPE, "PIPE"},
    {SIGALRM, "ALRM"},   {SIGTERM, "TERM"},     {SIGUSR1, "USR1"},   {SIGUSR2, "USR2"},
    {SIGCHLD, "CHLD"},   {SIGCONT, "CONT"},     {SIGSTOP, "STOP"},   {SIGTSTP, "TSTP"},
    {SIGTTIN, "TTIN"},   {SIGTTOU, "TTOU"},     {SIGURG, "URG"},     {SIGXCPU, "XCPU"},
    {SIGXFSZ, "XFSZ"},   {SIGVTALRM, "VTALRM"}, {SIGPROF, "PROF"},   {SIGWINCH, "WINCH"},
};

// Parses a `trap`/`kill` condition: "EXIT" or "0" (trap only - kill has
// no EXIT pseudo-signal, but harmlessly accepting it there doesn't hurt),
// a bare number, or a signal name with or without the "SIG" prefix.
std::optional<int> parseSignalCondition(const std::string& s) {
    if (s == "EXIT" || s == "0") return 0;
    std::string name = s.rfind("SIG", 0) == 0 ? s.substr(3) : s;
    for (const auto& spec : kSignalTable) {
        if (name == spec.name) return spec.number;
    }
    try {
        std::size_t consumed = 0;
        int n = std::stoi(s, &consumed);
        if (consumed == s.size()) return n;
    } catch (...) {
    }
    return std::nullopt;
}

std::string signalConditionName(int signum) {
    if (signum == 0) return "EXIT";
    for (const auto& spec : kSignalTable) {
        if (spec.number == signum) return spec.name;
    }
    return std::to_string(signum);
}

// --- special builtins -------------------------------------------------

int biColon(Executor&, const std::vector<std::string>&) { return 0; }

int biExit(Executor& ex, const std::vector<std::string>& args) {
    int status = ex.env().lastExitStatus;
    if (args.size() >= 2) status = parseIntArg(args[1], 2);
    ex.env().lastExitStatus = status;  // so $? in an EXIT trap sees this, not the prior command's
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
    auto resolvedOpt = ex.searchPath(filename, R_OK);
    if (!resolvedOpt) {
        std::fprintf(stderr, "ush: .: %s: not found\n", filename.c_str());
        return 1;
    }
    std::ifstream f(*resolvedOpt);
    if (!f) {
        std::fprintf(stderr, "ush: .: %s: cannot open\n", resolvedOpt->c_str());
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
    auto resolved = ex.searchPath(name);
    bool foundExecutable = resolved.has_value();
    if (foundExecutable) ::execve(resolved->c_str(), argv.data(), envp.data());
    std::fprintf(stderr, "ush: exec: %s: %s\n", name.c_str(),
                 foundExecutable ? std::strerror(errno) : "command not found");
    // §2.14 exec: failing to execute causes a non-interactive shell to
    // exit (127 if not found, 126 if found but not executable).
    throw ExitSignal{foundExecutable ? 126 : 127};
}

// `trap [action condition...]`: with no operands, lists every currently-
// trapped condition. `action` of "-" resets to the default disposition;
// an empty string means "ignore". As a backward-compatible shorthand
// (§2.14), a single all-digit operand alone means "reset that condition
// to default", equivalent to `trap - N`.
int biTrap(Executor& ex, const std::vector<std::string>& args) {
    if (args.size() == 1) {
        for (int signum : ex.trappedSignals()) {
            auto action = ex.trapAction(signum);
            std::printf("trap -- '%s' %s\n", action->c_str(), signalConditionName(signum).c_str());
        }
        return 0;
    }

    std::size_t i = 1;
    if (args[i] == "-p") {
        std::vector<int> signums;
        if (i + 1 < args.size()) {
            for (std::size_t k = i + 1; k < args.size(); ++k) {
                auto sig = parseSignalCondition(args[k]);
                if (sig) signums.push_back(*sig);
            }
        } else {
            signums = ex.trappedSignals();
        }
        for (int signum : signums) {
            auto action = ex.trapAction(signum);
            if (action) {
                std::printf("trap -- '%s' %s\n", action->c_str(), signalConditionName(signum).c_str());
            }
        }
        return 0;
    }

    if (args.size() == 2) {
        bool allDigits = !args[1].empty();
        for (char c : args[1]) {
            if (!std::isdigit(static_cast<unsigned char>(c))) allDigits = false;
        }
        if (allDigits) {
            if (auto sig = parseSignalCondition(args[1])) {
                ex.unsetTrap(*sig);
                return 0;
            }
        }
    }

    const std::string& action = args[i];
    ++i;
    if (i >= args.size()) {
        std::fprintf(stderr, "ush: trap: usage: trap [action] condition...\n");
        return 2;
    }
    for (; i < args.size(); ++i) {
        auto sig = parseSignalCondition(args[i]);
        if (!sig) {
            std::fprintf(stderr, "ush: trap: %s: invalid condition\n", args[i].c_str());
            return 1;
        }
        if (action == "-") {
            ex.unsetTrap(*sig);
        } else {
            ex.setTrap(*sig, action);
        }
    }
    return 0;
}

int biTimes(Executor&, const std::vector<std::string>&) {
    struct tms t;
    if (::times(&t) == static_cast<clock_t>(-1)) {
        std::perror("ush: times");
        return 1;
    }
    long hz = ::sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    auto printLine = [&](clock_t userTicks, clock_t sysTicks) {
        double u = static_cast<double>(userTicks) / hz;
        double s = static_cast<double>(sysTicks) / hz;
        std::printf("%ldm%.3fs %ldm%.3fs\n", static_cast<long>(u / 60), std::fmod(u, 60.0),
                    static_cast<long>(s / 60), std::fmod(s, 60.0));
    };
    printLine(t.tms_utime, t.tms_stime);    // this shell process
    printLine(t.tms_cutime, t.tms_cstime);  // its children
    return 0;
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

// Backslash-escape processing shared by printf's format string and its
// %b conversion: \\ \a \b \f \n \r \t \v and \0NNN (1-3 octal digits).
// An unrecognized backslash sequence is left as-is (backslash kept).
std::string interpretBackslashEscapes(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i++];
            continue;
        }
        char n = s[i + 1];
        switch (n) {
            case 'n': out += '\n'; i += 2; continue;
            case 't': out += '\t'; i += 2; continue;
            case 'r': out += '\r'; i += 2; continue;
            case 'a': out += '\a'; i += 2; continue;
            case 'b': out += '\b'; i += 2; continue;
            case 'f': out += '\f'; i += 2; continue;
            case 'v': out += '\v'; i += 2; continue;
            case '\\': out += '\\'; i += 2; continue;
            default: break;
        }
        if (n >= '0' && n <= '7') {
            std::size_t j = i + 1;
            int value = 0, count = 0;
            while (j < s.size() && s[j] >= '0' && s[j] <= '7' && count < 3) {
                value = value * 8 + (s[j] - '0');
                ++j;
                ++count;
            }
            out += static_cast<char>(value);
            i = j;
            continue;
        }
        out += s[i++];  // unrecognized escape: keep the backslash literally
    }
    return out;
}

// `printf format [args...]`: a practical subset of §2.14's printf -
// %s %d %i %u %o %x %X %c %b %%, with flags/width/precision delegated to
// the real libc snprintf (parsed out of the format string, then combined
// with the right argument type) rather than reimplemented. Missing
// arguments are treated as "" (for %s/%b) or 0 (numeric); the format is
// reused for as many cycles as needed to consume every argument, per
// spec. Not implemented: %e/%f/%g (floating point - POSIX shells rarely
// need it and none of ush's own arithmetic is floating point anyway).
int biPrintf(Executor&, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::fprintf(stderr, "ush: printf: usage: printf format [argument...]\n");
        return 2;
    }
    std::string format = interpretBackslashEscapes(args[1]);
    std::vector<std::string> printfArgs(args.begin() + 2, args.end());
    std::size_t argIdx = 0;
    bool anyArgConsumed = false;
    auto nextArg = [&]() -> std::string {
        anyArgConsumed = true;
        return argIdx < printfArgs.size() ? printfArgs[argIdx++] : std::string();
    };

    constexpr std::size_t kBufSize = 4096;
    char buf[kBufSize];

    do {
        std::string out;
        for (std::size_t i = 0; i < format.size();) {
            char c = format[i];
            if (c != '%') {
                out += c;
                ++i;
                continue;
            }
            if (i + 1 < format.size() && format[i + 1] == '%') {
                out += '%';
                i += 2;
                continue;
            }
            std::size_t j = i + 1;
            while (j < format.size() && std::strchr("-+ #0", format[j])) ++j;
            while (j < format.size() && std::isdigit(static_cast<unsigned char>(format[j]))) ++j;
            if (j < format.size() && format[j] == '.') {
                ++j;
                while (j < format.size() && std::isdigit(static_cast<unsigned char>(format[j]))) ++j;
            }
            if (j >= format.size()) {
                out += format.substr(i);  // malformed trailing '%...' - dump literally
                break;
            }
            char conv = format[j];
            std::string spec = format.substr(i, j - i + 1);  // e.g. "%-5d"
            std::string body = spec.substr(0, spec.size() - 1);

            switch (conv) {
                case 's':
                    std::snprintf(buf, kBufSize, (body + "s").c_str(), nextArg().c_str());
                    out += buf;
                    break;
                case 'b':
                    out += interpretBackslashEscapes(nextArg());
                    break;
                case 'd':
                case 'i': {
                    long long v = 0;
                    try {
                        v = std::stoll(nextArg(), nullptr, 0);
                    } catch (...) {
                    }
                    std::snprintf(buf, kBufSize, (body + "lld").c_str(), v);
                    out += buf;
                    break;
                }
                case 'o':
                case 'u':
                case 'x':
                case 'X': {
                    unsigned long long v = 0;
                    try {
                        v = std::stoull(nextArg(), nullptr, 0);
                    } catch (...) {
                    }
                    std::snprintf(buf, kBufSize, (body + "ll" + conv).c_str(), v);
                    out += buf;
                    break;
                }
                case 'c': {
                    std::string a = nextArg();
                    std::snprintf(buf, kBufSize, spec.c_str(), a.empty() ? '\0' : a[0]);
                    out += buf;
                    break;
                }
                default:
                    out += spec;  // unknown conversion: print literally
                    break;
            }
            i = j + 1;
        }
        std::fputs(out.c_str(), stdout);
    } while (anyArgConsumed && argIdx < printfArgs.size());

    return 0;
}

// `read [-r] name...`: reads one line from stdin, splits it on IFS into
// the named variables (the last one absorbs any extra fields - joined
// back with a single space rather than preserving the original
// separators exactly, a documented simplification). Without -r, a
// backslash-newline continues the line and any other backslash-char pair
// is unescaped (backslash dropped, character kept literally) while
// reading, matching §2.14's default (non-raw) behavior.
int biRead(Executor& ex, const std::vector<std::string>& args) {
    bool raw = false;
    std::size_t i = 1;
    if (i < args.size() && args[i] == "-r") {
        raw = true;
        ++i;
    }
    if (i >= args.size()) {
        std::fprintf(stderr, "ush: read: usage: read [-r] name...\n");
        return 2;
    }
    std::vector<std::string> names(args.begin() + static_cast<long>(i), args.end());

    std::string line;
    bool sawAnyInput = false;
    while (true) {
        int c = std::fgetc(stdin);
        if (c == EOF) break;
        sawAnyInput = true;
        if (c == '\n') break;
        if (!raw && c == '\\') {
            int n = std::fgetc(stdin);
            if (n == '\n') continue;  // line continuation: both chars vanish
            if (n == EOF) {
                line += '\\';
                break;
            }
            line += static_cast<char>(n);
            continue;
        }
        line += static_cast<char>(c);
    }

    std::string ifs = ex.env().ifsOrDefault();
    ExpandedWord raw_ew{{line, false, false}};
    std::vector<std::string> fields;
    for (auto& f : splitFields(raw_ew, ifs)) fields.push_back(flatten(f));

    for (std::size_t k = 0; k < names.size(); ++k) {
        if (k + 1 == names.size() && fields.size() > names.size()) {
            std::string rest;
            for (std::size_t m = k; m < fields.size(); ++m) {
                if (m > k) rest += ' ';
                rest += fields[m];
            }
            ex.env().set(names[k], rest);
        } else {
            ex.env().set(names[k], k < fields.size() ? fields[k] : std::string());
        }
    }
    return sawAnyInput ? 0 : 1;
}

int biCommand(Executor& ex, const std::vector<std::string>& args) {
    std::size_t i = 1;
    bool verbose = false;
    if (i < args.size() && (args[i] == "-v" || args[i] == "-V")) {
        verbose = true;
        ++i;
    }
    if (i >= args.size()) return 1;
    if (verbose) {
        const std::string& name = args[i];
        if (isBuiltin(name)) {
            std::printf("%s\n", name.c_str());
            return 0;
        }
        if (auto resolved = ex.searchPath(name)) {
            std::printf("%s\n", resolved->c_str());
            return 0;
        }
        return 1;
    }
    return ex.runNameDirectly(std::vector<std::string>(args.begin() + static_cast<long>(i), args.end()));
}

int biType(Executor& ex, const std::vector<std::string>& args) {
    int status = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& name = args[i];
        if (isSpecialBuiltin(name)) {
            std::printf("%s is a special shell builtin\n", name.c_str());
        } else if (ex.isFunction(name)) {
            std::printf("%s is a function\n", name.c_str());
        } else if (isBuiltin(name)) {
            std::printf("%s is a shell builtin\n", name.c_str());
        } else if (auto resolved = ex.searchPath(name)) {
            std::printf("%s is %s\n", name.c_str(), resolved->c_str());
        } else {
            std::fprintf(stderr, "ush: type: %s: not found\n", name.c_str());
            status = 1;
        }
    }
    return status;
}

// `getopts optstring name [arg...]`: standard option parsing, including
// bundled short options (`-abc`). POSIX doesn't name a variable for the
// "which character within the current arg" position, since it's meant to
// be shell-internal state; ush keeps it in an internal environment
// variable (alongside the standard OPTIND/OPTARG) rather than inventing a
// separate mechanism.
int biGetopts(Executor& ex, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::fprintf(stderr, "ush: getopts: usage: getopts optstring name [arg...]\n");
        return 2;
    }
    const std::string& optstring = args[1];
    const std::string& varname = args[2];
    std::vector<std::string> operands = args.size() > 3
                                             ? std::vector<std::string>(args.begin() + 3, args.end())
                                             : ex.env().positionalParams();

    bool silent = !optstring.empty() && optstring[0] == ':';
    std::string opts = silent ? optstring.substr(1) : optstring;

    auto getIntVar = [&](const char* name) {
        if (auto v = ex.env().get(name)) {
            try {
                return std::max(1, std::stoi(*v));
            } catch (...) {
            }
        }
        return 1;
    };
    int optind = getIntVar("OPTIND");
    int charIdx = getIntVar("_ush_getopts_charidx");

    auto finish = [&](int newOptind) {
        ex.env().set("OPTIND", std::to_string(newOptind));
        ex.env().set("_ush_getopts_charidx", "1");
    };

    std::size_t idx = static_cast<std::size_t>(optind - 1);
    if (idx >= operands.size()) {
        finish(optind);
        return 1;
    }
    const std::string& cur = operands[idx];
    if (cur == "--") {
        finish(optind + 1);
        return 1;
    }
    if (cur.size() < 2 || cur[0] != '-') {
        finish(optind);
        return 1;
    }

    char optChar = cur[static_cast<std::size_t>(charIdx)];
    bool lastCharOfArg = static_cast<std::size_t>(charIdx) + 1 >= cur.size();
    auto specPos = opts.find(optChar);

    if (specPos == std::string::npos) {
        ex.env().set(varname, "?");
        if (silent) {
            ex.env().set("OPTARG", std::string(1, optChar));
        } else {
            std::fprintf(stderr, "ush: illegal option -- %c\n", optChar);
            ex.env().unset("OPTARG");
        }
        if (lastCharOfArg) {
            finish(optind + 1);
        } else {
            ex.env().set("OPTIND", std::to_string(optind));
            ex.env().set("_ush_getopts_charidx", std::to_string(charIdx + 1));
        }
        return 0;
    }

    bool takesArg = specPos + 1 < opts.size() && opts[specPos + 1] == ':';
    if (!takesArg) {
        ex.env().set(varname, std::string(1, optChar));
        ex.env().unset("OPTARG");
        if (lastCharOfArg) {
            finish(optind + 1);
        } else {
            ex.env().set("OPTIND", std::to_string(optind));
            ex.env().set("_ush_getopts_charidx", std::to_string(charIdx + 1));
        }
        return 0;
    }

    if (!lastCharOfArg) {
        ex.env().set(varname, std::string(1, optChar));
        ex.env().set("OPTARG", cur.substr(static_cast<std::size_t>(charIdx) + 1));
        finish(optind + 1);
        return 0;
    }
    if (idx + 1 < operands.size()) {
        ex.env().set(varname, std::string(1, optChar));
        ex.env().set("OPTARG", operands[idx + 1]);
        finish(optind + 2);
        return 0;
    }
    // missing required argument
    ex.env().set(varname, silent ? ":" : "?");
    if (silent) {
        ex.env().set("OPTARG", std::string(1, optChar));
    } else {
        std::fprintf(stderr, "ush: option requires an argument -- %c\n", optChar);
        ex.env().unset("OPTARG");
    }
    finish(optind + 1);
    return 0;
}

// `wait [pid]`: with no argument, reaps every child of this process
// (there's no job table to wait on specific *known* background jobs
// otherwise); with a pid, waits for exactly that one and reports its
// exit status. Best-effort, since ush doesn't track a job list - a
// documented simplification (see docs/DESIGN.md).
int biWait(Executor& ex, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        int status = 0;
        while (true) {
            int wstatus = 0;
            pid_t r = ::waitpid(-1, &wstatus, 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;  // ECHILD: no more children
            }
            status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);
        }
        return status;
    }
    if (!args[1].empty() && args[1][0] == '%') {
        Executor::Job* job = resolveJobSpec(ex, args[1]);
        if (!job) {
            std::fprintf(stderr, "ush: wait: %s: no such job\n", args[1].c_str());
            return 127;
        }
        int id = job->id;
        std::vector<pid_t> pids = job->pids;  // copy: this loop reaps directly, bypassing the
                                               // job table's own bookkeeping (see removeJob())
        int status = 0;
        for (pid_t p : pids) {
            int wstatus = 0;
            pid_t r;
            while ((r = ::waitpid(p, &wstatus, 0)) < 0 && errno == EINTR) {
            }
            if (r == p) status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);
        }
        // Reaped directly above, so there's nothing left for
        // updateAndNotifyJobs()'s own waitpid(2) calls to see - erase the
        // entry directly instead, or it would look stuck forever.
        ex.removeJob(id);
        return status;
    }

    pid_t pid = static_cast<pid_t>(parseIntArg(args[1], -1));
    if (pid < 0) {
        std::fprintf(stderr, "ush: wait: %s: not a pid\n", args[1].c_str());
        return 2;
    }
    int wstatus = 0;
    pid_t r;
    while ((r = ::waitpid(pid, &wstatus, 0)) < 0 && errno == EINTR) {
    }
    if (r < 0) {
        std::fprintf(stderr, "ush: wait: %s: %s\n", args[1].c_str(), std::strerror(errno));
        return 127;
    }
    return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);
}

int biUmask(Executor&, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        mode_t cur = ::umask(0);
        ::umask(cur);
        std::printf("%04o\n", cur);
        return 0;
    }
    try {
        std::size_t consumed = 0;
        int mode = std::stoi(args[1], &consumed, 8);
        if (consumed != args[1].size() || mode < 0 || mode > 0777) throw std::invalid_argument(args[1]);
        ::umask(static_cast<mode_t>(mode));
        return 0;
    } catch (...) {
        std::fprintf(stderr, "ush: umask: %s: invalid mode\n", args[1].c_str());
        return 1;
    }
}

// `kill [-signal] pid...` / `kill -l`: sends a signal (default TERM) to
// each pid (or, given a "%n" operand, to that job's whole process group -
// only meaningful when job control is active), or lists known signal
// names.
int biKill(Executor& ex, const std::vector<std::string>& args) {
    std::size_t i = 1;
    if (i < args.size() && args[i] == "-l") {
        for (const auto& spec : kSignalTable) std::printf("%s ", spec.name);
        std::printf("\n");
        return 0;
    }

    int signum = SIGTERM;
    if (i < args.size() && args[i].size() > 1 && args[i][0] == '-') {
        auto sig = parseSignalCondition(args[i].substr(1));
        if (!sig || *sig == 0) {
            std::fprintf(stderr, "ush: kill: %s: invalid signal\n", args[i].c_str());
            return 1;
        }
        signum = *sig;
        ++i;
    }
    if (i >= args.size()) {
        std::fprintf(stderr, "ush: kill: usage: kill [-signal] pid...\n");
        return 2;
    }

    int status = 0;
    for (; i < args.size(); ++i) {
        pid_t target;
        if (!args[i].empty() && args[i][0] == '%') {
            Executor::Job* job = resolveJobSpec(ex, args[i]);
            if (!job) {
                std::fprintf(stderr, "ush: kill: %s: no such job\n", args[i].c_str());
                status = 1;
                continue;
            }
            target = -job->pgid;  // negative: signal the whole process group
        } else {
            std::size_t consumed = 0;
            long pidVal = 0;
            try {
                pidVal = std::stol(args[i], &consumed);
            } catch (...) {
            }
            if (consumed != args[i].size()) {
                std::fprintf(stderr, "ush: kill: %s: arguments must be process IDs\n", args[i].c_str());
                status = 1;
                continue;
            }
            target = static_cast<pid_t>(pidVal);
        }
        if (::kill(target, signum) != 0) {
            std::fprintf(stderr, "ush: kill: (%s): %s\n", args[i].c_str(), std::strerror(errno));
            status = 1;
        }
    }
    return status;
}

// `jobs [-l]`: lists background/stopped jobs. `-l` also shows each job's
// process group id.
int biJobs(Executor& ex, const std::vector<std::string>& args) {
    ex.updateAndNotifyJobs();
    bool longFormat = args.size() > 1 && args[1] == "-l";
    for (const auto& job : ex.jobs()) {
        const char* stateStr = job.state == Executor::Job::State::Running ? "Running" : "Stopped";
        if (longFormat) {
            std::printf("[%d]  %d  %-8s%s\n", job.id, static_cast<int>(job.pgid), stateStr,
                        job.command.c_str());
        } else {
            std::printf("[%d]+  %-8s%s\n", job.id, stateStr, job.command.c_str());
        }
    }
    return 0;
}

// `fg [%job]`: resumes a stopped or backgrounded job (SIGCONT) in the
// foreground, hands it the terminal, and waits for it.
int biFg(Executor& ex, const std::vector<std::string>& args) {
    if (!ex.jobControlActive()) {
        std::fprintf(stderr, "ush: fg: no job control\n");
        return 1;
    }
    ex.updateAndNotifyJobs();
    Executor::Job* job = resolveJobSpec(ex, args.size() > 1 ? args[1] : std::string());
    if (!job) {
        std::fprintf(stderr, "ush: fg: %s: no such job\n", args.size() > 1 ? args[1].c_str() : "current");
        return 1;
    }
    return ex.resumeJob(*job, /*foreground=*/true);
}

// `bg [%job]`: resumes a stopped job (SIGCONT) in the background.
int biBg(Executor& ex, const std::vector<std::string>& args) {
    if (!ex.jobControlActive()) {
        std::fprintf(stderr, "ush: bg: no job control\n");
        return 1;
    }
    ex.updateAndNotifyJobs();
    Executor::Job* job = resolveJobSpec(ex, args.size() > 1 ? args[1] : std::string());
    if (!job) {
        std::fprintf(stderr, "ush: bg: %s: no such job\n", args.size() > 1 ? args[1].c_str() : "current");
        return 1;
    }
    return ex.resumeJob(*job, /*foreground=*/false);
}

// `history [-c] [n]`: not itself a POSIX built-in (the standard interface
// is `fc -l`, below), but a widely-expected convenience now that ush has
// a real history list to show. With no operands, lists every retained
// entry; `-c` clears the list; a bare `n` lists only the last `n`.
int biHistory(Executor& ex, const std::vector<std::string>& args) {
    History& hist = ex.history();
    if (args.size() > 1 && args[1] == "-c") {
        hist.clear();
        return 0;
    }
    std::size_t from = hist.firstNumber();
    if (args.size() > 1) {
        int n = parseIntArg(args[1], -1);
        if (n < 0) {
            std::fprintf(stderr, "ush: history: %s: numeric argument required\n", args[1].c_str());
            return 2;
        }
        std::size_t want = static_cast<std::size_t>(n);
        if (want < hist.size()) from = hist.lastNumber() - want + 1;
    }
    for (std::size_t num = from; num <= hist.lastNumber(); ++num) {
        const std::string* s = hist.byNumber(num);
        if (s) std::printf("%5zu  %s\n", num, s->c_str());
    }
    return 0;
}

// `fc [-r] [-e editor] [first [last]]` (edit a range of history commands
// and re-execute the result), `fc -l [-nr] [first [last]]` (list instead
// of editing), `fc -s [old=new] [first]` (re-execute directly, with an
// optional textual substitution, no editor involved) - POSIX's interface
// to the history list. `first`/`last` (and `-s`'s `first`) each name a
// history entry: a plain number, a negative number (relative to the most
// recent command), or a string (the most recent command starting with
// it) - see resolveHistRef().
int biFc(Executor& ex, const std::vector<std::string>& args) {
    bool listMode = false, suppressNumbers = false, reverse = false, subMode = false;
    std::string editorOverride;
    std::size_t i = 1;
    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.empty() || a[0] != '-' || a == "-") break;
        if (a == "-l") {
            listMode = true;
        } else if (a == "-n") {
            suppressNumbers = true;
        } else if (a == "-r") {
            reverse = true;
        } else if (a == "-s") {
            subMode = true;
        } else if (a == "-e") {
            if (++i >= args.size()) {
                std::fprintf(stderr, "ush: fc: -e: option requires an argument\n");
                return 2;
            }
            editorOverride = args[i];
        } else {
            std::fprintf(stderr, "ush: fc: %s: invalid option\n", a.c_str());
            return 2;
        }
    }
    std::vector<std::string> operands(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
    History& hist = ex.history();

    if (subMode) {
        std::string oldStr, newStr, ref;
        bool hasSub = false;
        for (const auto& op : operands) {
            auto eq = op.find('=');
            if (!hasSub && eq != std::string::npos) {
                oldStr = op.substr(0, eq);
                newStr = op.substr(eq + 1);
                hasSub = true;
            } else {
                ref = op;
            }
        }
        std::optional<std::size_t> num = ref.empty() ? previousCommandNumber(hist) : resolveHistRef(hist, ref);
        if (!num) {
            std::fprintf(stderr, "ush: fc: %s: no such command\n", ref.empty() ? "(none)" : ref.c_str());
            return 1;
        }
        std::string cmd = *hist.byNumber(*num);
        if (hasSub && !oldStr.empty()) {
            auto pos = cmd.find(oldStr);
            if (pos != std::string::npos) cmd = cmd.substr(0, pos) + newStr + cmd.substr(pos + oldStr.size());
        }
        std::printf("%s\n", cmd.c_str());
        std::fflush(stdout);
        hist.add(cmd);
        return ex.runSourceInCurrentContext(cmd);
    }

    std::size_t firstNum, lastNum;
    bool rangeGivenDescending = false;
    if (operands.empty()) {
        if (hist.empty()) {
            std::fprintf(stderr, "ush: fc: history is empty\n");
            return 1;
        }
        if (listMode) {
            firstNum = hist.size() > 16 ? hist.lastNumber() - 15 : hist.firstNumber();
            lastNum = hist.lastNumber();
        } else {
            auto prev = previousCommandNumber(hist);
            if (!prev) {
                std::fprintf(stderr, "ush: fc: no previous command\n");
                return 1;
            }
            firstNum = lastNum = *prev;
        }
    } else if (operands.size() == 1) {
        auto n = resolveHistRef(hist, operands[0]);
        if (!n) {
            std::fprintf(stderr, "ush: fc: %s: no such command\n", operands[0].c_str());
            return 1;
        }
        firstNum = *n;
        lastNum = listMode ? hist.lastNumber() : *n;
    } else {
        auto n1 = resolveHistRef(hist, operands[0]);
        auto n2 = resolveHistRef(hist, operands[1]);
        if (!n1 || !n2) {
            std::fprintf(stderr, "ush: fc: %s: no such command\n",
                         (!n1 ? operands[0] : operands[1]).c_str());
            return 1;
        }
        rangeGivenDescending = *n1 > *n2;
        firstNum = std::min(*n1, *n2);
        lastNum = std::max(*n1, *n2);
    }
    bool descending = reverse != rangeGivenDescending;

    std::vector<std::size_t> nums;
    for (std::size_t n = firstNum; n <= lastNum; ++n) nums.push_back(n);
    if (descending) std::reverse(nums.begin(), nums.end());

    if (listMode) {
        for (std::size_t n : nums) {
            const std::string* s = hist.byNumber(n);
            if (!s) continue;
            if (suppressNumbers) {
                std::printf("%s\n", s->c_str());
            } else {
                std::printf("%5zu  %s\n", n, s->c_str());
            }
        }
        return 0;
    }

    // Edit mode: write the selected commands to a temp file, run an
    // editor on it, then execute whatever comes back.
    std::string editor = editorOverride;  // POSIX's default editor is
                                           // unspecified - "vi" below is a
                                           // documented, deliberate choice
    if (editor.empty()) {
        if (auto fcedit = ex.env().get("FCEDIT")) {
            editor = *fcedit;
        } else if (auto e = ex.env().get("EDITOR")) {
            editor = *e;
        } else {
            editor = "vi";
        }
    }
    char tmpl[] = "/tmp/ush_fc_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        std::fprintf(stderr, "ush: fc: %s\n", std::strerror(errno));
        return 1;
    }
    {
        std::string content;
        for (std::size_t n : nums) {
            const std::string* s = hist.byNumber(n);
            if (s) {
                content += *s;
                content += '\n';
            }
        }
        ::write(fd, content.data(), content.size());
        ::close(fd);
    }

    pid_t pid = ::fork();
    if (pid == 0) {
        std::vector<char*> argv = {const_cast<char*>(editor.c_str()), tmpl, nullptr};
        ::execvp(editor.c_str(), argv.data());
        _exit(127);
    }
    int edited = 1;
    if (pid > 0) {
        int wstatus = 0;
        pid_t r;
        while ((r = ::waitpid(pid, &wstatus, 0)) < 0 && errno == EINTR) {
        }
        edited = (r == pid && WIFEXITED(wstatus)) ? WEXITSTATUS(wstatus) : 1;
    }

    std::ifstream in(tmpl);
    std::ostringstream ss;
    if (in) ss << in.rdbuf();
    ::unlink(tmpl);

    if (edited != 0) {
        std::fprintf(stderr, "ush: fc: editor exited nonzero; not executing\n");
        return edited;
    }
    std::string finalSource = ss.str();
    std::printf("%s", finalSource.c_str());
    std::fflush(stdout);
    hist.add(finalSource);
    return ex.runSourceInCurrentContext(finalSource);
}

// --- registry -----------------------------------------------------------

const std::unordered_set<std::string>& specialBuiltinNames() {
    static const std::unordered_set<std::string> names = {
        ":", ".", "break", "continue", "eval", "exec", "exit",
        "export", "readonly", "return", "set", "shift", "trap", "times", "unset",
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
        {"trap", biTrap},       {"times", biTimes},     {"unset", biUnset},

        {"cd", biCd},           {"pwd", biPwd},         {"echo", biEcho},
        {"true", biTrueBuiltin}, {"false", biFalseBuiltin},
        {"test", biTest},       {"[", biBracket},

        {"printf", biPrintf},   {"read", biRead},       {"command", biCommand},
        {"type", biType},       {"getopts", biGetopts}, {"wait", biWait},
        {"umask", biUmask},     {"kill", biKill},
        {"jobs", biJobs},       {"fg", biFg},           {"bg", biBg},
        {"fc", biFc},           {"history", biHistory},
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
