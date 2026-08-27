#include "expand/field_split.hpp"

namespace ush {

namespace {

bool isIfsWhitespace(char c, const std::string& ifs) {
    return (c == ' ' || c == '\t' || c == '\n') && ifs.find(c) != std::string::npos;
}

bool isIfsChar(char c, const std::string& ifs) { return ifs.find(c) != std::string::npos; }

// Splits one segment (a run of the word with no forced $@-style field
// break inside it) on IFS. See splitFields() below for the overall
// algorithm; this implements the actual character-level delimiter state
// machine for a single segment. Assumes `ifs` is non-empty (the caller
// handles the "IFS is empty: no splitting" case before ever getting
// here).
std::vector<ExpandedWord> splitOneSegment(const ExpandedWord& segment, const std::string& ifs) {
    std::vector<ExpandedWord> fields;

    ExpandedWord curField;
    bool curFieldNonDiscardable = false;  // has real content, or came from quoting (even if empty)

    auto appendToCurrent = [&](const std::string& text, bool quoted) {
        if (text.empty() && !quoted) return;
        if (!curField.empty() && curField.back().quoted == quoted) {
            curField.back().text += text;
        } else {
            curField.push_back({text, quoted, false});
        }
        curFieldNonDiscardable = true;
    };
    auto emitField = [&]() {
        fields.push_back(std::move(curField));
        curField.clear();
        curFieldNonDiscardable = false;
    };

    // IFS white-space is trimmed entirely at the very start (only) -
    // §2.6.5 rule 1. A leading non-whitespace IFS character is NOT
    // trimmed: it still delimits, producing a leading empty field.
    bool inLeadingTrim = true;
    // While scanning a run of unquoted IFS characters, `inDelimiter`
    // tracks that we're in one, and `delimiterSawNonWhite` tracks whether
    // it has already consumed its (at most one) non-whitespace IFS
    // character - a second one starts a NEW delimiter (§2.6.5 rule 2/3).
    bool inDelimiter = false;
    bool delimiterSawNonWhite = false;

    auto endDelimiterIfPending = [&]() {
        if (!inDelimiter) return;
        if (!inLeadingTrim) emitField();  // even if empty - e.g. "a::b"'s middle field
        inDelimiter = false;
        delimiterSawNonWhite = false;
        inLeadingTrim = false;
    };

    for (const auto& piece : segment) {
        if (piece.quoted) {
            endDelimiterIfPending();
            inLeadingTrim = false;
            appendToCurrent(piece.text, true);
            continue;
        }
        std::size_t i = 0, n = piece.text.size();
        while (i < n) {
            char c = piece.text[i];
            bool white = isIfsWhitespace(c, ifs);
            bool other = !white && isIfsChar(c, ifs);
            if (white || other) {
                if (inLeadingTrim) {
                    if (other) {
                        inLeadingTrim = false;
                        inDelimiter = true;
                        delimiterSawNonWhite = true;
                    }
                    // pure whitespace during leading trim: consume silently
                } else if (!inDelimiter) {
                    inDelimiter = true;
                    delimiterSawNonWhite = other;
                } else if (other) {
                    if (delimiterSawNonWhite) {
                        emitField();  // second non-whitespace delimiter char -> empty field between
                    } else {
                        delimiterSawNonWhite = true;
                    }
                }
                // (whitespace while already inside a delimiter just
                // extends it - no state change needed.)
                ++i;
                continue;
            }
            // Regular (non-IFS) character: ends any pending delimiter.
            if (inDelimiter) {
                if (!inLeadingTrim) emitField();
                inDelimiter = false;
                delimiterSawNonWhite = false;
            }
            inLeadingTrim = false;
            std::size_t start = i;
            while (i < n && !isIfsWhitespace(piece.text[i], ifs) && !isIfsChar(piece.text[i], ifs)) {
                ++i;
            }
            appendToCurrent(piece.text.substr(start, i - start), false);
        }
    }

    // A trailing delimiter (of any kind) is discarded - but that only
    // means no *extra* field is manufactured after it; the field that was
    // accumulated *before* the delimiter started (curField) was already
    // complete and still needs emitting, regardless of whether we're
    // currently sitting mid-delimiter at end of input.
    if (curFieldNonDiscardable) emitField();

    return fields;
}

}  // namespace

std::vector<ExpandedWord> splitFields(const ExpandedWord& word, const std::string& ifs) {
    std::vector<ExpandedWord> allFields;

    ExpandedWord segment;
    auto flushSegment = [&]() {
        if (ifs.empty()) {
            bool anyQuoted = false, anyText = false;
            for (const auto& p : segment) {
                anyQuoted = anyQuoted || p.quoted;
                anyText = anyText || !p.text.empty();
            }
            if (anyText || anyQuoted) allFields.push_back(segment);
        } else {
            for (auto& f : splitOneSegment(segment, ifs)) allFields.push_back(std::move(f));
        }
        segment.clear();
    };

    for (const auto& piece : word) {
        segment.push_back(piece);
        if (piece.fieldBreakAfter) flushSegment();
    }
    if (!segment.empty()) flushSegment();

    return allFields;
}

}  // namespace ush
