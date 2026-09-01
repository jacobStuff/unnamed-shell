// ush entry point.
//
//   ush -i                          force interactive mode (mainly for
//                                    testing without a real terminal - see
//                                    docs/DESIGN.md)
//   ush -c 'command string' [name [arg...]]
//   ush script [arg...]
//   ush                              interactive if stdin is a terminal,
//                                    otherwise reads the whole script from
//                                    stdin
//
// Non-interactive runs (the -c/script/piped-stdin forms) read and parse
// the whole input as one program before execution begins. Interactive
// mode instead reads and parses incrementally, line by line, prompting
// with PS1/PS2 - see runInteractive() below.

#include <signal.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ast/ast.hpp"
#include "exec/control_signals.hpp"
#include "exec/executor.hpp"
#include "interactive/line_editor.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "runtime/environment.hpp"

namespace {

std::string readAll(std::istream& in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// PS1 only (§2.5.3): "The shell shall replace each instance of the
// character '!' in PS1 with the history file number of the next command
// to be typed. Escaping the '!' with another '!' (that is, "!!") shall
// place the literal character '!' in the prompt." Operates on PS1's raw
// value, before any further prompt expansion - a literal substitution on
// the variable's own text, not on whatever a nested command substitution
// happens to print.
std::string applyHistoryBang(const std::string& raw, std::size_t nextCommandNumber) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '!') {
            out += raw[i];
            continue;
        }
        if (i + 1 < raw.size() && raw[i + 1] == '!') {
            out += '!';
            ++i;  // consume both '!'s, emit one
        } else {
            out += std::to_string(nextCommandNumber);
        }
    }
    return out;
}

// Expands `raw` the same way any other word would be (tilde/parameter/
// command/arithmetic expansion + quote removal - no field splitting or
// pathname expansion, since these are all "one opaque configuration
// string" spots, not a command's argument list). Falls back to the raw
// text unexpanded if expansion itself fails, so a bad value can't wedge
// startup or the prompt entirely. Shared by PS1/PS2 (expandPrompt()
// below) and $ENV - POSIX §2.5.3 only strictly requires *parameter*
// expansion for either, but supporting the same expansion any other word
// gets is more useful and is what's actually expected (`PS1='$(pwd) $ '`
// is probably the single most common real prompt customization) - see
// docs/DESIGN.md's "Known hard corners" for this deliberate choice.
std::string expandConfigWord(ush::Executor& executor, const std::string& raw) {
    try {
        ush::Lexer lex(raw);
        ush::Word word = lex.scanWordUntilEnd();
        return flatten(executor.expander().expand(word));
    } catch (...) {
        return raw;
    }
}

// Expands a prompt variable (PS1/PS2). `applyBang`: see
// applyHistoryBang() - true only for PS1 (POSIX doesn't give PS2/PS4 the
// same "!" treatment).
std::string expandPrompt(ush::Executor& executor, ush::Environment& env, const char* varName,
                          const std::string& defaultValue, bool applyBang = false) {
    std::string raw = env.get(varName).value_or(defaultValue);
    if (applyBang) raw = applyHistoryBang(raw, executor.history().lastNumber() + 1);
    return expandConfigWord(executor, raw);
}

// $HISTFILE, or ~/.ush_history if unset - std::nullopt (no persistence
// this session) if neither HISTFILE nor HOME is available.
std::optional<std::string> historyFilePath(ush::Environment& env) {
    if (auto f = env.get("HISTFILE")) return f;
    if (auto home = env.get("HOME")) return *home + "/.ush_history";
    return std::nullopt;
}

// $HISTSIZE, defaulting to 500 if unset or not a valid non-negative
// integer (a negative or garbage HISTSIZE just falls back rather than
// wedging history entirely).
std::size_t historySizeFromEnv(ush::Environment& env) {
    auto raw = env.get("HISTSIZE");
    if (!raw) return 500;
    try {
        std::size_t consumed = 0;
        long n = std::stol(*raw, &consumed);
        if (consumed == raw->size() && n >= 0) return static_cast<std::size_t>(n);
    } catch (...) {
    }
    return 500;
}

// Runs ush interactively: prompt, read a line, accumulate lines until a
// complete command parses, run it, repeat. See docs/DESIGN.md for the
// design (how "incomplete input" is distinguished from a real syntax
// error, and why parsed programs must be kept alive for the whole
// session rather than one at a time).
int runInteractive(ush::Environment& env, ush::Executor& executor) {
    // Ctrl-C/Ctrl-\ must not kill the shell itself, whether at the prompt
    // or while waiting for a foreground job - but a foreground child
    // (external program, pipeline stage, subshell, or command
    // substitution) resets these to default right after fork() (see
    // executor.cpp's resetForegroundSignalsInChild()), so they still work
    // as expected for interrupting whatever's actually running. A
    // background ("&") job deliberately keeps the inherited ignore, like
    // real shells.
    ::signal(SIGINT, SIG_IGN);
    ::signal(SIGQUIT, SIG_IGN);

    // Puts the shell in its own process group and takes the controlling
    // terminal (both best-effort - harmless if there isn't one, e.g. when
    // testing via `-i` with piped stdin), and ignores the job-control
    // signals (SIGTSTP/SIGTTIN/SIGTTOU) for the shell itself - see
    // Executor::enableJobControl().
    executor.enableJobControl();

    // Every parsed program must be kept alive for the rest of the session:
    // Executor keeps raw pointers into it for any function defined there
    // (see executor.hpp's lifetime note), and a function defined on one
    // line must still be callable on a later one. Appending to this
    // vector never invalidates those pointers even if it reallocates -
    // they point at Command objects that live behind stable per-node
    // heap allocations (ast::Pipeline::commands is a
    // vector<unique_ptr<Command>>), not at the List/Pipeline structs
    // themselves.
    std::vector<ush::ast::List> programs;
    int status = 0;

    // Parses and runs `source` exactly as if it had been typed at the
    // interactive prompt in one go (not via the `.`/eval machinery,
    // which deliberately lets exit/return/break/continue escape to
    // whatever called it - there's no such enclosing context for a
    // startup file). `programs` is where the parsed AST lives - a
    // function defined in a startup file must stay callable for the
    // rest of the session exactly like one defined on any other line, so
    // this can't just discard it after running. A syntax error is
    // reported but doesn't stop the shell from starting, matching real
    // shells' tolerance for a broken rc file. Returns true if `exit` was
    // called from within `source` - the caller should end the session
    // immediately, exactly as if EOF had been read at the very first
    // prompt.
    auto runStartupSource = [&](const std::string& source) -> bool {
        ush::ast::List program;
        try {
            ush::Parser parser(source);
            program = parser.parseProgram();
        } catch (const ush::LexError& e) {
            std::cerr << "ush: syntax error: " << e.what() << '\n';
            return false;
        } catch (const ush::ParseError& e) {
            std::cerr << "ush: syntax error: " << e.what() << '\n';
            return false;
        }
        programs.push_back(std::move(program));
        auto outcome = executor.runProgramCatchingExit(programs.back());
        if (outcome.exitRequested) status = outcome.status;
        return outcome.exitRequested;
    };
    // As above, but for a file: silently does nothing if it doesn't
    // exist or can't be opened - a missing startup file isn't an error,
    // matching both POSIX's treatment of $ENV and real shells' `.bashrc`/
    // `.zshrc` conventions.
    auto sourceStartupFile = [&](const std::string& path) -> bool {
        std::ifstream f(path);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        return runStartupSource(ss.str());
    };

    // Startup files, run before the first prompt: POSIX's $ENV (§2.5.3 -
    // only for an interactive shell, which this always is; skipped if
    // real/effective uid or gid differ, so a setuid/setgid `sh` can't be
    // tricked into running arbitrary commands via an inherited ENV) and
    // ush's own ~/.ushrc - NOT part of POSIX, the same convention bash's
    // ~/.bashrc and zsh's ~/.zshrc follow, added because that's
    // specifically the feature asked for (see docs/DESIGN.md). Both run
    // in the shell's current environment, like `.` - not a subshell - so
    // variables/functions/aliases they set are visible for the rest of
    // the session. $ENV runs first: it's an explicit, user-chosen
    // override, so anything ~/.ushrc does afterward "wins" if the two
    // happen to overlap.
    bool exitedDuringStartup = false;
    if (::geteuid() == ::getuid() && ::getegid() == ::getgid()) {
        if (auto envPath = env.get("ENV")) {
            std::string expanded = expandConfigWord(executor, *envPath);
            if (!expanded.empty()) exitedDuringStartup = sourceStartupFile(expanded);
        }
    }
    if (!exitedDuringStartup) {
        if (auto home = env.get("HOME")) exitedDuringStartup = sourceStartupFile(*home + "/.ushrc");
    }

    // Command history (fc, HISTFILE/HISTSIZE) lives on the executor (see
    // Executor::history()) so the `fc`/`history` built-ins can reach it
    // too. Recording happens here regardless of whether the fancy raw-
    // mode editor below is actually in use - a real shell still keeps
    // history when run interactively with piped/redirected stdin (e.g.
    // `ush -i < script`, or every integration test that drives `ush -i`
    // this way), it just can't offer arrow-key recall for it without a
    // real terminal.
    std::optional<std::string> histFile = historyFilePath(env);
    executor.history().setMaxSize(historySizeFromEnv(env));
    if (histFile) executor.history().load(*histFile);

    // The raw-mode line editor (cursor movement, in-place editing,
    // arrow-key history recall) only makes sense - and only works at all,
    // termios-wise - on an actual terminal. Otherwise every read below
    // falls back to plain std::getline, exactly the previous behavior.
    bool useEditor = ush::LineEditor::isUsable();
    std::optional<ush::LineEditor> editor;
    if (useEditor) editor.emplace(executor);

    std::string buffer;
    bool continuing = false;

    // Every return path below falls through to here instead of returning
    // directly, so the EXIT trap (if any) always gets to run exactly once,
    // right before the session actually ends - see
    // Executor::runExitTrapIfSet(). `!exitedDuringStartup`: if `exit` was
    // already called from $ENV/~/.ushrc above, skip straight past the
    // loop to that same cleanup, exactly as if EOF had been read at the
    // very first prompt.
    try {
        while (!exitedDuringStartup) {
            // Background/stopped job state changes are reported here,
            // right before the next prompt - not asynchronously as they
            // happen - matching real shells' default behavior.
            if (!continuing) executor.updateAndNotifyJobs();

            std::string prompt =
                continuing ? expandPrompt(executor, env, "PS2", "> ")
                           : expandPrompt(executor, env, "PS1", ::geteuid() == 0 ? "# " : "$ ",
                                          /*applyBang=*/true);

            std::string line;
            if (useEditor) {
                auto result = editor->readLine(prompt, line);
                if (result == ush::LineEditor::Result::Interrupted) {
                    // Ctrl-C: discard whatever multi-line command was
                    // being accumulated and start fresh, like real shells.
                    buffer.clear();
                    continuing = false;
                    continue;
                }
                if (result == ush::LineEditor::Result::Eof) {
                    if (!buffer.empty()) {
                        std::cerr << "ush: syntax error: unexpected end of file\n";
                        status = 2;
                    } else {
                        status = env.lastExitStatus;
                    }
                    break;
                }
            } else {
                std::fputs(prompt.c_str(), stdout);
                std::fflush(stdout);
                if (!std::getline(std::cin, line)) {
                    std::fputc('\n', stdout);
                    if (!buffer.empty()) {
                        std::cerr << "ush: syntax error: unexpected end of file\n";
                        status = 2;
                    } else {
                        status = env.lastExitStatus;
                    }
                    break;
                }
            }
            buffer += line;
            buffer += '\n';

            ush::ast::List program;
            try {
                ush::Parser parser(buffer);
                program = parser.parseProgram();
            } catch (const ush::LexError& e) {
                if (e.incomplete()) {
                    continuing = true;
                    continue;
                }
                std::cerr << "ush: syntax error: " << e.what() << '\n';
                buffer.clear();
                continuing = false;
                continue;
            } catch (const ush::ParseError& e) {
                if (e.incomplete()) {
                    continuing = true;
                    continue;
                }
                std::cerr << "ush: syntax error: " << e.what() << '\n';
                buffer.clear();
                continuing = false;
                continue;
            }

            executor.history().add(buffer);
            buffer.clear();
            continuing = false;

            programs.push_back(std::move(program));
            auto outcome = executor.runProgramCatchingExit(programs.back());
            if (outcome.exitRequested) {
                status = outcome.status;
                break;
            }
        }
    } catch (const ush::ExitSignal& e) {
        // An INT (or other trapped-signal) handler run from inside the
        // line editor itself (sitting at the prompt, no foreground child
        // to attribute it to - see LineEditor::readByte()) called `exit`.
        status = e.status & 0xFF;
    }
    if (histFile) executor.history().save(*histFile);
    return executor.runExitTrapIfSet(status);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    bool forceInteractive = false;
    if (!args.empty() && args[0] == "-i") {
        forceInteractive = true;
        args.erase(args.begin());
    }

    std::string source;
    std::string shellName = "ush";
    std::vector<std::string> scriptArgs;
    bool readFromStdin = true;

    if (!args.empty() && args[0] == "-c") {
        if (args.size() < 2) {
            std::cerr << "ush: -c: option requires an argument\n";
            return 2;
        }
        source = args[1];
        shellName = args.size() >= 3 ? args[2] : "ush";
        for (std::size_t i = 3; i < args.size(); ++i) scriptArgs.push_back(args[i]);
        readFromStdin = false;
    } else if (!args.empty()) {
        std::ifstream f(args[0]);
        if (!f) {
            std::cerr << "ush: " << args[0] << ": No such file or directory\n";
            return 127;
        }
        source = readAll(f);
        shellName = args[0];
        for (std::size_t i = 1; i < args.size(); ++i) scriptArgs.push_back(args[i]);
        readFromStdin = false;
    }

    ush::Environment env(shellName);
    env.setPositionalParams(scriptArgs);
    ush::Executor executor(env);

    if (readFromStdin && (forceInteractive || ::isatty(STDIN_FILENO))) {
        return runInteractive(env, executor);
    }
    if (readFromStdin) source = readAll(std::cin);

    ush::ast::List program;
    try {
        ush::Parser parser(source);
        program = parser.parseProgram();
    } catch (const ush::LexError& e) {
        std::cerr << "ush: syntax error: " << e.what() << '\n';
        return 2;
    } catch (const ush::ParseError& e) {
        std::cerr << "ush: syntax error: " << e.what() << '\n';
        return 2;
    }

    int status = executor.runProgram(program);
    return executor.runExitTrapIfSet(status);
}
