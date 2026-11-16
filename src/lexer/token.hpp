#pragma once

#include <string>

namespace toyc {

// 内部类别。stdout 的类型名由 token_type_name 决定，不直接打印枚举名。
enum class TokenKind {
  Ident,
  IntConst,
  KwInt,
  KwVoid,
  KwConst,
  KwIf,
  KwElse,
  KwWhile,
  KwBreak,
  KwContinue,
  KwReturn,
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Assign,
  Eq,
  NotEq,
  Lt,
  Gt,
  Le,
  Ge,
  AndAnd,
  OrOr,
  Not,
  LParen,
  RParen,
  LBrace,
  RBrace,
  Semi,
  Comma,
};

struct Token {
  TokenKind kind = TokenKind::Ident;
  std::string lexeme;
  int line = 0;    // 0-based
  int column = 0;  // 0-based
};

// 标识符全文匹配关键字。int0、integer、Int 都不是关键字。
TokenKind classify_identifier(const std::string& lexeme);

// 关键字、运算符、标点：'字面量'；标识符：Ident；整数：IntConst。
std::string token_type_name(const Token& token);

}  // namespace toyc
