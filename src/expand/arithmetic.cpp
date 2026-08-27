#include "expand/arithmetic.hpp"

#include <cctype>
#include <unordered_map>
#include <vector>

#include "runtime/environment.hpp"

namespace ush {

namespace {

enum class TokKind {
    Number,
    Ident,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Shl,
    Shr,
    Lt,
    Le,
    Gt,
    Ge,
    EqEq,
    NotEq,
    Amp,
    Caret,
    Pipe,
    AndAnd,
    OrOr,
    Bang,
    Tilde,
    Question,
    Colon,
    Assign,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq,
    AmpEq,
    CaretEq,
    PipeEq,
    ShlEq,
    ShrEq,
    Comma,
    LParen,
    RParen,
    End,
};

struct Tok {
    TokKind kind;
    std::intmax_t number = 0;
    std::string ident;
};

std::vector<Tok> tokenize(const std::string& s) {
    std::vector<Tok> out;
    std::size_t i = 0, n = s.size();
    auto peek = [&](std::size_t o = 0) { return i + o < n ? s[i + o] : '\0'; };

    while (i < n) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t start = i;
            if (c == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
                i += 2;
                while (i < n && std::isxdigit(static_cast<unsigned char>(s[i]))) ++i;
            } else {
                while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            }
            std::string numText = s.substr(start, i - start);
            try {
                std::size_t consumed = 0;
                std::intmax_t v = static_cast<std::intmax_t>(std::stoll(numText, &consumed, 0));
                if (consumed != numText.size()) throw std::invalid_argument(numText);
                out.push_back({TokKind::Number, v, {}});
            } catch (...) {
                throw ArithError("invalid number '" + numText + "'");
            }
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t start = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            out.push_back({TokKind::Ident, 0, s.substr(start, i - start)});
            continue;
        }

        auto three = [&](char a, char b, char cc, TokKind k) {
            if (c == a && peek(1) == b && peek(2) == cc) {
                out.push_back({k, 0, {}});
                i += 3;
                return true;
            }
            return false;
        };
        auto two = [&](char a, char b, TokKind k) {
            if (c == a && peek(1) == b) {
                out.push_back({k, 0, {}});
                i += 2;
                return true;
            }
            return false;
        };

        if (three('<', '<', '=', TokKind::ShlEq)) continue;
        if (three('>', '>', '=', TokKind::ShrEq)) continue;
        if (two('<', '<', TokKind::Shl)) continue;
        if (two('>', '>', TokKind::Shr)) continue;
        if (two('<', '=', TokKind::Le)) continue;
        if (two('>', '=', TokKind::Ge)) continue;
        if (two('=', '=', TokKind::EqEq)) continue;
        if (two('!', '=', TokKind::NotEq)) continue;
        if (two('&', '&', TokKind::AndAnd)) continue;
        if (two('|', '|', TokKind::OrOr)) continue;
        if (two('+', '=', TokKind::PlusEq)) continue;
        if (two('-', '=', TokKind::MinusEq)) continue;
        if (two('*', '=', TokKind::StarEq)) continue;
        if (two('/', '=', TokKind::SlashEq)) continue;
        if (two('%', '=', TokKind::PercentEq)) continue;
        if (two('&', '=', TokKind::AmpEq)) continue;
        if (two('^', '=', TokKind::CaretEq)) continue;
        if (two('|', '=', TokKind::PipeEq)) continue;

        TokKind k;
        switch (c) {
            case '+': k = TokKind::Plus; break;
            case '-': k = TokKind::Minus; break;
            case '*': k = TokKind::Star; break;
            case '/': k = TokKind::Slash; break;
            case '%': k = TokKind::Percent; break;
            case '<': k = TokKind::Lt; break;
            case '>': k = TokKind::Gt; break;
            case '&': k = TokKind::Amp; break;
            case '^': k = TokKind::Caret; break;
            case '|': k = TokKind::Pipe; break;
            case '!': k = TokKind::Bang; break;
            case '~': k = TokKind::Tilde; break;
            case '?': k = TokKind::Question; break;
            case ':': k = TokKind::Colon; break;
            case '=': k = TokKind::Assign; break;
            case ',': k = TokKind::Comma; break;
            case '(': k = TokKind::LParen; break;
            case ')': k = TokKind::RParen; break;
            default:
                throw ArithError(std::string("unexpected character '") + c +
                                  "' in arithmetic expression");
        }
        out.push_back({k, 0, {}});
        ++i;
    }
    out.push_back({TokKind::End, 0, {}});
    return out;
}

bool isAssignOp(TokKind k) {
    switch (k) {
        case TokKind::Assign:
        case TokKind::PlusEq:
        case TokKind::MinusEq:
        case TokKind::StarEq:
        case TokKind::SlashEq:
        case TokKind::PercentEq:
        case TokKind::AmpEq:
        case TokKind::CaretEq:
        case TokKind::PipeEq:
        case TokKind::ShlEq:
        case TokKind::ShrEq:
            return true;
        default:
            return false;
    }
}

// Recursive-descent evaluator over a pre-tokenized expression. Evaluates
// directly (no separate AST) since nothing downstream needs to reuse or
// inspect an arithmetic expression's structure.
class ArithEvaluator {
public:
    ArithEvaluator(std::vector<Tok> toks, Environment& env) : toks_(std::move(toks)), env_(env) {}

    std::intmax_t run() {
        std::intmax_t v = parseComma();
        if (!check(TokKind::End)) throw ArithError("syntax error in arithmetic expression");
        return v;
    }

private:
    std::vector<Tok> toks_;
    std::size_t pos_ = 0;
    Environment& env_;

    const Tok& cur() const { return toks_[pos_]; }
    bool check(TokKind k) const { return cur().kind == k; }
    void advance() {
        if (pos_ + 1 < toks_.size()) ++pos_;
    }
    bool match(TokKind k) {
        if (check(k)) {
            advance();
            return true;
        }
        return false;
    }
    void expect(TokKind k, const char* what) {
        if (!match(k)) throw ArithError(std::string("expected ") + what);
    }

    // A shell variable that is unset, or whose value doesn't parse cleanly
    // as an integer constant, evaluates to 0. (Some shells additionally
    // re-evaluate a non-numeric value as another arithmetic expression, or
    // chase a value that is itself a variable name; ush does not - a
    // documented simplification, see docs/DESIGN.md.)
    std::intmax_t lookupVar(const std::string& name) {
        auto v = env_.get(name);
        if (!v || v->empty()) return 0;
        try {
            std::size_t consumed = 0;
            std::intmax_t val = static_cast<std::intmax_t>(std::stoll(*v, &consumed, 0));
            if (consumed != v->size()) return 0;
            return val;
        } catch (...) {
            return 0;
        }
    }

    std::intmax_t parseComma() {
        std::intmax_t v = parseAssignment();
        while (match(TokKind::Comma)) v = parseAssignment();
        return v;
    }

    std::intmax_t parseAssignment() {
        if (check(TokKind::Ident) && pos_ + 1 < toks_.size() && isAssignOp(toks_[pos_ + 1].kind)) {
            std::string name = cur().ident;
            advance();  // identifier
            TokKind op = cur().kind;
            advance();  // operator
            std::intmax_t rhs = parseAssignment();  // right-associative

            std::intmax_t result;
            if (op == TokKind::Assign) {
                result = rhs;
            } else {
                std::intmax_t curVal = lookupVar(name);
                switch (op) {
                    case TokKind::PlusEq: result = curVal + rhs; break;
                    case TokKind::MinusEq: result = curVal - rhs; break;
                    case TokKind::StarEq: result = curVal * rhs; break;
                    case TokKind::SlashEq:
                        if (rhs == 0) throw ArithError("division by zero");
                        result = curVal / rhs;
                        break;
                    case TokKind::PercentEq:
                        if (rhs == 0) throw ArithError("division by zero");
                        result = curVal % rhs;
                        break;
                    case TokKind::AmpEq: result = curVal & rhs; break;
                    case TokKind::CaretEq: result = curVal ^ rhs; break;
                    case TokKind::PipeEq: result = curVal | rhs; break;
                    case TokKind::ShlEq: result = curVal << rhs; break;
                    case TokKind::ShrEq: result = curVal >> rhs; break;
                    default: result = rhs; break;  // unreachable
                }
            }
            env_.set(name, std::to_string(result));
            return result;
        }
        return parseConditional();
    }

    std::intmax_t parseConditional() {
        std::intmax_t cond = parseLogicalOr();
        if (match(TokKind::Question)) {
            std::intmax_t thenV = parseAssignment();
            expect(TokKind::Colon, "':'");
            std::intmax_t elseV = parseConditional();
            return cond != 0 ? thenV : elseV;
        }
        return cond;
    }

    std::intmax_t parseLogicalOr() {
        std::intmax_t v = parseLogicalAnd();
        while (match(TokKind::OrOr)) {
            std::intmax_t rhs = parseLogicalAnd();
            v = (v != 0 || rhs != 0) ? 1 : 0;
        }
        return v;
    }
    std::intmax_t parseLogicalAnd() {
        std::intmax_t v = parseBitOr();
        while (match(TokKind::AndAnd)) {
            std::intmax_t rhs = parseBitOr();
            v = (v != 0 && rhs != 0) ? 1 : 0;
        }
        return v;
    }
    std::intmax_t parseBitOr() {
        std::intmax_t v = parseBitXor();
        while (match(TokKind::Pipe)) v |= parseBitXor();
        return v;
    }
    std::intmax_t parseBitXor() {
        std::intmax_t v = parseBitAnd();
        while (match(TokKind::Caret)) v ^= parseBitAnd();
        return v;
    }
    std::intmax_t parseBitAnd() {
        std::intmax_t v = parseEquality();
        while (match(TokKind::Amp)) v &= parseEquality();
        return v;
    }
    std::intmax_t parseEquality() {
        std::intmax_t v = parseRelational();
        while (true) {
            if (match(TokKind::EqEq)) v = (v == parseRelational());
            else if (match(TokKind::NotEq)) v = (v != parseRelational());
            else break;
        }
        return v;
    }
    std::intmax_t parseRelational() {
        std::intmax_t v = parseShift();
        while (true) {
            if (match(TokKind::Lt)) v = (v < parseShift());
            else if (match(TokKind::Le)) v = (v <= parseShift());
            else if (match(TokKind::Gt)) v = (v > parseShift());
            else if (match(TokKind::Ge)) v = (v >= parseShift());
            else break;
        }
        return v;
    }
    std::intmax_t parseShift() {
        std::intmax_t v = parseAdditive();
        while (true) {
            if (match(TokKind::Shl)) v <<= parseAdditive();
            else if (match(TokKind::Shr)) v >>= parseAdditive();
            else break;
        }
        return v;
    }
    std::intmax_t parseAdditive() {
        std::intmax_t v = parseMultiplicative();
        while (true) {
            if (match(TokKind::Plus)) v += parseMultiplicative();
            else if (match(TokKind::Minus)) v -= parseMultiplicative();
            else break;
        }
        return v;
    }
    std::intmax_t parseMultiplicative() {
        std::intmax_t v = parseUnary();
        while (true) {
            if (match(TokKind::Star)) {
                v *= parseUnary();
            } else if (match(TokKind::Slash)) {
                std::intmax_t rhs = parseUnary();
                if (rhs == 0) throw ArithError("division by zero");
                v /= rhs;
            } else if (match(TokKind::Percent)) {
                std::intmax_t rhs = parseUnary();
                if (rhs == 0) throw ArithError("division by zero");
                v %= rhs;
            } else {
                break;
            }
        }
        return v;
    }
    std::intmax_t parseUnary() {
        if (match(TokKind::Plus)) return parseUnary();
        if (match(TokKind::Minus)) return -parseUnary();
        if (match(TokKind::Bang)) return parseUnary() == 0 ? 1 : 0;
        if (match(TokKind::Tilde)) return ~parseUnary();
        return parsePrimary();
    }
    std::intmax_t parsePrimary() {
        if (check(TokKind::Number)) {
            std::intmax_t v = cur().number;
            advance();
            return v;
        }
        if (check(TokKind::Ident)) {
            std::string name = cur().ident;
            advance();
            return lookupVar(name);
        }
        if (match(TokKind::LParen)) {
            std::intmax_t v = parseComma();
            expect(TokKind::RParen, "')'");
            return v;
        }
        throw ArithError("expected an expression");
    }
};

}  // namespace

std::intmax_t evaluateArithmetic(const std::string& expr, Environment& env) {
    return ArithEvaluator(tokenize(expr), env).run();
}

}  // namespace ush
