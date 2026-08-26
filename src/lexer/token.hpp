// Token and Word representation produced by the lexer.
//
// See docs/DESIGN.md, "Word representation (deferred quote removal)" for
// the rationale behind representing a word as a tree of WordParts instead
// of a flat, sentinel-stuffed string.
//
// Spec references are to POSIX.1-2017, Shell & Utilities, Chapter 2.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ush {

// A position in the original source, for diagnostics.
struct SourceLoc {
    std::size_t line = 1;
    std::size_t column = 1;
};

enum class WordPartKind {
    Literal,           // unquoted literal text; subject to later expansion
                        // (parameter/tilde detection, field splitting,
                        // pathname expansion)
    SingleQuoted,       // text from '...', AND single backslash-escaped
                        // characters outside/inside quotes. Both mean the
                        // same thing downstream: fully protected text.
                        // (§2.2.2, §2.2.3)
    DoubleQuoted,       // text from "...". No field splitting/globbing on
                        // the whole, but the nested `parts` can still
                        // contain expansions (§2.2.3).
    ParamExpansion,     // $name or ${...}. `text` holds the raw body
                        // (without the $ / ${ } delimiters) for the
                        // expansion stage to parse (§2.6.2).
    CommandSubDollar,   // $( ... ). `text` holds the raw inner command
                        // text (§2.6.3).
    CommandSubBacktick, // `...`. `text` holds the raw inner command text,
                        // with backtick-style backslash rules already
                        // applied (§2.6.3).
    ArithExpansion,     // $(( ... )). `text` holds the raw expression text
                        // (§2.6.4).
};

struct WordPart {
    WordPartKind kind;
    std::string text;             // meaning depends on `kind`, see above
    std::vector<WordPart> parts;  // only populated when kind == DoubleQuoted

    explicit WordPart(WordPartKind k, std::string t = {})
        : kind(k), text(std::move(t)) {}
};

using Word = std::vector<WordPart>;

// True if a Word is exactly one, entirely-unquoted Literal part - i.e. it
// is eligible to be checked against the reserved-word set or treated as an
// assignment word, per the "none of the characters are quoted" rule
// (§2.10.2 rule 7b, §2.10.1 rule 1). The parser decides *whether* a
// reserved word is expected at this grammar position; the lexer only
// reports whether the token *could* be one.
bool wordIsUnquotedLiteral(const Word& word);

// Returns the literal text if wordIsUnquotedLiteral(word) is true,
// otherwise an empty string. Convenience for the parser.
std::string wordAsUnquotedLiteral(const Word& word);

enum class TokenType {
    Word,
    Newline,
    EndOfInput,

    // Control operators (§2.10.1 "operator" token rule; §2.10.2 grammar
    // token names in parentheses below)
    And,        // &
    AndIf,      // && (AND_IF)
    Pipe,       // |
    OrIf,       // || (OR_IF)
    Semi,       // ;
    DSemi,      // ;; (DSEMI)
    LParen,     // (
    RParen,     // )

    // Redirection operators (§2.7)
    Less,       // <
    Great,      // >
    DGreat,     // >> (DGREAT)
    DLess,      // << (DLESS)
    DLessDash,  // <<- (DLESSDASH)
    LessAnd,    // <& (LESSAND)
    GreatAnd,   // >& (GREATAND)
    LessGreat,  // <> (LESSGREAT)
    Clobber,    // >| (CLOBBER)

    IoNumber,   // digit sequence immediately preceding a redirection
                // operator (§2.10.1 rule 6)
};

struct Token {
    TokenType type = TokenType::EndOfInput;
    SourceLoc loc;

    // Valid when type == Word.
    Word word;

    // True iff `word` is exactly one, fully-unquoted Literal part -
    // cached here so the parser doesn't need to re-walk the tree to decide
    // "could this be a reserved word / assignment word?".
    bool couldBeReservedWord = false;

    // Valid when type == IoNumber: the digits, as text (kept as text
    // rather than parsed to int here; the parser/executor decides what
    // range is valid).
    std::string ioNumberText;
};

const char* tokenTypeName(TokenType type);

}  // namespace ush
