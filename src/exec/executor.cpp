#include "exec/executor.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <variant>

#include "exec/builtins.hpp"
#include "exec/control_signals.hpp"
#include "expand/arithmetic.hpp"
#include "expand/field_split.hpp"
#include "expand/pathname_expand.hpp"
#include "expand/pattern.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "runtime/environment.hpp"

namespace ush {

// ---------------------------------------------------------------------
// small process/fd helpers
// ---------------------------------------------------------------------

namespace {

int statusFromWait(int wstatus) {
    if (WIFEXITED(wstatus)) return WEXITSTATUS(wstatus);
    if (WIFSIGNALED(wstatus)) return 128 + WTERMSIG(wstatus);
    return 1;
}

[[noreturn]] void exitChild(int status) { std::exit(status & 0xFF); }

// waitpid(2), retrying on EINTR. Needed once the shell can have a signal
// handler installed at all (interactive mode ignores SIGINT/SIGQUIT so
// Ctrl-C/Ctrl-\ at the prompt don't kill the shell - see main.cpp) -
// without retrying, a blocked wait interrupted by a delivered-but-ignored
// signal would return early with `wstatus` never actually filled in by
// the child's real exit, misreporting its status.
pid_t waitpidRetry(pid_t pid, int* status, int options) {
    pid_t r;
    while ((r = ::waitpid(pid, status, options)) < 0 && errno == EINTR) {
    }
    return r;
}

// A foreground child (external program, pipeline stage, or subshell)
// should be interruptible by Ctrl-C/Ctrl-\ even when the shell itself
// ignores them (see main.cpp's interactive setup) - reset to the default
// disposition right after fork(), before running anything else. Not used
// for the async ("&") fork: a background job inheriting the shell's
// ignored disposition (when interactive) is what real shells do too.
void resetForegroundSignalsInChild() {
    ::signal(SIGINT, SIG_DFL);
    ::signal(SIGQUIT, SIG_DFL);
}

// fork(2) duplicates the whole process, including any output sitting
// unflushed in libc's stdio buffers (e.g. from a builtin's printf/fputs).
// Without this, a child that later flushes those buffers (any normal
// exit path does) re-emits whatever the parent had already buffered but
// not yet written - classic fork+stdio duplicate-output bug. Flushing
// right before every fork() call keeps each process's buffer holding
// only what it writes itself from that point on.
void flushStdioBeforeFork() { std::fflush(nullptr); }

int defaultFdFor(TokenType op) {
    switch (op) {
        case TokenType::Less:
        case TokenType::LessAnd:
        case TokenType::LessGreat:
        case TokenType::DLess:
        case TokenType::DLessDash:
            return 0;
        default:
            return 1;
    }
}

std::vector<char*> toArgv(const std::vector<std::string>& strings) {
    std::vector<char*> argv;
    argv.reserve(strings.size() + 1);
    for (const auto& s : strings) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    return argv;
}

}  // namespace

// Saves the original state of any fd that gets touched, restoring it on
// destruction - used so builtins/functions/compound commands with
// redirects (running in-process, no fork) don't permanently clobber the
// shell's own stdin/stdout/etc.
class Executor::FdRestorer {
public:
    ~FdRestorer() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            if (it->second < 0) {
                ::close(it->first);
            } else {
                ::dup2(it->second, it->first);
                ::close(it->second);
            }
        }
    }

    void save(int fd) {
        if (!savedSet_.insert(fd).second) return;
        saved_.emplace_back(fd, ::dup(fd));  // dup() returns -1 (and sets errno)
                                              // if fd wasn't open; restore()
                                              // treats that as "close it".
    }

private:
    std::vector<std::pair<int, int>> saved_;
    std::unordered_set<int> savedSet_;
};

// ---------------------------------------------------------------------
// construction / top level
// ---------------------------------------------------------------------

Executor::Executor(Environment& env) : env_(env), expander_(env, this) {}

Executor::ProgramOutcome Executor::runProgramCatchingExit(const ast::List& program) {
    try {
        return {runList(program), false};
    } catch (const ExitSignal& e) {
        return {e.status & 0xFF, true};
    } catch (const ReturnSignal& r) {
        // `return` outside any function/dot-script: treat like exit, a
        // reasonable simplification (POSIX leaves this unspecified).
        return {r.status & 0xFF, false};
    } catch (const BreakSignal&) {
        return {0, false};  // break/continue outside any loop: ignored
    } catch (const ContinueSignal&) {
        return {0, false};
    } catch (const ExpansionError& e) {
        std::fprintf(stderr, "ush: %s\n", e.what());
        return {1, false};
    } catch (const ArithError& e) {
        std::fprintf(stderr, "ush: %s\n", e.what());
        return {1, false};
    } catch (const LexError& e) {
        std::fprintf(stderr, "ush: syntax error: %s\n", e.what());
        return {2, false};
    } catch (const ParseError& e) {
        std::fprintf(stderr, "ush: syntax error: %s\n", e.what());
        return {2, false};
    }
}

int Executor::runProgram(const ast::List& program) { return runProgramCatchingExit(program).status; }

int Executor::runSourceInCurrentContext(const std::string& source) {
    ast::List program;
    try {
        Parser parser(source);
        program = parser.parseProgram();
    } catch (const LexError& e) {
        std::fprintf(stderr, "ush: syntax error: %s\n", e.what());
        return 2;
    } catch (const ParseError& e) {
        std::fprintf(stderr, "ush: syntax error: %s\n", e.what());
        return 2;
    }
    return runList(program);
}

std::string Executor::runAndCaptureStdout(const std::string& source) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        std::perror("ush: pipe");
        return "";
    }
    flushStdioBeforeFork();
    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("ush: fork");
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return "";
    }
    if (pid == 0) {
        resetForegroundSignalsInChild();
        ::close(pipefd[0]);
        ::dup2(pipefd[1], 1);
        ::close(pipefd[1]);
        int status = 1;
        try {
            Parser parser(source);
            ast::List program = parser.parseProgram();
            status = runList(program);
        } catch (const ExitSignal& e) {
            status = e.status;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ush: %s\n", e.what());
            status = 1;
        }
        exitChild(status);
    }

    ::close(pipefd[1]);
    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) output.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);

    int wstatus = 0;
    waitpidRetry(pid, &wstatus, 0);
    env_.lastExitStatus = statusFromWait(wstatus);
    return output;
}

// ---------------------------------------------------------------------
// lists / and-or / pipelines
// ---------------------------------------------------------------------

int Executor::runList(const ast::List& list) {
    int status = 0;
    for (const auto& item : list.items) {
        if (item.sep == ast::Separator::Async) {
            flushStdioBeforeFork();
            pid_t pid = ::fork();
            if (pid == 0) {
                int s = 1;
                try {
                    s = runAndOr(item.andOr);
                } catch (const ExitSignal& e) {
                    s = e.status;
                }
                exitChild(s);
            }
            if (pid > 0) env_.lastBackgroundPid = static_cast<int>(pid);
            status = 0;  // §2.9.3: an async list's exit status is 0
            env_.lastExitStatus = status;
        } else {
            status = runAndOr(item.andOr);
        }
    }
    return status;
}

int Executor::runAndOr(const ast::AndOr& andOr) {
    int status = runPipeline(andOr.first);
    env_.lastExitStatus = status;
    for (const auto& [isAnd, pipeline] : andOr.rest) {
        bool shouldRun = isAnd ? (status == 0) : (status != 0);
        if (shouldRun) {
            status = runPipeline(pipeline);
            env_.lastExitStatus = status;
        }
    }
    return status;
}

int Executor::runPipeline(const ast::Pipeline& pipeline) {
    std::size_t n = pipeline.commands.size();
    int status;

    if (n == 1) {
        status = runCommand(*pipeline.commands[0]);
    } else {
        std::vector<std::array<int, 2>> pipes(n - 1);
        for (auto& p : pipes) {
            if (::pipe(p.data()) != 0) {
                std::perror("ush: pipe");
                return 1;
            }
        }
        std::vector<pid_t> pids(n);
        for (std::size_t i = 0; i < n; ++i) {
            flushStdioBeforeFork();
            pid_t pid = ::fork();
            if (pid == 0) {
                resetForegroundSignalsInChild();
                if (i > 0) ::dup2(pipes[i - 1][0], 0);
                if (i + 1 < n) ::dup2(pipes[i][1], 1);
                for (auto& p : pipes) {
                    ::close(p[0]);
                    ::close(p[1]);
                }
                int s = 1;
                try {
                    s = runCommand(*pipeline.commands[i]);
                } catch (const ExitSignal& e) {
                    s = e.status;
                }
                exitChild(s);
            }
            pids[i] = pid;
        }
        for (auto& p : pipes) {
            ::close(p[0]);
            ::close(p[1]);
        }
        int lastStatus = 1;
        for (std::size_t i = 0; i < n; ++i) {
            int wstatus = 0;
            waitpidRetry(pids[i], &wstatus, 0);
            if (i + 1 == n) lastStatus = statusFromWait(wstatus);
        }
        status = lastStatus;
    }

    return pipeline.negated ? (status == 0 ? 1 : 0) : status;
}

// ---------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------

int Executor::runCommand(const ast::Command& cmd) {
    if (auto* simple = std::get_if<ast::SimpleCommand>(&cmd.value)) {
        return runSimpleCommand(*simple);
    }
    if (auto* compound = std::get_if<ast::CompoundCommand>(&cmd.value)) {
        FdRestorer restorer;
        if (!applyRedirects(cmd.redirects, &restorer)) return 1;
        return runCompoundCommand(*compound);
    }
    const auto& fd = std::get<ast::FunctionDefinition>(cmd.value);
    functions_[fd.name] = &fd;
    return 0;
}

bool Executor::trySetVar(const std::string& name, const std::string& value) {
    try {
        env_.set(name, value);
        return true;
    } catch (const ReadonlyVariableError& e) {
        std::fprintf(stderr, "ush: %s\n", e.what());
        return false;
    }
}

int Executor::runSimpleCommand(const ast::SimpleCommand& cmd) {
    std::vector<std::string> args;
    for (const auto& w : cmd.words) {
        for (auto& field : expandToFields(w)) args.push_back(std::move(field));
    }

    // Assignment-only / redirect-only command (no words at all): the
    // assignments persist, redirects are applied and then undone.
    if (args.empty()) {
        FdRestorer restorer;
        if (!applyRedirects(cmd.redirects, &restorer)) return 1;
        for (const auto& a : cmd.assignments) {
            if (!trySetVar(a.name, expandNoSplit(a.value))) return 1;
        }
        return 0;
    }

    const std::string& name = args[0];

    std::vector<std::string> envOverrides;
    for (const auto& a : cmd.assignments) envOverrides.push_back(a.name + "=" + expandNoSplit(a.value));

    bool isSpecial = isSpecialBuiltin(name);
    bool isFunction = !isSpecial && functions_.count(name) != 0;
    bool isRegularBuiltin = !isSpecial && !isFunction && isBuiltin(name);

    if (isSpecial) {
        // Special builtins: assignments are NOT temporary (they persist
        // after the command), and don't fork.
        for (const auto& a : cmd.assignments) {
            if (!trySetVar(a.name, expandNoSplit(a.value))) return 1;
        }
        if (name == "exec") {
            // `exec`'s redirects (and, if a command is given, the exec
            // itself) are permanent for the shell process - no restore.
            if (!applyRedirects(cmd.redirects, nullptr)) return 1;
            return callBuiltin(name, *this, args);
        }
        FdRestorer restorer;
        if (!applyRedirects(cmd.redirects, &restorer)) return 1;
        return callBuiltin(name, *this, args);
    }

    // Regular commands/builtins/functions: assignments are exported into
    // the child's environment (external) or the current environment
    // (builtin/function), and restored afterward either way (§2.9.1).
    struct SavedVar {
        std::string name;
        std::optional<std::string> value;
    };
    std::vector<SavedVar> saved;
    auto restoreVars = [&] {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            if (it->value) {
                env_.set(it->name, *it->value);
            } else {
                env_.unset(it->name);
            }
        }
    };
    for (const auto& a : cmd.assignments) {
        std::optional<std::string> prior = env_.get(a.name);
        if (!trySetVar(a.name, expandNoSplit(a.value))) {
            restoreVars();
            return 1;
        }
        saved.push_back({a.name, prior});
    }

    int status;
    if (isFunction) {
        FdRestorer restorer;
        if (!applyRedirects(cmd.redirects, &restorer)) {
            restoreVars();
            return 1;
        }
        status = callFunction(*functions_[name], std::vector<std::string>(args.begin() + 1, args.end()));
    } else if (isRegularBuiltin) {
        FdRestorer restorer;
        if (!applyRedirects(cmd.redirects, &restorer)) {
            restoreVars();
            return 1;
        }
        status = callBuiltin(name, *this, args);
    } else {
        status = execExternal(name, args, cmd.redirects, envOverrides);
    }
    restoreVars();
    return status;
}

std::optional<std::string> Executor::searchPath(const std::string& name, int mode) const {
    if (name.find('/') != std::string::npos) {
        return ::access(name.c_str(), mode) == 0 ? std::optional(name) : std::nullopt;
    }
    std::string path = env_.get("PATH").value_or("/bin:/usr/bin");
    std::size_t start = 0;
    while (true) {
        std::size_t colon = path.find(':', start);
        std::string dir =
            path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (dir.empty()) dir = ".";
        std::string candidate = dir + "/" + name;
        if (::access(candidate.c_str(), mode) == 0) return candidate;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return std::nullopt;
}

int Executor::runNameDirectly(const std::vector<std::string>& args) {
    if (args.empty()) return 1;
    if (isBuiltin(args[0]) && !isSpecialBuiltin(args[0])) return callBuiltin(args[0], *this, args);
    return execExternal(args[0], args, {}, {});
}

int Executor::execExternal(const std::string& name, const std::vector<std::string>& args,
                            const std::vector<ast::Redirect>& redirects,
                            const std::vector<std::string>& envOverrides) {
    flushStdioBeforeFork();
    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("ush: fork");
        return 1;
    }
    if (pid == 0) {
        resetForegroundSignalsInChild();
        if (!applyRedirects(redirects, nullptr)) exitChild(1);

        std::vector<std::string> envStrings = env_.exportedEnviron();
        for (const auto& o : envOverrides) envStrings.push_back(o);
        std::vector<char*> envp = toArgv(envStrings);
        std::vector<char*> argv = toArgv(args);

        if (name.find('/') != std::string::npos) {
            ::execve(name.c_str(), argv.data(), envp.data());
            std::fprintf(stderr, "ush: %s: %s\n", name.c_str(), std::strerror(errno));
            exitChild(errno == ENOENT ? 127 : 126);
        }

        std::string path = env_.get("PATH").value_or("/bin:/usr/bin");
        std::size_t start = 0;
        bool foundExecutable = false;
        while (true) {
            std::size_t colon = path.find(':', start);
            std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos
                                                                             : colon - start);
            if (dir.empty()) dir = ".";
            std::string candidate = dir + "/" + name;
            if (::access(candidate.c_str(), X_OK) == 0) {
                foundExecutable = true;
                ::execve(candidate.c_str(), argv.data(), envp.data());
                // execve only returns on failure; keep trying remaining
                // PATH entries (matches typical shell behavior for e.g.
                // a directory entry that turns out not to be executable
                // after all, or a transient error).
            }
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
        std::fprintf(stderr, "ush: %s: %s\n", name.c_str(),
                     foundExecutable ? std::strerror(errno) : "command not found");
        exitChild(foundExecutable ? 126 : 127);
    }

    int wstatus = 0;
    waitpidRetry(pid, &wstatus, 0);
    return statusFromWait(wstatus);
}

// ---------------------------------------------------------------------
// compound commands
// ---------------------------------------------------------------------

int Executor::runCompoundCommand(const ast::CompoundCommand& cc) {
    if (auto* bg = std::get_if<ast::BraceGroup>(&cc)) return runList(bg->body);
    if (auto* sh = std::get_if<ast::Subshell>(&cc)) return runSubshell(*sh);
    if (auto* fc = std::get_if<ast::ForClause>(&cc)) return runForClause(*fc);
    if (auto* cl = std::get_if<ast::CaseClause>(&cc)) return runCaseClause(*cl);
    if (auto* ic = std::get_if<ast::IfClause>(&cc)) return runIfClause(*ic);
    if (auto* wc = std::get_if<ast::WhileClause>(&cc)) return runLoopClause(*wc, false);
    return runLoopClause(std::get<ast::UntilClause>(cc), true);
}

int Executor::runIfClause(const ast::IfClause& ic) {
    for (const auto& branch : ic.branches) {
        if (runList(branch.cond) == 0) return runList(branch.body);
    }
    if (ic.elseBranch) return runList(*ic.elseBranch);
    return 0;
}

template <typename LoopClause>
int Executor::runLoopClause(const LoopClause& clause, bool isUntil) {
    int status = 0;
    while (true) {
        int condStatus = runList(clause.cond);
        bool keepGoing = isUntil ? (condStatus != 0) : (condStatus == 0);
        if (!keepGoing) break;
        try {
            status = runList(clause.body);
        } catch (BreakSignal& b) {
            if (--b.levels > 0) throw;
            break;
        } catch (ContinueSignal& c) {
            if (--c.levels > 0) throw;
            continue;
        }
    }
    return status;
}
template int Executor::runLoopClause<ast::WhileClause>(const ast::WhileClause&, bool);
template int Executor::runLoopClause<ast::UntilClause>(const ast::UntilClause&, bool);

int Executor::runForClause(const ast::ForClause& fc) {
    std::vector<std::string> items;
    if (fc.words) {
        for (const auto& w : *fc.words) {
            for (auto& field : expandToFields(w)) items.push_back(std::move(field));
        }
    } else {
        items = env_.positionalParams();
    }

    int status = 0;
    for (const auto& item : items) {
        if (!trySetVar(fc.varName, item)) return 1;
        try {
            status = runList(fc.body);
        } catch (BreakSignal& b) {
            if (--b.levels > 0) throw;
            break;
        } catch (ContinueSignal& c) {
            if (--c.levels > 0) throw;
            continue;
        }
    }
    return status;
}

int Executor::runCaseClause(const ast::CaseClause& cc) {
    std::string subject = expandNoSplit(cc.subject);
    for (const auto& item : cc.items) {
        for (const auto& pattern : item.patterns) {
            std::string patternText = buildPattern(expander_.expand(pattern)).pattern;
            if (matchesPattern(patternText, subject)) return runList(item.body);
        }
    }
    return 0;
}

int Executor::runSubshell(const ast::Subshell& sh) {
    flushStdioBeforeFork();
    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("ush: fork");
        return 1;
    }
    if (pid == 0) {
        resetForegroundSignalsInChild();
        int status = 1;
        try {
            status = runList(sh.body);
        } catch (const ExitSignal& e) {
            status = e.status;
        }
        exitChild(status);
    }
    int wstatus = 0;
    waitpidRetry(pid, &wstatus, 0);
    return statusFromWait(wstatus);
}

int Executor::callFunction(const ast::FunctionDefinition& fd, const std::vector<std::string>& args) {
    std::vector<std::string> saved = env_.positionalParams();
    env_.setPositionalParams(args);
    int status;
    try {
        status = runCompoundCommand(fd.body);
    } catch (const ReturnSignal& r) {
        status = r.status;
    } catch (...) {
        env_.setPositionalParams(std::move(saved));
        throw;
    }
    env_.setPositionalParams(std::move(saved));
    return status;
}

// ---------------------------------------------------------------------
// expansion helpers
// ---------------------------------------------------------------------

std::vector<std::string> Executor::expandToFields(const Word& word) {
    std::vector<std::string> result;
    for (auto& field : splitFields(expander_.expand(word), env_.ifsOrDefault())) {
        for (auto& s : expandPathname(field)) result.push_back(std::move(s));
    }
    return result;
}

std::string Executor::expandNoSplit(const Word& word) { return flatten(expander_.expand(word)); }

// ---------------------------------------------------------------------
// redirections
// ---------------------------------------------------------------------

bool Executor::applyRedirects(const std::vector<ast::Redirect>& redirects, FdRestorer* restorer) {
    for (const auto& r : redirects) {
        if (!applyOneRedirect(r, restorer)) return false;
    }
    return true;
}

bool Executor::applyOneRedirect(const ast::Redirect& r, FdRestorer* restorer) {
    int targetFd = r.ioNumber.value_or(defaultFdFor(r.op));
    if (restorer) restorer->save(targetFd);

    if (auto* hdp = std::get_if<std::shared_ptr<ast::HereDoc>>(&r.target)) {
        const ast::HereDoc& hd = **hdp;
        std::string body = hd.literal ? hd.body : expander_.expandHeredocBody(hd.body);
        FILE* tmp = std::tmpfile();
        if (!tmp) {
            std::perror("ush: tmpfile");
            return false;
        }
        if (!body.empty() && std::fwrite(body.data(), 1, body.size(), tmp) != body.size()) {
            std::perror("ush: write");
            std::fclose(tmp);
            return false;
        }
        std::fflush(tmp);
        std::rewind(tmp);
        int fd = ::fileno(tmp);
        bool ok = ::dup2(fd, targetFd) >= 0;
        if (!ok) std::perror("ush: dup2");
        std::fclose(tmp);
        return ok;
    }

    const Word& targetWord = std::get<Word>(r.target);
    std::string text = expandNoSplit(targetWord);

    if (r.op == TokenType::LessAnd || r.op == TokenType::GreatAnd) {
        if (text == "-") {
            ::close(targetFd);
            return true;
        }
        char* end = nullptr;
        long srcFd = std::strtol(text.c_str(), &end, 10);
        if (*end != '\0' || end == text.c_str()) {
            std::fprintf(stderr, "ush: %s: bad file descriptor\n", text.c_str());
            return false;
        }
        if (::dup2(static_cast<int>(srcFd), targetFd) < 0) {
            std::perror("ush: dup2");
            return false;
        }
        return true;
    }

    int flags = 0;
    switch (r.op) {
        case TokenType::Less: flags = O_RDONLY; break;
        case TokenType::Great:
        case TokenType::Clobber: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case TokenType::DGreat: flags = O_WRONLY | O_CREAT | O_APPEND; break;
        case TokenType::LessGreat: flags = O_RDWR | O_CREAT; break;
        default: return false;  // unreachable: every TokenType is handled above or is a HereDoc
    }
    int fd = ::open(text.c_str(), flags, 0666);
    if (fd < 0) {
        std::fprintf(stderr, "ush: %s: %s\n", text.c_str(), std::strerror(errno));
        return false;
    }
    bool ok = ::dup2(fd, targetFd) >= 0;
    if (!ok) std::perror("ush: dup2");
    ::close(fd);
    return ok;
}

}  // namespace ush
