// The interactive command history list: what backs the line editor's
// arrow-key recall and the `fc`/`history` built-ins. Not itself tied to a
// terminal or to Executor - just an ordered, optionally size-capped list
// of past command lines, with the load/save-to-a-file behavior POSIX
// describes for `fc`/HISTFILE/HISTSIZE.
//
// Entries are numbered like real shells number them: ever-increasing for
// the life of the object, even as old entries get dropped once the list
// exceeds its cap - so "fc -l 5 8" still means the same five commands
// whether or not anything has been trimmed since they were added.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ush {

class History {
public:
    // Loads entries from `path`, appending them after whatever's already
    // in memory (so this can be called once at startup on an otherwise-
    // empty History). Missing or unreadable file: silently does nothing -
    // a shell with no history yet isn't an error. Applies the current
    // size cap (see setMaxSize()) after loading.
    void load(const std::string& path);

    // Writes every retained entry to `path`, one per line, overwriting
    // whatever was there. Best-effort: a write failure (unwritable
    // directory, etc.) is silently ignored, matching load()'s treatment
    // of a missing file.
    void save(const std::string& path) const;

    // Appends `entry` as a new history entry, unless it is empty/all
    // whitespace or textually identical to the immediately preceding
    // entry (both match real shells' default "ignoreboth"-ish behavior,
    // and stop a bare Enter or an unmodified recalled-then-rerun command
    // from bloating the list). A single trailing '\n', if present
    // (multi-line commands are handed over with one), is trimmed first.
    void add(std::string entry);

    // Caps the list at `n` most-recent entries from now on (0 means
    // unlimited); applied immediately to what's already stored. Backs
    // $HISTSIZE.
    void setMaxSize(std::size_t n);

    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    // 1-based history numbers, matching `fc`/`history`'s numbering.
    // firstNumber() > lastNumber() (or both 0) when empty.
    std::size_t firstNumber() const { return empty() ? 0 : firstNumber_; }
    std::size_t lastNumber() const { return empty() ? 0 : firstNumber_ + entries_.size() - 1; }

    // The entry with this history number, or nullptr if `number` is out
    // of the currently-retained range.
    const std::string* byNumber(std::size_t number) const;

    // Positional access for the line editor's recall (0 == oldest
    // retained entry): distinct from history numbers, which survive
    // trimming - this doesn't.
    const std::string& operator[](std::size_t index) const { return entries_[index]; }
    const std::vector<std::string>& entries() const { return entries_; }

private:
    std::vector<std::string> entries_;
    std::size_t maxSize_ = 0;
    std::size_t firstNumber_ = 1;  // history number of entries_[0]

    void trim();
};

}  // namespace ush
