// Field splitting, POSIX.1-2017 Shell & Utilities §2.6.5.
//
// Consumes the ExpandedWord produced by Expander::expand() (see
// expander.hpp) and splits it into zero or more fields, each itself an
// ExpandedWord (a field can still contain a quoted/unquoted-tagged piece
// sequence, since pathname expansion - the next stage - needs to know
// which characters within a field are eligible to act as glob
// metacharacters).

#pragma once

#include <string>
#include <vector>

#include "expand/expander.hpp"

namespace ush {

// Splits `word` into fields per §2.6.5, using the characters of `ifs`
// (see Environment::ifsOrDefault()) as delimiters - except:
//   - a piece with `quoted == true` is never split, and its characters
//     are never treated as delimiters;
//   - a piece with `fieldBreakAfter == true` forces a field boundary
//     immediately after it regardless of IFS (used only for $@ - see
//     Expander::expandAtOrStar());
//   - if `ifs` is empty, no IFS-based splitting happens at all (the
//     fieldBreakAfter boundaries still apply).
// A field that ends up empty AND contains no quoted piece (not even an
// empty one) is dropped entirely, per §2.6.5's null-field-removal rule -
// so e.g. an unset, unquoted `$x` contributes zero fields, while `"$x"`
// (quoted, even if x is empty) always contributes exactly one, possibly
// empty, field.
std::vector<ExpandedWord> splitFields(const ExpandedWord& word, const std::string& ifs);

}  // namespace ush
