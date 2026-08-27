# ush design notes

`ush` ("unnamed shell") is a from-scratch implementation of the POSIX Shell
Command Language, built directly against **POSIX.1-2017 (IEEE Std 1003.1-2017),
Volume: Shell & Utilities, Chapter 2 ("Shell Command Language")**. It does not
copy or crib from any existing shell's source (dash, bash, ksh, ...); where
the spec is ambiguous or underspecified, we pick a documented, tested
behavior and note the choice here.

Spec references below use section numbers from that chapter, e.g. "2.10.2"
means Shell & Utilities §2.10.2 "Shell Grammar Rules".

## Goals

- Broad POSIX conformance targeted from the start, not bolted on later:
  the architecture is grammar-driven (real tokenizer → real parser → real
  AST → separate expansion pass → executor), not a quick hack that only
  handles simple commands.
- C++20, built with CMake, unit-tested with Catch2 at the lexer/parser/
  expansion level, plus integration tests that run the built `ush` binary
  against shell scripts and check output/exit status.
- Non-goals (for now): full interactive line editing (readline-alike),
  full job control (XSI), locale/i18n correctness, csh/bash-isms
  (`[[`, arrays, `function` keyword, `;&`, process substitution, etc).
  These may come later but are explicitly *not* part of "broad POSIX".

## Pipeline

```
source text
   │
   ▼
 Lexer            (src/lexer)   §2.3 Token Recognition, §2.2 Quoting
   │  produces Token stream: WORD (with structured Word/WordPart tree),
   │  operators, NEWLINE, IO_NUMBER
   ▼
 Parser           (src/parser)  §2.10.2 Shell Grammar Rules
   │  recursive-descent / operator-precedence over the POSIX grammar,
   │  recognizes reserved words contextually, builds an AST
   ▼
 AST              (src/ast)
   │  lists, pipelines, simple commands, compound commands
   │  (if/for/while/until/case/subshell/brace-group/function-def),
   │  redirections, here-documents
   ▼
 Expansion        (src/expand)  §2.6 Word Expansions
   │  tilde → parameter → command substitution → arithmetic
   │  → field splitting → pathname expansion (glob) → quote removal
   ▼
 Exec             (src/exec)    §2.9 Shell Command Language execution rules
   │  fork/exec, pipelines, redirections, control flow, exit status,
   │  traps, special/positional parameters
   ▼
 Builtins         (src/builtins) §2.14 Special Built-In Utilities + regular builtins
```

Each stage is a separate library target so it can be unit-tested in
isolation (e.g. the lexer doesn't need a parser to test quoting rules).

## Word representation (deferred quote removal)

Rather than the classic C-shell trick of "stuffing" sentinel bytes into a
flat char buffer to mark quoted characters (what dash/ash do), `ush`
represents a shell *word* as a tree:

```cpp
enum class WordPartKind {
    Literal,             // unquoted literal text, subject to later expansion
    SingleQuoted,        // '...' verbatim text, AND single backslash-escaped
                          // chars (both are "fully quoted", no expansion,
                          // no field split, no glob) - see note below
    DoubleQuoted,         // "..." - nested parts, no field split/glob on the
                          // whole, but $ / ` expansions inside still happen
    ParamExpansion,       // $name or ${...}; raw body text, parsed by the
                          // expansion stage (this is where the '${...}'
                          // sub-grammar - :-  :=  :?  :+  #  ##  %  %%  -
                          // lives, not the lexer)
    CommandSubDollar,     // $( ... ) - raw, balance-scanned text
    CommandSubBacktick,   // `...`    - raw, balance-scanned text
    ArithExpansion,       // $(( ... )) - raw text
};

struct WordPart {
    WordPartKind kind;
    std::string text;            // literal text, or raw inner content
    std::vector<WordPart> parts; // nested parts, only for DoubleQuoted
};

using Word = std::vector<WordPart>;
```

A backslash-escaped character outside quotes (`\*`) and a character inside
`'...'` are semantically identical (both are fully protected from
expansion/splitting/globbing), so the lexer represents an escaped char as a
one-character `SingleQuoted` part instead of inventing a fourth "escaped"
kind. This keeps the tree small while still letting later stages tell `\*`
apart from a bare `*`.

Tilde expansion (§2.6.1) does not get a dedicated `WordPartKind`: it only
applies to unquoted `~` at specific word-initial positions, which the
expansion stage can detect by inspecting the first `Literal` part directly.

## Known hard corners (tracked here, not silently glossed over)

- **`$((...))` vs `$(...)` ambiguity.** POSIX explicitly allows this
  ambiguity to exist syntactically (see the Rationale for §2.6.4). We use
  the common resolution: after `$((`, tentatively scan as arithmetic
  (paren-balanced, expects the matching close to be immediately followed by
  a second `)`); if that scan fails to close cleanly, fall back to treating
  it as `$(` command substitution whose content happens to start with `(`.
- **Backtick command substitution backslash rules** (§2.6.3) are more
  restrictive than double-quote backslash rules and technically allow
  embedded, escaped backticks for nesting. V1 lexer implements the common
  case (scan to the first unescaped backtick); full nested-backtick fidelity
  is a follow-up.
- **Alias substitution** (§2.3.1) interacts with tokenizing (an alias is
  substituted and the result re-lexed at the point the command-name word is
  recognized). Still deferred (now that the parser exists, this would slot
  into `Parser::parseSimpleCommand`, expanding the command-name word and
  re-parsing before it's used) - not yet needed since nothing executes yet.
- **Reserved words** are only reserved when totally unquoted *and* in a
  grammar position where a reserved word is expected (e.g. command-name
  position, or right after `do`). The lexer never decides this; it just
  tells the parser whether a WORD token was produced from a single,
  entirely-unquoted `Literal` part (`Token::couldBeReservedWord`), and the
  parser checks that string against the reserved word set only where the
  grammar allows it. **Simplification:** ush recognizes a reserved word
  only as the very first token of a `command`, before any leading
  redirects or assignment-words have been consumed. Strict POSIX arguably
  allows a leading redirect (but never an assignment-word) to still permit
  the following word to be recognized as reserved (e.g. `>f if ...; fi`);
  ush treats anything after a command's first token as never reserved,
  which matches every real shell once an assignment-word has been consumed
  (`FOO=bar if` runs a program named `if`) and is a clean, defensible
  choice for the redirect case too - which is vanishingly rare in real
  scripts. See the comment on `Parser` in `src/parser/parser.hpp`.
- **`for` loop separator leniency.** The strict grammar only allows a
  linebreak (never a `;`) between `for name` and a bare `do` (no `in`
  clause), and only allows that linebreak when an `in` clause *does*
  follow. ush accepts a `;` and/or a linebreak in both cases, matching
  real-world shell behavior for `for i; do ... done` and
  `for i\ndo ... done`.
- **Two more lexer re-scanning entry points**, alongside the ones used for
  heredocs: `Lexer::scanWordUntilEnd()` (full word syntax - quotes,
  backslash, `$`/`` ` `` - but never stops at a blank/operator, since the
  text was already delimited by balanced braces/parens) for `${...}`
  operands, and `Lexer::scanExpansionsUntilEnd()` (double-quote-body rules
  - only `$`/`` ` ``/backslash-before-`$ ` " \` are special, quote
  characters are literal) for `$((...))` pre-expansion and (not yet wired
  up - needs the executor) unquoted here-document bodies, both of which
  spec says undergo "the same expansions as double-quoted text".
- **`${#}` (braced, nothing between `#` and `}`)** is treated as the
  special parameter `#` (positional parameter count, same as bare `$#`)
  rather than "the length of a parameter with an empty name" - the latter
  reading is nonsensical, and this matches real shells.
- **`$@`/`$*` used as the *operand* of a `${...}` operator** (e.g.
  `${@:-default}`) fall back to a single joined-by-first-IFS-char string
  (like unmodified `$*`) for the purposes of that operator, rather than
  preserving `$@`'s per-field identity through the operator - a rare
  combination in practice.
- **A shell variable's value is not "chased"** when used as a bare
  identifier in arithmetic: `x=y; y=5; echo $((x))` evaluates `x` as 0
  (not a clean integer), not as 5. Some shells (bash) do this chasing;
  ush's arithmetic evaluator treats an unset-or-non-numeric variable as 0
  and stops there.
- **A trailing IFS delimiter never produces a trailing empty field**,
  whether it's whitespace or a non-whitespace IFS character - `a:` with
  `IFS=:` splits to just `["a"]`, not `["a", ""]`. This matches ush's
  best-effort recollection of real shell behavior (confidently verified:
  leading whitespace is trimmed and never produces a leading field, and a
  leading *non-whitespace* delimiter does; less independently verified:
  that the trailing case is fully symmetric for non-whitespace
  delimiters). Internal empty fields between two non-whitespace
  delimiters (`a::b` -> `["a", "", "b"]`) are on solid ground either way.
- **Pathname expansion doesn't track `set -f`/`noglob`** (no `set` builtin
  exists yet to set it) - globbing is always attempted when a pattern has
  an unquoted metacharacter. Also doesn't special-case a pattern's
  trailing `/` to require the match be a directory - matches still must
  be directories to serve as a *non-final* path component, but a trailing
  slash on the pattern itself currently has no extra effect.
- **Here-document delimiter quote-removal** (`Parser::flattenSimpleWord`)
  only handles delimiters built from literal text and quoting; a delimiter
  word containing a live `$`/`` ` ``/arithmetic expansion contributes no
  text for those parts. This is spec-adjacent (such a delimiter is
  nonsensical/undefined in real usage) and not expected to matter in
  practice.

## Status / roadmap

1. [x] Project scaffold (CMake, Catch2, directory layout).
2. [x] Lexer (§2.2, §2.3, §2.10.1) - tokens, quoting, operators, IO_NUMBER,
       comments, command/param/arith substitution scanning.
3. [x] Parser (§2.10.2) - AST (`src/ast/ast.hpp`) for lists/pipelines/
       simple & compound commands (brace group, subshell, `for`, `case`,
       `if`/`elif`/`else`, `while`, `until`, function definitions)/
       redirections/here-docs, contextual reserved-word handling, here-doc
       body extraction wired through `Lexer::consumeHeredocBody` and
       `Parser::advance`. See the "known hard corners" above for the two
       documented leniencies. 49 Catch2 test cases covering the grammar.
4. [~] Expansion (§2.6):
       - [x] `src/runtime/environment.hpp` (§2.5) - shell variable storage
             (get/set/export/readonly/unset), positional parameters,
             special parameters ($?/$$/$!/$0), IFS lookup, and the
             exported-variable list execve() will need later. This is the
             substrate every other expansion piece (and eventually the
             executor) reads from and writes to.
       - [x] Arithmetic expansion (§2.6.4, `src/expand/arithmetic.cpp`) - a
             recursive-descent evaluator over `intmax_t`, covering every
             operator POSIX lists (unary +-!~, `* / %`, binary +-, shifts,
             relational, equality, bitwise & ^ |, && ||, `?:`, all the
             assignment operators, comma), with assignment writing back to
             `Environment`. One documented simplification: an unset or
             non-numeric variable evaluates to 0 rather than being chased
             as another variable name/expression (which e.g. bash does).
       - [x] `src/expand/expander.cpp` - tilde (§2.6.1), parameter
             (§2.6.2, the full `${...}` operator sub-grammar `:- := :? :+
             - = ? + % %% # ##`, `${#param}`, positional/special
             parameters, `$@`/`$*` including their quoted multi-field/
             joined behavior), command substitution (§2.6.3, via an
             injected `CommandRunner` interface so this module doesn't
             depend on the executor - a real executor implementing it is
             still to come), and arithmetic expansion (§2.6.4, via
             `evaluateArithmetic`). Produces an `ExpandedWord` (a sequence
             of quote-tagged `ExpansionPiece`s) rather than a plain
             string - see the file's header comment - so field splitting
             and pathname expansion (next) have the quoting information
             they need, and so `$@` can express "hard field break here,
             regardless of IFS" via `fieldBreakAfter`. Quote removal
             (§2.6.7) is the trivial `flatten()` over that structure.
             Reuses `Lexer::scanWordUntilEnd()`/`scanExpansionsUntilEnd()`
             (added alongside this) to re-lex raw text the lexer captured
             as opaque strings (`${...}` operands, `$((...))` bodies) -
             see "Known hard corners" below for the two new lexer entry
             points and what each treats as special.
       - [x] `src/expand/field_split.cpp` - field splitting (§2.6.5): a
             character-level delimiter state machine over `IFS` (the
             whitespace-vs-"other" IFS character distinction, adjacent-
             whitespace absorption into a single delimiter, leading-
             whitespace trim, and the null-field-removal rule), run
             independently on each `fieldBreakAfter`-delimited segment of
             an `ExpandedWord` so `$@` still gets its guaranteed
             per-parameter boundaries regardless of `IFS`.
       - [x] `src/expand/pathname_expand.cpp` - pathname expansion
             (§2.6.6), via `fnmatch(3)` for pattern matching
             (`src/expand/pattern.cpp`) and `opendir`/`readdir` for
             directory walking (real POSIX C-library APIs, not shell
             source - revised from an earlier note about reimplementing
             glob semantics from scratch, since fnmatch(3) already *is*
             the POSIX spec for this, just as libc's fork/exec/waitpid are
             for process management). Handles multi-component patterns
             (`a*/b?/*.txt`), the leading-dot/hidden-file rule, sorted
             results, and "no match -> left unchanged" - and is careful to
             `stat()` (follow symlinks) rather than `lstat()` when
             checking whether an intermediate path component is
             traversable, since e.g. macOS's `/var` is itself a symlink.
       - [x] Quote removal (§2.6.7) - `flatten()` in expander.hpp, since
             the representation never carries quote characters to begin
             with (see "Word representation" above).
       - [ ] Command substitution needs a real (if initially minimal)
             executor plugged in as a `CommandRunner` to do anything; this
             is what pulls the executor into scope next - the last piece
             of §2.6, and everything else in the pipeline besides.
5. [ ] Executor (§2.9) - simple command exec (fork/exec/PATH search),
       pipelines, `&&`/`||`/`;`/`&` lists, compound commands, subshells,
       redirection setup (§2.7), exit status rules, `$?`/`$$`/`$!`/`$#`/`$@`/
       `$*`/positional params, traps (§2.11/2.13 `trap` builtin, minimal
       signal handling).
6. [ ] Special built-ins (§2.14: `:`, `.`, `break`, `continue`, `eval`,
       `exec`, `exit`, `export`, `readonly`, `return`, `set`, `shift`,
       `times`, `trap`, `unset`) - these must be implemented in-process
       (not forked) and skip `PATH` search, per spec.
7. [ ] Regular built-in utilities needed for a usable shell: `cd`, `pwd`,
       `echo`, `printf`, `test`/`[`, `true`, `false`, `type`, `hash`,
       `command`, `read`, `getopts`, `umask`, `wait`, `kill`, `alias`/
       `unalias`.
8. [ ] Interactive mode: prompt expansion (`PS1`/`PS2`), basic line editing,
       history. Job control (`bg`/`fg`/`jobs`, `SIGTSTP` handling) - XSI,
       may land after everything above is solid.
9. [ ] Integration test suite (shell scripts + expected output/exit status,
       run against the built `ush` binary).

We will work through 3-9 incrementally; "broad POSIX from the start" means
the *architecture* supports the full grammar/expansion/execution model from
day one (no simple-commands-only shortcut baked into the design), not that
every feature ships in the first patch.
