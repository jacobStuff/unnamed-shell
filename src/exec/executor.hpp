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

#include <string>
#include <unordered_map>
#include <vector>

#include "ast/ast.hpp"
#include "expand/expander.hpp"

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

private:
    Environment& env_;
    Expander expander_;

    // Function bodies, by name. Raw pointers into whatever ast::List is
    // currently being run - see the file header comment on lifetime.
    std::unordered_map<std::string, const ast::FunctionDefinition*> functions_;

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
