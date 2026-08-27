// Parser: turns a Lexer's token stream into an ast::List, implementing
// POSIX.1-2017 Shell & Utilities §2.10.2 "Shell Grammar Rules".
//
// Documented simplification vs. the strict grammar: reserved words (see
// reservedWords() below) are recognized ONLY as the very first token of a
// `command` - i.e. before any leading redirects or assignment-words of
// that command have been consumed. In strict POSIX, a leading redirect
// (but not an assignment-word) may still permit the word after it to be
// recognized as reserved (e.g. arguably ">f if ...; fi"). ush treats
// anything after the first token of a command as never reserved, which
// matches every real shell's behavior once an assignment-word has been
// consumed (e.g. "FOO=bar if" runs a program literally named "if") and is
// a defensible, simple choice for the redirect case too, which is
// vanishingly rare in real scripts. See docs/DESIGN.md.

#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ast/ast.hpp"
#include "lexer/lexer.hpp"
#include "lexer/token.hpp"

namespace ush {

class ParseError : public std::runtime_error {
public:
    ParseError(std::string message, SourceLoc loc)
        : std::runtime_error(std::move(message)), loc_(loc) {}

    const SourceLoc& location() const { return loc_; }

private:
    SourceLoc loc_;
};

class Parser {
public:
    explicit Parser(std::string source);

    // Parses the entire input as one top-level compound_list and requires
    // EndOfInput immediately after (i.e. no stray closing keyword/paren
    // left over). Throws ParseError or LexError on failure.
    ast::List parseProgram();

private:
    Lexer lexer_;
    Token current_;
    std::deque<Token> lookahead_;

    // Here-documents registered on the current source line, in the order
    // their "<<"/"<<-" operators were parsed, awaiting their bodies. See
    // advance().
    std::vector<std::shared_ptr<ast::HereDoc>> pendingHeredocs_;

    // --- token stream ---------------------------------------------------

    // Advances current_ to the next token (from the lookahead buffer if
    // non-empty, else straight from the lexer). If the token being left
    // behind is a NEWLINE, first drains pendingHeredocs_ by reading their
    // bodies directly from the lexer - this is the one place that must
    // happen, since here-document content begins exactly at the source
    // position the lexer is left at after producing a NEWLINE token, and
    // must be consumed before any further tokenizing (which would
    // otherwise try to lex the here-document body as shell syntax).
    void advance();

    // Returns the token `n` positions after current_ (n=1 is the very
    // next token), buffering as needed. Never looks past a NEWLINE - see
    // the .cpp for why every current caller is already safe (short-circuit
    // evaluation) and what would need to change if that stops being true.
    const Token& peekAhead(std::size_t n);

    // --- reserved words (§2.4) ------------------------------------------
    static std::optional<std::string> reservedWordIfAny(const Token& tok);
    static bool isReservedWord(const Token& tok, std::string_view word);
    void expectReservedWord(std::string_view word);

    static bool isValidName(const std::string& s);

    struct AssignmentSplit {
        std::string name;
        Word value;
    };
    // Recognizes ASSIGNMENT_WORD per rule 1: an unquoted NAME, an unquoted
    // '=', then the rest of the word (which may itself contain quoting/
    // expansions) as the value.
    static std::optional<AssignmentSplit> trySplitAssignment(const Word& word);

    struct FlattenedWord {
        std::string text;
        bool anyQuoted = false;
    };
    // Flattens a Word made of only Literal/SingleQuoted/DoubleQuoted parts
    // into plain text (quote removal), tracking whether any quoting was
    // present. Parameter/command/arithmetic expansion parts contribute no
    // text (see docs/DESIGN.md) - fine for the two places this is used
    // (here-document delimiters, rule 3; nothing else yet), where a live
    // expansion would be highly unusual.
    static FlattenedWord flattenSimpleWord(const Word& word);

    // --- grammar productions (§2.10.2) ----------------------------------
    ast::List parseCompoundList();  // linebreak term
    ast::List parseTerm();          // term
    static bool canStartCommand(const Token& tok);
    void skipLinebreak();
    void expectSequentialSep();  // ';' linebreak | newline_list

    ast::AndOr parseAndOr();
    ast::Pipeline parsePipeline();
    std::unique_ptr<ast::Command> parseCommand();
    ast::CompoundCommand parseCompoundCommandBody();
    ast::SimpleCommand parseSimpleCommand();
    ast::FunctionDefinition parseFunctionDefinition();

    ast::BraceGroup parseBraceGroup();
    ast::Subshell parseSubshell();
    ast::ForClause parseForClause();
    ast::CaseClause parseCaseClause();
    std::vector<Word> parsePatternList();
    ast::IfClause parseIfClause();
    ast::WhileClause parseWhileClause();
    ast::UntilClause parseUntilClause();
    ast::List parseDoGroup();

    bool isRedirectStart(const Token& tok) const;
    std::vector<ast::Redirect> parseRedirectList();
    ast::Redirect parseRedirect();

    void expectOp(TokenType type, const char* what);

    [[noreturn]] void error(const std::string& message) const;
};

}  // namespace ush
