#include <catch2/catch_test_macros.hpp>

#include "runtime/environment.hpp"

using namespace ush;

TEST_CASE("unset variable is not set and reads as nullopt", "[environment]") {
    Environment env;
    env.unset("__ush_test_definitely_unset__");
    CHECK_FALSE(env.isSet("__ush_test_definitely_unset__"));
    CHECK_FALSE(env.get("__ush_test_definitely_unset__").has_value());
}

TEST_CASE("set/get round-trips a variable", "[environment]") {
    Environment env;
    env.set("FOO", "bar");
    REQUIRE(env.isSet("FOO"));
    REQUIRE(env.get("FOO").has_value());
    CHECK(*env.get("FOO") == "bar");

    env.set("FOO", "baz");
    CHECK(*env.get("FOO") == "baz");
}

TEST_CASE("unset removes a variable", "[environment]") {
    Environment env;
    env.set("FOO", "bar");
    env.unset("FOO");
    CHECK_FALSE(env.isSet("FOO"));
}

TEST_CASE("a new variable is not exported by default", "[environment]") {
    Environment env;
    env.set("FOO", "bar");
    CHECK_FALSE(env.isExported("FOO"));
    env.setExported("FOO", true);
    CHECK(env.isExported("FOO"));
}

TEST_CASE("readonly variables reject further assignment", "[environment]") {
    Environment env;
    env.set("FOO", "bar");
    env.setReadonly("FOO", true);
    CHECK(env.isReadonly("FOO"));
    CHECK_THROWS_AS(env.set("FOO", "baz"), ReadonlyVariableError);
    CHECK(*env.get("FOO") == "bar");  // unchanged
}

TEST_CASE("positional parameters", "[environment]") {
    Environment env;
    CHECK(env.positionalParams().empty());
    env.setPositionalParams({"a", "b", "c"});
    REQUIRE(env.positionalParams().size() == 3);
    CHECK(env.positionalParams()[1] == "b");
}

TEST_CASE("special parameters have sane defaults", "[environment]") {
    Environment env("myush");
    CHECK(env.shellName == "myush");
    CHECK(env.lastExitStatus == 0);
    CHECK_FALSE(env.lastBackgroundPid.has_value());
    CHECK(env.shellPid > 0);
}

TEST_CASE("IFS defaults to space/tab/newline when unset", "[environment]") {
    Environment env;
    env.unset("IFS");
    CHECK(env.ifsOrDefault() == " \t\n");
    env.set("IFS", ":");
    CHECK(env.ifsOrDefault() == ":");
}

TEST_CASE("the real process environment is imported as exported variables",
          "[environment]") {
    Environment env;
    // PATH is essentially always set in any environment that could run this
    // test binary.
    REQUIRE(env.isSet("PATH"));
    CHECK(env.isExported("PATH"));
}

TEST_CASE("exportedEnviron only includes exported variables", "[environment]") {
    Environment env;
    env.set("__ush_test_local__", "x");
    env.set("__ush_test_exported__", "y");
    env.setExported("__ush_test_exported__", true);

    auto entries = env.exportedEnviron();
    bool sawLocal = false, sawExported = false;
    for (const auto& e : entries) {
        if (e == "__ush_test_local__=x") sawLocal = true;
        if (e == "__ush_test_exported__=y") sawExported = true;
    }
    CHECK_FALSE(sawLocal);
    CHECK(sawExported);
}
