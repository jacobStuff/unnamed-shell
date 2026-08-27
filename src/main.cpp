// ush entry point.
//
// Supports non-interactive use only so far (see docs/DESIGN.md roadmap
// item 8 - a real interactive prompt/line-editing loop is still to
// come):
//   ush -c 'command string' [name [arg...]]
//   ush script [arg...]
//   ush                        (reads the whole script from stdin)
//
// In every form, the whole input is read and parsed as one program
// before execution begins (no line-by-line interactive parsing yet).

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ast/ast.hpp"
#include "exec/executor.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "runtime/environment.hpp"

namespace {

std::string readAll(std::istream& in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    std::string source;
    std::string shellName = "ush";
    std::vector<std::string> scriptArgs;

    if (argc >= 3 && std::string(argv[1]) == "-c") {
        source = argv[2];
        shellName = argc >= 4 ? argv[3] : "ush";
        for (int i = 4; i < argc; ++i) scriptArgs.emplace_back(argv[i]);
    } else if (argc >= 2) {
        std::ifstream f(argv[1]);
        if (!f) {
            std::cerr << "ush: " << argv[1] << ": No such file or directory\n";
            return 127;
        }
        source = readAll(f);
        shellName = argv[1];
        for (int i = 2; i < argc; ++i) scriptArgs.emplace_back(argv[i]);
    } else {
        source = readAll(std::cin);
    }

    ush::Environment env(shellName);
    env.setPositionalParams(scriptArgs);
    ush::Executor executor(env);

    ush::ast::List program;
    try {
        ush::Parser parser(source);
        program = parser.parseProgram();
    } catch (const ush::LexError& e) {
        std::cerr << "ush: syntax error: " << e.what() << '\n';
        return 2;
    } catch (const ush::ParseError& e) {
        std::cerr << "ush: syntax error: " << e.what() << '\n';
        return 2;
    }

    return executor.runProgram(program);
}
