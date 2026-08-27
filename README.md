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

The `ush` executable currently prints the token stream produced by the
lexer (there's no parser/executor yet):

```bash
echo 'echo "hi $USER" | cat' | ./build/src/ush
```

To build without tests: `cmake -S . -B build -DUSH_BUILD_TESTS=OFF`.

## Status

Early scaffolding, but word expansion (§2.6) is now functionally complete:
the lexer (§2.2/§2.3 token recognition and quoting), the parser (§2.10.2
grammar → AST, including here-documents), the shell variable/parameter
environment (§2.5), and all of word expansion - tilde, the full `${...}`
parameter expansion grammar, command substitution plumbing, arithmetic
expansion, field splitting, pathname expansion, and quote removal. Done
and unit-tested: 576 assertions across 123 cases. Still to come: the
executor (which command substitution needs a real implementation of) and
built-ins - i.e. everything needed to actually *run* a script. See the
roadmap in [docs/DESIGN.md](docs/DESIGN.md#status--roadmap).

## License

TBD (likely GPL).
