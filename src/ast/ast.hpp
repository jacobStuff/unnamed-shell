// Abstract syntax tree produced by the parser, per POSIX.1-2017 Shell &
// Utilities §2.10.2 "Shell Grammar Rules".
//
// Node types are ordered in this file specifically so that the only
// recursive edge (a Pipeline's commands, which are Commands, which can
// contain compound commands, which contain Lists, which contain
// Pipelines...) is broken by a single forward declaration of `Command`
// plus `std::vector<std::unique_ptr<Command>>` in Pipeline. Every other
// node holds its children by value. See docs/DESIGN.md.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "lexer/token.hpp"

namespace ush::ast {

using ush::TokenType;
using ush::Word;

struct Assignment {
    std::string name;
    Word value;
};

// A here-document (§2.7.4). `body` is filled in by the parser once the
// NEWLINE that ends the current command line is reached (here-document
// content always starts on the line(s) immediately following that
// newline, however many more tokens appear before it on the same line -
// see Parser::advance()).
struct HereDoc {
    bool stripLeadingTabs = false;  // "<<-" vs "<<"
    // True if the delimiter word was quoted in any way ('...', "...", or a
    // backslash escape). A literal here-document's body is used verbatim;
    // otherwise it undergoes the same expansions as double-quoted text
    // (§2.7.4) - which, like other raw substitution text produced by the
    // lexer, is the expansion stage's job, not the parser's.
    bool literal = false;
    std::string delimiter;  // delimiter text after quote removal
    std::string body;
};

// One redirection. `op` is always one of Less/Great/DGreat/LessAnd/
// GreatAnd/LessGreat/Clobber/DLess/DLessDash.
struct Redirect {
    std::optional<int> ioNumber;
    TokenType op = TokenType::Less;
    std::variant<Word, std::shared_ptr<HereDoc>> target;
};

struct SimpleCommand {
    std::vector<Assignment> assignments;  // leading cmd_prefix ASSIGNMENT_WORDs
    std::vector<Word> words;              // words[0] is the command name, if any
    std::vector<Redirect> redirects;      // from cmd_prefix/cmd_suffix, in source order
};

struct Command;  // see file header comment

struct Pipeline {
    bool negated = false;  // leading '!'
    std::vector<std::unique_ptr<Command>> commands;  // pipe_sequence, left to right
};

enum class Separator {
    None,        // no separator followed (only valid, and always true, for a list's last item)
    Sequential,  // ';' or a bare newline: wait for completion before continuing
    Async,       // '&': run asynchronously
};

struct AndOr {
    Pipeline first;
    std::vector<std::pair<bool /* isAnd: && vs || */, Pipeline>> rest;
};

struct ListItem {
    AndOr andOr;
    Separator sep;
};

struct List {
    std::vector<ListItem> items;
};

struct BraceGroup {
    List body;
};

struct Subshell {
    List body;
};

struct ForClause {
    std::string varName;
    std::optional<std::vector<Word>> words;  // nullopt: no "in" clause (defaults to "$@")
    List body;
};

struct CaseItem {
    std::vector<Word> patterns;  // alternatives joined by '|' in the source
    List body;                    // may be empty (no compound_list before ';;'/esac)
};

struct CaseClause {
    Word subject;
    std::vector<CaseItem> items;
};

struct IfClause {
    struct Branch {
        List cond;
        List body;
    };
    std::vector<Branch> branches;  // branches[0] is the `if`; the rest are `elif`s
    std::optional<List> elseBranch;
};

struct WhileClause {
    List cond;
    List body;
};

struct UntilClause {
    List cond;
    List body;
};

using CompoundCommand = std::variant<BraceGroup, Subshell, ForClause, CaseClause, IfClause,
                                      WhileClause, UntilClause>;

struct FunctionDefinition {
    std::string name;
    CompoundCommand body;
    std::vector<Redirect> redirects;  // trailing redirect_list on the function_body (rule 9)
};

struct Command {
    std::variant<SimpleCommand, CompoundCommand, FunctionDefinition> value;
    // Trailing redirect_list attached directly to a compound command
    // (grammar: "command: compound_command redirect_list"). Always empty
    // when `value` holds a SimpleCommand (which carries its own redirects
    // inline) or a FunctionDefinition (which carries its own per rule 9).
    std::vector<Redirect> redirects;
};

}  // namespace ush::ast
