#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>

#include "ast/ast.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"

using namespace ush;
using namespace ush::ast;

namespace {

List parse(const std::string& src) {
    Parser p(src);
    return p.parseProgram();
}

// Test convenience for the (very common, in these tests) case of a plain,
// fully-unquoted word.
std::string wordText(const Word& w) {
    REQUIRE(wordIsUnquotedLiteral(w));
    return wordAsUnquotedLiteral(w);
}

std::vector<std::string> wordTexts(const std::vector<Word>& words) {
    std::vector<std::string> out;
    for (const auto& w : words) out.push_back(wordText(w));
    return out;
}

// Assumes `list` is exactly one item: a single pipeline of a single
// SimpleCommand, with no &&/||/negation. Returns that SimpleCommand.
const SimpleCommand& soleSimpleCommand(const List& list) {
    REQUIRE(list.items.size() == 1);
    const AndOr& andOr = list.items[0].andOr;
    REQUIRE(andOr.rest.empty());
    REQUIRE_FALSE(andOr.first.negated);
    REQUIRE(andOr.first.commands.size() == 1);
    return std::get<SimpleCommand>(andOr.first.commands[0]->value);
}

// Assumes `list` is exactly one item: a single pipeline of a single
// compound command. Returns that CompoundCommand.
const CompoundCommand& soleCompoundCommand(const List& list) {
    REQUIRE(list.items.size() == 1);
    const AndOr& andOr = list.items[0].andOr;
    REQUIRE(andOr.rest.empty());
    REQUIRE(andOr.first.commands.size() == 1);
    return std::get<CompoundCommand>(andOr.first.commands[0]->value);
}

}  // namespace

TEST_CASE("simple command with a leading assignment", "[parser]") {
    List l = parse("FOO=bar echo hello world");
    const SimpleCommand& cmd = soleSimpleCommand(l);
    REQUIRE(cmd.assignments.size() == 1);
    CHECK(cmd.assignments[0].name == "FOO");
    CHECK(wordText(cmd.assignments[0].value) == "bar");
    CHECK(wordTexts(cmd.words) == std::vector<std::string>{"echo", "hello", "world"});
    CHECK(cmd.redirects.empty());
}

TEST_CASE("assignment-only simple command has no words", "[parser]") {
    List l = parse("FOO=bar");
    const SimpleCommand& cmd = soleSimpleCommand(l);
    REQUIRE(cmd.assignments.size() == 1);
    CHECK(cmd.assignments[0].name == "FOO");
    CHECK(cmd.words.empty());
}

TEST_CASE("pipeline of three commands", "[parser]") {
    List l = parse("echo hi | cat | wc -l");
    REQUIRE(l.items.size() == 1);
    const Pipeline& p = l.items[0].andOr.first;
    REQUIRE(p.commands.size() == 3);
    CHECK_FALSE(p.negated);
    CHECK(wordText(std::get<SimpleCommand>(p.commands[0]->value).words[0]) == "echo");
    CHECK(wordText(std::get<SimpleCommand>(p.commands[1]->value).words[0]) == "cat");
    CHECK(wordText(std::get<SimpleCommand>(p.commands[2]->value).words[0]) == "wc");
}

TEST_CASE("negated pipeline", "[parser]") {
    List l = parse("! false");
    REQUIRE(l.items.size() == 1);
    const Pipeline& p = l.items[0].andOr.first;
    CHECK(p.negated);
    REQUIRE(p.commands.size() == 1);
    CHECK(wordText(std::get<SimpleCommand>(p.commands[0]->value).words[0]) == "false");
}

TEST_CASE("and/or list", "[parser]") {
    List l = parse("true && echo yes || echo no");
    REQUIRE(l.items.size() == 1);
    const AndOr& ao = l.items[0].andOr;
    CHECK(wordText(std::get<SimpleCommand>(ao.first.commands[0]->value).words[0]) == "true");
    REQUIRE(ao.rest.size() == 2);
    CHECK(ao.rest[0].first);   // &&
    CHECK(wordText(std::get<SimpleCommand>(ao.rest[0].second.commands[0]->value).words[0]) ==
          "echo");
    CHECK_FALSE(ao.rest[1].first);  // ||
}

TEST_CASE("sequential and async separators", "[parser]") {
    List l = parse("echo a; echo b & echo c");
    REQUIRE(l.items.size() == 3);
    CHECK(l.items[0].sep == Separator::Sequential);
    CHECK(l.items[1].sep == Separator::Async);
    CHECK(l.items[2].sep == Separator::None);
}

TEST_CASE("redirection operators and IO_NUMBER attach to a simple command", "[parser]") {
    List l = parse("cmd < in > out 2>&1 >>append <>rw >|clob");
    const SimpleCommand& cmd = soleSimpleCommand(l);
    REQUIRE(cmd.redirects.size() == 6);

    CHECK_FALSE(cmd.redirects[0].ioNumber.has_value());
    CHECK(cmd.redirects[0].op == TokenType::Less);
    CHECK(wordText(std::get<Word>(cmd.redirects[0].target)) == "in");

    CHECK(cmd.redirects[1].op == TokenType::Great);
    CHECK(wordText(std::get<Word>(cmd.redirects[1].target)) == "out");

    REQUIRE(cmd.redirects[2].ioNumber.has_value());
    CHECK(*cmd.redirects[2].ioNumber == 2);
    CHECK(cmd.redirects[2].op == TokenType::GreatAnd);
    CHECK(wordText(std::get<Word>(cmd.redirects[2].target)) == "1");

    CHECK(cmd.redirects[3].op == TokenType::DGreat);
    CHECK(wordText(std::get<Word>(cmd.redirects[3].target)) == "append");

    CHECK(cmd.redirects[4].op == TokenType::LessGreat);
    CHECK(wordText(std::get<Word>(cmd.redirects[4].target)) == "rw");

    CHECK(cmd.redirects[5].op == TokenType::Clobber);
    CHECK(wordText(std::get<Word>(cmd.redirects[5].target)) == "clob");
}

TEST_CASE("if/elif/else/fi", "[parser]") {
    List l = parse("if true; then echo a; elif false; then echo b; else echo c; fi");
    const CompoundCommand& cc = soleCompoundCommand(l);
    const IfClause& ic = std::get<IfClause>(cc);
    REQUIRE(ic.branches.size() == 2);
    CHECK(wordText(std::get<SimpleCommand>(
                        ic.branches[0].cond.items[0].andOr.first.commands[0]->value)
                        .words[0]) == "true");
    CHECK(wordText(std::get<SimpleCommand>(
                        ic.branches[0].body.items[0].andOr.first.commands[0]->value)
                        .words[1]) == "a");
    CHECK(wordText(std::get<SimpleCommand>(
                        ic.branches[1].cond.items[0].andOr.first.commands[0]->value)
                        .words[0]) == "false");
    REQUIRE(ic.elseBranch.has_value());
    CHECK(wordText(
              std::get<SimpleCommand>(ic.elseBranch->items[0].andOr.first.commands[0]->value)
                  .words[1]) == "c");
}

TEST_CASE("while and until", "[parser]") {
    {
        List l = parse("while true; do echo x; done");
        const WhileClause& wc = std::get<WhileClause>(soleCompoundCommand(l));
        CHECK(wordText(std::get<SimpleCommand>(wc.cond.items[0].andOr.first.commands[0]->value)
                            .words[0]) == "true");
        CHECK(wc.body.items.size() == 1);
    }
    {
        List l = parse("until false; do echo y; done");
        const UntilClause& uc = std::get<UntilClause>(soleCompoundCommand(l));
        CHECK(wordText(std::get<SimpleCommand>(uc.cond.items[0].andOr.first.commands[0]->value)
                            .words[0]) == "false");
    }
}

TEST_CASE("for with an 'in' word list", "[parser]") {
    List l = parse("for i in a b c; do echo $i; done");
    const ForClause& fc = std::get<ForClause>(soleCompoundCommand(l));
    CHECK(fc.varName == "i");
    REQUIRE(fc.words.has_value());
    CHECK(wordTexts(*fc.words) == std::vector<std::string>{"a", "b", "c"});
    CHECK(fc.body.items.size() == 1);
}

TEST_CASE("for without an 'in' clause defaults to nullopt (meaning \"$@\")", "[parser]") {
    List l = parse("for i; do echo $i; done");
    const ForClause& fc = std::get<ForClause>(soleCompoundCommand(l));
    CHECK(fc.varName == "i");
    CHECK_FALSE(fc.words.has_value());
}

TEST_CASE("for is lenient about a linebreak before a bare 'do'", "[parser]") {
    List l = parse("for i\ndo echo $i; done");
    const ForClause& fc = std::get<ForClause>(soleCompoundCommand(l));
    CHECK(fc.varName == "i");
    CHECK_FALSE(fc.words.has_value());
}

TEST_CASE("for in with an empty word list means zero iterations, not \"$@\"", "[parser]") {
    List l = parse("for i in; do :; done");
    const ForClause& fc = std::get<ForClause>(soleCompoundCommand(l));
    REQUIRE(fc.words.has_value());
    CHECK(fc.words->empty());
}

TEST_CASE("case clause with alternatives and a final item with no trailing ';;'", "[parser]") {
    List l = parse("case $x in\n  a|b) echo ab;;\n  c) echo c;;\n  *) echo other\nesac");
    const CaseClause& cc = std::get<CaseClause>(soleCompoundCommand(l));
    REQUIRE(cc.subject.size() == 1);
    CHECK(cc.subject[0].kind == WordPartKind::ParamExpansion);
    CHECK(cc.subject[0].text == "x");

    REQUIRE(cc.items.size() == 3);
    CHECK(wordTexts(cc.items[0].patterns) == std::vector<std::string>{"a", "b"});
    CHECK(wordTexts(cc.items[1].patterns) == std::vector<std::string>{"c"});
    CHECK(wordTexts(cc.items[2].patterns) == std::vector<std::string>{"*"});
    CHECK(wordText(std::get<SimpleCommand>(cc.items[2].body.items[0].andOr.first.commands[0]->value)
                       .words[1]) == "other");
}

TEST_CASE("case item with optional leading paren and an empty body", "[parser]") {
    List l = parse("case x in (a) ;; esac");
    const CaseClause& cc = std::get<CaseClause>(soleCompoundCommand(l));
    REQUIRE(cc.items.size() == 1);
    CHECK(wordTexts(cc.items[0].patterns) == std::vector<std::string>{"a"});
    CHECK(cc.items[0].body.items.empty());
}

TEST_CASE("brace group and subshell", "[parser]") {
    {
        List l = parse("{ echo a; echo b; }");
        const BraceGroup& bg = std::get<BraceGroup>(soleCompoundCommand(l));
        CHECK(bg.body.items.size() == 2);
    }
    {
        List l = parse("(echo a; echo b)");
        const Subshell& sh = std::get<Subshell>(soleCompoundCommand(l));
        CHECK(sh.body.items.size() == 2);
    }
}

TEST_CASE("function definition", "[parser]") {
    List l = parse("foo() { echo hi; }");
    REQUIRE(l.items.size() == 1);
    const auto& commands = l.items[0].andOr.first.commands;
    REQUIRE(commands.size() == 1);
    const FunctionDefinition& fd = std::get<FunctionDefinition>(commands[0]->value);
    CHECK(fd.name == "foo");
    const BraceGroup& bg = std::get<BraceGroup>(fd.body);
    CHECK(bg.body.items.size() == 1);
}

TEST_CASE("basic here-document", "[parser]") {
    List l = parse("cat <<EOF\nhello\nworld\nEOF\n");
    const SimpleCommand& cmd = soleSimpleCommand(l);
    REQUIRE(cmd.redirects.size() == 1);
    CHECK(cmd.redirects[0].op == TokenType::DLess);
    auto hd = std::get<std::shared_ptr<HereDoc>>(cmd.redirects[0].target);
    CHECK_FALSE(hd->literal);
    CHECK(hd->delimiter == "EOF");
    CHECK(hd->body == "hello\nworld\n");
}

TEST_CASE("here-document with '<<-' strips leading tabs from body and delimiter", "[parser]") {
    List l = parse("cat <<-EOF\n\thello\n\tEOF\n");
    const SimpleCommand& cmd = soleSimpleCommand(l);
    auto hd = std::get<std::shared_ptr<HereDoc>>(cmd.redirects[0].target);
    CHECK(hd->body == "hello\n");
}

TEST_CASE("here-document with a quoted delimiter is marked literal", "[parser]") {
    List l = parse("cat <<'EOF'\n$x\nEOF\n");
    const SimpleCommand& cmd = soleSimpleCommand(l);
    auto hd = std::get<std::shared_ptr<HereDoc>>(cmd.redirects[0].target);
    CHECK(hd->literal);
    CHECK(hd->delimiter == "EOF");
    CHECK(hd->body == "$x\n");
}

TEST_CASE("two here-documents on one line are consumed in order", "[parser]") {
    List l = parse("cat <<A <<B\nfirst\nA\nsecond\nB\n");
    const SimpleCommand& cmd = soleSimpleCommand(l);
    REQUIRE(cmd.redirects.size() == 2);
    auto a = std::get<std::shared_ptr<HereDoc>>(cmd.redirects[0].target);
    auto b = std::get<std::shared_ptr<HereDoc>>(cmd.redirects[1].target);
    CHECK(a->body == "first\n");
    CHECK(b->body == "second\n");
}

TEST_CASE("reserved words after the first word of a command stay literal arguments", "[parser]") {
    List l = parse("echo if then done");
    const SimpleCommand& cmd = soleSimpleCommand(l);
    CHECK(wordTexts(cmd.words) == std::vector<std::string>{"echo", "if", "then", "done"});
}

TEST_CASE("comments and blank lines between commands are transparent", "[parser]") {
    List l = parse("echo a # comment\n\n\necho b\n");
    REQUIRE(l.items.size() == 2);
    CHECK(wordText(std::get<SimpleCommand>(l.items[0].andOr.first.commands[0]->value).words[1]) ==
          "a");
    CHECK(wordText(std::get<SimpleCommand>(l.items[1].andOr.first.commands[0]->value).words[1]) ==
          "b");
}

TEST_CASE("mismatched compound-command keyword is a parse error", "[parser]") {
    REQUIRE_THROWS_AS(parse("if true; then echo a; done"), ParseError);
}

TEST_CASE("a stray closing keyword at program level is a parse error", "[parser]") {
    REQUIRE_THROWS_AS(parse("echo a; fi"), ParseError);
}

TEST_CASE("an unterminated here-document is a lex error", "[parser]") {
    REQUIRE_THROWS_AS(parse("cat <<EOF\nhello\n"), LexError);
}
