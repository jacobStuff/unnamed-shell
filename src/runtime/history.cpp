#include "runtime/history.hpp"

#include <fstream>

namespace ush {

void History::trim() {
    if (maxSize_ == 0 || entries_.size() <= maxSize_) return;
    std::size_t drop = entries_.size() - maxSize_;
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(drop));
    firstNumber_ += drop;
}

void History::setMaxSize(std::size_t n) {
    maxSize_ = n;
    trim();
}

void History::clear() {
    firstNumber_ += entries_.size();
    entries_.clear();
}

void History::add(std::string entry) {
    if (!entry.empty() && entry.back() == '\n') entry.pop_back();
    // "empty/all whitespace" - a bare Enter, or a continuation buffer that
    // somehow ended up with nothing but blank lines.
    bool blank = entry.find_first_not_of(" \t\n") == std::string::npos;
    if (blank) return;
    if (!entries_.empty() && entries_.back() == entry) return;
    entries_.push_back(std::move(entry));
    trim();
}

const std::string* History::byNumber(std::size_t number) const {
    if (empty() || number < firstNumber_ || number > lastNumber()) return nullptr;
    return &entries_[number - firstNumber_];
}

void History::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) add(line);
}

void History::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    for (const auto& e : entries_) out << e << '\n';
}

}  // namespace ush
