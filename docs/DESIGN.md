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
  characters are literal) for `$((...))` pre-expansion and (now wired up
  in `Executor::applyOneRedirect`, via `Expander::expandHeredocBody`)
  unquoted here-document bodies, both of which spec says undergo "the
  same expansions as double-quoted text".
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
- **fork(2) and libc stdio buffering.** A real bug the integration tests
  caught: a builtin's `printf`/`fputs` output can sit unflushed in libc's
  stdio buffer; if a *later* `fork()` happens before it's flushed, the
  child inherits a copy of that buffered data, and whenever the child's
  own stdio buffers eventually flush (any normal exit path does), it
  re-emits the parent's leftover bytes too - duplicated, misordered
  output. Fixed by flushing (`fflush(nullptr)`) immediately before every
  `fork()` call in `executor.cpp`, not by avoiding buffered I/O.
- **Pipeline stages that are external programs or nested subshells fork
  twice**, not once: `runCommand()` is written to be safe to call from
  any process context (see its header comment), so a pipeline forks once
  per stage and, if that stage happens to itself need a fork (an external
  program, or a subshell), `runCommand()` forks again inside the already-
  forked child. This is a deliberate simplicity-over-efficiency choice,
  not a correctness issue - real shells avoid the second fork for the
  common case, ush doesn't yet.
- **A readonly-variable assignment failure doesn't abort the whole
  (non-interactive) script**, only the one command that attempted it -
  `Executor::trySetVar` catches `ReadonlyVariableError` and reports it,
  rather than letting it propagate as an uncaught C++ exception (which,
  before this was added, crashed the entire shell process via
  `std::terminate` on the very first readonly violation - caught by the
  integration tests, not a unit test, since it required actually running
  a script through the executor). Strict POSIX says a variable-assignment
  error should exit a non-interactive shell entirely; ush's softer
  behavior is a documented simplification.
- **Distinguishing "needs more input" from "syntax error"** for the
  interactive REPL's `PS2` continuation turned out to have a single,
  minimally-invasive fix rather than needing every error site touched
  individually: every `Lexer` "unterminated ..." error already checks
  `atEnd()` immediately before throwing, and every `Parser` grammar-
  expectation failure already goes through one `error()` method that
  looks at `current_` right after failing to match something against it.
  So `Lexer::errorAt()` just records `atEnd()` as `LexError::incomplete()`,
  and `Parser::error()` just records `current_.type == EndOfInput` as
  `ParseError::incomplete()` - both computed once, at the single existing
  throw site, rather than threaded through every individual call site.
  This also means unterminated here-documents surface as "incomplete"
  for free (an unfound heredoc delimiter is exactly an `atEnd()`-triggered
  `LexError`), with no heredoc-specific code in the REPL at all.
- **Keeping function definitions alive across an interactive session**
  without cloning the AST: `runInteractive()` pushes each successfully-
  parsed `ast::List` onto a `std::vector` that lives for the whole
  session, and `Executor::functions_` keeps raw pointers into whichever
  `ast::List` a function was defined in (see `executor.hpp`'s lifetime
  note). Appending to that vector - even when it reallocates - never
  invalidates those pointers, because they point at `Command` objects
  reached through `ast::Pipeline::commands` (a
  `vector<unique_ptr<Command>>`): reallocating the *outer* vector moves
  `unique_ptr`s around, not the heap-allocated `Command` objects they own.
  A single-shot run (`-c`/script/piped stdin) doesn't need any of this -
  it parses once into one `ast::List` that simply outlives the one
  `runProgram()` call.
- **Ctrl-C/Ctrl-\\ in interactive mode** (ignored by the shell itself,
  reset to default in every forked foreground child - see
  `resetForegroundSignalsInChild()` in `executor.cpp`) was verified
  manually rather than with an automated test: sending `SIGINT` directly
  to a running interactive `ush` leaves it alive and responsive, and
  sending it to a foreground child (e.g. a `sleep`) kills that child
  promptly while the shell continues - both confirmed by hand. Automating
  this reliably would mean asserting on real process-group signal
  delivery and timing, which is a more likely source of flaky CI than of
  genuine regression coverage, so it's deliberately not in the suite.
- **`signal(2)` vs `sigaction(2)`/`SA_RESTART` for trap handlers** - a
  real bug caught by hand (before it had automated coverage; it does
  now). Installing a trap's signal handler with the legacy `signal(2)`
  wrapper worked in the sense that the handler ran and the trap action
  eventually executed, but on macOS (and potentially other libcs)
  `signal(2)` implies `SA_RESTART`, which makes the kernel silently
  resume an interrupted blocking syscall instead of returning `EINTR` to
  it. `Executor::waitForChild()` depends entirely on seeing that `EINTR`
  to know it should call `servicePendingTraps()` - without it, a
  `sleep 30` in the foreground would fully absorb the interrupt and the
  trap would only run once that 30 seconds elapsed on its own, which
  defeats the actual point of trapping a signal for prompt shutdown.
  Confusingly, the trap still visibly "worked" in ad hoc testing (its
  `echo` output showed up in the end) because fully-buffered stdio (a
  non-tty stdout) made *when* it ran indistinguishable from *whether* it
  ran without directly timing the process's actual death - a reminder
  that "the expected output eventually appears" is a much weaker check
  than "it appears when it's supposed to". Fixed by installing trap
  handlers via `sigaction(2)` with `sa_flags = 0` (no `SA_RESTART`)
  instead - see `installSignalDisposition()` in `executor.cpp`.
- **A stop signal getting lost inside the "double fork"** - the most
  subtle bug job control surfaced. `runCommand()`'s pre-existing
  "simplicity over efficiency" tradeoff (an external command or subshell
  nested inside an already-forked pipeline stage/subshell/async job
  forks *again*, rather than the single fork a hand-optimized shell
  would use) means the process actually running the target program is
  often a *grandchild* of the shell, not a direct child - and POSIX
  `wait`/`waitpid` can only ever reap direct children. The intermediate
  "wrapper" layer's own wait for its child was a plain, untracked wait
  (no `WUNTRACED`), so when a real, group-wide `SIGTSTP` (Ctrl-Z) hit the
  whole tree - the wrapper inherited the job's process group, same as
  the grandchild - the *grandchild* would stop, but the wrapper (which
  itself typically ignores `SIGTSTP`, inherited from the interactive
  shell) would just sit blocked forever in its ordinary wait, never
  telling the top-level shell (which can only see the wrapper, its
  actual direct child) that anything had happened. Fixed by making
  `Executor::waitForChild()` - the shared helper every non-job-control-
  owning wait goes through - pass `WUNTRACED` and, on seeing its own
  child stop, `raise(SIGSTOP)` on *itself*. `SIGSTOP` can't be blocked or
  ignored, so this always takes effect immediately; a group-wide
  `SIGCONT` (from `bg`/`fg`) then resumes every layer together, and the
  wrapper's wait loop falls through to an ordinary wait again. This
  makes every intermediate layer, however many there are, visibly mirror
  a stop, so whichever ancestor *is* actually doing job-control
  bookkeeping (via `waitForJob()`'s own `WUNTRACED` wait on its direct
  child) sees it. Caught by an integration test - but only after fixing
  a bug *in the test itself* first: it originally sent `SIGTSTP` to a
  single pid rather than negating it to target the whole process group,
  which is what a real terminal's Ctrl-Z actually does - a good example
  of a test's own fidelity to the real mechanism mattering as much as
  the assertions in it.
- **`wait %job` must retire the job table entry itself, not rely on
  `updateAndNotifyJobs()` to notice.** `wait %job`'s own `waitpid(2)`
  calls (needed to get that specific job's exit status directly) already
  consume the underlying wait-status events for its pids; a subsequent
  call to `updateAndNotifyJobs()` (which does its *own* group-wide
  `waitpid(2)` calls to detect changes) would then find nothing left to
  reap and leave the job looking permanently stuck at whatever state it
  was last seen in. `Executor::removeJob()` erases the entry directly
  once `wait %job` has already reaped it, sidestepping the conflict.
- **Job-table/notification command text is approximate, not a real
  unparser.** `describeWord()`/`describeCommand()`/`describePipeline()`
  in `executor.cpp` reconstruct enough of a job's source text for `jobs`/
  "[n]+ Stopped ..." to be recognizable - a word that isn't a single
  unquoted literal (any quoting or expansion) prints as `...`, and a
  compound command prints as a generic `{ ... }`. Good enough to tell
  jobs apart by eye; not intended to reproduce the exact source.
- **`Ctrl-C` while the line editor is reading a key either aborts the
  line or defers to a user `trap`, depending on which was already
  true when `readLine()` started - never both, and never neither.**
  `LineEditor` doesn't install its own `SIGINT` handler unconditionally:
  it first asks `Executor::trapAction(SIGINT)`. If no trap is set (the
  common case - the shell's own default disposition is `SIG_IGN`, from
  `main.cpp`), it installs a small temporary handler (own
  `sig_atomic_t` flag, mirroring the pattern in executor.cpp) for the
  duration of the read, so a blocking `read(2)` sees `EINTR` and the
  editor can abort the current (possibly multi-line) input and return
  to a fresh prompt - the behavior every interactive shell's line
  editor has. If a trap *is* set, that installation is skipped
  entirely: the trap's own handler (installed by `Executor::setTrap`)
  stays in place untouched, `read(2)` still sees `EINTR` from it (same
  `sigaction`-without-`SA_RESTART` discipline), and the editor's
  `readByte()` calls `Executor::servicePendingTraps()` to run the
  trap's action right then - then just retries the read, keeping
  whatever was typed so far, rather than discarding it. This means an
  `INT` trap fires promptly even while sitting at the prompt typing
  (previously it wouldn't fire until the next blocked wait for a
  foreground child, since nothing polled `servicePendingTraps()` at an
  idle prompt at all) - a small but real improvement that fell out of
  needing EINTR-driven interruption for the editor anyway. A trap
  action that itself calls `exit` propagates as `ExitSignal` straight
  out of `readByte()`/`readLine()` (the `RawMode`/`TempSigintHandler`
  RAII guards restore the terminal/signal disposition correctly even
  as it unwinds); `main.cpp`'s interactive loop has its own
  `try`/`catch (const ExitSignal&)` around the whole loop body to catch
  that specific case, distinct from the existing `outcome.exitRequested`
  path (which only covers `exit` from an already-parsed command, not
  from a trap firing between prompts).
- **The line editor redraws the whole line on every keystroke rather
  than diffing.** `LineEditor::redraw()` does `\r` + clear-to-end-of-
  line + reprint prompt+buffer + reposition the cursor, unconditionally,
  on every edit. Simple and correct for anything that fits on one
  terminal row (the overwhelmingly common case for a shell command
  line), but it doesn't account for line-wrap: a command long enough to
  wrap across multiple terminal rows will redraw incorrectly, since
  `\r` only returns to column 0 of the *current* row, not the row the
  prompt started on. A real line editor (readline/zsh's ZLE) tracks
  terminal width and cursor row explicitly; this one doesn't.
- **Distinguishing a lone Escape keypress from the start of a real
  escape sequence uses a timing heuristic, not a proper terminfo/
  terminal-capability parse.** Arrow keys/Home/End/Delete all arrive as
  multi-byte sequences starting with `ESC` (`0x1B`); a bare press of the
  Escape key is just that one byte with nothing following. Since the
  editor's main read loop blocks indefinitely (`VMIN=1`, `VTIME=0`) to
  avoid busy-waiting between ordinary keystrokes, telling these apart
  needs a *different* read call specifically after seeing an `ESC`:
  `LineEditor::readByteTimed()` temporarily switches to `VMIN=0`/
  `VTIME=n` (a tenths-of-a-second poll-style read) just for the
  byte(s) that would complete a sequence, restoring the normal blocking
  settings immediately after. In practice a real terminal's escape
  sequence bytes arrive together in one burst well within the ~100-200ms
  windows used here, so this reliably tells "lone Escape" (times out,
  treated as a no-op) from "arrow key" (bytes arrive immediately)
  without needing a real terminfo database - a pragmatic, if inexact,
  substitute for one.
- **Every test that spawns an interactive `ush` must set `HISTFILE`
  itself, or it will load/save the real user's `~/.ush_history`.**
  `historyFilePath()` in `main.cpp` falls back to `$HOME/.ush_history`
  when `HISTFILE` is unset - correct behavior for a real shell, but a
  test process still inherits the real `$HOME` from whoever is running
  the test suite. Caught by hand (not a test) after the first job-
  control-era test run silently wrote real-looking history entries into
  the developer's actual `~/.ush_history` - every one of this test
  file's spawn points (`runUshWithArgv`, `InteractiveSession`,
  `PtySession`) now defaults its `histFile` parameter to `/dev/null`
  (in-memory-only for the test: `load()` finds nothing, `save()` writes
  harmlessly nowhere), with an explicit real temp path only where a test
  specifically wants to exercise persistence.
- **`fc`'s edit-mode default editor (`vi`, absent `-e`/`$FCEDIT`/
  `$EDITOR`) is a documented choice, not something POSIX mandates.**
  POSIX leaves `fc`'s default editor unspecified; this implementation
  picks `vi` (present on essentially every POSIX system, unlike e.g.
  `ed`) rather than trying to detect "the historical default."
- **`PS1`/`PS2` get more expansion than §2.5.3's literal text asks for -
  a deliberate, documented choice, not an oversight.** The spec says
  each "shall be subjected to parameter expansion" only (§2.6.2) - not
  tilde expansion, not command substitution, not arithmetic expansion.
  Taken literally, that would make `PS1='$(pwd) $ '` - probably the
  single most common real prompt customization - not work: `$(pwd)`
  would appear in the prompt as the literal four characters `$`, `(`,
  `p`... rather than being run. `expandPrompt()` in `main.cpp` runs PS1/
  PS2 through the same tilde/parameter/command/arithmetic expansion (+
  quote removal) any other word gets, deliberately going beyond the
  letter of the spec for what's clearly the intent of "customizable
  prompt string."
- **PS1's `!`/`!!` substitution is a raw-text pass over the *variable's
  own value*, done before any other prompt expansion runs - not a
  post-pass over the final expanded/displayed string.** This matters
  for a prompt like `PS1='$(echo !)'`: the `!` is part of PS1's literal
  text (about to be handed to a command substitution as its script,
  which will just echo it back), so it gets replaced with the history
  number *first* - `$(echo 5)` - and command substitution then runs
  that. The alternative (substituting `!` in the *output* of expansion)
  would instead touch any literal `!` characters a command substitution
  happens to print, which has nothing to do with what §2.5.3 is
  describing ("the character '!' in PS1"). Also why this only applies to
  PS1, never PS2/PS4 (see item 8's Status entry above) - the spec text
  granting this treatment names PS1 specifically.
- **A real bug, caught while implementing the above and unrelated to
  it: `history -c` didn't actually clear anything.** It was implemented
  as `setMaxSize(0)` (meant to drop everything by capping at zero)
  immediately followed by `setMaxSize(500)` (meant to restore the normal
  cap) - but `setMaxSize(0)` means *unlimited*, not *zero*, so it never
  trimmed anything in the first place, and the very next call just
  raised the cap back to 500 over a list that was never emptied. Fixed
  by adding a real `History::clear()` (erases every entry, advances the
  internal "next number" counter past them so numbering keeps climbing
  rather than reusing numbers) instead of trying to express "clear" as
  two cap changes.
- **`$ENV`/`~/.ushrc` are deliberately NOT run through `.`/`eval`'s
  machinery (`Executor::runSourceInCurrentContext`), even though
  "source a file in the current environment" is exactly what that
  function does - and this turned out to be the right call for a reason
  beyond the one originally intended.** The original reason:
  `runSourceInCurrentContext()` deliberately lets `exit`/`return`/
  `break`/`continue` escape uncaught, on the assumption that whatever
  called it (a loop body for break/continue, a function call for
  return, `runProgramCatchingExit` for exit) will catch what applies to
  its own context - but a startup file has no such enclosing context,
  so those would simply crash the process as uncaught C++ exceptions.
  The second, more serious reason, found while double-checking the
  first one: `runSourceInCurrentContext`'s `ast::List` is a *local*,
  destroyed the moment the call returns - and it turns out a function
  *defined inside it* is NOT a self-contained, independent AST node the
  way this note originally (incorrectly) assumed. `functions_` stores a
  raw pointer straight into whichever `ast::List` was being run when the
  definition was reached (see its own doc comment), so a function
  defined via `eval`/`.` is left holding a dangling pointer the instant
  that call returns - calling it later is undefined behavior, observed
  in practice as a silent no-op (`type` still reports it as a function;
  calling it produces no output and exit status 0, no crash, no error).
  **This is a confirmed, pre-existing bug in `eval`/`.`, not something
  this feature works around for them** - flagged separately for a real
  fix (Executor needs to own the AST of anything it runs this way, not
  rely on the caller to keep it alive, the way `runInteractive`'s
  `programs` vector does for ordinary interactive lines). `~/.ushrc`/
  `$ENV` simply never hit this bug because they were written from
  scratch with their own `runStartupSource()`/`sourceStartupFile()`
  lambdas, which parse the startup file into a program pushed onto that
  same session-lifetime `programs` vector, then run it via
  `runProgramCatchingExit()` (the exact same top-level tolerance every
  interactively-typed line already gets) -
  functionally "like `.`", just without going through the `.` builtin's
  own code path.
- **Every test that spawns an interactive `ush` must also override
  `$HOME` (and unset `$ENV`), or it will run whatever's in the *real*
  developer's actual `~/.ushrc`/`$ENV`-pointed file.** The same class of
  problem as the HISTFILE isolation issue above, for the same reason (a
  test process still inherits the real environment) - caught by
  reasoning about it up front this time rather than by an actual
  incident, since by now `~/.ushrc` existing for real (this project's
  own author has since set ush as their login shell) was a real, not
  hypothetical, risk. Every spawn point in the integration test file
  (`runUshWithArgv`, `InteractiveSession`, `PtySession`) defaults `HOME`
  to a fixed nonexistent path and unsets `ENV` outright, with optional
  parameters to point at a real temp-directory `~/.ushrc` or `$ENV`
  target for the tests that specifically exercise startup-file sourcing.
- **A `TEST_CASE` name that happens to start with a character Catch2's
  own CLI/test-spec parser treats specially breaks `ctest`'s discovery-
  based invocation of it - silently, without failing.** `catch_discover_
  tests` (CMake's Catch2 integration) generates one `ctest` entry per
  `TEST_CASE`, each invoking the test binary with that exact name string
  as a single positional argument - which Catch2 then parses as a test
  *spec*, not literal text, so a name beginning with one of Catch2's
  special spec characters is parsed as something else entirely instead
  of matching just itself. Two real instances of this in this file so
  far: a name starting with `"-i "` was parsed as Catch2's own `-i`/
  `--invisibles` CLI flag (`ctest` reported that one test as failed,
  "No test cases matched", since nothing named the empty remainder) -
  fixed by rewording it to not start with `-i`. A name starting with
  `"~/"` was parsed as a *negated* spec (`~pattern` means "everything
  NOT matching pattern") - since nothing in the whole suite matches the
  nonsensical remainder `/.ushrc is...`, the negation matched *every*
  test, so `ctest` silently re-ran the entire 212-test suite under that
  one test's name every time (**passing**, since every real test in it
  still passed, but with wildly inflated - and easy to mistake for a
  hang or a flaky test - timing: ~91s for what should have been a single
  sub-second test, initially misread as evidence of a real bug before
  `--list-tests` with the same string revealed it matched all 212 cases)
  - fixed by rewording it to not start with `~`. The general lesson: a
  `TEST_CASE` name is also, incidentally, a `ctest`-invocation command-
  line argument, and needs to survive being parsed as one - avoid
  leading `-`, `~`, `[`, and (to be safe) commas.

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
5. [x] Executor (§2.9, `src/exec/executor.cpp`) - simple command exec
       (fork/exec/PATH search), pipelines (real `pipe(2)`s, one fork per
       stage), `&&`/`||`/`;`/`&` lists, all compound commands (subshells
       via real `fork(2)` - copy-on-write gives isolation for free, no
       manual environment cloning needed), redirection setup (§2.7, all
       operators including here-documents via `tmpfile()` to avoid a
       pipe-buffer deadlock on large bodies), exit status rules,
       `$?`/`$$`/`$!`/`$#`/`$@`/`$*`/positional params, functions
       (definition + call, with `$1..`/`$#` rebound and `return`
       unwinding via an exception - see `src/exec/control_signals.hpp`,
       also used for `break [n]`/`continue [n]`/`exit [n]`). Command
       substitution (the `CommandRunner` seam `Expander` was built
       against) now actually runs things, in a forked child with stdout
       piped back. Real signal/trap handling (§2.11): trapped signals
       install a `sigaction(2)` handler (deliberately without
       `SA_RESTART` - see the hard-corners note) that only records the
       signal as pending; `Executor::servicePendingTraps()` runs the
       actual trap action later, at a safe point - between list items
       (so a trap fires promptly even in a tight builtin-only loop) and
       whenever a blocked wait for a foreground child is interrupted
       (so it fires promptly there too, without waiting for that child
       to finish on its own). The `EXIT` trap runs exactly once, at the
       two real termination points (`main.cpp`, both the non-interactive
       path and the end of the interactive session) - not inside
       `runProgram()`, which interactive mode calls once per input line.
       Job control (§2.9.3.1, XSI): each foreground pipeline/external
       command/subshell gets its own process group (`setpgid(2)`, the
       standard "both parent and child call it" race-safe idiom) and is
       handed the controlling terminal (`tcsetpgrp(2)`, best-effort -
       harmless without one); a group that stops (Ctrl-Z/`SIGTSTP`)
       is detected via a `WUNTRACED` wait, registered as a job, and
       reported once, before the next prompt. `enableJobControl()` (only
       ever called by `main.cpp`, only when actually interactive) puts
       the shell in its own group and ignores `SIGTSTP`/`SIGTTIN`/
       `SIGTTOU` for itself; every forked descendant resets them to
       default. When job control isn't active (every non-interactive run,
       and every one of ush's own forked descendants - see
       `isJobControlShell_`), every fork point behaves exactly as it did
       before job control existed: no groups, no terminal handoff, no
       job table. See the "known hard corners" above for four real bugs
       the tests caught (fork+stdio buffer duplication; `lstat` vs
       `stat` on symlinked directories; `signal` vs `sigaction`/
       `SA_RESTART` silently defeating prompt trap delivery; a stop
       signal getting lost inside the "double fork" for a backgrounded
       external command/pipeline stage, fixed by making every
       intermediate layer mirror a child's stop with `raise(SIGSTOP)` on
       itself). **Not implemented**: `SIGTTIN`/`SIGTTOU`-driven
       background-job terminal-access suspension, `%+`/`%-` as distinct
       from a generic "current job", and stopped-then-re-stopped jobs
       keeping a stable job number across an `fg` cycle.
6. [x] Special built-ins (§2.14, `src/exec/builtins.cpp`): `:`, `.`,
       `break`, `continue`, `eval`, `exec`, `exit`, `export`, `readonly`,
       `return`, `set` (positional-parameter form only - option flags
       like `-e`/`-x` are accepted but not implemented), `shift`,
       `trap` (condition list, `-p`, the single-numeric-operand
       backward-compatible reset form), `times`, `unset`.
7. [x] Regular built-in utilities: `cd`, `pwd`, `echo`, `test`/`[`
       (a practical subset - no `-a`/`-o`/parenthesization), `true`,
       `false`, `printf` (`%s %d %i %u %o %x %X %c %b %%` with real
       flags/width/precision delegated to libc `snprintf` per
       conversion, not reimplemented - no `%e`/`%f`/`%g` floating
       point), `read` (`-r`, IFS field splitting with the last variable
       absorbing extras - joined with a plain space rather than
       preserving original separators, a documented simplification),
       `command` (`-v`, and bypassing function lookup), `type`,
       `getopts` (bundled short options, `OPTARG`/`OPTIND`, silent mode
       via a leading `:` - the "which character within the current arg"
       position POSIX leaves as shell-internal state lives in an
       internal env var, `_ush_getopts_charidx`), `wait` (a bare `pid` or
       a `%job` operand; with no argument, reaps *all* children), `umask`,
       `kill` (`-signal`/`-l`, plus a `%job` operand that signals the
       job's whole process group), `jobs` (`-l`), `fg`/`bg` (`%job`, or
       the current job with no operand), `fc` (`-l`/`-n`/`-r` to list;
       `-e editor`/`$FCEDIT`/`$EDITOR` to edit-and-reexecute a range;
       `-s [old=new]` to reexecute directly, with an optional textual
       substitution - all three history-reference forms: a plain number,
       a negative number relative to the most recent command, or a
       string matched against the most recent command starting with it),
       and `history` (not itself POSIX - a convenience listing/`-c`-
       clearing wrapper around the same list `fc` uses). **Not
       implemented**: `hash`, `alias`/`unalias`.
8. [x] Interactive mode (`src/main.cpp`'s `runInteractive`): a real REPL -
       prompts with `PS1`/`PS2` (expanded the same as any other word:
       tilde/parameter/command/arithmetic + quote removal - broader than
       the letter of §2.5.3, which only mandates parameter expansion for
       these; see "Known hard corners" below), plus §2.5.3's one PS1-
       specific rule: an unescaped `!` in PS1's own text is replaced with
       the history number the *next* command will get, and `!!` becomes
       a literal `!` (`applyHistoryBang()`, applied to PS1's raw value
       before the rest of prompt expansion runs - `PS1='[!] '` shows
       `[1] ` for the first command, `[2] ` for the second, and so on).
       Not implemented for PS2 or PS4 - POSIX gives that treatment to PS1
       alone. Reads and
       parses incrementally so a `PS2` continuation prompt appears for
       any unfinished construct (unclosed quote, compound command,
       here-document, trailing `&&`/`|`/...), and recovers cleanly from
       a genuine syntax error (reports it, discards the bad input,
       keeps going) versus an incomplete one (keeps reading). Every
       parsed program is kept alive for the session, so a function
       defined on one line is callable on a later one - see the
       "Known hard corners" note on `LexError`/`ParseError::incomplete()`
       and on session-lifetime AST ownership. `Ctrl-C`/`Ctrl-\` no
       longer kill the shell itself in interactive mode (ignored there,
       reset to default in every forked foreground child - external
       programs, pipeline stages, subshells, command substitution -
       so they're still interruptible; a background `&` job keeps the
       inherited ignore, matching real shells) - manually verified (see
       the hard-corners note) rather than covered by an automated test,
       since reliably automating signal/process-group timing tends to
       be a source of test flakiness rather than of confidence.
       Job control (see item 5) is fully wired up here too: `jobs`
       are announced/reported at the right points in this loop's own
       prompt cycle. A real raw-mode line editor
       (`src/interactive/line_editor.cpp`, `LineEditor`) now sits in
       front of every prompt when stdin/stdout are actual terminals
       (`LineEditor::isUsable()`): cursor movement (`^A`/`^E`/`^B`/`^F`/
       arrow keys/Home/End), in-place editing (backspace, Delete, `^K`/
       `^U`/`^W` kill plus `^Y` yank, `^L` redraw), and arrow-key/`^P`/
       `^N` recall through a real command history
       (`src/runtime/history.hpp`, `Executor::history()`, `fc`/
       `history` - item 7). `Ctrl-C` while editing aborts the current
       (possibly multi-line) input and returns to a fresh prompt - see
       the hard-corners note on how that coexists with a user `trap ...
       INT`. History is recorded (and `HISTFILE`/`HISTSIZE` loaded/
       saved) whenever interactive, whether or not the fancy editor
       itself is active - piped/redirected stdin (`ush -i < script`,
       and every non-pty integration test) still falls back to plain
       line-at-a-time reading, matching real shells' behavior in the
       same situation. Before the first prompt, two startup files are
       sourced in the shell's own environment (like `.` - not a
       subshell, so variables/functions they set are visible for the
       rest of the session): POSIX's `$ENV` (§2.5.3 - only when real and
       effective uid/gid match, so a setuid/setgid `sh` can't be tricked
       into running arbitrary commands via an inherited `ENV`), then
       ush's own `~/.ushrc` - NOT part of POSIX, the same convention
       bash's `~/.bashrc`/zsh's `~/.zshrc` follow, added on request
       rather than because the spec asks for it. Either file's parsed
       AST is kept alive for the session exactly like any other
       interactively-typed line (so a function defined in `~/.ushrc` is
       still callable later), a missing file of either kind is silently
       skipped (not an error), a syntax error is reported but doesn't
       stop the shell from starting, and `exit` from within either ends
       the session immediately - see "Known hard corners" below for why
       neither goes through the `.`/`eval` machinery. **Not
       implemented**: completion (filename/
       command), a real vi editing mode (`set -o vi`/`-o emacs` aren't
       recognized as anything other than a no-op), and multi-byte
       UTF-8-aware cursor movement (each byte counts as one column).
       `main.cpp` also supports
       `ush -c 'cmd' [name [arg...]]` and `ush script [arg...]` for
       non-interactive use, and `ush -i` to force interactive mode
       without a real terminal (used by the integration tests, and a
       real, if minor, usability feature in its own right - matches
       other shells' `-i`).
9. [x] Integration test suite (`tests/integration/`) - runs the actual
       built `ush` binary (via `fork`+`execve`, not `popen`, to avoid a
       second layer of shell quoting) against real scripts and checks
       combined stdout/stderr and exit status. This is what caught the
       three bugs mentioned in item 5 - process-level behavior that unit
       tests mocking nothing couldn't have exercised. Also covers
       interactive mode end to end (via `-i`, see item 8): PS1/PS2
       prompts appearing exactly where expected, multi-line continuation,
       function persistence across lines, `exit` ending the session
       immediately, and syntax-error recovery; and trap/signal handling,
       including one test that sends a real `SIGTERM` to a running `ush`
       child process and requires it to react within ~5s despite being
       in the middle of a 30s `sleep` - a deliberately generous but still
       meaningful deadline, guarding against the `SA_RESTART` regression
       coming back without being sensitive to CI timing noise. Job
       control gets the same treatment: an `InteractiveSession` helper
       (a live `ush -i` with pipe stdin/stdout the test drives
       incrementally - reading a job's announced pid from mid-session
       output before it can send that job a real signal, unlike every
       other interactive test here) covers backgrounding, the `Done`
       notification, a real `SIGTSTP` sent to a job's *process group*
       (matching what a terminal's Ctrl-Z actually does - a test bug
       where this was first sent to a single pid instead is exactly what
       led to finding the "double fork" stop-mirroring bug in item 5),
       `jobs`/`bg`/`fg`, and `wait %job`/`kill %job`. Line editing gets
       its own `PtySession` helper - a live `ush -i` connected to a real
       pseudo-terminal (`forkpty(3)`), not pipes, since
       `LineEditor::isUsable()` requires stdin/stdout to actually be a
       tty - covering cursor movement/backspace editing a command before
       it runs, arrow-key history recall, `^A`/`^K` line-clearing, and
       `Ctrl-C` aborting a line without killing the shell; `fc`/
       `history`/HISTFILE-persistence-across-sessions are covered via the
       existing pipe-based `InteractiveSession` (they don't need a real
       terminal - see item 8), and `History` itself has a direct Catch2
       unit-test file (`tests/runtime/test_history.cpp`) for its
       numbering/dedup/trim/load-save behavior in isolation. See the
       hard-corners note on why every one of these helpers sets
       `HISTFILE=/dev/null` (or a throwaway temp path) rather than
       leaving it unset.

Items 1-9 above are all now at least minimally done; "broad POSIX from
the start" meant the *architecture* supported the full grammar/expansion/
execution model from day one, and it has - what's left is breadth within
each piece (a couple of niche built-ins, completion, a real vi editing
mode) rather than architectural gaps.
