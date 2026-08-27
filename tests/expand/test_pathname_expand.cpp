#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>

#include "expand/pathname_expand.hpp"

using namespace ush;
namespace fs = std::filesystem;

namespace {

ExpandedWord lit(const std::string& text) { return {{text, false, false}}; }

ExpandedWord concatPieces(std::initializer_list<ExpansionPiece> pieces) { return {pieces}; }

void touch(const fs::path& p) { std::ofstream(p).put('x'); }

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("ush_pathname_test_" + std::to_string(++counter) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    fs::path child(const std::string& name) const { return path_ / name; }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

}  // namespace

TEST_CASE("a field with no metacharacter is returned unchanged, without touching the filesystem",
          "[pathname_expand]") {
    auto result = expandPathname(lit("/definitely/does/not/exist/anywhere"));
    CHECK(result == std::vector<std::string>{"/definitely/does/not/exist/anywhere"});
}

TEST_CASE("'*' matches files in a directory, sorted", "[pathname_expand]") {
    TempDir dir;
    touch(dir.child("b.txt"));
    touch(dir.child("a.txt"));
    touch(dir.child("c.txt"));

    auto result = expandPathname(lit(dir.path().string() + "/*.txt"));
    CHECK(result == std::vector<std::string>{
                        dir.child("a.txt").string(),
                        dir.child("b.txt").string(),
                        dir.child("c.txt").string(),
                    });
}

TEST_CASE("'?' matches exactly one character", "[pathname_expand]") {
    TempDir dir;
    touch(dir.child("a1"));
    touch(dir.child("a22"));

    auto result = expandPathname(lit(dir.path().string() + "/a?"));
    CHECK(result == std::vector<std::string>{dir.child("a1").string()});
}

TEST_CASE("bracket expressions match a set of characters", "[pathname_expand]") {
    TempDir dir;
    touch(dir.child("x"));
    touch(dir.child("y"));
    touch(dir.child("z"));

    auto result = expandPathname(lit(dir.path().string() + "/[xy]"));
    CHECK(result == std::vector<std::string>{dir.child("x").string(), dir.child("y").string()});
}

TEST_CASE("'*' does not match a leading dot unless the pattern has a literal leading dot",
          "[pathname_expand]") {
    TempDir dir;
    touch(dir.child(".hidden"));
    touch(dir.child("visible"));

    auto starResult = expandPathname(lit(dir.path().string() + "/*"));
    CHECK(starResult == std::vector<std::string>{dir.child("visible").string()});

    auto dotStarResult = expandPathname(lit(dir.path().string() + "/.*"));
    // Matches ".hidden" but not "." / ".." (excluded explicitly), nor
    // "visible" (no leading dot).
    CHECK(dotStarResult == std::vector<std::string>{dir.child(".hidden").string()});
}

TEST_CASE("no match leaves the field unchanged (quote-removed) rather than vanishing",
          "[pathname_expand]") {
    TempDir dir;
    auto result = expandPathname(lit(dir.path().string() + "/*.nonexistent_ext_xyz"));
    CHECK(result == std::vector<std::string>{dir.path().string() + "/*.nonexistent_ext_xyz"});
}

TEST_CASE("multi-level patterns walk subdirectories", "[pathname_expand]") {
    TempDir dir;
    fs::create_directories(dir.child("sub1"));
    fs::create_directories(dir.child("sub2"));
    touch(dir.child("sub1") / "f.txt");
    touch(dir.child("sub2") / "f.txt");
    touch(dir.child("f.txt"));  // not inside a "sub*" dir - must not match

    auto result = expandPathname(lit(dir.path().string() + "/sub*/f.txt"));
    CHECK(result == std::vector<std::string>{
                        (dir.child("sub1") / "f.txt").string(),
                        (dir.child("sub2") / "f.txt").string(),
                    });
}

TEST_CASE("a quoted metacharacter is matched literally, not as a wildcard",
          "[pathname_expand]") {
    TempDir dir;
    touch(dir.child("foo*bar"));
    touch(dir.child("fooXbar"));

    // "foo" (unquoted) + "*" (quoted, i.e. escaped) + "bar" (unquoted)
    ExpandedWord field = concatPieces({
        {dir.path().string() + "/foo", false, false},
        {"*", true, false},
        {"bar", false, false},
    });
    auto result = expandPathname(field);
    CHECK(result == std::vector<std::string>{dir.child("foo*bar").string()});
}
