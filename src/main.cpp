#include "codegen/codegen.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "sema/sema.hpp"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

void print_ast_result(std::vector<toyc::Diagnostic> diagnostics) {
  std::stable_sort(diagnostics.begin(), diagnostics.end(),
                   [](const toyc::Diagnostic& lhs, const toyc::Diagnostic& rhs) {
                     if (lhs.line != rhs.line) {
                       return lhs.line < rhs.line;
                     }
                     return lhs.column < rhs.column;
                   });
  if (diagnostics.empty()) {
    std::cout << "accept\n";
    return;
  }
  std::cout << "reject\n";
  for (const toyc::Diagnostic& diagnostic : diagnostics) {
    std::cout << diagnostic.line << ' ' << diagnostic.message << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  bool token_mode = false;
  bool ast_mode = false;
  bool asm_mode = false;
  bool optimize = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-token") {
      token_mode = true;
      continue;
    }
    if (arg == "-ast") {
      ast_mode = true;
      continue;
    }
    if (arg == "-asm") {
      asm_mode = true;
      continue;
    }
    if (arg == "-opt") {
      optimize = true;
      continue;
    }
    std::cerr << "unsupported argument: " << arg << '\n';
    std::cerr << "usage: compiler -token|-ast|-asm [-opt] < input\n";
    return 2;
  }
  const int modes = static_cast<int>(token_mode) + static_cast<int>(ast_mode) +
                    static_cast<int>(asm_mode);
  if (modes != 1 || (optimize && !asm_mode)) {
    std::cerr << "usage: compiler -token|-ast|-asm [-opt] < input\n";
    return 2;
  }

  toyc::Lexer lexer(read_stdin());
  const toyc::LexResult lex = lexer.tokenize();
  if (token_mode) {
    print_result(lex);
    return 0;
  }
  if (asm_mode) {
    if (!lex.diagnostics.empty()) {
      std::cerr << "lexical error\n";
      return 1;
    }
    std::vector<toyc::Diagnostic> syntax;
    toyc::Parser parser(lex.tokens, syntax, lex.eof_line, lex.eof_column);
    std::unique_ptr<toyc::CompUnit> unit = parser.parse();
    if (!syntax.empty() || !unit) {
      std::cerr << "syntax error\n";
      return 1;
    }
    std::vector<toyc::Diagnostic> semantic;
    toyc::analyze(*unit, semantic);
    std::cout << toyc::emit_asm(*unit, optimize);
    return 0;
  }

  std::vector<toyc::Diagnostic> syntax;
  toyc::Parser parser(lex.tokens, syntax, lex.eof_line, lex.eof_column);
  // 树只留在内存里供语义分析和后续代码生成使用，-ast 不打印树。
  std::unique_ptr<toyc::CompUnit> unit = parser.parse();
  std::vector<toyc::Diagnostic> semantic;
  // 参考文档：语法通过之后才做语义检查，避免未闭合结构再派生 missing return。
  if (syntax.empty() && unit) {
    toyc::analyze(*unit, semantic);
  }
  std::vector<toyc::Diagnostic> all = lex.diagnostics;
  all.insert(all.end(), syntax.begin(), syntax.end());
  all.insert(all.end(), semantic.begin(), semantic.end());
  print_ast_result(std::move(all));
  return 0;
}
