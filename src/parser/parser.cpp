#include "parser/parser.hpp"

#include <cctype>
#include <cstdlib>
#include <unordered_set>

namespace ush {

using namespace ush::ast;

Parser::Parser(std::string source) : lexer_(std::move(source)) {
    advance();  // prime current_ with the first token
}

// ---------------------------------------------------------------------
// token stream
// ---------------------------------------------------------------------

void Parser::advance() {
    if (current_.type == TokenType::Newline && !pendingHeredocs_.empty()) {
        auto pending = std::move(pendingHeredocs_);
        pendingHeredocs_.clear();
        for (auto& hd : pending) {
            hd->body = lexer_.consumeHeredocBody(hd->delimiter, hd->stripLeadingTabs);
        }
    }
    if (!lookahead_.empty()) {
        current_ = std::move(lookahead_.front());
        lookahead_.pop_front();
    } else {
        current_ = lexer_.next();
    }
}

const Token& Parser::peekAhead(std::size_t n) {
    while (lookahead_.size() < n) {
        if (!lookahead_.empty() && lookahead_.back().type == TokenType::Newline) {
            // See the header comment on peekAhead(): every current call
            // site is guarded so this is never actually reached (a `&&`
            // short-circuits before asking for a token past a NEWLINE).
            // Reading past a NEWLINE here would be unsafe: a pending
            // here-document's body sits at exactly that source position,
            // and must be read via advance() (which knows how), not
            // tokenized as ordinary shell syntax by a speculative peek.
            throw std::logic_error("ush parser: lookahead attempted past a NEWLINE token");
        }
        lookahead_.push_back(lexer_.next());
    }
    return lookahead_[n - 1];
}

// ---------------------------------------------------------------------
// reserved words (§2.4)
// ---------------------------------------------------------------------

namespace {
const std::unordered_set<std::string>& reservedWordSet() {
    static const std::unordered_set<std::string> words = {
        "!", "{", "}", "case", "do",   "done", "elif", "else",
        "esac", "fi", "for", "if", "in", "then", "until", "while",
    };
    return words;
}

// Reserved words that close a compound_list (used to know when parseTerm
// should stop without erroring, leaving the token for the caller to
// validate). RParen and DSemi are handled separately since they aren't
// reserved words.
const std::unordered_set<std::string>& closerWords() {
    static const std::unordered_set<std::string> words = {
        "}", "then", "do", "done", "fi", "esac", "elif", "else",
    };
    return words;
}

// Reserved words that can start a compound_command.
const std::unordered_set<std::string>& compoundStarterWords() {
    static const std::unordered_set<std::string> words = {"{", "for", "case", "if", "while",
                                                            "until"};
    return words;
}
}  // namespace

std::optional<std::string> Parser::reservedWordIfAny(const Token& tok) {
    if (tok.type != TokenType::Word || !tok.couldBeReservedWord) return std::nullopt;
    const std::string& text = wordAsUnquotedLiteral(tok.word);
    if (reservedWordSet().count(text)) return text;
    return std::nullopt;
}

bool Parser::isReservedWord(const Token& tok, std::string_view word) {
    auto rw = reservedWordIfAny(tok);
    return rw && *rw == word;
}

void Parser::expectReservedWord(std::string_view word) {
    if (!isReservedWord(current_, word)) {
        error("expected '" + std::string(word) + "'");
    }
    advance();
}

bool Parser::isValidName(const std::string& s) {
    if (s.empty()) return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        char c = s[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

std::optional<Parser::AssignmentSplit> Parser::trySplitAssignment(const Word& word) {
    if (word.empty() || word[0].kind != WordPartKind::Literal) return std::nullopt;
    const std::string& first = word[0].text;
    auto eq = first.find('=');
    if (eq == std::string::npos || eq == 0) return std::nullopt;
    for (std::size_t i = 0; i < eq; ++i) {
        char c = first[i];
        bool ok = (i == 0) ? (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
                            : (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
        if (!ok) return std::nullopt;
    }

    AssignmentSplit result;
    result.name = first.substr(0, eq);

    Word value;
    std::string rest = first.substr(eq + 1);
    if (!rest.empty()) value.emplace_back(WordPartKind::Literal, rest);
    for (std::size_t i = 1; i < word.size(); ++i) value.push_back(word[i]);
    result.value = std::move(value);
    return result;
}

Parser::FlattenedWord Parser::flattenSimpleWord(const Word& word) {
    FlattenedWord result;
    for (const auto& part : word) {
        switch (part.kind) {
            case WordPartKind::Literal:
                result.text += part.text;
                break;
            case WordPartKind::SingleQuoted:
                result.text += part.text;
                result.anyQuoted = true;
                break;
            case WordPartKind::DoubleQuoted: {
                result.anyQuoted = true;
                FlattenedWord nested = flattenSimpleWord(part.parts);
                result.text += nested.text;
                break;
            }
            case WordPartKind::ParamExpansion:
            case WordPartKind::CommandSubDollar:
            case WordPartKind::CommandSubBacktick:
            case WordPartKind::ArithExpansion:
                // See the header comment on flattenSimpleWord().
                break;
        }
    }
    return result;
}

// ---------------------------------------------------------------------
// list / term / separators
// ---------------------------------------------------------------------

bool Parser::canStartCommand(const Token& tok) {
    if (tok.type == TokenType::EndOfInput) return false;
    if (tok.type == TokenType::RParen) return false;
    if (tok.type == TokenType::DSemi) return false;
    if (tok.type == TokenType::Newline) return false;  // always pre-skipped; see skipLinebreak()
    if (auto rw = reservedWordIfAny(tok)) {
        if (closerWords().count(*rw)) return false;
    }
    return true;
}

void Parser::skipLinebreak() {
    while (current_.type == TokenType::Newline) advance();
}

void Parser::expectSequentialSep() {
    if (current_.type == TokenType::Semi) {
        advance();
        skipLinebreak();
        return;
    }
    if (current_.type == TokenType::Newline) {
        skipLinebreak();
        return;
    }
    error("expected ';' or a newline");
}

ast::List Parser::parseCompoundList() {
    skipLinebreak();
    return parseTerm();
}

ast::List Parser::parseTerm() {
    List list;
    while (canStartCommand(current_)) {
        ListItem item;
        item.andOr = parseAndOr();
        item.sep = Separator::None;

        if (current_.type == TokenType::Semi) {
            item.sep = Separator::Sequential;
            advance();
        } else if (current_.type == TokenType::And) {
            item.sep = Separator::Async;
            advance();
        }
        bool sawNewline = false;
        while (current_.type == TokenType::Newline) {
            advance();
            sawNewline = true;
        }
        if (item.sep == Separator::None && sawNewline) item.sep = Separator::Sequential;

        list.items.push_back(std::move(item));
        if (list.items.back().sep == Separator::None) break;
    }
    return list;
}

// ---------------------------------------------------------------------
// and_or / pipeline / command
// ---------------------------------------------------------------------

ast::AndOr Parser::parseAndOr() {
    AndOr result;
    result.first = parsePipeline();
    while (current_.type == TokenType::AndIf || current_.type == TokenType::OrIf) {
        bool isAnd = current_.type == TokenType::AndIf;
        advance();
        skipLinebreak();
        result.rest.emplace_back(isAnd, parsePipeline());
    }
    return result;
}

ast::Pipeline Parser::parsePipeline() {
    Pipeline p;
    if (isReservedWord(current_, "!")) {
        p.negated = true;
        advance();
    }
    p.commands.push_back(parseCommand());
    while (current_.type == TokenType::Pipe) {
        advance();
        skipLinebreak();
        p.commands.push_back(parseCommand());
    }
    return p;
}

std::unique_ptr<ast::Command> Parser::parseCommand() {
    auto cmd = std::make_unique<Command>();

    bool isCompoundStart = current_.type == TokenType::LParen;
    if (!isCompoundStart) {
        if (auto rw = reservedWordIfAny(current_)) {
            isCompoundStart = compoundStarterWords().count(*rw) != 0;
        }
    }
    if (isCompoundStart) {
        cmd->value = parseCompoundCommandBody();
        cmd->redirects = parseRedirectList();
        return cmd;
    }

    // function_definition: fname '(' ')' linebreak function_body (rule 8).
    // The `&&` below short-circuits before ever calling peekAhead(2) if
    // peekAhead(1) isn't even LParen, which keeps this safe per the
    // invariant documented on peekAhead().
    if (current_.type == TokenType::Word && current_.couldBeReservedWord &&
        isValidName(wordAsUnquotedLiteral(current_.word)) &&
        peekAhead(1).type == TokenType::LParen && peekAhead(2).type == TokenType::RParen) {
        cmd->value = parseFunctionDefinition();
        return cmd;
    }

    cmd->value = parseSimpleCommand();
    return cmd;
}

ast::CompoundCommand Parser::parseCompoundCommandBody() {
    if (current_.type == TokenType::LParen) return parseSubshell();
    if (auto rw = reservedWordIfAny(current_)) {
        if (*rw == "{") return parseBraceGroup();
        if (*rw == "for") return parseForClause();
        if (*rw == "case") return parseCaseClause();
        if (*rw == "if") return parseIfClause();
        if (*rw == "while") return parseWhileClause();
        if (*rw == "until") return parseUntilClause();
    }
    error("expected a compound command");
}

ast::SimpleCommand Parser::parseSimpleCommand() {
    SimpleCommand cmd;
    bool haveCmdName = false;
    while (true) {
        if (isRedirectStart(current_)) {
            cmd.redirects.push_back(parseRedirect());
            continue;
        }
        if (current_.type != TokenType::Word) break;

        if (!haveCmdName) {
            if (auto assign = trySplitAssignment(current_.word)) {
                cmd.assignments.push_back(Assignment{std::move(assign->name), std::move(assign->value)});
                advance();
                continue;
            }
            cmd.words.push_back(current_.word);
            haveCmdName = true;
            advance();
            continue;
        }

        // Subsequent words are plain arguments: no assignment check (rule
        // 1 only applies before the command name) and no reserved-word
        // check (rule 7b only applies to the first word of a command; see
        // the note on the Parser class).
        cmd.words.push_back(current_.word);
        advance();
    }
    if (cmd.assignments.empty() && cmd.words.empty() && cmd.redirects.empty()) {
        error("expected a command");
    }
    return cmd;
}

ast::FunctionDefinition Parser::parseFunctionDefinition() {
    FunctionDefinition fd;
    fd.name = wordAsUnquotedLiteral(current_.word);
    advance();  // fname
    expectOp(TokenType::LParen, "'('");
    expectOp(TokenType::RParen, "')'");
    skipLinebreak();
    fd.body = parseCompoundCommandBody();
    fd.redirects = parseRedirectList();
    return fd;
}

// ---------------------------------------------------------------------
// compound commands
// ---------------------------------------------------------------------

ast::BraceGroup Parser::parseBraceGroup() {
    expectReservedWord("{");
    BraceGroup bg;
    bg.body = parseCompoundList();
    expectReservedWord("}");
    return bg;
}

ast::Subshell Parser::parseSubshell() {
    expectOp(TokenType::LParen, "'('");
    Subshell sh;
    sh.body = parseCompoundList();
    expectOp(TokenType::RParen, "')'");
    return sh;
}

ast::List Parser::parseDoGroup() {
    expectReservedWord("do");
    List body = parseCompoundList();
    expectReservedWord("done");
    return body;
}

ast::ForClause Parser::parseForClause() {
    expectReservedWord("for");
    if (!(current_.type == TokenType::Word && current_.couldBeReservedWord)) {
        error("expected a name after 'for'");
    }
    std::string name = wordAsUnquotedLiteral(current_.word);
    if (!isValidName(name)) error("'" + name + "' is not a valid identifier");
    advance();

    // Strictly, POSIX only allows a linebreak here when an "in" clause
    // follows, and never a ';'; ush is lenient and accepts either a ';'
    // or a linebreak (or both fall through as neither, in the "for i
    // do...done" form), regardless of whether "in" follows - matching
    // every real-world shell's behavior for forms like "for i; do ... done"
    // and "for i\ndo ... done".
    if (current_.type == TokenType::Semi) advance();
    skipLinebreak();

    ForClause fc;
    fc.varName = std::move(name);
    if (isReservedWord(current_, "in")) {
        advance();
        std::vector<Word> words;
        while (current_.type == TokenType::Word) {
            words.push_back(current_.word);
            advance();
        }
        fc.words = std::move(words);
        expectSequentialSep();
    } else {
        fc.words = std::nullopt;
    }
    fc.body = parseDoGroup();
    return fc;
}

std::vector<Word> Parser::parsePatternList() {
    std::vector<Word> patterns;
    if (current_.type != TokenType::Word) error("expected a case pattern");
    patterns.push_back(current_.word);
    advance();
    while (current_.type == TokenType::Pipe) {
        advance();
        if (current_.type != TokenType::Word) error("expected a case pattern after '|'");
        patterns.push_back(current_.word);
        advance();
    }
    return patterns;
}

ast::CaseClause Parser::parseCaseClause() {
    expectReservedWord("case");
    CaseClause cc;
    if (current_.type != TokenType::Word) error("expected a word after 'case'");
    cc.subject = current_.word;
    advance();
    skipLinebreak();
    expectReservedWord("in");
    skipLinebreak();

    while (!isReservedWord(current_, "esac")) {
        CaseItem item;
        if (current_.type == TokenType::LParen) advance();  // optional leading '('
        item.patterns = parsePatternList();
        expectOp(TokenType::RParen, "')'");
        item.body = parseCompoundList();  // may be empty; naturally stops at DSemi/esac
        cc.items.push_back(std::move(item));

        if (current_.type == TokenType::DSemi) {
            advance();
            skipLinebreak();
            continue;
        }
        break;  // must be 'esac' now (this was the final, ";;"-less item)
    }
    expectReservedWord("esac");
    return cc;
}

ast::IfClause Parser::parseIfClause() {
    IfClause ic;
    expectReservedWord("if");
    IfClause::Branch first;
    first.cond = parseCompoundList();
    expectReservedWord("then");
    first.body = parseCompoundList();
    ic.branches.push_back(std::move(first));

    while (isReservedWord(current_, "elif")) {
        advance();
        IfClause::Branch branch;
        branch.cond = parseCompoundList();
        expectReservedWord("then");
        branch.body = parseCompoundList();
        ic.branches.push_back(std::move(branch));
    }
    if (isReservedWord(current_, "else")) {
        advance();
        ic.elseBranch = parseCompoundList();
    }
    expectReservedWord("fi");
    return ic;
}

ast::WhileClause Parser::parseWhileClause() {
    expectReservedWord("while");
    WhileClause wc;
    wc.cond = parseCompoundList();
    wc.body = parseDoGroup();
    return wc;
}

ast::UntilClause Parser::parseUntilClause() {
    expectReservedWord("until");
    UntilClause uc;
    uc.cond = parseCompoundList();
    uc.body = parseDoGroup();
    return uc;
}

// ---------------------------------------------------------------------
// redirections
// ---------------------------------------------------------------------

bool Parser::isRedirectStart(const Token& tok) const {
    if (tok.type == TokenType::IoNumber) return true;
    switch (tok.type) {
        case TokenType::Less:
        case TokenType::Great:
        case TokenType::DGreat:
        case TokenType::DLess:
        case TokenType::DLessDash:
        case TokenType::LessAnd:
        case TokenType::GreatAnd:
        case TokenType::LessGreat:
        case TokenType::Clobber:
            return true;
        default:
            return false;
    }
}

std::vector<ast::Redirect> Parser::parseRedirectList() {
    std::vector<Redirect> redirects;
    while (isRedirectStart(current_)) redirects.push_back(parseRedirect());
    return redirects;
}

ast::Redirect Parser::parseRedirect() {
    Redirect r;
    if (current_.type == TokenType::IoNumber) {
        r.ioNumber = std::atoi(current_.ioNumberText.c_str());
        advance();
    }

    TokenType op = current_.type;
    switch (op) {
        case TokenType::Less:
        case TokenType::Great:
        case TokenType::DGreat:
        case TokenType::DLess:
        case TokenType::DLessDash:
        case TokenType::LessAnd:
        case TokenType::GreatAnd:
        case TokenType::LessGreat:
        case TokenType::Clobber:
            r.op = op;
            break;
        default:
            error("expected a redirection operator");
    }
    advance();

    if (current_.type != TokenType::Word) error("expected a word after redirection operator");

    if (op == TokenType::DLess || op == TokenType::DLessDash) {
        FlattenedWord flat = flattenSimpleWord(current_.word);
        auto hd = std::make_shared<HereDoc>();
        hd->stripLeadingTabs = (op == TokenType::DLessDash);
        hd->literal = flat.anyQuoted;
        hd->delimiter = flat.text;
        r.target = hd;
        pendingHeredocs_.push_back(hd);
    } else {
        r.target = current_.word;
    }
    advance();
    return r;
}

void Parser::expectOp(TokenType type, const char* what) {
    if (current_.type != type) error(std::string("expected ") + what);
    advance();
}

// ---------------------------------------------------------------------
// top level / errors
// ---------------------------------------------------------------------

ast::List Parser::parseProgram() {
    List program = parseCompoundList();
    if (current_.type != TokenType::EndOfInput) {
        error("unexpected token '" + std::string(tokenTypeName(current_.type)) + "'");
    }
    return program;
}

void Parser::error(const std::string& message) const {
    // Every grammar-expectation failure looks at current_ right after
    // trying (and failing) to match something against it; if current_ is
    // EndOfInput, the input so far was a valid prefix of a complete
    // command that simply hasn't finished yet - see
    // ParseError::incomplete().
    throw ParseError(message, current_.loc, /*incomplete=*/current_.type == TokenType::EndOfInput);
}

}  // namespace ush
