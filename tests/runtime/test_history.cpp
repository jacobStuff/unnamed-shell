#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "runtime/history.hpp"

using namespace ush;
namespace fs = std::filesystem;

TEST_CASE("a new History is empty", "[history]") {
    History h;
    CHECK(h.empty());
    CHECK(h.size() == 0);
    CHECK(h.firstNumber() == 0);
    CHECK(h.lastNumber() == 0);
    CHECK(h.byNumber(1) == nullptr);
}

TEST_CASE("add() numbers entries starting at 1", "[history]") {
    History h;
    h.add("echo one");
    h.add("echo two");
    REQUIRE(h.size() == 2);
    CHECK(h.firstNumber() == 1);
    CHECK(h.lastNumber() == 2);
    REQUIRE(h.byNumber(1) != nullptr);
    CHECK(*h.byNumber(1) == "echo one");
    REQUIRE(h.byNumber(2) != nullptr);
    CHECK(*h.byNumber(2) == "echo two");
}

TEST_CASE("add() trims exactly one trailing newline", "[history]") {
    History h;
    h.add("echo one\n");
    REQUIRE(h.size() == 1);
    CHECK(*h.byNumber(1) == "echo one");
}

TEST_CASE("add() ignores a blank/whitespace-only entry", "[history]") {
    History h;
    h.add("echo one");
    h.add("");
    h.add("   \t  ");
    h.add("\n");
    CHECK(h.size() == 1);
}

TEST_CASE("add() ignores an entry identical to the immediately preceding one", "[history]") {
    History h;
    h.add("echo dup");
    h.add("echo dup");
    CHECK(h.size() == 1);
    h.add("echo other");
    h.add("echo dup");  // not adjacent to the first "echo dup" - kept
    CHECK(h.size() == 3);
}

TEST_CASE("setMaxSize trims from the front and keeps history numbers increasing", "[history]") {
    History h;
    for (int i = 1; i <= 5; ++i) h.add("cmd" + std::to_string(i));
    REQUIRE(h.size() == 5);

    h.setMaxSize(3);
    CHECK(h.size() == 3);
    // The oldest two (cmd1, cmd2) were dropped, but the survivors keep
    // their original numbers rather than renumbering from 1.
    CHECK(h.firstNumber() == 3);
    CHECK(h.lastNumber() == 5);
    REQUIRE(h.byNumber(3) != nullptr);
    CHECK(*h.byNumber(3) == "cmd3");
    CHECK(h.byNumber(1) == nullptr);
    CHECK(h.byNumber(2) == nullptr);

    // Cap keeps applying to further additions.
    h.add("cmd6");
    CHECK(h.size() == 3);
    CHECK(h.firstNumber() == 4);
    CHECK(h.lastNumber() == 6);
}

TEST_CASE("setMaxSize(0) means unlimited", "[history]") {
    History h;
    h.setMaxSize(2);
    h.add("a");
    h.add("b");
    h.add("c");
    CHECK(h.size() == 2);
    h.setMaxSize(0);
    h.add("d");
    h.add("e");
    CHECK(h.size() == 4);  // b, c, d, e - no trimming once unlimited
}

TEST_CASE("clear() empties the list and keeps numbering climbing afterward", "[history]") {
    History h;
    h.add("a");
    h.add("b");
    h.add("c");
    REQUIRE(h.lastNumber() == 3);

    h.clear();
    CHECK(h.empty());
    CHECK(h.size() == 0);
    CHECK(h.firstNumber() == 0);
    CHECK(h.lastNumber() == 0);
    CHECK(h.byNumber(1) == nullptr);
    CHECK(h.byNumber(3) == nullptr);

    h.add("d");
    // "d" gets number 4, not 1 - clear() doesn't rewind numbering.
    REQUIRE(h.size() == 1);
    CHECK(h.firstNumber() == 4);
    CHECK(h.lastNumber() == 4);
    CHECK(*h.byNumber(4) == "d");
}

TEST_CASE("save() then load() round-trips the retained entries", "[history]") {
    fs::path path = fs::temp_directory_path() / "ush_history_roundtrip_test";
    std::error_code ec;
    fs::remove(path, ec);

    {
        History h;
        h.add("echo one");
        h.add("echo two");
        h.add("echo three");
        h.save(path.string());
    }
    {
        History h2;
        h2.load(path.string());
        REQUIRE(h2.size() == 3);
        CHECK(*h2.byNumber(1) == "echo one");
        CHECK(*h2.byNumber(2) == "echo two");
        CHECK(*h2.byNumber(3) == "echo three");
    }
    fs::remove(path, ec);
}

TEST_CASE("load() from a missing file is a silent no-op", "[history]") {
    History h;
    h.load("/this/path/definitely/does/not/exist/ush_history");
    CHECK(h.empty());
}

TEST_CASE("load() applies the current size cap", "[history]") {
    fs::path path = fs::temp_directory_path() / "ush_history_cap_test";
    std::error_code ec;
    fs::remove(path, ec);
    {
        History h;
        for (int i = 1; i <= 5; ++i) h.add("cmd" + std::to_string(i));
        h.save(path.string());
    }
    History h2;
    h2.setMaxSize(2);
    h2.load(path.string());
    CHECK(h2.size() == 2);
    CHECK(*h2.byNumber(h2.lastNumber()) == "cmd5");
    fs::remove(path, ec);
}
