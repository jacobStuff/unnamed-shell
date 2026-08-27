#include "expand/pathname_expand.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>

#include "expand/pattern.hpp"

namespace ush {

namespace {

// Splits `field` into path components at every literal '/' character
// (from any piece, quoted or not - '/' is always a separator, never a
// pattern metacharacter, regardless of quoting; see PatternWithMeta's
// comment on this in pattern.hpp).
std::vector<ExpandedWord> splitPathComponents(const ExpandedWord& field) {
    std::vector<ExpandedWord> components;
    ExpandedWord cur;
    for (const auto& piece : field) {
        std::size_t start = 0;
        for (std::size_t i = 0; i < piece.text.size(); ++i) {
            if (piece.text[i] == '/') {
                if (i > start) cur.push_back({piece.text.substr(start, i - start), piece.quoted, false});
                components.push_back(std::move(cur));
                cur.clear();
                start = i + 1;
            }
        }
        if (start < piece.text.size()) cur.push_back({piece.text.substr(start), piece.quoted, false});
    }
    components.push_back(std::move(cur));
    return components;
}

bool isComponentEmpty(const ExpandedWord& c) {
    for (const auto& p : c) if (!p.text.empty()) return false;
    return true;
}

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty()) return name;
    if (base == "/") return "/" + name;
    return base + "/" + name;
}

// Matches `comps[componentIndex]` against every entry of the directories
// in `bases`, returning the resulting paths (filtered to directories only
// unless this is the last component). Handles both the literal case (no
// metacharacter: a plain existence check) and the wildcard case (a real
// directory scan + fnmatch, with the usual "a leading '.' in the pattern
// is required to match a leading '.' in the filename" rule).
std::vector<std::string> matchOneComponent(const std::vector<std::string>& bases,
                                            const ExpandedWord& component, bool isLast) {
    PatternWithMeta info = buildPattern(component);
    std::vector<std::string> results;

    // For "is this a directory we can descend into" checks (non-last
    // components) we must follow symlinks - e.g. on macOS /var is itself
    // a symlink to /private/var, and lstat() on it reports a symlink, not
    // a directory, which would otherwise wrongly block traversal through
    // any path starting with /var (as most temp-file paths do).
    // Existence checks for the *last* component use lstat() instead, so a
    // broken symlink still counts as a match (as real shells do).
    auto isTraversableDir = [](const std::string& path) {
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    };

    if (!info.hasMetachar) {
        std::string name = flatten(component);
        for (const auto& base : bases) {
            std::string candidate = joinPath(base, name);
            struct stat st;
            if (::lstat(candidate.c_str(), &st) != 0) continue;
            if (isLast || isTraversableDir(candidate)) results.push_back(candidate);
        }
        return results;
    }

    std::string compFlat = flatten(component);
    bool patternStartsWithDot = !compFlat.empty() && compFlat[0] == '.';

    for (const auto& base : bases) {
        std::string dirToRead = base.empty() ? "." : base;
        DIR* dir = ::opendir(dirToRead.c_str());
        if (!dir) continue;

        std::vector<std::string> names;
        struct dirent* entry;
        while ((entry = ::readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (!name.empty() && name[0] == '.' && !patternStartsWithDot) continue;
            if (matchesPattern(info.pattern, name)) names.push_back(std::move(name));
        }
        ::closedir(dir);
        std::sort(names.begin(), names.end());

        for (const auto& name : names) {
            std::string candidate = joinPath(base, name);
            if (!isLast && !isTraversableDir(candidate)) continue;
            results.push_back(std::move(candidate));
        }
    }
    return results;
}

}  // namespace

std::vector<std::string> expandPathname(const ExpandedWord& field) {
    if (!buildPattern(field).hasMetachar) return {flatten(field)};

    std::vector<ExpandedWord> components = splitPathComponents(field);

    bool absolute = false;
    if (components.size() > 1 && isComponentEmpty(components.front())) {
        absolute = true;
        components.erase(components.begin());
    }
    // Collapse embedded/trailing empty components (from "//" or a
    // trailing '/'); a trailing '/' requiring the match to be a directory
    // is not specially enforced beyond the normal "non-last components
    // must be directories" rule below (a documented simplification).
    components.erase(std::remove_if(components.begin(), components.end(), isComponentEmpty),
                      components.end());

    std::vector<std::string> paths = {absolute ? "/" : ""};
    for (std::size_t i = 0; i < components.size() && !paths.empty(); ++i) {
        bool isLast = (i + 1 == components.size());
        paths = matchOneComponent(paths, components[i], isLast);
    }

    if (components.empty()) {
        // The whole field was just "/" (or all slashes) - already handled
        // by the no-metachar fast path above in every real case, but stay
        // consistent if reached.
        return {absolute ? "/" : flatten(field)};
    }

    std::sort(paths.begin(), paths.end());
    if (paths.empty()) return {flatten(field)};  // §2.6.6: no match -> left unchanged
    return paths;
}

}  // namespace ush
