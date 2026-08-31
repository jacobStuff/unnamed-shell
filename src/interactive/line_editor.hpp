// A minimal emacs-style line editor for the interactive prompt: cursor
// movement, in-place editing, and arrow-key history recall - the "real
// line editing/history" item from docs/DESIGN.md's roadmap.
//
// Only usable on an actual terminal (see isUsable()) - main.cpp falls
// back to plain line-at-a-time reading otherwise (piped/redirected
// stdin, e.g. every integration test that drives `ush -i` over a pipe,
// and `ush -i < script`), matching how real shells disable interactive
// editing in the same situation while still building a command history.
// History itself (recording, `fc`/`history`) is NOT gated on this - see
// Executor::history() - only the raw-mode keystroke UI is.

#pragma once

#include <optional>
#include <string>

#include "runtime/history.hpp"

namespace ush {

class Executor;

class LineEditor {
public:
    explicit LineEditor(Executor& executor);

    // True if both stdin and stdout are actual terminals - the only
    // situation raw-mode editing makes sense in (tcgetattr/tcsetattr on a
    // non-tty fail outright, and there'd be no point anyway).
    static bool isUsable();

    enum class Result { Ok, Eof, Interrupted };

    // Reads one line, having already displayed `prompt`. On Result::Ok,
    // `line` holds the text (no trailing newline). This does NOT add
    // `line` to Executor::history() itself - that's the caller's job,
    // once a full (possibly multi-line) logical command is known to have
    // parsed; see main.cpp. On Eof
    // (Ctrl-D on an empty line, or the terminal going away), `line` is
    // left untouched. On Interrupted (Ctrl-C with no `trap ... INT` in
    // effect - see the .cpp for how a real trap is handled instead), a
    // newline has already been echoed and `line` is untouched; the
    // caller should discard whatever multi-line command it was
    // accumulating and re-prompt from the top, like real shells do.
    Result readLine(const std::string& prompt, std::string& line);

private:
    Executor& executor_;
    std::string prompt_;
    std::string buf_;
    std::size_t cursor_ = 0;
    std::string killBuf_;

    // Arrow-key recall state for the *current* readLine() call - reset at
    // its start. historyPos_ == history_.size() means "not currently
    // browsing, looking at the fresh line the user is typing"; draftLine_
    // stashes that fresh line so pressing Down back past the newest
    // history entry restores exactly what was there before Up was first
    // pressed.
    History& history_;
    std::size_t historyPos_ = 0;
    std::string draftLine_;

    void redraw();
    void insert(char c);
    void deleteBackward();
    void deleteForward();
    void killToEnd();
    void killToStart();
    void killPrevWord();
    void yank();
    void moveToHistory(std::size_t newPos);
    // Reads one raw byte, retrying/servicing traps across EINTR. Returns
    // false on EOF/unrecoverable read error.
    bool readByte(char& c);
    // Reads one byte with a short timeout (for telling a lone Escape key
    // from the start of a real escape sequence apart). std::nullopt on
    // timeout.
    std::optional<char> readByteTimed(int tenthsOfSecond);
    void handleEscapeSequence();
};

}  // namespace ush
