#include "lexer/lexer.hpp"

#include <cctype>

namespace ush {

Lexer::Lexer(std::string source) : src_(std::move(source)) {}

// ---------------------------------------------------------------------
// low-level character access
// ---------------------------------------------------------------------

bool Lexer::atEnd(std::size_t offset) const {
    return pos_ + offset >= src_.size();
}

char Lexer::peekChar(std::size_t offset) const {
    return atEnd(offset) ? '\0' : src_[pos_ + offset];
}

char Lexer::advanceChar() {
    char c = src_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

void Lexer::error(std::string message) const { errorAt(std::move(message), here()); }

void Lexer::errorAt(std::string message, SourceLoc loc) const {
    throw LexError(std::move(message), loc);
}

// ---------------------------------------------------------------------
// Word/WordPart building helpers
// ---------------------------------------------------------------------

void Lexer::appendPart(Word& parts, WordPart part) { parts.push_back(std::move(part)); }

void Lexer::appendText(Word& parts, WordPartKind kind, std::string_view text) {
    if (!parts.empty() && parts.back().kind == kind && kind != WordPartKind::DoubleQuoted) {
        parts.back().text.append(text);
    } else {
        parts.emplace_back(kind, std::string(text));
    }
}

void Lexer::appendChar(Word& parts, WordPartKind kind, char c) {
    appendText(parts, kind, std::string_view(&c, 1));
}

// ---------------------------------------------------------------------
// top-level driver (§2.3 Token Recognition)
// ---------------------------------------------------------------------

void Lexer::skipBlanksAndComments() {
    while (!atEnd()) {
        char c = peekChar();
        if (c == ' ' || c == '\t') {
            advanceChar();
            continue;
        }
        if (c == '#') {
            // Comments run to (but exclude) the next newline. This is only
            // ever invoked at a position where a new token could start, so
            // "foo#bar" (no separating blank) never reaches here - the '#'
            // there is just consumed as a literal word character by
            // scanWord, matching real-shell behavior.
            while (!atEnd() && peekChar() != '\n') advanceChar();
            continue;
        }
        break;
    }
}

Token Lexer::next() {
    skipBlanksAndComments();

    if (atEnd()) {
        Token tok;
        tok.type = TokenType::EndOfInput;
        tok.loc = here();
        return tok;
    }

    char c = peekChar();
    if (c == '\n') {
        Token tok;
        tok.loc = here();
        advanceChar();
        tok.type = TokenType::Newline;
        return tok;
    }

    if (c == '&' || c == '|' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')') {
        return lexOperator();
    }

    return lexWordOrIoNumber();
}

Token Lexer::lexOperator() {
    SourceLoc loc = here();
    char c = advanceChar();
    Token tok;
    tok.loc = loc;

    switch (c) {
        case '&':
            if (!atEnd() && peekChar() == '&') {
                advanceChar();
                tok.type = TokenType::AndIf;
            } else {
                tok.type = TokenType::And;
            }
            break;
        case '|':
            if (!atEnd() && peekChar() == '|') {
                advanceChar();
                tok.type = TokenType::OrIf;
            } else {
                tok.type = TokenType::Pipe;
            }
            break;
        case ';':
            if (!atEnd() && peekChar() == ';') {
                advanceChar();
                tok.type = TokenType::DSemi;
            } else {
                tok.type = TokenType::Semi;
            }
            break;
        case '(':
            tok.type = TokenType::LParen;
            break;
        case ')':
            tok.type = TokenType::RParen;
            break;
        case '<':
            if (!atEnd() && peekChar() == '<') {
                advanceChar();
                if (!atEnd() && peekChar() == '-') {
                    advanceChar();
                    tok.type = TokenType::DLessDash;
                } else {
                    tok.type = TokenType::DLess;
                }
            } else if (!atEnd() && peekChar() == '&') {
                advanceChar();
                tok.type = TokenType::LessAnd;
            } else if (!atEnd() && peekChar() == '>') {
                advanceChar();
                tok.type = TokenType::LessGreat;
            } else {
                tok.type = TokenType::Less;
            }
            break;
        case '>':
            if (!atEnd() && peekChar() == '>') {
                advanceChar();
                tok.type = TokenType::DGreat;
            } else if (!atEnd() && peekChar() == '&') {
                advanceChar();
                tok.type = TokenType::GreatAnd;
            } else if (!atEnd() && peekChar() == '|') {
                advanceChar();
                tok.type = TokenType::Clobber;
            } else {
                tok.type = TokenType::Great;
            }
            break;
        default:
            errorAt("internal lexer error: unexpected operator start '" + std::string(1, c) + "'", loc);
    }
    return tok;
}

Token Lexer::lexWordOrIoNumber() {
    SourceLoc loc = here();
    Word w = scanWord();

    // §2.10.1 rule 6: a token consisting solely of digits, immediately
    // (no blank) followed by '<' or '>', is an IO_NUMBER rather than a
    // WORD.
    bool allDigits = wordIsUnquotedLiteral(w) && !w.empty() && !w[0].text.empty();
    if (allDigits) {
        for (char ch : w[0].text) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) {
                allDigits = false;
                break;
            }
        }
    }

    Token tok;
    tok.loc = loc;
    if (allDigits && !atEnd() && (peekChar() == '<' || peekChar() == '>')) {
        tok.type = TokenType::IoNumber;
        tok.ioNumberText = w[0].text;
        return tok;
    }

    tok.type = TokenType::Word;
    tok.couldBeReservedWord = wordIsUnquotedLiteral(w);
    tok.word = std::move(w);
    return tok;
}

// ---------------------------------------------------------------------
// here-document bodies (§2.7.4)
// ---------------------------------------------------------------------

std::string Lexer::consumeHeredocBody(const std::string& delimiter, bool stripLeadingTabs) {
    std::string body;
    while (true) {
        std::string line;
        while (!atEnd() && peekChar() != '\n') line += advanceChar();
        bool hadNewline = !atEnd();  // current char (if any) is the '\n'
        if (hadNewline) advanceChar();

        if (stripLeadingTabs) {
            std::size_t i = 0;
            while (i < line.size() && line[i] == '\t') ++i;
            line.erase(0, i);
        }

        if (line == delimiter) return body;

        if (!hadNewline) {
            errorAt("unterminated here-document: missing delimiter '" + delimiter + "'", here());
        }
        body += line;
        body += '\n';
    }
}

// ---------------------------------------------------------------------
// word scanning
// ---------------------------------------------------------------------

Word Lexer::scanWord() { return scanWordImpl(/*stopAtBlankOrOperator=*/true); }

Word Lexer::scanWordUntilEnd() { return scanWordImpl(/*stopAtBlankOrOperator=*/false); }

Word Lexer::scanWordImpl(bool stopAtBlankOrOperator) {
    Word parts;
    while (!atEnd()) {
        char c = peekChar();
        if (stopAtBlankOrOperator) {
            if (c == ' ' || c == '\t' || c == '\n') break;
            if (c == '&' || c == '|' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')') {
                break;
            }
        }

        if (c == '\\') {
            advanceChar();
            if (atEnd()) {
                appendChar(parts, WordPartKind::Literal, '\\');
                break;
            }
            char n = peekChar();
            if (n == '\n') {
                advanceChar();  // line continuation: both chars vanish
                continue;
            }
            appendChar(parts, WordPartKind::SingleQuoted, advanceChar());
            continue;
        }
        if (c == '\'') {
            advanceChar();
            scanSingleQuoted(parts);
            continue;
        }
        if (c == '"') {
            advanceChar();
            scanDoubleQuoted(parts);
            continue;
        }
        if (c == '$') {
            advanceChar();
            scanDollar(parts);
            continue;
        }
        if (c == '`') {
            advanceChar();
            scanBacktick(parts);
            continue;
        }
        appendChar(parts, WordPartKind::Literal, advanceChar());
    }
    return parts;
}

void Lexer::scanSingleQuoted(Word& parts) {
    // Opening quote already consumed. No character is special inside
    // single quotes, not even backslash (§2.2.2) - copy verbatim until the
    // next "'".
    std::string buffer;
    while (true) {
        if (atEnd()) errorAt("unterminated single-quoted string: missing closing \"'\"", here());
        char c = advanceChar();
        if (c == '\'') break;
        buffer += c;
    }
    appendPart(parts, WordPart(WordPartKind::SingleQuoted, std::move(buffer)));
}

void Lexer::scanDoubleQuoted(Word& parts) {
    // Opening quote already consumed.
    Word inner;
    scanExpansionAwareBody(inner, /*stopAtDoubleQuote=*/true);
    WordPart dq(WordPartKind::DoubleQuoted);
    dq.parts = std::move(inner);
    appendPart(parts, std::move(dq));
}

Word Lexer::scanExpansionsUntilEnd() {
    Word parts;
    scanExpansionAwareBody(parts, /*stopAtDoubleQuote=*/false);
    return parts;
}

void Lexer::scanExpansionAwareBody(Word& parts, bool stopAtDoubleQuote) {
    // Inside double quotes, only $ ` " \ are recognized as special
    // (§2.2.3); single quotes are NOT special. Same rules apply to the
    // other raw text this is used for (see the header comment on
    // scanExpansionsUntilEnd()).
    while (true) {
        if (atEnd()) {
            if (stopAtDoubleQuote) {
                errorAt("unterminated double-quoted string: missing closing '\"'", here());
            }
            return;
        }
        char c = peekChar();
        if (stopAtDoubleQuote && c == '"') {
            advanceChar();
            return;
        }
        if (c == '\\') {
            advanceChar();
            if (atEnd()) {
                appendChar(parts, WordPartKind::Literal, '\\');
                continue;  // top-of-loop atEnd() check handles what happens next
            }
            char n = peekChar();
            if (n == '$' || n == '`' || n == '"' || n == '\\') {
                appendChar(parts, WordPartKind::SingleQuoted, advanceChar());
            } else if (n == '\n') {
                advanceChar();  // line continuation: both chars vanish
            } else {
                // Backslash keeps its literal meaning; the following
                // character is processed on its own next iteration.
                appendChar(parts, WordPartKind::Literal, '\\');
            }
            continue;
        }
        if (c == '$') {
            advanceChar();
            scanDollar(parts);
            continue;
        }
        if (c == '`') {
            advanceChar();
            scanBacktick(parts);
            continue;
        }
        appendChar(parts, WordPartKind::Literal, advanceChar());
    }
}

// ---------------------------------------------------------------------
// $ / ` expansion scanning (§2.6)
// ---------------------------------------------------------------------

void Lexer::scanDollar(Word& parts) {
    // '$' already consumed by caller.
    if (atEnd()) {
        appendChar(parts, WordPartKind::Literal, '$');
        return;
    }

    char c = peekChar();

    if (c == '(') {
        advanceChar();  // consume first '('
        if (!atEnd() && peekChar() == '(') {
            std::string text;
            bool isArith = scanDollarDoubleParen(text);
            appendPart(parts, WordPart(isArith ? WordPartKind::ArithExpansion
                                                : WordPartKind::CommandSubDollar,
                                        std::move(text)));
        } else {
            std::string text = scanBalancedParens();
            appendPart(parts, WordPart(WordPartKind::CommandSubDollar, std::move(text)));
        }
        return;
    }

    if (c == '{') {
        advanceChar();  // consume '{'
        std::string text = scanBalancedBraces();
        appendPart(parts, WordPart(WordPartKind::ParamExpansion, std::move(text)));
        return;
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        std::string name;
        while (!atEnd()) {
            char n = peekChar();
            if (std::isalnum(static_cast<unsigned char>(n)) || n == '_') {
                name += advanceChar();
            } else {
                break;
            }
        }
        appendPart(parts, WordPart(WordPartKind::ParamExpansion, std::move(name)));
        return;
    }

    if (std::isdigit(static_cast<unsigned char>(c))) {
        // Positional parameter: exactly one digit. Multi-digit positional
        // parameters need braces, e.g. ${10} (§2.5.2).
        std::string name(1, advanceChar());
        appendPart(parts, WordPart(WordPartKind::ParamExpansion, std::move(name)));
        return;
    }

    static constexpr std::string_view kSpecialParams = "@*#?-$!";
    if (kSpecialParams.find(c) != std::string_view::npos) {
        std::string name(1, advanceChar());
        appendPart(parts, WordPart(WordPartKind::ParamExpansion, std::move(name)));
        return;
    }

    // Nothing recognizable follows - '$' is just a literal character.
    appendChar(parts, WordPartKind::Literal, '$');
}

void Lexer::scanBacktick(Word& parts) {
    // Opening '`' already consumed. Per §2.6.3, inside a backquoted command
    // substitution a backslash keeps its literal meaning except before
    // '$', '`', or '\\' (where it escapes that character, e.g. to nest
    // another backquoted substitution).
    std::string buffer;
    while (true) {
        if (atEnd()) errorAt("unterminated command substitution: missing closing '`'", here());
        char c = advanceChar();
        if (c == '`') break;
        if (c == '\\' && !atEnd()) {
            char n = peekChar();
            if (n == '$' || n == '`' || n == '\\') {
                buffer += advanceChar();  // drop the backslash, keep the escaped char
                continue;
            }
        }
        buffer += c;
    }
    appendPart(parts, WordPart(WordPartKind::CommandSubBacktick, std::move(buffer)));
}

// ---------------------------------------------------------------------
// balanced-text scanning for $( ), ${ }, $(( ))
// ---------------------------------------------------------------------

void Lexer::copyQuotedRaw(char quoteChar, std::string& buffer) {
    while (true) {
        if (atEnd()) errorAt("unterminated quoted string", here());
        char c = advanceChar();
        buffer += c;
        if (quoteChar == '"' && c == '\\' && !atEnd()) {
            buffer += advanceChar();
            continue;
        }
        if (c == quoteChar) return;
    }
}

std::string Lexer::scanBalancedParens() {
    // Precondition: the opening '(' has already been consumed.
    std::string buffer;
    int depth = 1;
    while (true) {
        if (atEnd()) errorAt("unterminated command substitution: missing ')'", here());
        char c = advanceChar();

        if (c == '\\') {
            buffer += c;
            if (!atEnd()) buffer += advanceChar();
            continue;
        }
        if (c == '\'' || c == '"') {
            buffer += c;
            copyQuotedRaw(c, buffer);
            continue;
        }
        if (c == '`') {
            // Copy a nested backquoted substitution verbatim so an
            // unquoted ')' inside it can't end this one early.
            buffer += c;
            while (true) {
                if (atEnd()) errorAt("unterminated command substitution: missing closing '`'", here());
                char b = advanceChar();
                buffer += b;
                if (b == '\\' && !atEnd()) {
                    buffer += advanceChar();
                    continue;
                }
                if (b == '`') break;
            }
            continue;
        }
        if (c == '(') {
            ++depth;
            buffer += c;
            continue;
        }
        if (c == ')') {
            --depth;
            if (depth == 0) return buffer;
            buffer += c;
            continue;
        }
        buffer += c;
    }
}

std::string Lexer::scanBalancedBraces() {
    // Precondition: the opening '{' (of "${") has already been consumed.
    std::string buffer;
    int depth = 1;
    while (true) {
        if (atEnd()) errorAt("unterminated parameter expansion: missing '}'", here());
        char c = advanceChar();

        if (c == '\\') {
            buffer += c;
            if (!atEnd()) buffer += advanceChar();
            continue;
        }
        if (c == '\'' || c == '"') {
            buffer += c;
            copyQuotedRaw(c, buffer);
            continue;
        }
        if (c == '`') {
            buffer += c;
            while (true) {
                if (atEnd()) errorAt("unterminated parameter expansion: missing closing '`'", here());
                char b = advanceChar();
                buffer += b;
                if (b == '\\' && !atEnd()) {
                    buffer += advanceChar();
                    continue;
                }
                if (b == '`') break;
            }
            continue;
        }
        if (c == '$') {
            buffer += c;
            if (!atEnd() && peekChar() == '{') {
                buffer += advanceChar();
                ++depth;
            }
            continue;
        }
        if (c == '}') {
            --depth;
            if (depth == 0) return buffer;
            buffer += c;
            continue;
        }
        buffer += c;
    }
}

bool Lexer::scanDollarDoubleParen(std::string& text) {
    std::size_t savedPos = pos_;
    std::size_t savedLine = line_;
    std::size_t savedColumn = column_;

    auto restoreAndFallBack = [&]() {
        pos_ = savedPos;
        line_ = savedLine;
        column_ = savedColumn;
        text = scanBalancedParens();
    };

    advanceChar();  // consume the second '('
    std::string buffer;
    int depth = 0;
    while (true) {
        if (atEnd()) {
            restoreAndFallBack();
            return false;
        }
        char c = advanceChar();

        if (c == '\\') {
            buffer += c;
            if (!atEnd()) buffer += advanceChar();
            continue;
        }
        if (c == '\'' || c == '"') {
            buffer += c;
            try {
                copyQuotedRaw(c, buffer);
            } catch (const LexError&) {
                restoreAndFallBack();
                return false;
            }
            continue;
        }
        if (c == '(') {
            ++depth;
            buffer += c;
            continue;
        }
        if (c == ')') {
            if (depth > 0) {
                --depth;
                buffer += c;
                continue;
            }
            // depth == 0: this may be the first ')' of the closing "))".
            if (!atEnd() && peekChar() == ')') {
                advanceChar();
                text = std::move(buffer);
                return true;
            }
            restoreAndFallBack();
            return false;
        }
        buffer += c;
    }
}

}  // namespace ush
