#include "interactive/line_editor.hpp"

#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdio>

#include "exec/executor.hpp"

namespace ush {

namespace {

// Set (async-signal-safe: touches only a sig_atomic_t) by
// editorSigintHandler(), read and cleared by LineEditor::readByte() -
// mirrors Executor's own trap-signal pending-flag pattern (see
// executor.cpp), just local to the editor and only ever relevant when no
// user `trap ... INT` is in effect (see readLine()).
volatile sig_atomic_t g_editorInterrupt = 0;

void editorSigintHandler(int) { g_editorInterrupt = 1; }

// Thrown by readByte() when g_editorInterrupt fires, to unwind straight
// back to readLine()'s single catch site regardless of how deep the
// current key-handling call is - simpler than threading an "aborted"
// sentinel through every editing operation. Never escapes LineEditor.
struct InterruptedEditing {};

// RAII: puts stdin into "cbreak" mode for its lifetime - ICANON and ECHO
// off (so the editor gets bytes as they're typed and draws the line
// itself), but ISIG deliberately left on, so Ctrl-C/Ctrl-\/Ctrl-Z still
// generate their usual signals exactly as they would in canonical mode;
// only the terminal driver's own line buffering/echoing is disabled.
// Restores the original settings on destruction, including when an
// exception (ExitSignal, from a trap that calls `exit` mid-line - see
// readByte()) unwinds through here.
//
// Uses TCSANOW, not TCSADRAIN: TCSADRAIN waits for every byte already
// written to the terminal to actually be *read* by whatever's on the
// other end before the attribute change takes effect - normally
// instantaneous, but if the reader ever falls behind (a terminal
// emulator that's scrolled back/paused, or - as caught by an
// integration test - a test harness that stops reading once it's sent
// the input it cares about) this shell process would block forever
// inside what looks like a harmless mode-restore. TCSANOW changes the
// attributes immediately regardless of what's still queued to be read;
// this shell has no ordering dependency between "output written so far"
// and "raw mode restored" that would require the wait.
class RawMode {
public:
    RawMode() {
        if (::tcgetattr(STDIN_FILENO, &orig_) != 0) return;
        ok_ = true;
        struct termios raw = orig_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    ~RawMode() {
        if (ok_) ::tcsetattr(STDIN_FILENO, TCSANOW, &orig_);
    }
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;

private:
    struct termios orig_ {};
    bool ok_ = false;
};

// RAII: temporarily installs `handler` for SIGINT (via sigaction, no
// SA_RESTART - see executor.cpp's installSignalDisposition for why that
// matters: a blocking read() must actually see EINTR), restoring
// whatever was there before on destruction. Only constructed when there
// is no user `trap ... INT` in effect; when there is one, the trap's own
// handler (installed by Executor::setTrap) is left completely alone and
// this class is never involved, so the trap still runs normally.
class TempSigintHandler {
public:
    explicit TempSigintHandler(void (*handler)(int)) {
        struct sigaction sa {};
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sa.sa_handler = handler;
        if (::sigaction(SIGINT, &sa, &old_) == 0) installed_ = true;
    }
    ~TempSigintHandler() {
        if (installed_) ::sigaction(SIGINT, &old_, nullptr);
    }
    TempSigintHandler(const TempSigintHandler&) = delete;
    TempSigintHandler& operator=(const TempSigintHandler&) = delete;

private:
    struct sigaction old_ {};
    bool installed_ = false;
};

}  // namespace

LineEditor::LineEditor(Executor& executor) : executor_(executor), history_(executor.history()) {}

bool LineEditor::isUsable() { return ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0; }

void LineEditor::redraw() {
    std::string out;
    out += '\r';
    out += "\x1b[K";
    out += prompt_;
    out += buf_;
    if (cursor_ < buf_.size()) {
        out += "\x1b[";
        out += std::to_string(buf_.size() - cursor_);
        out += 'D';
    }
    ::write(STDOUT_FILENO, out.data(), out.size());
}

void LineEditor::insert(char c) {
    buf_.insert(buf_.begin() + static_cast<std::ptrdiff_t>(cursor_), c);
    ++cursor_;
    redraw();
}

void LineEditor::deleteBackward() {
    if (cursor_ == 0) return;
    buf_.erase(buf_.begin() + static_cast<std::ptrdiff_t>(cursor_ - 1));
    --cursor_;
    redraw();
}

void LineEditor::deleteForward() {
    if (cursor_ >= buf_.size()) return;
    buf_.erase(buf_.begin() + static_cast<std::ptrdiff_t>(cursor_));
    redraw();
}

void LineEditor::killToEnd() {
    if (cursor_ >= buf_.size()) return;
    killBuf_ = buf_.substr(cursor_);
    buf_.erase(cursor_);
    redraw();
}

void LineEditor::killToStart() {
    if (cursor_ == 0) return;
    killBuf_ = buf_.substr(0, cursor_);
    buf_.erase(0, cursor_);
    cursor_ = 0;
    redraw();
}

void LineEditor::killPrevWord() {
    if (cursor_ == 0) return;
    std::size_t end = cursor_;
    std::size_t i = cursor_;
    while (i > 0 && std::isspace(static_cast<unsigned char>(buf_[i - 1]))) --i;
    while (i > 0 && !std::isspace(static_cast<unsigned char>(buf_[i - 1]))) --i;
    killBuf_ = buf_.substr(i, end - i);
    buf_.erase(i, end - i);
    cursor_ = i;
    redraw();
}

void LineEditor::yank() {
    if (killBuf_.empty()) return;
    buf_.insert(cursor_, killBuf_);
    cursor_ += killBuf_.size();
    redraw();
}

void LineEditor::moveToHistory(std::size_t newPos) {
    if (newPos == historyPos_) return;
    if (historyPos_ == history_.size()) draftLine_ = buf_;  // stash the in-progress draft
    historyPos_ = newPos;
    buf_ = (historyPos_ == history_.size()) ? draftLine_ : history_[historyPos_];
    cursor_ = buf_.size();
    redraw();
}

bool LineEditor::readByte(char& c) {
    while (true) {
        ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n == 1) return true;
        if (n == 0) return false;  // EOF: terminal hung up
        if (errno == EINTR) {
            executor_.servicePendingTraps();
            if (g_editorInterrupt) {
                g_editorInterrupt = 0;
                throw InterruptedEditing{};
            }
            redraw();  // a trap that just ran may have printed something
            continue;
        }
        return false;  // unrecoverable read error - treat like EOF
    }
}

std::optional<char> LineEditor::readByteTimed(int tenthsOfSecond) {
    struct termios cur {};
    if (::tcgetattr(STDIN_FILENO, &cur) != 0) return std::nullopt;
    struct termios timed = cur;
    timed.c_cc[VMIN] = 0;
    timed.c_cc[VTIME] = static_cast<cc_t>(tenthsOfSecond);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &timed);
    char c = 0;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &cur);
    if (n == 1) return c;
    return std::nullopt;
}

// Parses the byte(s) following an ESC (0x1B) read by the main loop, using
// short read timeouts to tell a lone Escape key press (no-op) from the
// start of a real terminal escape sequence (which arrives as a fast
// burst) - covers the arrow keys, Home/End (both the ESC[H/ESC[F and the
// ESC[<n>~ forms different terminals use), and Delete (ESC[3~). Anything
// else recognized as CSI/SS3 but not one of these is silently ignored;
// this is a practical subset, not a full terminfo-driven implementation.
void LineEditor::handleEscapeSequence() {
    auto b1 = readByteTimed(1);
    if (!b1) return;  // lone Escape
    if (*b1 != '[' && *b1 != 'O') return;
    auto b2 = readByteTimed(2);
    if (!b2) return;
    char k = *b2;
    if (k == 'A') {
        if (historyPos_ > 0) moveToHistory(historyPos_ - 1);
        return;
    }
    if (k == 'B') {
        if (historyPos_ < history_.size()) moveToHistory(historyPos_ + 1);
        return;
    }
    if (k == 'C') {
        if (cursor_ < buf_.size()) {
            ++cursor_;
            redraw();
        }
        return;
    }
    if (k == 'D') {
        if (cursor_ > 0) {
            --cursor_;
            redraw();
        }
        return;
    }
    if (k == 'H') {
        cursor_ = 0;
        redraw();
        return;
    }
    if (k == 'F') {
        cursor_ = buf_.size();
        redraw();
        return;
    }
    if (k >= '0' && k <= '9') {
        std::string digits(1, k);
        std::optional<char> next;
        while ((next = readByteTimed(2)) && *next >= '0' && *next <= '9') digits += *next;
        if (next && *next == '~') {
            if (digits == "3") {
                deleteForward();
            } else if (digits == "1" || digits == "7") {
                cursor_ = 0;
                redraw();
            } else if (digits == "4" || digits == "8") {
                cursor_ = buf_.size();
                redraw();
            }
        }
    }
}

LineEditor::Result LineEditor::readLine(const std::string& prompt, std::string& outLine) {
    prompt_ = prompt;
    buf_.clear();
    cursor_ = 0;
    historyPos_ = history_.size();
    draftLine_.clear();

    std::fputs(prompt_.c_str(), stdout);
    std::fflush(stdout);

    RawMode raw;

    bool hasIntTrap = executor_.trapAction(SIGINT).has_value();
    std::optional<TempSigintHandler> tempSigint;
    if (!hasIntTrap) tempSigint.emplace(editorSigintHandler);

    try {
        while (true) {
            char c;
            if (!readByte(c)) {
                if (buf_.empty()) return Result::Eof;
                // A hard read failure with something already typed:
                // don't silently discard it - finish the line as if
                // Enter had been pressed.
                std::fputc('\n', stdout);
                std::fflush(stdout);
                outLine = buf_;
                return Result::Ok;
            }
            if (c == '\r' || c == '\n') {
                std::fputc('\n', stdout);
                std::fflush(stdout);
                outLine = buf_;
                return Result::Ok;
            }
            switch (c) {
                case 0x01:  // Ctrl-A: beginning of line
                    cursor_ = 0;
                    redraw();
                    break;
                case 0x05:  // Ctrl-E: end of line
                    cursor_ = buf_.size();
                    redraw();
                    break;
                case 0x02:  // Ctrl-B: back one character
                    if (cursor_ > 0) {
                        --cursor_;
                        redraw();
                    }
                    break;
                case 0x06:  // Ctrl-F: forward one character
                    if (cursor_ < buf_.size()) {
                        ++cursor_;
                        redraw();
                    }
                    break;
                case 0x04:  // Ctrl-D: EOF if the line is empty, else delete-forward
                    if (buf_.empty()) return Result::Eof;
                    deleteForward();
                    break;
                case 0x7F:
                case 0x08:  // Backspace (0x7F on most terminals, 0x08 on some)
                    deleteBackward();
                    break;
                case 0x0B:  // Ctrl-K: kill to end of line
                    killToEnd();
                    break;
                case 0x15:  // Ctrl-U: kill to beginning of line
                    killToStart();
                    break;
                case 0x17:  // Ctrl-W: kill previous word
                    killPrevWord();
                    break;
                case 0x19:  // Ctrl-Y: yank last kill
                    yank();
                    break;
                case 0x0C:  // Ctrl-L: clear screen, redraw
                    ::write(STDOUT_FILENO, "\x1b[H\x1b[2J", 7);
                    redraw();
                    break;
                case 0x10:  // Ctrl-P: previous history entry
                    if (historyPos_ > 0) moveToHistory(historyPos_ - 1);
                    break;
                case 0x0E:  // Ctrl-N: next history entry
                    if (historyPos_ < history_.size()) moveToHistory(historyPos_ + 1);
                    break;
                case 0x03:  // a raw ETX byte (VINTR remapped away from the
                            // signal path by unusual stty settings) - treat
                            // it the same as the signal would be
                    std::fputc('\n', stdout);
                    std::fflush(stdout);
                    return Result::Interrupted;
                case 0x1B:  // Escape: arrow keys, Home/End, Delete
                    handleEscapeSequence();
                    break;
                default:
                    if (static_cast<unsigned char>(c) >= 0x20) insert(c);
                    break;
            }
        }
    } catch (const InterruptedEditing&) {
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return Result::Interrupted;
    }
}

}  // namespace ush
