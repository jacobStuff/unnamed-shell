#include "expand/pattern.hpp"

#include <fnmatch.h>

namespace ush {

bool matchesPattern(const std::string& pattern, const std::string& text) {
    return fnmatch(pattern.c_str(), text.c_str(), 0) == 0;
}

}  // namespace ush
