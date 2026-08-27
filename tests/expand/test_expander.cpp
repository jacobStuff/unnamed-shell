#include <catch2/catch_test_macros.hpp>

#include <unordered_map>

#include "expand/expander.hpp"
#include "lexer/lexer.hpp"
#include "runtime/environment.hpp"

using namespace ush;

namespace {

Word lexOneWord(const std::string& src) {
    Lexer lex(src);
    Token tok = lex.next();
    REQUIRE(tok.type == TokenType::Word);
    return tok.word;
}

std::string expandFlat(Expander& ex, const std::string& src) {
    return flatten(ex.expand(lexOneWord(src)));
}

class FakeRunner : public CommandRunner {
public:
    std::unordered_map<std::string, std::string> responses;
    std::string runAndCaptureStdout(const std::string& source) override {
        auto it = responses.find(source);
        return it != responses.end() ? it->second : std::string();
    }
};

}  // namespace

TEST_CASE("plain literal text expands to itself", "[expander]") {
    Environment env;
    Expander ex(env);
    CHECK(expandFlat(ex, "hello") == "hello");
}

TEST_CASE("single and double quoted text is quote-removed", "[expander]") {
    Environment env;
    Expander ex(env);
    CHECK(expandFlat(ex, "'a b'") == "a b");
    CHECK(expandFlat(ex, "\"a b\"") == "a b");
    CHECK(expandFlat(ex, "a\\ b") == "a b");
}

TEST_CASE("tilde expansion", "[expander]") {
    Environment env;
    env.set("HOME", "/home/alice");
    Expander ex(env);
    CHECK(expandFlat(ex, "~") == "/home/alice");
    CHECK(expandFlat(ex, "~/docs") == "/home/alice/docs");
    // No such user - left unchanged, per §2.6.1.
    CHECK(expandFlat(ex, "~this_user_almost_certainly_does_not_exist") ==
          "~this_user_almost_certainly_does_not_exist");
}

TEST_CASE("tilde expansion only applies at the very start of a word", "[expander]") {
    Environment env;
    env.set("HOME", "/home/alice");
    Expander ex(env);
    CHECK(expandFlat(ex, "a~b") == "a~b");
}

TEST_CASE("simple parameter expansion", "[expander]") {
    Environment env;
    env.set("foo", "bar");
    Expander ex(env);
    CHECK(expandFlat(ex, "$foo") == "bar");
    CHECK(expandFlat(ex, "${foo}") == "bar");
    CHECK(expandFlat(ex, "$unset_var_xyz") == "");
}

TEST_CASE("special and positional parameters", "[expander]") {
    Environment env("myush");
    env.lastExitStatus = 7;
    env.setPositionalParams({"one", "two", "three"});
    Expander ex(env);
    CHECK(expandFlat(ex, "$#") == "3");
    CHECK(expandFlat(ex, "$?") == "7");
    CHECK(expandFlat(ex, "$0") == "myush");
    CHECK(expandFlat(ex, "$1") == "one");
    CHECK(expandFlat(ex, "$3") == "three");
    CHECK(expandFlat(ex, "${10}") == "");  // out of range -> unset -> empty
    CHECK(expandFlat(ex, "$$") == std::to_string(env.shellPid));
}

TEST_CASE("length operator", "[expander]") {
    Environment env;
    env.set("foo", "hello");
    env.setPositionalParams({"a", "b"});
    Expander ex(env);
    CHECK(expandFlat(ex, "${#foo}") == "5");
    CHECK(expandFlat(ex, "${#nope}") == "0");
    CHECK(expandFlat(ex, "${#@}") == "2");
    CHECK(expandFlat(ex, "${#}") == "2");  // ${#} alone means $# per ush's rule
}

TEST_CASE(":- and - use a default only when appropriate", "[expander]") {
    Environment env;
    env.set("empty", "");
    env.set("full", "value");
    Expander ex(env);

    CHECK(expandFlat(ex, "${unset:-def}") == "def");
    CHECK(expandFlat(ex, "${empty:-def}") == "def");  // :- treats null as needing default
    CHECK(expandFlat(ex, "${full:-def}") == "value");

    CHECK(expandFlat(ex, "${unset-def}") == "def");
    CHECK(expandFlat(ex, "${empty-def}") == "");  // - only checks unset, not null
    CHECK(expandFlat(ex, "${full-def}") == "value");

    // Using the default does not assign it.
    expandFlat(ex, "${unset:-def}");
    CHECK_FALSE(env.isSet("unset"));
}

TEST_CASE(":= and = assign a default only when appropriate", "[expander]") {
    Environment env;
    env.set("empty", "");
    Expander ex(env);

    CHECK(expandFlat(ex, "${a:=def}") == "def");
    CHECK(env.get("a") == "def");

    CHECK(expandFlat(ex, "${empty:=x}") == "x");
    CHECK(env.get("empty") == "x");

    Environment env2;
    env2.set("empty2", "");
    Expander ex2(env2);
    CHECK(expandFlat(ex2, "${empty2=x}") == "");  // = only assigns when unset
    CHECK(env2.get("empty2") == "");
    CHECK(expandFlat(ex2, "${b=x}") == "x");
    CHECK(env2.get("b") == "x");
}

TEST_CASE(":? and ? raise an error only when appropriate", "[expander]") {
    Environment env;
    env.set("empty", "");
    env.set("full", "value");
    Expander ex(env);

    CHECK_THROWS_AS(expandFlat(ex, "${unset:?}"), ExpansionError);
    CHECK_THROWS_AS(expandFlat(ex, "${empty:?is empty}"), ExpansionError);
    CHECK(expandFlat(ex, "${full:?msg}") == "value");

    CHECK_THROWS_AS(expandFlat(ex, "${unset?}"), ExpansionError);
    CHECK(expandFlat(ex, "${empty?msg}") == "");  // ? only errors when unset
}

TEST_CASE(":+ and + substitute an alternate only when appropriate", "[expander]") {
    Environment env;
    env.set("empty", "");
    env.set("full", "value");
    Expander ex(env);

    CHECK(expandFlat(ex, "${unset:+alt}") == "");
    CHECK(expandFlat(ex, "${empty:+alt}") == "");
    CHECK(expandFlat(ex, "${full:+alt}") == "alt");

    CHECK(expandFlat(ex, "${unset+alt}") == "");
    CHECK(expandFlat(ex, "${empty+alt}") == "alt");  // + only checks unset, not null
}

TEST_CASE("prefix and suffix pattern removal", "[expander]") {
    Environment env;
    env.set("path", "/usr/local/bin");
    env.set("file", "archive.tar.gz");
    Expander ex(env);

    CHECK(expandFlat(ex, "${path#/*/}") == "local/bin");
    CHECK(expandFlat(ex, "${path##/*/}") == "bin");
    CHECK(expandFlat(ex, "${file%.*}") == "archive.tar");
    CHECK(expandFlat(ex, "${file%%.*}") == "archive");
    CHECK(expandFlat(ex, "${file#*}") == "archive.tar.gz");   // smallest match of '*' is empty
    CHECK(expandFlat(ex, "${file##*}") == "");                // largest match of '*' is everything
}

TEST_CASE("quoted characters in a pattern operand are literal, not glob metachars",
          "[expander]") {
    Environment env;
    env.set("v", "a*b");
    Expander ex(env);
    CHECK(expandFlat(ex, "${v#'a*'}") == "b");   // quoted "a*" matches literally
    CHECK(expandFlat(ex, "${v#a\\*}") == "b");   // backslash-escaped '*' is also literal
}

TEST_CASE("command substitution via $() and backticks", "[expander]") {
    Environment env;
    FakeRunner runner;
    runner.responses["echo hi"] = "hi\n";
    runner.responses["date"] = "2026\n\n";
    Expander ex(env, &runner);

    CHECK(expandFlat(ex, "$(echo hi)") == "hi");
    CHECK(expandFlat(ex, "`echo hi`") == "hi");
    CHECK(expandFlat(ex, "$(date)") == "2026");  // all trailing newlines stripped
}

TEST_CASE("command substitution without a runner throws", "[expander]") {
    Environment env;
    Expander ex(env);
    CHECK_THROWS_AS(expandFlat(ex, "$(echo hi)"), ExpansionError);
}

TEST_CASE("arithmetic expansion, including variables and nested command substitution",
          "[expander]") {
    Environment env;
    env.set("x", "10");
    FakeRunner runner;
    runner.responses["echo 5"] = "5\n";
    Expander ex(env, &runner);

    CHECK(expandFlat(ex, "$((1 + 2))") == "3");
    CHECK(expandFlat(ex, "$((x * 2))") == "20");
    CHECK(expandFlat(ex, "$(($(echo 5) + 1))") == "6");
}

TEST_CASE("a literal leading '~' inside $(( )) is bitwise-not, not tilde expansion",
          "[expander]") {
    Environment env;
    env.set("HOME", "/home/alice");
    Expander ex(env);
    CHECK(expandFlat(ex, "$((~0))") == "-1");
}

TEST_CASE("default-value operand can itself contain expansions", "[expander]") {
    Environment env;
    env.set("bar", "barval");
    Expander ex(env);
    CHECK(expandFlat(ex, "${unset:-$bar}") == "barval");
    CHECK(expandFlat(ex, "${unset:-prefix-$bar-suffix}") == "prefix-barval-suffix");
}

TEST_CASE("quoted \"$@\" splits into one protected field per positional parameter",
          "[expander]") {
    Environment env;
    env.setPositionalParams({"a", "b c", "d"});
    Expander ex(env);

    ExpandedWord w = ex.expand(lexOneWord("\"$@\""));
    REQUIRE(w.size() == 3);
    CHECK(w[0].text == "a");
    CHECK(w[0].quoted);
    CHECK(w[0].fieldBreakAfter);
    CHECK(w[1].text == "b c");
    CHECK(w[1].quoted);
    CHECK(w[1].fieldBreakAfter);
    CHECK(w[2].text == "d");
    CHECK(w[2].quoted);
    CHECK_FALSE(w[2].fieldBreakAfter);
}

TEST_CASE("quoted \"$@\" with zero positional parameters contributes nothing", "[expander]") {
    Environment env;
    Expander ex(env);
    ExpandedWord w = ex.expand(lexOneWord("\"$@\""));
    CHECK(w.empty());
}

TEST_CASE("surrounding text splices onto the first/last field of \"$@\"", "[expander]") {
    // Pieces don't equal fields - "x"/"a" share a field because there's no
    // break between them, and likewise "b"/"y"; only the break after "a"
    // matters. Once field splitting (a later stage) groups pieces with no
    // break between them, this yields exactly two fields: "xa" and "by".
    Environment env;
    env.setPositionalParams({"a", "b"});
    Expander ex(env);
    ExpandedWord w = ex.expand(lexOneWord("\"x$@y\""));
    REQUIRE(w.size() == 4);
    CHECK(w[0].text == "x");
    CHECK_FALSE(w[0].fieldBreakAfter);
    CHECK(w[1].text == "a");
    CHECK(w[1].fieldBreakAfter);
    CHECK(w[2].text == "b");
    CHECK_FALSE(w[2].fieldBreakAfter);
    CHECK(w[3].text == "y");
    CHECK_FALSE(w[3].fieldBreakAfter);
}

TEST_CASE("unquoted $@ also produces a hard field break per parameter", "[expander]") {
    Environment env;
    env.setPositionalParams({"a", "b"});
    Expander ex(env);
    ExpandedWord w = ex.expand(lexOneWord("$@"));
    REQUIRE(w.size() == 2);
    CHECK_FALSE(w[0].quoted);
    CHECK(w[0].fieldBreakAfter);
    CHECK_FALSE(w[1].fieldBreakAfter);
}

TEST_CASE("quoted \"$*\" joins with the first character of IFS", "[expander]") {
    Environment env;
    env.setPositionalParams({"a", "b", "c"});
    env.set("IFS", ",");
    Expander ex(env);
    ExpandedWord w = ex.expand(lexOneWord("\"$*\""));
    REQUIRE(w.size() == 1);
    CHECK(w[0].text == "a,b,c");
    CHECK(w[0].quoted);
}

TEST_CASE("bad substitution syntax is an expansion error", "[expander]") {
    Environment env;
    Expander ex(env);
    CHECK_THROWS_AS(expandFlat(ex, "${1abc}"), ExpansionError);
    CHECK_THROWS_AS(expandFlat(ex, "${}"), ExpansionError);
}

TEST_CASE("cannot assign a default to a non-variable parameter", "[expander]") {
    // $1 must be unset for ':=' to even attempt an assignment; with no
    // positional parameters set, it is.
    Environment env;
    Expander ex(env);
    CHECK_THROWS_AS(expandFlat(ex, "${1:=x}"), ExpansionError);
}
