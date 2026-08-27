#include <catch2/catch_test_macros.hpp>

#include "expand/field_split.hpp"

using namespace ush;

namespace {

std::vector<std::string> flattenFields(const std::vector<ExpandedWord>& fields) {
    std::vector<std::string> out;
    for (const auto& f : fields) out.push_back(flatten(f));
    return out;
}

const std::string kDefaultIfs = " \t\n";

}  // namespace

TEST_CASE("a plain unquoted word with no delimiters is one field", "[field_split]") {
    ExpandedWord w = {{"hello", false, false}};
    CHECK(flattenFields(splitFields(w, kDefaultIfs)) == std::vector<std::string>{"hello"});
}

TEST_CASE("unquoted whitespace splits into multiple fields", "[field_split]") {
    ExpandedWord w = {{"a b c", false, false}};
    CHECK(flattenFields(splitFields(w, kDefaultIfs)) == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("leading and trailing IFS whitespace is trimmed, not turned into empty fields",
          "[field_split]") {
    ExpandedWord w = {{"  a  ", false, false}};
    CHECK(flattenFields(splitFields(w, kDefaultIfs)) == std::vector<std::string>{"a"});
}

TEST_CASE("a quoted piece is never split even if it contains IFS characters",
          "[field_split]") {
    ExpandedWord w = {{"a b", true, false}};
    CHECK(flattenFields(splitFields(w, kDefaultIfs)) == std::vector<std::string>{"a b"});
}

TEST_CASE("quoting part of a word protects only that part from splitting", "[field_split]") {
    // No delimiters anywhere (the quoted piece's space doesn't count, and
    // there's no unquoted whitespace around it), so this is one field.
    ExpandedWord w = {{"x", false, false}, {"a b", true, false}, {"y", false, false}};
    CHECK(flattenFields(splitFields(w, kDefaultIfs)) == std::vector<std::string>{"xa by"});
}

TEST_CASE("unquoted whitespace around a quoted piece still delimits it into its own field",
          "[field_split]") {
    // Here the unquoted pieces DO contain delimiting whitespace, so the
    // quoted piece ends up as its own (middle) field.
    ExpandedWord w = {{"x ", false, false}, {"a b", true, false}, {" y", false, false}};
    CHECK(flattenFields(splitFields(w, kDefaultIfs)) ==
          std::vector<std::string>{"x", "a b", "y"});
}

TEST_CASE("an empty unquoted word contributes zero fields", "[field_split]") {
    ExpandedWord w = {{"", false, false}};
    CHECK(splitFields(w, kDefaultIfs).empty());
}

TEST_CASE("an empty quoted word contributes exactly one empty field", "[field_split]") {
    ExpandedWord w = {{"", true, false}};
    auto fields = splitFields(w, kDefaultIfs);
    REQUIRE(fields.size() == 1);
    CHECK(flatten(fields[0]) == "");
}

TEST_CASE("custom non-whitespace IFS: consecutive delimiters produce empty fields",
          "[field_split]") {
    ExpandedWord w = {{"a::b", false, false}};
    CHECK(flattenFields(splitFields(w, ":")) == std::vector<std::string>{"a", "", "b"});
}

TEST_CASE("custom non-whitespace IFS: a leading delimiter produces a leading empty field",
          "[field_split]") {
    ExpandedWord w = {{":a", false, false}};
    CHECK(flattenFields(splitFields(w, ":")) == std::vector<std::string>{"", "a"});
}

TEST_CASE("custom non-whitespace IFS: a trailing delimiter is discarded, not an empty field",
          "[field_split]") {
    ExpandedWord w = {{"a:", false, false}};
    CHECK(flattenFields(splitFields(w, ":")) == std::vector<std::string>{"a"});
}

TEST_CASE("a non-whitespace IFS character absorbs adjacent IFS whitespace into one delimiter",
          "[field_split]") {
    ExpandedWord w = {{"a : b", false, false}};
    CHECK(flattenFields(splitFields(w, ": ")) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("an empty IFS disables field splitting entirely", "[field_split]") {
    ExpandedWord w = {{"a b c", false, false}};
    CHECK(flattenFields(splitFields(w, "")) == std::vector<std::string>{"a b c"});
}

TEST_CASE("an empty IFS still discards a fully empty unquoted word", "[field_split]") {
    ExpandedWord w = {{"", false, false}};
    CHECK(splitFields(w, "").empty());
}

TEST_CASE("fieldBreakAfter forces a boundary even with no IFS splitting", "[field_split]") {
    ExpandedWord w = {{"a", false, true}, {"b", false, false}};
    CHECK(flattenFields(splitFields(w, "")) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("fieldBreakAfter combines with ordinary IFS splitting within each segment",
          "[field_split]") {
    // Segment 1: "a b" (splits into two fields on its own), then a hard
    // break, then segment 2: "c".
    ExpandedWord w = {{"a b", false, true}, {"c", false, false}};
    CHECK(flattenFields(splitFields(w, kDefaultIfs)) == std::vector<std::string>{"a", "b", "c"});
}
