#include "lexer/lexer.hpp"

#include <iostream>
#include <iterator>
#include <string>

namespace {

std::string read_stdin() {
  return std::string(std::istreambuf_iterator<char>(std::cin),
                     std::istreambuf_iterator<char>());
}

void print_result(const toyc::LexResult& result) {
  if (!result.diagnostics.empty()) {
    std::cout << "reject\n";
    for (const auto& diagnostic : result.diagnostics) {
      std::cout << diagnostic.line << ' ' << diagnostic.message << '\n';
    }
    std::cout << '\n';
  }
  for (std::size_t i = 0; i < result.tokens.size(); ++i) {
    const toyc::Token& token = result.tokens[i];
    std::cout << i << ':' << toyc::token_type_name(token) << ":\"" << token.lexeme
              << "\"\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  bool token_mode = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-token") {
      token_mode = true;
      continue;
    }
    std::cerr << "unsupported argument: " << arg << '\n';
    std::cerr << "usage: compiler -token < input\n";
    return 2;
  }
  if (!token_mode) {
    std::cerr << "usage: compiler -token < input\n";
    return 2;
  }

  toyc::Lexer lexer(read_stdin());
  print_result(lexer.tokenize());
  return 0;
}
