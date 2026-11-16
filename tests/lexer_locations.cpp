#include "lexer/lexer.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL " << message << '\n';
    ++failures;
  }
}

}  // namespace

int main() {
  {
    toyc::Lexer lexer("int\nmain");
    const toyc::LexResult result = lexer.tokenize();
    expect(result.diagnostics.empty(), "plain program has no diagnostic");
    expect(result.tokens.size() == 2, "two tokens");
    expect(result.tokens[0].kind == toyc::TokenKind::KwInt && result.tokens[0].line == 0 &&
               result.tokens[0].column == 0,
           "int at 0:0");
    expect(result.tokens[1].kind == toyc::TokenKind::Ident && result.tokens[1].lexeme == "main" &&
               result.tokens[1].line == 1 && result.tokens[1].column == 0,
           "main at 1:0");
  }
  {
    toyc::Lexer lexer("  x");
    const toyc::LexResult result = lexer.tokenize();
    expect(result.tokens.size() == 1 && result.tokens[0].column == 2, "column skips spaces");
  }
  {
    toyc::Lexer lexer("/*\n*/\n$");
    const toyc::LexResult result = lexer.tokenize();
    expect(result.tokens.empty(), "only illegal after comment");
    expect(result.diagnostics.size() == 1 && result.diagnostics[0].line == 2 &&
               result.diagnostics[0].column == 0,
           "dollar line follows block comment");
  }
  {
    toyc::Lexer lexer("a/*\n$*/b");
    const toyc::LexResult result = lexer.tokenize();
    expect(result.diagnostics.empty(), "dollar inside block comment is silent");
    expect(result.tokens.size() == 2, "a and b survive the comment");
    expect(result.tokens[1].lexeme == "b" && result.tokens[1].line == 1 &&
               result.tokens[1].column == 3,
           "b column after comment closer");
  }
  {
    toyc::Lexer lexer("\n/*");
    const toyc::LexResult result = lexer.tokenize();
    expect(result.diagnostics.size() == 1 && result.diagnostics[0].line == 1 &&
               result.diagnostics[0].column == 0 &&
               result.diagnostics[0].message == "unterminated comment",
           "unterminated comment reports its start");
    expect(result.tokens.empty(), "unclosed comment has no tokens");
  }
  {
    toyc::Lexer lexer("/* $ still");
    const toyc::LexResult result = lexer.tokenize();
    expect(result.diagnostics.size() == 1, "unclosed comment does not also report $");
    expect(result.diagnostics[0].message == "unterminated comment", "only comment diagnostic");
  }
  {
    toyc::Lexer lexer("a\r\nb\rc");
    const toyc::LexResult result = lexer.tokenize();
    expect(result.tokens.size() == 3 && result.tokens[1].line == 1 && result.tokens[2].line == 2,
           "CRLF and CR each count as one line break");
  }
  {
    toyc::Lexer lexer("-1");
    const toyc::LexResult result = lexer.tokenize();
    expect(result.tokens.size() == 2 && result.tokens[0].kind == toyc::TokenKind::Minus &&
               result.tokens[1].kind == toyc::TokenKind::IntConst &&
               result.tokens[1].lexeme == "1",
           "adjacent -1 stays two tokens");
  }
  {
    toyc::Lexer lexer("012");
    const toyc::LexResult result = lexer.tokenize();
    expect(result.tokens.size() == 2 && result.tokens[0].lexeme == "0" &&
               result.tokens[1].lexeme == "12",
           "leading zero is its own IntConst");
  }

  if (failures != 0) {
    std::cerr << failures << " location checks failed\n";
    return 1;
  }
  std::cout << "location checks passed\n";
  return 0;
}
