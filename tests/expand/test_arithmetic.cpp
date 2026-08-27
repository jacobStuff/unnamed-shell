#include <catch2/catch_test_macros.hpp>

#include "expand/arithmetic.hpp"
#include "runtime/environment.hpp"

using namespace ush;

namespace {
std::intmax_t eval(const std::string& expr) {
    Environment env;
    return evaluateArithmetic(expr, env);
}
}  // namespace

TEST_CASE("integer literals: decimal, hex, octal", "[arithmetic]") {
    CHECK(eval("42") == 42);
    CHECK(eval("0x2A") == 42);
    CHECK(eval("052") == 42);
    CHECK(eval("0") == 0);
}

TEST_CASE("basic arithmetic and precedence", "[arithmetic]") {
    CHECK(eval("1 + 2") == 3);
    CHECK(eval("2 + 3 * 4") == 14);
    CHECK(eval("(2 + 3) * 4") == 20);
    CHECK(eval("10 - 4 - 3") == 3);       // left-associative
    CHECK(eval("2 * 3 % 4") == 2);
    CHECK(eval("7 / 2") == 3);
    CHECK(eval("-7 / 2") == -3);
}

TEST_CASE("unary operators", "[arithmetic]") {
    CHECK(eval("-5") == -5);
    CHECK(eval("+5") == 5);
    CHECK(eval("!0") == 1);
    CHECK(eval("!5") == 0);
    CHECK(eval("~0") == -1);
    CHECK(eval("- -3") == 3);
}

TEST_CASE("shifts and bitwise operators", "[arithmetic]") {
    CHECK(eval("1 << 4") == 16);
    CHECK(eval("256 >> 4") == 16);
    CHECK(eval("6 & 3") == 2);
    CHECK(eval("6 | 1") == 7);
    CHECK(eval("6 ^ 3") == 5);
}

TEST_CASE("comparisons and equality yield 0 or 1", "[arithmetic]") {
    CHECK(eval("1 < 2") == 1);
    CHECK(eval("2 < 1") == 0);
    CHECK(eval("2 <= 2") == 1);
    CHECK(eval("3 > 2") == 1);
    CHECK(eval("3 >= 4") == 0);
    CHECK(eval("3 == 3") == 1);
    CHECK(eval("3 != 3") == 0);
}

TEST_CASE("logical operators short-circuit on values, not evaluation order", "[arithmetic]") {
    CHECK(eval("1 && 1") == 1);
    CHECK(eval("1 && 0") == 0);
    CHECK(eval("0 || 0") == 0);
    CHECK(eval("0 || 5") == 1);
}

TEST_CASE("ternary conditional", "[arithmetic]") {
    CHECK(eval("1 ? 2 : 3") == 2);
    CHECK(eval("0 ? 2 : 3") == 3);
    CHECK(eval("1 ? 2 : 1 ? 3 : 4") == 2);
}

TEST_CASE("comma operator evaluates to the last expression", "[arithmetic]") {
    CHECK(eval("(1, 2, 3)") == 3);
}

TEST_CASE("parentheses and full operator precedence chain", "[arithmetic]") {
    CHECK(eval("1 + 2 == 3 && 4 - 1 == 3") == 1);
}

TEST_CASE("division and modulo by zero are errors", "[arithmetic]") {
    CHECK_THROWS_AS(eval("1 / 0"), ArithError);
    CHECK_THROWS_AS(eval("1 % 0"), ArithError);
}

TEST_CASE("a malformed expression is a syntax error", "[arithmetic]") {
    CHECK_THROWS_AS(eval("1 +"), ArithError);
    CHECK_THROWS_AS(eval("(1 + 2"), ArithError);
    CHECK_THROWS_AS(eval("1 2"), ArithError);
}

TEST_CASE("variables are read from the environment", "[arithmetic]") {
    Environment env;
    env.set("x", "10");
    CHECK(evaluateArithmetic("x + 1", env) == 11);
    CHECK(evaluateArithmetic("x * x", env) == 100);
}

TEST_CASE("an unset or non-numeric variable evaluates to 0", "[arithmetic]") {
    Environment env;
    env.unset("nope");
    CHECK(evaluateArithmetic("nope", env) == 0);
    env.set("junk", "hello");
    CHECK(evaluateArithmetic("junk", env) == 0);
}

TEST_CASE("assignment writes back to the environment and yields the assigned value",
          "[arithmetic]") {
    Environment env;
    CHECK(evaluateArithmetic("x = 5", env) == 5);
    CHECK(env.get("x") == "5");

    CHECK(evaluateArithmetic("x += 3", env) == 8);
    CHECK(env.get("x") == "8");

    CHECK(evaluateArithmetic("x <<= 2", env) == 32);
    CHECK(env.get("x") == "32");
}

TEST_CASE("assignment is right-associative", "[arithmetic]") {
    Environment env;
    evaluateArithmetic("a = b = 7", env);
    CHECK(env.get("a") == "7");
    CHECK(env.get("b") == "7");
}
