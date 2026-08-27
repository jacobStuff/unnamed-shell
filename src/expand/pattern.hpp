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

namespace ush {

// True if `text` matches `pattern` in its entirety. `pattern` is expected
// to already have quote-removal-aware escaping applied by the caller (see
// Expander's use of this) so that characters from quoted source text are
// literal rather than glob metacharacters - this function itself applies
// no quoting logic.
bool matchesPattern(const std::string& pattern, const std::string& text);

}  // namespace ush
