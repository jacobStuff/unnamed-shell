// Arithmetic expansion, POSIX.1-2017 Shell & Utilities §2.6.4.
//
// POSIX specifies the operators, precedence, and associativity to be
// "as specified in the ISO C standard" for its arithmetic operators, and
// lists them explicitly (decreasing precedence):
//
//   ( )                                grouping
//   unary + - ! ~
//   * / %
//   binary + -
//   << >>
//   < <= > >=
//   == !=
//   &
//   ^
//   |
//   &&
//   ||
//   ?:
//   = += -= *= /= %= &= ^= |= <<= >>=
//   ,
//
// evaluated over signed integers (here, intmax_t). A bare identifier is
// looked up as a shell variable (like an unquoted parameter expansion);
// assignment operators write the result back to the shell variable.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace ush {

class Environment;

class ArithError : public std::runtime_error {
public:
    explicit ArithError(std::string message) : std::runtime_error(std::move(message)) {}
};

// Evaluates `expr` as a POSIX arithmetic expression. Bare identifiers
// resolve against `env` (unset, or set to a non-numeric value, evaluates
// as 0 - see the note in arithmetic.cpp on the one deliberate
// simplification here). Assignment operators mutate `env`. Throws
// ArithError on a malformed expression or division/modulo by zero.
std::intmax_t evaluateArithmetic(const std::string& expr, Environment& env);

}  // namespace ush
