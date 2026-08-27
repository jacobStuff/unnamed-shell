// Shell pattern matching, POSIX.1-2017 Shell & Utilities §2.13 "Pattern
// Matching Notation" (`*`, `?`, `[...]`).
//
// Implemented via the POSIX C library's fnmatch(3), which implements
// exactly this notation - using it here is analogous to using
// fork/exec/waitpid for process management rather than reimplementing
// those: it's the standard POSIX API for this, not shell source. See
// docs/DESIGN.md.

#pragma once

#include <string>

#include "expand/expander.hpp"

namespace ush {

// True if `text` matches `pattern` in its entirety. `pattern` is expected
// to already have quote-removal-aware escaping applied (see buildPattern
// below) so that characters from quoted source text are literal rather
// than glob metacharacters - this function itself applies no quoting
// logic.
bool matchesPattern(const std::string& pattern, const std::string& text);

struct PatternWithMeta {
    // An fnmatch(3)-compatible pattern: characters from a quoted piece are
    // backslash-escaped (making them literal, per §2.13's rule that
    // quoting removes a pattern character's special meaning); characters
    // from an unquoted piece pass through so `* ? [...]` keep their glob
    // meaning. '/' is never escaped even when it came from quoted source,
    // since it is always a literal path separator, never a pattern
    // metacharacter.
    std::string pattern;
    // True if any unquoted character in `word` is a pattern
    // metacharacter (`* ? [`) - i.e. whether matching should even be
    // attempted, as opposed to just using the pattern text literally.
    bool hasMetachar = false;
};

// Builds a PatternWithMeta from an already-expanded word (see the
// comments on ExpandedWord in expander.hpp) - shared by parameter
// expansion's #/##/%/%% operators and pathname expansion.
PatternWithMeta buildPattern(const ExpandedWord& word);

}  // namespace ush
