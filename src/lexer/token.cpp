#include "lexer/token.hpp"

#include <unordered_map>

namespace toyc {

TokenKind classify_identifier(const std::string& lexeme) {
  static const std::unordered_map<std::string, TokenKind> kKeywords = {
      {"int", TokenKind::KwInt},         {"void", TokenKind::KwVoid},
      {"const", TokenKind::KwConst},     {"if", TokenKind::KwIf},
      {"else", TokenKind::KwElse},       {"while", TokenKind::KwWhile},
      {"break", TokenKind::KwBreak},     {"continue", TokenKind::KwContinue},
      {"return", TokenKind::KwReturn},
  };
  const auto it = kKeywords.find(lexeme);
  if (it == kKeywords.end()) {
    return TokenKind::Ident;
  }
  return it->second;
}

std::string token_type_name(const Token& token) {
  if (token.kind == TokenKind::Ident) {
    return "Ident";
  }
  if (token.kind == TokenKind::IntConst) {
    return "IntConst";
  }
  return "'" + token.lexeme + "'";
}

}  // namespace toyc
