// Word expansion, POSIX.1-2017 Shell & Utilities §2.6, minus field
// splitting (§2.6.5) and pathname expansion (§2.6.6), which are separate,
// later stages that consume this one's output (see docs/DESIGN.md). This
// covers, in the single left-to-right pass §2.6 describes:
//
//   - tilde expansion       (§2.6.1)
//   - parameter expansion   (§2.6.2), including the full ${...} operator
//     grammar (:- := :? :+ - = ? + # ## % %%) and $#/$@/$*/$?/$$/$!/$0/
//     positional parameters
//   - command substitution  (§2.6.3), via an injected CommandRunner so
//     this module doesn't need to depend on the (not yet written)
//     executor
//   - arithmetic expansion  (§2.6.4), via expand/arithmetic.hpp
//
// Quote removal (§2.6.7) is implicit in the output representation (see
// ExpansionPiece below) and available immediately via flatten() for
// contexts that don't need field splitting/pathname expansion at all
// (assignment values, case subjects, here-document delimiters, ...).

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "lexer/token.hpp"

namespace ush {

class Environment;

class ExpansionError : public std::runtime_error {
public:
    explicit ExpansionError(std::string message) : std::runtime_error(std::move(message)) {}
};

// One contiguous run of already-expanded text, tagged with whether it
// came from quoted source (single/double quotes, a backslash escape, or -
// even though the source wasn't literally quoted - a quoted "$@"/"$*"
// element). `quoted` pieces are never eligible for field splitting
// (§2.6.5) or pathname-expansion metacharacter interpretation (§2.6.6).
//
// `text` never includes the quote characters themselves (the lexer's
// WordPart::text already excludes them), so quote removal (§2.6.7) is
// just concatenating every piece's text regardless of `quoted` - see
// flatten().
//
// `fieldBreakAfter` forces a field boundary immediately after this piece,
// independent of IFS. It exists only for $@ (quoted or not): each
// positional parameter must become its own field regardless of IFS,
// including in the pathological case where IFS has been changed to
// exclude whitespace entirely. Every other expansion relies on ordinary
// IFS-based splitting instead.
struct ExpansionPiece {
    std::string text;
    bool quoted = false;
    bool fieldBreakAfter = false;
};
using ExpandedWord = std::vector<ExpansionPiece>;

// Quote removal (§2.6.7): concatenates every piece's text, discarding the
// quoted/unquoted distinction and any field-break markers.
std::string flatten(const ExpandedWord& word);

// Runs a shell command list and captures its standard output, for command
// substitution (§2.6.3). Implemented by the executor. Expander accepts a
// null CommandRunner for contexts where command substitution isn't
// available (e.g. many unit tests); attempting to use it in that case
// throws ExpansionError.
class CommandRunner {
public:
    virtual ~CommandRunner() = default;
    virtual std::string runAndCaptureStdout(const std::string& source) = 0;
};

class Expander {
public:
    explicit Expander(Environment& env, CommandRunner* runner = nullptr)
        : env_(env), runner_(runner) {}

    // Expands a single Word: tilde, parameter, command substitution, and
    // arithmetic expansion. Field splitting and pathname expansion are
    // not performed here - see the file header comment.
    ExpandedWord expand(const Word& word);

private:
    Environment& env_;
    CommandRunner* runner_;

    void expandPartsInto(const Word& word, std::size_t startIndex, ExpandedWord& out);
    void expandPart(const WordPart& part, ExpandedWord& out);
    void expandTildeAndFirstPart(const std::string& text, ExpandedWord& out);

    // Bare (no ${...} operator) $@ or $*, either at the top level
    // (`quoted` false) or as the direct content of a double-quoted string
    // (`quoted` true) - §2.5.2's special multi-field ($@) / IFS-joined
    // ($*) rules.
    void expandAtOrStar(const std::string& name, bool quoted, ExpandedWord& out);

    // Parses and evaluates the raw text captured for a ParamExpansion
    // WordPart (e.g. "foo", "bar:-baz", "#arr", "1") - the ${...}
    // sub-grammar of §2.6.2. Handles everything except a bare, operator-
    // less "@" or "*", which expandPart intercepts first (see
    // expandAtOrStar).
    void expandParameter(const std::string& raw, ExpandedWord& out);

    // §2.5.2 lookup rules for both plain parameter expansion and
    // arithmetic's bare-identifier references: named variables via
    // `env_`, positional parameters, and the special parameters other
    // than the "joined" forms of $@/$* (handled by expandAtOrStar
    // instead). Returns nullopt for an unset variable or an out-of-range
    // positional parameter.
    std::optional<std::string> lookupParameter(const std::string& name);
    std::size_t parameterLength(const std::string& name);

    // Re-lexes `raw` as a full word (quotes, backslash escapes, and $ / `
    // expansions all meaningful - see Lexer::scanWordUntilEnd()) and
    // expands it. Used for the operand of every ${...} operator.
    ExpandedWord expandOperandWord(const std::string& raw);

    std::string doCommandSubstitution(const std::string& rawSource);

    // Re-lexes `raw` applying the same rules as double-quoted text (only
    // $ / ` special - see Lexer::scanExpansionsUntilEnd()), expands the
    // result (deliberately not through expand(), to avoid misinterpreting
    // a leading '~' as a tilde-prefix instead of the bitwise-NOT
    // operator), flattens it, and evaluates it as an arithmetic
    // expression (§2.6.4).
    std::intmax_t evaluateArithmeticExpansion(const std::string& rawExpr);
};

}  // namespace ush
