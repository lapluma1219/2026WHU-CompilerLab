#pragma once

#include "common/diagnostic.hpp"
#include "lexer/token.hpp"

#include <string>
#include <vector>

namespace toyc {

struct LexResult {
  std::vector<Token> tokens;
  std::vector<Diagnostic> diagnostics;
  int eof_line = 0;
  int eof_column = 0;
};

// 手写扫描器。后续语法分析可以重复调用 tokenize，不依赖 stdout。
class Lexer {
 public:
  explicit Lexer(std::string source);

  LexResult tokenize();

 private:
  std::string source_;
  std::size_t pos_ = 0;
  int line_ = 0;
  int column_ = 0;
  std::vector<Token> tokens_;
  std::vector<Diagnostic> diagnostics_;

  bool eof() const;
  char peek(std::size_t ahead = 0) const;
  char advance();

  void add_token(TokenKind kind, std::string lexeme, int line, int column);
  void add_diagnostic(int line, int column, std::string message);

  void skip_trivia();
  void scan_one();
  void scan_identifier();
  void scan_number();
  void report_illegal_and_skip();
};

}  // namespace toyc
