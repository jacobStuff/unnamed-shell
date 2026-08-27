#include "expand/expander.hpp"

#include <cctype>
#include <pwd.h>

#include "expand/arithmetic.hpp"
#include "expand/pattern.hpp"
#include "lexer/lexer.hpp"
#include "runtime/environment.hpp"

namespace ush {

namespace {

bool isValidVariableName(const std::string& s) {
    if (s.empty()) return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        char c = s[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

// Finds the shortest (largest=false) or longest (largest=true) prefix of
// `value` matched in its entirety by `pattern`, and returns `value` with
// that prefix removed (or `value` unchanged if no prefix matches at all,
// including the empty one).
std::string trimMatchedPrefix(const std::string& value, const std::string& pattern,
                               bool largest) {
    std::size_t n = value.size();
    if (largest) {
        for (std::size_t k = n;; --k) {
            if (matchesPattern(pattern, value.substr(0, k))) return value.substr(k);
            if (k == 0) break;
        }
    } else {
        for (std::size_t k = 0; k <= n; ++k) {
            if (matchesPattern(pattern, value.substr(0, k))) return value.substr(k);
        }
    }
    return value;
}

// Same idea, from the end: finds the shortest/longest suffix matched in
// its entirety by `pattern`, and returns `value` with that suffix
// removed.
std::string trimMatchedSuffix(const std::string& value, const std::string& pattern,
                               bool largest) {
    std::size_t n = value.size();
    if (largest) {
        for (std::size_t m = n;; --m) {
            if (matchesPattern(pattern, value.substr(n - m))) return value.substr(0, n - m);
            if (m == 0) break;
        }
    } else {
        for (std::size_t m = 0; m <= n; ++m) {
            if (matchesPattern(pattern, value.substr(n - m))) return value.substr(0, n - m);
        }
    }
    return value;
}

}  // namespace

std::string flatten(const ExpandedWord& word) {
    std::string s;
    for (const auto& p : word) s += p.text;
    return s;
}

ExpandedWord Expander::expand(const Word& word) {
    ExpandedWord out;
    std::size_t startIndex = 0;
    if (!word.empty() && word[0].kind == WordPartKind::Literal && !word[0].text.empty() &&
        word[0].text[0] == '~') {
        expandTildeAndFirstPart(word[0].text, out);
        startIndex = 1;
    }
    expandPartsInto(word, startIndex, out);
    return out;
}

void Expander::expandPartsInto(const Word& word, std::size_t startIndex, ExpandedWord& out) {
    for (std::size_t i = startIndex; i < word.size(); ++i) expandPart(word[i], out);
}

void Expander::expandTildeAndFirstPart(const std::string& text, ExpandedWord& out) {
    // text[0] == '~' (checked by the caller).
    std::size_t slash = text.find('/');
    std::string userPart = (slash == std::string::npos) ? text.substr(1) : text.substr(1, slash - 1);
    std::string rest = (slash == std::string::npos) ? std::string() : text.substr(slash);

    std::optional<std::string> home;
    if (userPart.empty()) {
        home = env_.get("HOME");
    } else if (struct passwd* pw = getpwnam(userPart.c_str())) {
        home = std::string(pw->pw_dir);
    }

    if (home) {
        out.push_back({*home, false, false});
        if (!rest.empty()) out.push_back({rest, false, false});
    } else {
        // §2.6.1: if the tilde-prefix doesn't resolve (no HOME, or no such
        // user), it is used unmodified as ordinary (unquoted) text.
        out.push_back({text, false, false});
    }
}

void Expander::expandPart(const WordPart& part, ExpandedWord& out) {
    switch (part.kind) {
        case WordPartKind::Literal:
            out.push_back({part.text, false, false});
            return;
        case WordPartKind::SingleQuoted:
            out.push_back({part.text, true, false});
            return;
        case WordPartKind::DoubleQuoted:
            for (const auto& nestedPart : part.parts) {
                // "$@"/"$*" (bare, no operator) directly inside double
                // quotes get their own multi-field/joined handling; every
                // other nested part is expanded normally and then the
                // whole result is forced quoted, since §2.6.5/§2.6.6
                // exempt everything inside double quotes from splitting
                // and pathname-expansion metacharacters regardless of
                // what expanding it produced.
                if (nestedPart.kind == WordPartKind::ParamExpansion &&
                    (nestedPart.text == "@" || nestedPart.text == "*")) {
                    expandAtOrStar(nestedPart.text, /*quoted=*/true, out);
                    continue;
                }
                ExpandedWord nested;
                expandPart(nestedPart, nested);
                for (auto& piece : nested) {
                    piece.quoted = true;
                    out.push_back(std::move(piece));
                }
            }
            return;
        case WordPartKind::ParamExpansion:
            if (part.text == "@" || part.text == "*") {
                expandAtOrStar(part.text, /*quoted=*/false, out);
            } else {
                expandParameter(part.text, out);
            }
            return;
        case WordPartKind::CommandSubDollar:
        case WordPartKind::CommandSubBacktick:
            out.push_back({doCommandSubstitution(part.text), false, false});
            return;
        case WordPartKind::ArithExpansion:
            out.push_back(
                {std::to_string(evaluateArithmeticExpansion(part.text)), false, false});
            return;
    }
}

void Expander::expandAtOrStar(const std::string& name, bool quoted, ExpandedWord& out) {
    const auto& params = env_.positionalParams();
    if (name == "@") {
        if (params.empty()) return;  // contributes no fields at all
        for (std::size_t i = 0; i < params.size(); ++i) {
            out.push_back({params[i], quoted, i + 1 != params.size()});
        }
        return;
    }
    // name == "*": both quoted and unquoted forms join with the first
    // character of IFS (nothing, if IFS is empty) - §2.5.2. They differ
    // only in whether the joined result is later subject to field
    // splitting (unquoted: yes - and since the separator used here IS an
    // IFS character, that re-splitting reproduces the original fields;
    // quoted: no).
    std::string ifs = env_.ifsOrDefault();
    std::string sep = ifs.empty() ? std::string() : std::string(1, ifs[0]);
    std::string joined;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) joined += sep;
        joined += params[i];
    }
    out.push_back({joined, quoted, false});
}

std::optional<std::string> Expander::lookupParameter(const std::string& name) {
    if (name == "@" || name == "*") {
        ExpandedWord tmp;
        expandAtOrStar(name, /*quoted=*/true, tmp);
        return tmp.empty() ? std::string() : tmp[0].text;
    }
    if (name == "#") return std::to_string(env_.positionalParams().size());
    if (name == "?") return std::to_string(env_.lastExitStatus);
    if (name == "$") return std::to_string(env_.shellPid);
    if (name == "!") {
        return env_.lastBackgroundPid ? std::to_string(*env_.lastBackgroundPid) : std::string();
    }
    // "-": current option flags (`set -x` etc.) - not tracked yet.
    if (name == "-") return std::string();
    if (name == "0") return env_.shellName;

    if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) {
        std::size_t idx = static_cast<std::size_t>(std::stoul(name));
        if (idx == 0) return env_.shellName;
        const auto& params = env_.positionalParams();
        if (idx <= params.size()) return params[idx - 1];
        return std::nullopt;  // unset
    }

    return env_.get(name);
}

std::size_t Expander::parameterLength(const std::string& name) {
    if (name == "@" || name == "*") return env_.positionalParams().size();
    auto v = lookupParameter(name);
    return v ? v->size() : 0;
}

ExpandedWord Expander::expandOperandWord(const std::string& raw) {
    Lexer lex(raw);
    Word w = lex.scanWordUntilEnd();
    return expand(w);
}

void Expander::expandParameter(const std::string& raw, ExpandedWord& out) {
    if (raw.empty()) throw ExpansionError("${}: bad substitution");

    // ${#parameter}: length operator (§2.6.2). raw == "#" alone means the
    // special parameter '#' itself, not a length request on an
    // (impossible, empty-named) parameter.
    if (raw[0] == '#' && raw.size() > 1) {
        out.push_back({std::to_string(parameterLength(raw.substr(1))), false, false});
        return;
    }

    std::size_t i = 0;
    std::string name;
    if (raw[0] == '#') {
        name = "#";
        i = 1;
    } else if (std::isdigit(static_cast<unsigned char>(raw[0]))) {
        while (i < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i]))) ++i;
        name = raw.substr(0, i);
    } else if (std::isalpha(static_cast<unsigned char>(raw[0])) || raw[0] == '_') {
        while (i < raw.size() &&
               (std::isalnum(static_cast<unsigned char>(raw[i])) || raw[i] == '_')) {
            ++i;
        }
        name = raw.substr(0, i);
    } else if (std::string("@*?-$!").find(raw[0]) != std::string::npos) {
        name = std::string(1, raw[0]);
        i = 1;
    } else {
        throw ExpansionError("${" + raw + "}: bad substitution: invalid parameter name");
    }

    std::string rest = raw.substr(i);
    if (rest.empty()) {
        out.push_back({lookupParameter(name).value_or(""), false, false});
        return;
    }

    enum class Op {
        ColonDash, ColonEq, ColonQuestion, ColonPlus,
        Dash, Eq, Question, Plus,
        HashHash, Hash, PercentPercent, Percent,
    };
    Op op;
    std::string operandRaw;
    if (rest.starts_with(":-")) { op = Op::ColonDash; operandRaw = rest.substr(2); }
    else if (rest.starts_with(":=")) { op = Op::ColonEq; operandRaw = rest.substr(2); }
    else if (rest.starts_with(":?")) { op = Op::ColonQuestion; operandRaw = rest.substr(2); }
    else if (rest.starts_with(":+")) { op = Op::ColonPlus; operandRaw = rest.substr(2); }
    else if (rest.starts_with("##")) { op = Op::HashHash; operandRaw = rest.substr(2); }
    else if (rest.starts_with("#")) { op = Op::Hash; operandRaw = rest.substr(1); }
    else if (rest.starts_with("%%")) { op = Op::PercentPercent; operandRaw = rest.substr(2); }
    else if (rest.starts_with("%")) { op = Op::Percent; operandRaw = rest.substr(1); }
    else if (rest.starts_with("-")) { op = Op::Dash; operandRaw = rest.substr(1); }
    else if (rest.starts_with("=")) { op = Op::Eq; operandRaw = rest.substr(1); }
    else if (rest.starts_with("?")) { op = Op::Question; operandRaw = rest.substr(1); }
    else if (rest.starts_with("+")) { op = Op::Plus; operandRaw = rest.substr(1); }
    else throw ExpansionError("${" + raw + "}: bad substitution: unrecognized operator");

    std::optional<std::string> value = lookupParameter(name);
    bool isUnset = !value.has_value();
    bool isNullOrUnset = isUnset || value->empty();

    switch (op) {
        case Op::ColonDash:
        case Op::Dash: {
            bool useDefault = (op == Op::ColonDash) ? isNullOrUnset : isUnset;
            if (useDefault) {
                for (auto& p : expandOperandWord(operandRaw)) out.push_back(std::move(p));
            } else {
                out.push_back({*value, false, false});
            }
            return;
        }
        case Op::ColonEq:
        case Op::Eq: {
            bool useDefault = (op == Op::ColonEq) ? isNullOrUnset : isUnset;
            if (useDefault) {
                if (!isValidVariableName(name)) {
                    throw ExpansionError("${" + raw + "}: cannot assign to '" + name +
                                          "': not a valid identifier");
                }
                std::string flat = flatten(expandOperandWord(operandRaw));
                env_.set(name, flat);
                out.push_back({flat, false, false});
            } else {
                out.push_back({*value, false, false});
            }
            return;
        }
        case Op::ColonQuestion:
        case Op::Question: {
            bool isError = (op == Op::ColonQuestion) ? isNullOrUnset : isUnset;
            if (isError) {
                std::string msg = operandRaw.empty() ? "parameter null or not set"
                                                      : flatten(expandOperandWord(operandRaw));
                throw ExpansionError(name + ": " + msg);
            }
            out.push_back({*value, false, false});
            return;
        }
        case Op::ColonPlus:
        case Op::Plus: {
            bool useAlt = (op == Op::ColonPlus) ? !isNullOrUnset : !isUnset;
            if (useAlt) {
                for (auto& p : expandOperandWord(operandRaw)) out.push_back(std::move(p));
            }
            return;
        }
        case Op::Hash:
        case Op::HashHash:
        case Op::Percent:
        case Op::PercentPercent: {
            std::string base = value.value_or("");
            std::string pattern = buildPattern(expandOperandWord(operandRaw)).pattern;
            bool largest = (op == Op::HashHash || op == Op::PercentPercent);
            std::string result = (op == Op::Hash || op == Op::HashHash)
                                      ? trimMatchedPrefix(base, pattern, largest)
                                      : trimMatchedSuffix(base, pattern, largest);
            out.push_back({std::move(result), false, false});
            return;
        }
    }
}

std::string Expander::doCommandSubstitution(const std::string& rawSource) {
    if (!runner_) {
        throw ExpansionError("command substitution is not supported in this context");
    }
    std::string output = runner_->runAndCaptureStdout(rawSource);
    // §2.6.3: trailing newlines are removed (all of them).
    while (!output.empty() && output.back() == '\n') output.pop_back();
    return output;
}

std::intmax_t Expander::evaluateArithmeticExpansion(const std::string& rawExpr) {
    Lexer lex(rawExpr);
    Word w = lex.scanExpansionsUntilEnd();
    ExpandedWord ew;
    expandPartsInto(w, 0, ew);  // deliberately not expand(): see the header comment
    return evaluateArithmetic(flatten(ew), env_);
}

std::string Expander::expandHeredocBody(const std::string& rawBody) {
    Lexer lex(rawBody);
    Word w = lex.scanExpansionsUntilEnd();
    ExpandedWord ew;
    expandPartsInto(w, 0, ew);  // same reasoning as evaluateArithmeticExpansion() above
    return flatten(ew);
}

}  // namespace ush
