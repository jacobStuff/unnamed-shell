#include "lexer/token.hpp"

namespace ush {

bool wordIsUnquotedLiteral(const Word& word) {
    return word.size() == 1 && word[0].kind == WordPartKind::Literal;
}

std::string wordAsUnquotedLiteral(const Word& word) {
    return wordIsUnquotedLiteral(word) ? word[0].text : std::string();
}

const char* tokenTypeName(TokenType type) {
    switch (type) {
        case TokenType::Word: return "WORD";
        case TokenType::Newline: return "NEWLINE";
        case TokenType::EndOfInput: return "EOF";
        case TokenType::And: return "&";
        case TokenType::AndIf: return "&&";
        case TokenType::Pipe: return "|";
        case TokenType::OrIf: return "||";
        case TokenType::Semi: return ";";
        case TokenType::DSemi: return ";;";
        case TokenType::LParen: return "(";
        case TokenType::RParen: return ")";
        case TokenType::Less: return "<";
        case TokenType::Great: return ">";
        case TokenType::DGreat: return ">>";
        case TokenType::DLess: return "<<";
        case TokenType::DLessDash: return "<<-";
        case TokenType::LessAnd: return "<&";
        case TokenType::GreatAnd: return ">&";
        case TokenType::LessGreat: return "<>";
        case TokenType::Clobber: return ">|";
        case TokenType::IoNumber: return "IO_NUMBER";
    }
    return "?";
}

}  // namespace ush
