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

// Expands a prompt variable (PS1/PS2) the same way any other word would
// be expanded (tilde/parameter/command/arithmetic + quote removal - no
// field splitting or pathname expansion, since a prompt is one opaque
// display string, not a command's argument list). Falls back to the raw
// value (or default) if expansion itself fails, so a bad PS1 can't wedge
// the prompt entirely.
std::string expandPrompt(ush::Executor& executor, ush::Environment& env, const char* varName,
                          const std::string& defaultValue) {
    std::string raw = env.get(varName).value_or(defaultValue);
    try {
        ush::Lexer lex(raw);
        ush::Word word = lex.scanWordUntilEnd();
        return flatten(executor.expander().expand(word));
    } catch (...) {
        return raw;
    }
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
    int status = 0;

    // Every return path below falls through to here instead of returning
    // directly, so the EXIT trap (if any) always gets to run exactly once,
    // right before the session actually ends - see
    // Executor::runExitTrapIfSet().
    try {
        while (true) {
            // Background/stopped job state changes are reported here,
            // right before the next prompt - not asynchronously as they
            // happen - matching real shells' default behavior.
            if (!continuing) executor.updateAndNotifyJobs();

            std::string prompt =
                continuing ? expandPrompt(executor, env, "PS2", "> ")
                           : expandPrompt(executor, env, "PS1", ::geteuid() == 0 ? "# " : "$ ");

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
