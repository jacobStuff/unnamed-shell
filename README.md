# ush

`ush` ("unnamed shell") is a from-scratch, POSIX-compatible shell, written in
C++20 directly against POSIX.1-2017 (IEEE Std 1003.1-2017), Shell &
Utilities, Chapter 2 ("Shell Command Language") - no existing shell's source
is used as a reference implementation.

See [docs/DESIGN.md](docs/DESIGN.md) for the architecture and roadmap.

## Building

Requires CMake >= 3.20 and a C++20 compiler. Tests are built by default
(fetches [Catch2](https://github.com/catchorg/Catch2) via CMake
`FetchContent`; needs network access the first time).

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

To build without tests: `cmake -S . -B build -DUSH_BUILD_TESTS=OFF`.

## Running

```bash
./build/src/ush                               # interactive, if stdin is a terminal
./build/src/ush -c 'echo hello $((2+2))'      # run a command string
./build/src/ush script.sh arg1 arg2           # run a script file
echo 'echo hi' | ./build/src/ush              # run a script from stdin (non-interactive)
./build/src/ush -i                            # force interactive mode even without a terminal
```

Interactive mode has a real prompt (`PS1`/`PS2`, both expanded like any
other word), multi-line continuation for anything left unfinished
(an open quote, an `if` without its `fi` yet, a trailing `&&`, ...), and
job control (`&` to background, `Ctrl-Z` to stop the foreground job,
`jobs`/`fg`/`bg`/`kill %n`/`wait %n` to manage it, with terminal control
handed back and forth between the shell and each foreground job), but
no arrow-key history/cursor movement beyond what the terminal's own
canonical line mode provides for free (backspace, `^U`, `^W`) - see the
roadmap.

## Status

The core pipeline is end to end and can run real scripts, interactively
or not: the lexer (§2.2/§2.3), the parser (§2.10.2 grammar → AST,
including here-documents), the shell variable/parameter environment
(§2.5), all of word expansion (§2.6 - tilde, the full `${...}` parameter
expansion grammar, command substitution, arithmetic expansion, field
splitting, pathname expansion, quote removal), the executor (§2.9/§2.7 -
fork/exec/pipelines/redirection/all compound commands/functions), a real
interactive REPL (prompts, multi-line continuation, `Ctrl-C` no longer
killing the shell), real trap/signal handling (`trap`, an `EXIT`
handler that runs exactly once, and signals that interrupt a running
foreground command promptly rather than waiting for it to finish first),
and job control (process groups, terminal-control handoff via
`tcsetpgrp`, `Ctrl-Z` to stop a foreground job, and a real job table
backing `jobs`/`fg`/`bg`/`kill %n`/`wait %n`). Built-ins: every POSIX
special built-in, plus a practical regular set - `cd`, `pwd`, `echo`,
`printf`, `read`, `test`/`[`, `true`, `false`, `command`, `type`,
`getopts`, `wait`, `umask`, `kill`, `jobs`, `fg`, `bg`. Done and tested:
935 assertions across 184 cases, including integration tests that run
the actual built binary end to end (interactively too, via `-i`,
including sending real signals - both to interrupt a running child and
to stop/continue a whole job's process group).

Still to come: a couple of niche built-ins (`hash`, `alias`/`unalias`),
real line editing/history (arrow-key recall, cursor movement),
`SIGTTIN`/`SIGTTOU`-driven suspension of background jobs that touch the
terminal, and a proper `%+`/`%-` distinction between jobs. See the
roadmap in [docs/DESIGN.md](docs/DESIGN.md#status--roadmap).

## License

TBD (likely GPL).
