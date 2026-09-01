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

## Installing

```bash
brew install --HEAD --formula Formula/ush.rb   # Homebrew, builds from source
```

Prebuilt packages (a `.pkg` for macOS, `.deb`/`.rpm` for Linux, plus a
plain `.tar.gz` for either) are attached to
[GitHub Releases](https://github.com/jacobStuff/unnamed-shell/releases)
for every tagged version. See [docs/PACKAGING.md](docs/PACKAGING.md) for
how each of these is built, how to build one yourself
(`cmake --install`/`cpack`), and how to test a package locally before
trusting it.

## Running

```bash
./build/src/ush                               # interactive, if stdin is a terminal
./build/src/ush -c 'echo hello $((2+2))'      # run a command string
./build/src/ush script.sh arg1 arg2           # run a script file
echo 'echo hi' | ./build/src/ush              # run a script from stdin (non-interactive)
./build/src/ush -i                            # force interactive mode even without a terminal
```

Interactive mode has a real prompt (`PS1`/`PS2`, both expanded like any
other word, plus POSIX's one PS1-specific rule: an unescaped `!` becomes
the next command's history number, `!!` a literal `!`), multi-line
continuation for anything left unfinished
(an open quote, an `if` without its `fi` yet, a trailing `&&`, ...),
job control (`&` to background, `Ctrl-Z` to stop the foreground job,
`jobs`/`fg`/`bg`/`kill %n`/`wait %n` to manage it, with terminal control
handed back and forth between the shell and each foreground job), and a
real line editor on an actual terminal: cursor movement (arrow keys,
`Ctrl-A`/`Ctrl-E`/`Ctrl-B`/`Ctrl-F`, Home/End), in-place editing
(backspace, Delete, `Ctrl-K`/`Ctrl-U`/`Ctrl-W` kill plus `Ctrl-Y` yank,
`Ctrl-L` to redraw), and history recall (arrow keys or `Ctrl-P`/`Ctrl-N`,
backed by a real history list - `fc`, `history`, `HISTFILE`/`HISTSIZE`).
`Ctrl-C` aborts the line you're typing rather than the shell itself.
Before the first prompt, ush sources POSIX's `$ENV` file and then its
own `~/.ushrc` (not POSIX - the same convention bash's `~/.bashrc` and
zsh's `~/.zshrc` follow) in the current environment, so variables and
functions set there are available for the rest of the session; either
is silently skipped if missing.

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
job control (process groups, terminal-control handoff via `tcsetpgrp`,
`Ctrl-Z` to stop a foreground job, and a real job table backing
`jobs`/`fg`/`bg`/`kill %n`/`wait %n`), and a raw-mode line editor with
arrow-key/`Ctrl-P`/`Ctrl-N` history recall backed by a real command
history. Built-ins: every POSIX special built-in, plus a practical
regular set - `cd`, `pwd`, `echo`, `printf`, `read`, `test`/`[`, `true`,
`false`, `command`, `type`, `getopts`, `wait`, `umask`, `kill`, `jobs`,
`fg`, `bg`, `fc` (list/edit-and-reexecute/quick-reexecute a range of
history), `history`. Done and tested: 1062 assertions across 212 cases,
including integration tests that run the actual built binary end to end
(interactively too, via `-i` and, for the line editor specifically, over
a real pseudo-terminal - sending real signals, including to interrupt a
running child and to stop/continue a whole job's process group).

Still to come: a couple of niche built-ins (`hash`, `alias`/`unalias`),
completion, a real vi editing mode (`set -o vi`/`-o emacs` are no-ops),
`SIGTTIN`/`SIGTTOU`-driven suspension of background jobs that touch the
terminal, and a proper `%+`/`%-` distinction between jobs. See the
roadmap in [docs/DESIGN.md](docs/DESIGN.md#status--roadmap).

## License

The Unlicense
