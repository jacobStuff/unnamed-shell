// Lexer: turns shell source text into a Token stream.
//
// Implements POSIX.1-2017 Shell & Utilities §2.3 "Token Recognition" (the
// low-level tokenizing algorithm) together with the quoting rules of §2.2
// and the substitution-scanning rules of §2.6.2/§2.6.3/§2.6.4. It does NOT
// implement §2.10.2 grammar (reserved word recognition in context, here-
// document body extraction) - that is the parser's job; see
// docs/DESIGN.md.

#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "lexer/token.hpp"

namespace ush {

// Thrown for lexical errors: unterminated quotes, unterminated
// substitutions, etc.
class LexError : public std::runtime_error {
public:
    LexError(std::string message, SourceLoc loc)
        : std::runtime_error(std::move(message)), loc_(loc) {}

    const SourceLoc& location() const { return loc_; }

private:
    SourceLoc loc_;
};

class Lexer {
public:
    explicit Lexer(std::string source);

    // Returns the next token and advances past it. Returns a token with
    // type == TokenType::EndOfInput at end of input (and on every call
    // after that).
    Token next();

    // Reads a here-document body (§2.7.4): consumes whole lines up to and
    // including the first line that equals `delimiter` (after stripping
    // leading tabs from it too, when `stripLeadingTabs` is set), and
    // returns everything before that line (with leading tabs stripped from
    // each line when `stripLeadingTabs` is set). The delimiter line need
    // not end in a newline if it's the last thing in the input.
    //
    // Precondition: the lexer's position is exactly at the start of a
    // line, i.e. this is called right after Lexer::next() has produced a
    // NEWLINE token (or, transitively, right after a previous
    // consumeHeredocBody() call, for back-to-back here-documents on one
    // line). See docs/DESIGN.md and Parser::advance().
    //
    // Throws LexError if the delimiter line is never found before the end
    // of input.
    std::string consumeHeredocBody(const std::string& delimiter, bool stripLeadingTabs);

private:
    std::string src_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;

    // --- low-level character access -----------------------------------
    bool atEnd(std::size_t offset = 0) const;
    char peekChar(std::size_t offset = 0) const;
    char advanceChar();  // consumes one char, updates line_/column_
    SourceLoc here() const { return SourceLoc{line_, column_}; }

    // --- token-level helpers --------------------------------------------
    void skipBlanksAndComments();  // §2.3: blanks between tokens, '#' comments
                                    // (does NOT skip newlines - newline is a
                                    // token)
    Token lexOperator();
    Token lexWordOrIoNumber();

    // Scans one shell word starting at the current position (current char
    // is the first character of the word - a non-blank, non-newline,
    // non-operator-start character). Stops at the first unquoted blank,
    // newline, or operator-start character.
    Word scanWord();

    // Appends one raw character to the "current" WordPart of `parts`,
    // starting a new part if the last part isn't of kind `kind` (keeps
    // adjacent same-kind runs merged instead of exploding into one part
    // per character).
    static void appendChar(Word& parts, WordPartKind kind, char c);
    static void appendText(Word& parts, WordPartKind kind, std::string_view text);
    static void appendPart(Word& parts, WordPart part);

    // Scans '...' (opening quote already consumed). Appends a
    // SingleQuoted part to `parts`.
    void scanSingleQuoted(Word& parts);

    // Scans "..." (opening quote already consumed). Appends a
    // DoubleQuoted part (with nested parts for $ / ` expansions) to
    // `parts`.
    void scanDoubleQuoted(Word& parts);

    // Scans the inside of double quotes OR the top level of a word,
    // handling '$' expansions and (for double-quote context) backslash
    // rules that differ from bare-word context. `inDoubleQuotes` selects
    // which backslash rule set applies (§2.2.2 vs §2.2.3) and whether
    // single-quotes/further double-quotes are recognized as nested quoting
    // (they are not, inside double quotes - only $ ` \ " are special).
    void scanWordBody(Word& parts, bool inDoubleQuotes);

    // Called when we see an unescaped '$' at the current position (already
    // consumed). Appends the resulting expansion part (ParamExpansion /
    // CommandSubDollar / ArithExpansion), or a literal "$" if what follows
    // doesn't form a valid expansion.
    void scanDollar(Word& parts);

    // Called when we see an unescaped '`' at the current position (opening
    // backtick already consumed). Appends a CommandSubBacktick part.
    void scanBacktick(Word& parts);

    // Scans balanced text starting just after an opening '(' (already
    // consumed) up to and including its matching ')', honoring nested
    // quotes/parens/backslash-escapes. Returns the text strictly between
    // the parens (not including them). Used for $( ... ).
    std::string scanBalancedParens();

    // Scans $(( ... )) content, implementing the arithmetic-vs-command-
    // substitution disambiguation documented in docs/DESIGN.md.
    // Precondition: scanDollar has consumed '$' and the FIRST '(' of the
    // pair (so `pos_` points at what would be the second '('), and has
    // peeked - without consuming - that this next character is also '('.
    // On success (this really is arithmetic), consumes through the
    // matching "))", sets `text` to the expression text, and returns true.
    // On failure (it turns out to be a command substitution whose content
    // starts with '(', e.g. "$((a) (b))"), restores the position to just
    // after the already-consumed first '(' and delegates to
    // scanBalancedParens(), setting `text` to its result and returning
    // false.
    bool scanDollarDoubleParen(std::string& text);

    // Scans ${ ... } content (opening "${" already consumed). Returns the
    // raw text between the braces, honoring nested braces/quotes.
    //
    // Known limitation (see docs/DESIGN.md): brace-depth tracking only
    // treats "${" as opening a nested level; a bare, unquoted '{' with no
    // preceding '$' is not tracked as nesting.
    std::string scanBalancedBraces();

    // Consumes raw text up to and including the next unescaped occurrence
    // of `quoteChar`, appending everything (including escapes and the
    // closing quote itself) verbatim to `buffer`. For quoteChar == '"',
    // a backslash immediately before any character escapes it (both are
    // copied verbatim, and the escaped character can't end the string).
    // For quoteChar == '\'', backslash has no special meaning. Throws
    // LexError if input ends before the closing quote is found.
    // Precondition: the opening quote character has already been consumed
    // and appended to `buffer` by the caller.
    void copyQuotedRaw(char quoteChar, std::string& buffer);

    [[noreturn]] void error(std::string message) const;
    [[noreturn]] void errorAt(std::string message, SourceLoc loc) const;
};

}  // namespace ush
