#include "expand/pattern.hpp"

#include <fnmatch.h>

namespace ush {

bool matchesPattern(const std::string& pattern, const std::string& text) {
    return fnmatch(pattern.c_str(), text.c_str(), 0) == 0;
}

PatternWithMeta buildPattern(const ExpandedWord& word) {
    PatternWithMeta result;
    for (const auto& piece : word) {
        if (piece.quoted) {
            for (char c : piece.text) {
                if (c != '/') result.pattern += '\\';
                result.pattern += c;
            }
        } else {
            result.pattern += piece.text;
            for (char c : piece.text) {
                if (c == '*' || c == '?' || c == '[') {
                    result.hasMetachar = true;
                    break;
                }
            }
        }
    }
    return result;
}

}  // namespace ush
