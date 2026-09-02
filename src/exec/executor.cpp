#include "exec/executor.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
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

// Set by trapSignalHandler() (async-signal-safe: it only touches a
// sig_atomic_t array), read and cleared by Executor::servicePendingTraps()
// at a safe point - never run the trap action from the handler itself.
// Sized generously rather than relying on platform NSIG (which differs
// between macOS and Linux); every signal number this shell deals with
// fits well within it.
constexpr int kMaxTrapSignal = 64;
volatile sig_atomic_t g_pendingSignal[kMaxTrapSignal] = {};

void trapSignalHandler(int signum) {
    if (signum >= 0 && signum < kMaxTrapSignal) g_pendingSignal[signum] = 1;
}

// Installs `handler` (a real handler, SIG_IGN, or SIG_DFL) for `signum`
// via sigaction(2) rather than signal(2), and - critically - WITHOUT
// SA_RESTART: on at least macOS (and possibly other libcs), the legacy
// signal(2) wrapper installs handlers with SA_RESTART implied, which
// would make a blocked waitpid(2) transparently resume instead of
// returning EINTR when a trapped signal arrives. That silently defeats
// Executor::waitForChild()'s whole reason for existing - the trap would
// still eventually run, but only once the foreground child happened to
// exit on its own, not promptly when the signal arrived. Verified by
// hand: the difference between a "caught" message that took 10 real
// seconds to appear (buffered stdio makes it look prompt from output
// alone) versus one that actually interrupts a `sleep 10` immediately.
void installSignalDisposition(int signum, void (*handler)(int)) {
    struct sigaction sa {};
    sigemptyset(&sa.sa_mask);  // a macro on some platforms (e.g. macOS) - no "::" prefix
    sa.sa_flags = 0;
    sa.sa_handler = handler;
    ::sigaction(signum, &sa, nullptr);
}

// A foreground child (external program, pipeline stage, or subshell)
// should be interruptible by Ctrl-C/Ctrl-\ even when the shell itself
// ignores them (see main.cpp's interactive setup) - reset to the default
// disposition right after fork(), before running anything else. Not used
// for the async ("&") fork: a background job inheriting the shell's
// ignored disposition (when interactive) is what real shells do too.
// SIGTSTP/SIGTTIN/SIGTTOU are reset for the same reason once job control
// exists (see Executor::enableJobControl()): the shell ignores them so
// Ctrl-Z at the prompt (with no foreground job) does nothing, but a
// foreground child must still be stoppable normally.
void resetForegroundSignalsInChild() {
    ::signal(SIGINT, SIG_DFL);
    ::signal(SIGQUIT, SIG_DFL);
    ::signal(SIGTSTP, SIG_DFL);
    ::signal(SIGTTIN, SIG_DFL);
    ::signal(SIGTTOU, SIG_DFL);
}

// Best-effort reconstruction of a word/command/pipeline's source text,
// used only for job-table/notification display (`jobs`, "[n]+ Stopped
// ...", the "[n] pid" line printed when backgrounding). Not a real
// unparser: a word that isn't a single unquoted literal (i.e. anything
// involving quotes or expansions) is shown as "...", and a compound
// command is shown generically - good enough to recognize a job by, not
// meant to reproduce the exact source.
std::string describeWord(const Word& w) {
    if (wordIsUnquotedLiteral(w)) return wordAsUnquotedLiteral(w);
    return "...";
}

std::string describeCommand(const ast::Command& cmd) {
    if (auto* simple = std::get_if<ast::SimpleCommand>(&cmd.value)) {
        std::string s;
        for (const auto& w : simple->words) {
            if (!s.empty()) s += ' ';
            s += describeWord(w);
        }
        return s.empty() ? "?" : s;
    }
    if (std::holds_alternative<ast::CompoundCommand>(cmd.value)) return "{ ... }";
    return "function";  // FunctionDefinition - backgrounding one directly is rare
}

std::string describePipeline(const ast::Pipeline& p) {
    std::string s;
    if (p.negated) s += "! ";
    for (std::size_t i = 0; i < p.commands.size(); ++i) {
        if (i) s += " | ";
        s += describeCommand(*p.commands[i]);
    }
    return s;
}

std::string describeAndOr(const ast::AndOr& ao) {
    std::string s = describePipeline(ao.first);
    if (!ao.rest.empty()) s += " ...";
    return s;
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

// ---------------------------------------------------------------------
// trap (§2.14 trap, §2.11)
// ---------------------------------------------------------------------

void Executor::setTrap(int signum, std::string action) {
    if (signum != 0) {
        installSignalDisposition(signum, action.empty() ? SIG_IGN : trapSignalHandler);
    }
    trapActions_[signum] = std::move(action);
}

void Executor::unsetTrap(int signum) {
    trapActions_.erase(signum);
    if (signum != 0) installSignalDisposition(signum, SIG_DFL);
}

std::optional<std::string> Executor::trapAction(int signum) const {
    auto it = trapActions_.find(signum);
    if (it == trapActions_.end()) return std::nullopt;
    return it->second;
}

std::vector<int> Executor::trappedSignals() const {
    std::vector<int> result;
    for (const auto& [signum, action] : trapActions_) result.push_back(signum);
    return result;
}

void Executor::servicePendingTraps() {
    // Copy out what needs running before running any of it: an action
    // could itself call `trap` again (mutating trapActions_), which must
    // not invalidate the iteration above it.
    std::vector<std::pair<int, std::string>> toRun;
    for (const auto& [signum, action] : trapActions_) {
        if (signum == 0 || signum >= kMaxTrapSignal || !g_pendingSignal[signum]) continue;
        g_pendingSignal[signum] = 0;
        if (!action.empty()) toRun.emplace_back(signum, action);
    }
    for (const auto& [signum, action] : toRun) {
        (void)signum;
        runSourceInCurrentContext(action);
    }
}

int Executor::runExitTrapIfSet(int currentStatus) {
    auto it = trapActions_.find(0);
    if (it == trapActions_.end() || it->second.empty()) return currentStatus;
    std::string action = std::move(it->second);
    trapActions_.erase(it);  // avoid re-entering if the action itself calls `exit`
    env_.lastExitStatus = currentStatus;  // so the trap body's own $? sees it
    try {
        runSourceInCurrentContext(action);
    } catch (const ExitSignal& e) {
        return e.status & 0xFF;  // the trap explicitly called exit: it wins
    } catch (...) {
        // A broken exit trap shouldn't prevent the shell from exiting.
    }
    return currentStatus;
}

// ---------------------------------------------------------------------
// job control (§2.9.3.1, XSI)
// ---------------------------------------------------------------------

void Executor::enableJobControl() {
    isJobControlShell_ = true;
    shellPgid_ = ::getpid();
    ::setpgid(0, 0);                        // best-effort; harmless if already a group leader
    ::tcsetpgrp(STDIN_FILENO, shellPgid_);   // best-effort; fails harmlessly with no controlling tty
    // The shell ignores these itself (so e.g. Ctrl-Z at an empty prompt
    // does nothing); a foreground child gets them reset to default - see
    // resetForegroundSignalsInChild().
    installSignalDisposition(SIGTSTP, SIG_IGN);
    installSignalDisposition(SIGTTIN, SIG_IGN);
    installSignalDisposition(SIGTTOU, SIG_IGN);
}

int Executor::addJob(pid_t pgid, std::vector<pid_t> pids, std::string command, bool stopped) {
    Job job;
    job.id = nextJobId_++;
    job.pgid = pgid;
    job.pidDone.assign(pids.size(), false);
    job.pids = std::move(pids);
    job.command = std::move(command);
    job.state = stopped ? Job::State::Stopped : Job::State::Running;
    int id = job.id;
    jobs_.push_back(std::move(job));
    return id;
}

Executor::Job* Executor::findJob(int id) {
    for (auto& j : jobs_) {
        if (j.id == id) return &j;
    }
    return nullptr;
}

Executor::Job* Executor::currentJob() { return jobs_.empty() ? nullptr : &jobs_.back(); }

void Executor::removeJob(int id) {
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (it->id == id) {
            jobs_.erase(it);
            return;
        }
    }
}

void Executor::updateAndNotifyJobs() {
    for (auto& job : jobs_) {
        if (job.state == Job::State::Done) continue;
        int wstatus = 0;
        pid_t r;
        while ((r = ::waitpid(-job.pgid, &wstatus, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
            if (WIFCONTINUED(wstatus)) {
                if (job.state != Job::State::Running) {
                    job.state = Job::State::Running;
                    job.notified = false;
                }
                continue;
            }
            if (WIFSTOPPED(wstatus)) {
                if (job.state != Job::State::Stopped) {
                    job.state = Job::State::Stopped;
                    job.notified = false;
                }
                continue;
            }
            for (std::size_t i = 0; i < job.pids.size(); ++i) {
                if (job.pids[i] == r && !job.pidDone[i]) {
                    job.pidDone[i] = true;
                    if (i + 1 == job.pids.size()) job.exitStatus = statusFromWait(wstatus);
                    break;
                }
            }
        }
        bool allDone = true;
        for (bool d : job.pidDone) {
            if (!d) {
                allDone = false;
                break;
            }
        }
        if (allDone && job.state != Job::State::Done) {
            job.state = Job::State::Done;
            job.notified = false;
        }
    }

    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (!it->notified && (it->state == Job::State::Done || it->state == Job::State::Stopped)) {
            std::string label = it->state == Job::State::Done
                                     ? (it->exitStatus == 0 ? "Done"
                                                             : "Done(" + std::to_string(it->exitStatus) + ")")
                                     : "Stopped";
            std::printf("[%d]+  %-24s%s\n", it->id, label.c_str(), it->command.c_str());
            it->notified = true;
        }
        if (it->state == Job::State::Done) {
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

int Executor::resumeJob(Job& job, bool foreground) {
    pid_t pgid = job.pgid;
    std::vector<pid_t> pids = job.pids;
    std::string command = job.command;
    int id = job.id;

    ::kill(-pgid, SIGCONT);

    if (!foreground) {
        job.state = Job::State::Running;
        job.notified = false;
        std::printf("[%d]+  %s &\n", id, command.c_str());
        return 0;
    }

    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (it->id == id) {
            jobs_.erase(it);
            break;
        }
    }
    std::printf("%s\n", command.c_str());
    return waitForJob(pgid, pids, /*foreground=*/true, command);
}

int Executor::waitForJob(pid_t pgid, const std::vector<pid_t>& pids, bool foreground,
                          const std::string& command) {
    if (!isJobControlShell_) {
        // No job control active: a plain sequential wait, identical to
        // the pre-job-control behavior (no groups, no terminal, no job
        // table entry even if something here would otherwise "stop" -
        // WUNTRACED is never passed, so a stop is invisible and the wait
        // just continues, matching a shell with no job control at all).
        int lastStatus = 1;
        for (std::size_t i = 0; i < pids.size(); ++i) {
            int wstatus = 0;
            waitForChild(pids[i], &wstatus);
            if (i + 1 == pids.size()) lastStatus = statusFromWait(wstatus);
        }
        return lastStatus;
    }

    if (foreground) ::tcsetpgrp(STDIN_FILENO, pgid);  // best-effort

    pid_t lastPid = pids.back();
    int lastPidStatus = 1;
    std::vector<bool> done(pids.size(), false);
    std::size_t remaining = pids.size();
    bool stopped = false;

    while (remaining > 0) {
        int wstatus = 0;
        pid_t r = ::waitpid(-pgid, &wstatus, WUNTRACED);
        if (r < 0) {
            if (errno == EINTR) {
                servicePendingTraps();
                continue;
            }
            break;
        }
        if (WIFSTOPPED(wstatus)) {
            stopped = true;
            break;
        }
        for (std::size_t i = 0; i < pids.size(); ++i) {
            if (pids[i] == r && !done[i]) {
                done[i] = true;
                --remaining;
                break;
            }
        }
        if (r == lastPid) lastPidStatus = statusFromWait(wstatus);
    }

    if (foreground) ::tcsetpgrp(STDIN_FILENO, shellPgid_);  // reclaim, best-effort

    if (stopped) {
        // Notification is left to updateAndNotifyJobs() (called before
        // the next prompt) rather than printed here too, so it happens
        // exactly once.
        addJob(pgid, pids, command, /*stopped=*/true);
        return 128 + SIGTSTP;
    }
    return lastPidStatus;
}

pid_t Executor::waitForChild(pid_t pid, int* status) {
    while (true) {
        pid_t r = ::waitpid(pid, status, WUNTRACED);
        if (r < 0) {
            if (errno == EINTR) {
                servicePendingTraps();
                continue;
            }
            return r;
        }
        if (WIFSTOPPED(*status)) {
            // This process isn't the one doing job-control bookkeeping
            // for `pid` (see waitForJob() for that) - it's some
            // intermediate layer created by the "double fork" for a
            // pipeline stage/subshell/external command nested inside an
            // already-established job (see runCommand()'s header
            // comment). If `pid` stopped, it's almost certainly because
            // a real, group-wide SIGTSTP (Ctrl-Z) reached this whole
            // process tree, which inherited its process group from
            // further up. The only way whichever ancestor *is* watching
            // that group (via waitForJob()'s WUNTRACED wait) can see the
            // whole subtree stop together is if every intermediate layer
            // visibly stops too - so mirror it here. SIGSTOP can't be
            // blocked or ignored, so this always takes effect
            // immediately, and resuming (SIGCONT, sent group-wide by
            // `bg`/`fg`) resumes both this process and `pid` together,
            // letting the loop below fall through to an ordinary wait.
            ::raise(SIGSTOP);
            continue;
        }
        return r;
    }
}

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
        isJobControlShell_ = false;  // never job-control in a forked descendant
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
    waitForChild(pid, &wstatus);
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
            bool jc = isJobControlShell_;
            std::string desc = describeAndOr(item.andOr);
            flushStdioBeforeFork();
            pid_t pid = ::fork();
            if (pid == 0) {
                isJobControlShell_ = false;
                if (jc) ::setpgid(0, 0);  // own process group: isolated from the shell's, so
                                          // interactive Ctrl-C/Ctrl-Z at the prompt don't touch it
                int s = 1;
                try {
                    s = runAndOr(item.andOr);
                } catch (const ExitSignal& e) {
                    s = e.status;
                }
                exitChild(s);
            }
            if (pid > 0) {
                env_.lastBackgroundPid = static_cast<int>(pid);
                if (jc) {
                    ::setpgid(pid, pid);  // also from the parent side - race-safe idiom
                    int id = addJob(pid, {pid}, desc, /*stopped=*/false);
                    std::printf("[%d] %d\n", id, static_cast<int>(pid));
                }
            }
            status = 0;  // §2.9.3: an async list's exit status is 0
            env_.lastExitStatus = status;
        } else {
            status = runAndOr(item.andOr);
        }
    }
    return status;
}

int Executor::runAndOr(const ast::AndOr& andOr) {
    // Most trap-servicing happens naturally inside waitForChild() while
    // blocked on a foreground child, but a tight loop of pure builtins
    // (e.g. "while true; do :; done") never blocks there at all - check
    // here too, once per pipeline-level unit, so a trapped signal still
    // gets serviced promptly in that case.
    servicePendingTraps();
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
    // Set unconditionally (job control active or not - cheap) so it's
    // always ready for whichever fork point ends up consulting it; see
    // the member's doc comment on why nested pipelines overwriting it
    // before that point is safe.
    currentPipelineDescription_ = describePipeline(pipeline);

    std::size_t n = pipeline.commands.size();
    int status;

    if (n == 1) {
        status = runCommand(*pipeline.commands[0]);
    } else {
        bool jc = isJobControlShell_;
        std::vector<std::array<int, 2>> pipes(n - 1);
        for (auto& p : pipes) {
            if (::pipe(p.data()) != 0) {
                std::perror("ush: pipe");
                return 1;
            }
        }
        std::vector<pid_t> pids(n);
        pid_t pgid = 0;
        for (std::size_t i = 0; i < n; ++i) {
            flushStdioBeforeFork();
            pid_t pid = ::fork();
            if (pid == 0) {
                isJobControlShell_ = false;
                resetForegroundSignalsInChild();
                if (jc) ::setpgid(0, pgid);  // pgid==0 here (first stage): become the leader
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
            if (jc) {
                if (i == 0) pgid = pid;  // first child's pid becomes the whole pipeline's pgid
                ::setpgid(pid, pgid);    // also from the parent side - race-safe idiom
            }
            pids[i] = pid;
        }
        for (auto& p : pipes) {
            ::close(p[0]);
            ::close(p[1]);
        }
        if (jc) {
            status = waitForJob(pgid, pids, /*foreground=*/true, currentPipelineDescription_);
        } else {
            int lastStatus = 1;
            for (std::size_t i = 0; i < n; ++i) {
                int wstatus = 0;
                waitForChild(pids[i], &wstatus);
                if (i + 1 == n) lastStatus = statusFromWait(wstatus);
            }
            status = lastStatus;
        }
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
    bool jc = isJobControlShell_;
    flushStdioBeforeFork();
    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("ush: fork");
        return 1;
    }
    if (pid == 0) {
        isJobControlShell_ = false;
        resetForegroundSignalsInChild();
        if (jc) ::setpgid(0, 0);
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

    if (jc) {
        ::setpgid(pid, pid);
        return waitForJob(pid, {pid}, /*foreground=*/true, currentPipelineDescription_);
    }
    int wstatus = 0;
    waitForChild(pid, &wstatus);
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
    bool jc = isJobControlShell_;
    flushStdioBeforeFork();
    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("ush: fork");
        return 1;
    }
    if (pid == 0) {
        isJobControlShell_ = false;
        resetForegroundSignalsInChild();
        if (jc) ::setpgid(0, 0);
        int status = 1;
        try {
            status = runList(sh.body);
        } catch (const ExitSignal& e) {
            status = e.status;
        }
        exitChild(status);
    }
    if (jc) {
        ::setpgid(pid, pid);
        return waitForJob(pid, {pid}, /*foreground=*/true, currentPipelineDescription_);
    }
    int wstatus = 0;
    waitForChild(pid, &wstatus);
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
