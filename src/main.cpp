// Placeholder entry point.
//
// ush doesn't have a parser or executor yet (see docs/DESIGN.md for the
// roadmap). Until they exist, this reads the whole of stdin and prints the
// token stream the lexer produces, so the lexer can be exercised end-to-end
// from the command line while the rest of the pipeline is built out.

#include <iostream>
#include <sstream>
#include <string>

#include "lexer/lexer.hpp"
#include "lexer/token.hpp"

namespace {

std::string wordToDebugString(const ush::Word& word);

std::string wordPartToDebugString(const ush::WordPart& part) {
    using ush::WordPartKind;
    switch (part.kind) {
        case WordPartKind::Literal:
            return part.text;
        case WordPartKind::SingleQuoted:
            return "'" + part.text + "'";
        case WordPartKind::DoubleQuoted:
            return "\"" + wordToDebugString(part.parts) + "\"";
        case WordPartKind::ParamExpansion:
            return "${" + part.text + "}";
        case WordPartKind::CommandSubDollar:
            return "$(" + part.text + ")";
        case WordPartKind::CommandSubBacktick:
            return "`" + part.text + "`";
        case WordPartKind::ArithExpansion:
            return "$((" + part.text + "))";
    }
    return "";
}

std::string wordToDebugString(const ush::Word& word) {
    std::string out;
    for (const auto& part : word) out += wordPartToDebugString(part);
    return out;
}

}  // namespace

int main() {
    std::ostringstream buf;
    buf << std::cin.rdbuf();

    ush::Lexer lexer(buf.str());
    for (;;) {
        ush::Token tok;
        try {
            tok = lexer.next();
        } catch (const ush::LexError& e) {
            std::cerr << "ush: lex error at line " << e.location().line << ", column "
                      << e.location().column << ": " << e.what() << '\n';
            return 1;
        }
        if (tok.type == ush::TokenType::EndOfInput) break;

        std::cout << ush::tokenTypeName(tok.type);
        if (tok.type == ush::TokenType::Word) {
            std::cout << "(" << wordToDebugString(tok.word) << ")";
        } else if (tok.type == ush::TokenType::IoNumber) {
            std::cout << "(" << tok.ioNumberText << ")";
        }
        std::cout << '\n';
    }
    return 0;
}
