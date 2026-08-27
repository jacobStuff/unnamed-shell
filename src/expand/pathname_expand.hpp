// Pathname expansion, POSIX.1-2017 Shell & Utilities §2.6.6.
//
// Consumes one field (the output of field splitting, see
// field_split.hpp) and, if it contains an unquoted pattern
// metacharacter, matches it against the filesystem (relative to the
// process's current working directory - ush has no separate notion of
// "shell cwd" once `cd` is implemented via chdir(2), the two are the
// same thing) via opendir(3)/readdir(3) + fnmatch(3) (see pattern.hpp).

#pragma once

#include <string>
#include <vector>

#include "expand/expander.hpp"

namespace ush {

// Expands one field. If it has no unquoted `* ? [` metacharacter, or if
// matching finds no pathnames at all, returns a single-element vector
// holding the field's quote-removed text unchanged (§2.6.6: "if no
// pathnames are matched... the word shall be left unchanged"). Otherwise
// returns every matching pathname, sorted.
std::vector<std::string> expandPathname(const ExpandedWord& field);

}  // namespace ush
