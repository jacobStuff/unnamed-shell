#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "lexer/lexer.hpp"
#include "lexer/token.hpp"

using namespace ush;

namespace {

std::vector<Token> tokenize(const std::string& src) {
    Lexer lexer(src);
    std::vector<Token> toks;
    while (true) {
        Token t = lexer.next();
        bool isEnd = t.type == TokenType::EndOfInput;
        toks.push_back(std::move(t));
        if (isEnd) break;
    }
    return toks;
}

// True if `w` is exactly one, fully-unquoted Literal part equal to `text`.
bool isPlainWord(const Word& w, const std::string& text) {
    return w.size() == 1 && w[0].kind == WordPartKind::Literal && w[0].text == text;
}

}  // namespace

TEST_CASE("simple words separated by blanks", "[lexer]") {
    auto toks = tokenize("echo hello world");
    REQUIRE(toks.size() == 4);  // 3 words + EOF
    CHECK(isPlainWord(toks[0].word, "echo"));
    CHECK(isPlainWord(toks[1].word, "hello"));
    CHECK(isPlainWord(toks[2].word, "world"));
    CHECK(toks[3].type == TokenType::EndOfInput);
}

TEST_CASE("control operators are recognized", "[lexer]") {
    auto toks = tokenize("a && b || c ; d & e | f ;; g");
    std::vector<TokenType> types;
    for (auto& t : toks) types.push_back(t.type);
    std::vector<TokenType> expected = {
        TokenType::Word, TokenType::AndIf, TokenType::Word, TokenType::OrIf,
        TokenType::Word, TokenType::Semi,  TokenType::Word, TokenType::And,
        TokenType::Word, TokenType::Pipe,  TokenType::Word, TokenType::DSemi,
        TokenType::Word, TokenType::EndOfInput,
    };
    REQUIRE(types == expected);
}

TEST_CASE("parens and newline tokens", "[lexer]") {
    auto toks = tokenize("( a )\nb");
    std::vector<TokenType> types;
    for (auto& t : toks) types.push_back(t.type);
    std::vector<TokenType> expected = {
        TokenType::LParen, TokenType::Word, TokenType::RParen, TokenType::Newline,
        TokenType::Word,   TokenType::EndOfInput,
    };
    REQUIRE(types == expected);
}

TEST_CASE("redirection operators", "[lexer]") {
    auto toks = tokenize("a < b > c >> d << e <<- f <& g >& h <> i >| j");
    std::vector<TokenType> types;
    for (auto& t : toks) types.push_back(t.type);
    std::vector<TokenType> expected = {
        TokenType::Word, TokenType::Less,      TokenType::Word, TokenType::Great,
        TokenType::Word, TokenType::DGreat,    TokenType::Word, TokenType::DLess,
        TokenType::Word, TokenType::DLessDash, TokenType::Word, TokenType::LessAnd,
        TokenType::Word, TokenType::GreatAnd,  TokenType::Word, TokenType::LessGreat,
        TokenType::Word, TokenType::Clobber,   TokenType::Word, TokenType::EndOfInput,
    };
    REQUIRE(types == expected);
}

TEST_CASE("digits immediately before a redirection operator become IO_NUMBER", "[lexer]") {
    auto toks = tokenize("2>&1");
    REQUIRE(toks.size() == 4);
    CHECK(toks[0].type == TokenType::IoNumber);
    CHECK(toks[0].ioNumberText == "2");
    CHECK(toks[1].type == TokenType::GreatAnd);
    CHECK(isPlainWord(toks[2].word, "1"));
    CHECK(toks[3].type == TokenType::EndOfInput);
}

TEST_CASE("a blank between digits and redirection prevents IO_NUMBER", "[lexer]") {
    auto toks = tokenize("2 <file");
    REQUIRE(toks.size() == 4);
    CHECK(isPlainWord(toks[0].word, "2"));
    CHECK(toks[1].type == TokenType::Less);
    CHECK(isPlainWord(toks[2].word, "file"));
}

TEST_CASE("digits mixed with letters are not IO_NUMBER", "[lexer]") {
    auto toks = tokenize("12abc<foo");
    REQUIRE(toks.size() == 4);
    CHECK(isPlainWord(toks[0].word, "12abc"));
    CHECK(toks[1].type == TokenType::Less);
    CHECK(isPlainWord(toks[2].word, "foo"));
}

TEST_CASE("single quotes are verbatim, no escapes recognized", "[lexer]") {
    auto toks = tokenize(R"('a$b"c\d')");
    REQUIRE(toks.size() == 2);
    auto& w = toks[0].word;
    REQUIRE(w.size() == 1);
    CHECK(w[0].kind == WordPartKind::SingleQuoted);
    CHECK(w[0].text == "a$b\"c\\d");
}

TEST_CASE("backslash escape protects exactly one character", "[lexer]") {
    // a\ b -> one WORD token: Literal "a" + SingleQuoted " " + Literal "b"
    // (the escaped blank does not end the word).
    auto toks = tokenize("a\\ b");
    REQUIRE(toks.size() == 2);
    auto& w = toks[0].word;
    REQUIRE(w.size() == 3);
    CHECK(w[0].kind == WordPartKind::Literal);
    CHECK(w[0].text == "a");
    CHECK(w[1].kind == WordPartKind::SingleQuoted);
    CHECK(w[1].text == " ");
    CHECK(w[2].kind == WordPartKind::Literal);
    CHECK(w[2].text == "b");
}

TEST_CASE("backslash-newline is a line continuation, even mid-word", "[lexer]") {
    auto toks = tokenize("ec\\\nho hi");
    REQUIRE(toks.size() == 3);
    CHECK(isPlainWord(toks[0].word, "echo"));
    CHECK(isPlainWord(toks[1].word, "hi"));
}

TEST_CASE("double quotes contain nested expansions but not nested single quotes", "[lexer]") {
    auto toks = tokenize("\"a$b`cmd`c\"");
    REQUIRE(toks.size() == 2);
    auto& w = toks[0].word;
    REQUIRE(w.size() == 1);
    REQUIRE(w[0].kind == WordPartKind::DoubleQuoted);
    auto& inner = w[0].parts;
    REQUIRE(inner.size() == 4);
    CHECK(inner[0].kind == WordPartKind::Literal);
    CHECK(inner[0].text == "a");
    CHECK(inner[1].kind == WordPartKind::ParamExpansion);
    CHECK(inner[1].text == "b");
    CHECK(inner[2].kind == WordPartKind::CommandSubBacktick);
    CHECK(inner[2].text == "cmd");
    CHECK(inner[3].kind == WordPartKind::Literal);
    CHECK(inner[3].text == "c");
}

TEST_CASE("comments run to end of line; mid-word '#' stays literal", "[lexer]") {
    auto toks = tokenize("echo hi # comment here\nworld foo#bar");
    REQUIRE(toks.size() == 6);
    CHECK(isPlainWord(toks[0].word, "echo"));
    CHECK(isPlainWord(toks[1].word, "hi"));
    CHECK(toks[2].type == TokenType::Newline);
    CHECK(isPlainWord(toks[3].word, "world"));
    CHECK(isPlainWord(toks[4].word, "foo#bar"));
    CHECK(toks[5].type == TokenType::EndOfInput);
}

TEST_CASE("$() command substitution captures balanced raw text", "[lexer]") {
    auto toks = tokenize("$(echo $(date))");
    REQUIRE(toks.size() == 2);
    auto& w = toks[0].word;
    REQUIRE(w.size() == 1);
    CHECK(w[0].kind == WordPartKind::CommandSubDollar);
    CHECK(w[0].text == "echo $(date)");
}

TEST_CASE("$(( )) with an expression is treated as arithmetic expansion", "[lexer]") {
    auto toks = tokenize("$(( 1 + 2 ))");
    REQUIRE(toks.size() == 2);
    auto& w = toks[0].word;
    REQUIRE(w.size() == 1);
    CHECK(w[0].kind == WordPartKind::ArithExpansion);
    CHECK(w[0].text == " 1 + 2 ");
}

TEST_CASE("$((name)) resolves the common ambiguity as arithmetic", "[lexer]") {
    auto toks = tokenize("$((a))");
    auto& w = toks[0].word;
    REQUIRE(w.size() == 1);
    CHECK(w[0].kind == WordPartKind::ArithExpansion);
    CHECK(w[0].text == "a");
}

TEST_CASE("$(( falls back to command substitution when it can't close as arithmetic",
          "[lexer]") {
    auto toks = tokenize("$((a) (b))");
    auto& w = toks[0].word;
    REQUIRE(w.size() == 1);
    CHECK(w[0].kind == WordPartKind::CommandSubDollar);
    CHECK(w[0].text == "(a) (b)");
}

TEST_CASE("parameter expansion forms", "[lexer]") {
    auto toks = tokenize("$foo ${bar:-baz} $1 $9 $@ $$ ${#arr}");
    REQUIRE(toks.size() == 8);

    auto check = [](const Word& w, WordPartKind kind, const std::string& text) {
        REQUIRE(w.size() == 1);
        CHECK(w[0].kind == kind);
        CHECK(w[0].text == text);
    };
    check(toks[0].word, WordPartKind::ParamExpansion, "foo");
    check(toks[1].word, WordPartKind::ParamExpansion, "bar:-baz");
    check(toks[2].word, WordPartKind::ParamExpansion, "1");
    check(toks[3].word, WordPartKind::ParamExpansion, "9");
    check(toks[4].word, WordPartKind::ParamExpansion, "@");
    check(toks[5].word, WordPartKind::ParamExpansion, "$");
    check(toks[6].word, WordPartKind::ParamExpansion, "#arr");
}

TEST_CASE("a bare '$' with nothing valid after it is literal", "[lexer]") {
    auto toks = tokenize("$ $.");
    REQUIRE(toks.size() == 3);
    CHECK(isPlainWord(toks[0].word, "$"));
    CHECK(isPlainWord(toks[1].word, "$."));
}

TEST_CASE("couldBeReservedWord reflects fully-unquoted literal words", "[lexer]") {
    auto toks = tokenize("if 'if' i\\f");
    REQUIRE(toks.size() == 4);
    CHECK(toks[0].couldBeReservedWord);
    CHECK(wordAsUnquotedLiteral(toks[0].word) == "if");
    CHECK_FALSE(toks[1].couldBeReservedWord);
    CHECK_FALSE(toks[2].couldBeReservedWord);
}

TEST_CASE("unterminated single quote is a lex error", "[lexer]") {
    Lexer lexer("echo 'unterminated");
    REQUIRE(lexer.next().type == TokenType::Word);  // echo
    REQUIRE_THROWS_AS(lexer.next(), LexError);
}

TEST_CASE("unterminated double quote is a lex error", "[lexer]") {
    Lexer lexer("\"unterminated");
    REQUIRE_THROWS_AS(lexer.next(), LexError);
}

TEST_CASE("unterminated command substitution is a lex error", "[lexer]") {
    Lexer lexer("$(echo foo");
    REQUIRE_THROWS_AS(lexer.next(), LexError);
}

TEST_CASE("unterminated backquoted command substitution is a lex error", "[lexer]") {
    Lexer lexer("`echo foo");
    REQUIRE_THROWS_AS(lexer.next(), LexError);
}
