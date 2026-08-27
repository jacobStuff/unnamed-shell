// Control-flow "signals" for the executor, implemented as exceptions.
//
// `exit`, `return`, `break`, and `continue` (POSIX §2.14 special built-
// ins) all need to unwind an arbitrary number of stack frames - out of
// however many nested compound commands/function calls are in progress -
// to reach the point that knows what to do next (a loop body, a function
// call, or the top of the whole program). C++ exceptions are the natural
// tool for that; each is caught at exactly the scope the shell semantics
// say it should stop unwinding at (see executor.cpp).

#pragma once

namespace ush {

// `break n` / `continue n` (default n=1). Caught by a `for`/`while`/
// `until` loop body; if `levels > 1` when caught, the loop decrements it
// and rethrows so an enclosing loop keeps unwinding.
struct BreakSignal {
    int levels = 1;
};
struct ContinueSignal {
    int levels = 1;
};

// `return [n]`. Caught at a function call boundary (and, per POSIX, also
// valid inside a `.`/`eval` context - Executor::runSourceInCurrentContext
// catches it too as a documented simplification when there's no
// enclosing function call).
struct ReturnSignal {
    int status;
};

// `exit [n]`. Propagates all the way up to Executor::runProgram (or, for
// a command substitution's forked child, straight to _exit()).
struct ExitSignal {
    int status;
};

}  // namespace ush
