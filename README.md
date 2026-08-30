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

`ush` runs non-interactively so far - no prompt/line-editing REPL yet
(see the roadmap):

```bash
./build/src/ush -c 'echo hello $((2+2))'      # run a command string
./build/src/ush script.sh arg1 arg2           # run a script file
echo 'echo hi' | ./build/src/ush              # run a script from stdin
```

## Status

The core pipeline is end to end and can run real scripts: the lexer
(§2.2/§2.3), the parser (§2.10.2 grammar → AST, including here-documents),
the shell variable/parameter environment (§2.5), all of word expansion
(§2.6 - tilde, the full `${...}` parameter expansion grammar, command
substitution, arithmetic expansion, field splitting, pathname expansion,
quote removal), and the executor (§2.9/§2.7 -
fork/exec/pipelines/redirection/all compound commands/functions). Built-
ins: every POSIX special built-in except `trap`/`times`, plus a practical
regular set - `cd`, `pwd`, `echo`, `printf`, `read`, `test`/`[`, `true`,
`false`, `command`, `type`, `getopts`, `wait`, `umask`. Done and tested:
823 assertions across 161 cases, including integration tests that run the
actual built binary end to end.

Still to come: `trap`/real signal handling, job control (`jobs`/`bg`/`fg`
- `wait` exists but is best-effort with no job table yet), a few more
niche built-ins (`kill`, `hash`, `alias`), and interactive mode (a
prompt, line editing, history). See the roadmap in
[docs/DESIGN.md](docs/DESIGN.md#status--roadmap).

## License

TBD (likely GPL).
