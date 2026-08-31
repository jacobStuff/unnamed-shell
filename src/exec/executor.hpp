// Executor: walks an ast::List and actually runs it, per POSIX.1-2017
// Shell & Utilities §2.9 "Shell Command Language Execution", §2.12
// ("Shell Execution Environment"), and §2.7/§2.7.4 ("Redirection").
//
// Process model: subshells and external programs use real fork(2)/
// execve(2)/waitpid(2) - not simulated. This means a subshell's
// environment/variable isolation is free (copy-on-write gives the child
// its own copy of everything automatically; the parent is never
// touched), at the cost of an extra fork for the (rare) case of an
// external program or nested subshell that's also a pipeline stage - see
// the comment on runCommand() for the specific tradeoff made there.
//
// Lifetime: the executor stores raw pointers into the ast::List it is
// given (for function bodies, once defined - see functions_ below), so
// that ast::List must outlive the Executor. For a single script run
// (main.cpp's model: parse once, run once) this is automatic.

#pragma once

#include <unistd.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/ast.hpp"
#include "expand/expander.hpp"
#include "runtime/history.hpp"

namespace ush {

class Environment;

class Executor : public CommandRunner {
public:
    explicit Executor(Environment& env);

    // Runs a whole top-level program and returns its exit status. Catches
    // ExitSignal (from the `exit` builtin) and other control signals that
    // escaped their normal scope (break/continue/return outside any
    // loop/function - a no-op per this implementation's simplification;
    // see control_signals.hpp) and turns them into a plain exit status
    // instead of letting them propagate further.
    int runProgram(const ast::List& program);

    struct ProgramOutcome {
        int status;
        // True if `exit` was invoked while running this program. A
        // non-interactive run (runProgram) doesn't need to distinguish
        // this - it's about to terminate the process either way - but an
        // interactive REPL runs one program per input line and needs to
        // know whether to stop looping.
        bool exitRequested;
    };
    ProgramOutcome runProgramCatchingExit(const ast::List& program);

    // CommandRunner: runs `source` as a full program in a forked child
    // with stdout redirected to a pipe, and returns the captured output
    // (§2.6.3 - the caller, Expander, strips trailing newlines).
    std::string runAndCaptureStdout(const std::string& source) override;

    // Parses and runs `source` in the *current* process/environment (no
    // fork) - used by the `eval` and `.` (dot) built-ins.
    int runSourceInCurrentContext(const std::string& source);

    Environment& env() { return env_; }
    Expander& expander() { return expander_; }

    // --- entry points used by builtins.cpp ------------------------------
    // (public because builtins need them; not part of the "run a parsed
    // program" surface a caller like main.cpp would use)

    int runList(const ast::List& list);
    void eraseFunction(const std::string& name) { functions_.erase(name); }
    bool isFunction(const std::string& name) const { return functions_.count(name) != 0; }

    // Searches $PATH for a file named `name` satisfying access(2) `mode`
    // (or, if `name` contains '/', just checks that exact path), returning
    // the resolved path if found. Used by `command -v`/`type` (X_OK) and
    // `.`/dot (R_OK - a sourced file need not be executable).
    std::optional<std::string> searchPath(const std::string& name, int mode = X_OK) const;

    // Runs `args[0]` as a regular builtin or external program (bypassing
    // function lookup, but NOT special builtins - matches the `command`
    // built-in's semantics) with `args[1..]` as its arguments and no
    // redirects of its own (any on the `command` invocation itself were
    // already applied by the caller). Used by the `command` built-in.
    int runNameDirectly(const std::vector<std::string>& args);

    // --- trap (§2.14 trap, §2.11) ---------------------------------------
    //
    // Signal number 0 is used for EXIT throughout this interface,
    // matching `trap`'s own convention. Setting a trap for a real signal
    // installs an OS handler that only records the signal as pending
    // (async-signal-safe); the action itself runs later, at a safe point
    // - see servicePendingTraps() - not from within the handler.

    // Sets `action` to run when `signum` occurs. An empty `action` means
    // "ignore this condition" (SIG_IGN for a real signal).
    void setTrap(int signum, std::string action);
    // Resets `signum` to its default disposition and forgets any trap.
    void unsetTrap(int signum);
    // The currently-registered action for `signum`, or nullopt if it has
    // no trap set (default disposition). Used by `trap` with no operands
    // (list current traps).
    std::optional<std::string> trapAction(int signum) const;
    std::vector<int> trappedSignals() const;

    // Runs the action for any signal that arrived since the last call
    // (each exactly once), in an unspecified order. Called between list
    // items (so a trap fires promptly even in a tight builtin-only loop)
    // and while blocked waiting for a foreground child (see
    // waitForChild()) - never from inside the OS signal handler itself.
    void servicePendingTraps();

    // Runs the EXIT trap, if one is set, with `$?` visible to it as
    // `currentStatus` - then returns the shell's actual final exit
    // status: `currentStatus` unchanged, unless the trap action itself
    // calls `exit`, which overrides it. Called exactly once, by main.cpp,
    // right before the process actually terminates (not by
    // runProgram()/runProgramCatchingExit(), since interactive mode runs
    // those once per input line, not once per session).
    int runExitTrapIfSet(int currentStatus);

    // --- job control (§2.9.3.1 "Asynchronous Lists", XSI) ---------------
    //
    // Only ever active when the process is actually the interactive shell
    // itself, never in any of its forked descendants - see the comment on
    // isJobControlShell_. When inactive, every foreground/background
    // command runs exactly as it did before job control existed: no
    // process groups, no terminal handoff, no job table - see
    // docs/DESIGN.md for why that matters and how it's guaranteed.

    struct Job {
        int id;
        pid_t pgid;
        std::vector<pid_t> pids;
        std::vector<bool> pidDone;  // parallel to pids; job is Done once all are true
        std::string command;
        enum class State { Running, Stopped, Done } state;
        int exitStatus = 0;  // meaningful once state == Done; from pids.back() specifically
        bool notified = false;
    };

    // Called by main.cpp exactly once, only when actually interactive
    // with a controlling terminal: puts the shell in its own process
    // group and takes the terminal, so subsequently-created jobs can each
    // get their own group and have it handed to them in turn.
    void enableJobControl();
    bool jobControlActive() const { return isJobControlShell_; }

    // Reaps every job's processes that have changed state since the last
    // call (non-blocking) and prints "[n]+ Done"/"[n]+ Stopped"
    // notifications for ones not already reported - called before each
    // interactive prompt, and by the `jobs`/`wait` built-ins.
    void updateAndNotifyJobs();

    const std::vector<Job>& jobs() const { return jobs_; }
    // Job lookup for `fg`/`bg`/`wait`'s "%n" operands; nullptr if no such
    // job (already reaped, or never existed).
    Job* findJob(int id);
    // Removes `id` from the table directly, with no attempt to reap it -
    // for a caller (namely `wait %job`) that has already reaped the job's
    // processes itself via its own waitpid(2) calls, and so has nothing
    // left for updateAndNotifyJobs()'s own group-wide reaping loop to
    // observe (that syscall-level wait event was already consumed).
    // Calling updateAndNotifyJobs() instead in that situation would leave
    // the job stuck in the table forever, looking perpetually Stopped/
    // Running.
    void removeJob(int id);
    // The job most recently made current (POSIX's "current job", `%%`/
    // `%+`) - the last one added or referenced - or nullptr if none.
    Job* currentJob();

    // Sends SIGCONT to `job`'s process group and marks it Running. If
    // `foreground`, also hands it the terminal and blocks until it exits
    // or stops again (like a freshly-started foreground pipeline would);
    // otherwise leaves it running in the background. Used by `fg`/`bg`.
    int resumeJob(Job& job, bool foreground);

    // --- history (fc, HISTFILE/HISTSIZE) ---------------------------------
    //
    // Owned here (rather than by main.cpp's interactive loop alone) so
    // the `fc`/`history` built-ins can reach it like any other piece of
    // shell state. A non-interactive run never touches this - it's simply
    // never loaded, added to, or saved - so `fc`/`history` in a script see
    // an empty list, which matches "history doesn't really apply outside
    // interactive use."
    History& history() { return history_; }

private:
    Environment& env_;
    Expander expander_;

    // Function bodies, by name. Raw pointers into whatever ast::List is
    // currently being run - see the file header comment on lifetime.
    std::unordered_map<std::string, const ast::FunctionDefinition*> functions_;

    // Trap actions by condition (0 == EXIT). An empty string means
    // "ignore"; absence means "default disposition, not trapped".
    std::unordered_map<int, std::string> trapActions_;

    std::vector<Job> jobs_;
    int nextJobId_ = 1;

    History history_;

    // True only in the actual, top-level interactive shell process -
    // explicitly cleared to false in every forked child (right after
    // fork, before anything else) so job-control logic (process groups,
    // terminal ownership) never runs in a descendant, however deeply
    // nested. See enableJobControl() and docs/DESIGN.md.
    bool isJobControlShell_ = false;
    pid_t shellPgid_ = 0;

    // Set at the top of every runPipeline() call (whether or not job
    // control is active) to a best-effort reconstruction of that
    // pipeline's source text, purely for job-table/notification display.
    // A member rather than a threaded parameter: nested pipeline
    // executions (a compound command's body containing further commands)
    // naturally overwrite it with a more specific description before any
    // fork point that would actually consult it is reached - see
    // docs/DESIGN.md for why that's safe rather than a real global-state
    // hazard.
    std::string currentPipelineDescription_;

    int addJob(pid_t pgid, std::vector<pid_t> pids, std::string command, bool stopped);

    // Waits for every process in `pgid` to exit (reaping each), unless
    // job control is inactive, in which case this is a plain sequential
    // wait for each of `pids` with no group semantics at all - identical
    // to the pre-job-control behavior. When active: hands the group the
    // terminal first if `foreground` (reclaimed after), and if the group
    // stops (Ctrl-Z) rather than finishing, registers it as a Stopped
    // job (using `command` for display), reports it, and returns early
    // (128+SIGTSTP) without waiting for the rest. Otherwise returns the
    // exit status of `pids.back()` specifically (the pipeline's last
    // command - not just "whichever pid happens to finish last"), per
    // §2.9.2's exit-status rule for pipelines.
    int waitForJob(pid_t pgid, const std::vector<pid_t>& pids, bool foreground,
                   const std::string& command);

    // waitpid(2) for `pid`, but servicing pending traps (see
    // servicePendingTraps()) and resuming the wait each time one is
    // interrupted by a signal, instead of either busy-looping past it
    // (as a blind EINTR-retry would) or giving up. Returns once `pid`
    // has actually changed state.
    pid_t waitForChild(pid_t pid, int* status);

    int runAndOr(const ast::AndOr& andOr);
    int runPipeline(const ast::Pipeline& pipeline);

    // Runs exactly one Command. Safe to call from any process context
    // (the real shell process, or an already-forked pipeline/subshell
    // child): builtins/functions/compound commands always run directly
    // in whatever process calls this; external programs and subshells
    // each fork (and wait) internally as needed. This means a pipeline
    // stage that's an external program, or a subshell nested inside a
    // pipeline, forks twice (once for the pipeline stage, once more
    // here) rather than the single fork a hand-optimized shell would use
    // - a deliberate simplicity-over-efficiency tradeoff; see
    // docs/DESIGN.md.
    int runCommand(const ast::Command& cmd);

    int runSimpleCommand(const ast::SimpleCommand& cmd);
    int runCompoundCommand(const ast::CompoundCommand& cc);
    int runIfClause(const ast::IfClause& ic);
    template <typename LoopClause>
    int runLoopClause(const LoopClause& clause, bool isUntil);
    int runForClause(const ast::ForClause& fc);
    int runCaseClause(const ast::CaseClause& cc);
    int runSubshell(const ast::Subshell& sh);

    int callFunction(const ast::FunctionDefinition& fd, const std::vector<std::string>& args);

    // env_.set(), but catches ReadonlyVariableError, reports it to
    // stderr, and returns false instead of letting it escape uncaught
    // (an assignment to a readonly variable must fail that one command,
    // not crash the whole shell).
    bool trySetVar(const std::string& name, const std::string& value);

    // Runs an external program (fork + PATH search + execve + wait).
    // `envOverrides` are NAME=VALUE pairs from the command's own leading
    // assignments, added to the child's environment (§2.9.1).
    int execExternal(const std::string& name, const std::vector<std::string>& args,
                      const std::vector<ast::Redirect>& redirects,
                      const std::vector<std::string>& envOverrides);

    // Full expansion (tilde/parameter/command/arithmetic, field
    // splitting, pathname expansion) of one Word into zero or more
    // strings - what a simple command's words/arguments and a `for`
    // loop's word list use.
    std::vector<std::string> expandToFields(const Word& word);
    // Expansion without field splitting/pathname expansion, just quote
    // removal - what assignment values and case subjects/patterns use.
    std::string expandNoSplit(const Word& word);

    // Applies every redirect in `redirects` in order. `restorer`, if
    // non-null, has each affected fd saved into it first (RAII-restored
    // later) - used when running in-process (builtins/functions/compound
    // commands); pass null when already in a disposable forked child
    // (external programs, subshells), where there's nothing to restore.
    // Returns false (having already reported an error) on the first
    // failure, leaving any redirects already applied in place.
    class FdRestorer;
    bool applyRedirects(const std::vector<ast::Redirect>& redirects, FdRestorer* restorer);
    bool applyOneRedirect(const ast::Redirect& r, FdRestorer* restorer);
};

}  // namespace ush
