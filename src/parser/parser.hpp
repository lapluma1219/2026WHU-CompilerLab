#pragma once

#include "ast/ast.hpp"
#include "common/diagnostic.hpp"
#include "lexer/token.hpp"

#include <memory>
#include <vector>

namespace toyc {

class Parser {
 public:
  Parser(const std::vector<Token>& tokens, std::vector<Diagnostic>& errors);

  std::unique_ptr<CompUnit> parse();

 private:
  const std::vector<Token>& tokens_;
  std::vector<Diagnostic>& errors_;
  std::size_t pos_ = 0;

  bool eof() const;
  const Token* peek(std::size_t ahead = 0) const;
  bool check(TokenKind kind) const;
  bool match(TokenKind kind);
  const Token& previous() const;
  int error_line() const;
  int error_column() const;
  void error_at(int line, int column, const std::string& message);
  void error_here(const std::string& message);
  bool expect(TokenKind kind, const std::string& message);

  std::unique_ptr<FuncDef> parse_func();
  void parse_params(std::vector<Param>& params);
  std::unique_ptr<BlockStmt> parse_block();
  bool starts_stmt() const;
  std::unique_ptr<Stmt> parse_stmt();
  std::unique_ptr<Stmt> parse_if();
  std::unique_ptr<Stmt> parse_while();
  std::unique_ptr<Stmt> parse_return();
  std::unique_ptr<Stmt> parse_var_decl(bool is_const);
  std::unique_ptr<Stmt> parse_assign_or_expr();

  std::unique_ptr<Expr> parse_expr();
  std::unique_ptr<Expr> parse_lor();
  std::unique_ptr<Expr> parse_land();
  std::unique_ptr<Expr> parse_rel();
  std::unique_ptr<Expr> parse_add();
  std::unique_ptr<Expr> parse_mul();
  std::unique_ptr<Expr> parse_unary();
  std::unique_ptr<Expr> parse_primary();
  bool at_expr_sync() const;
};

}  // namespace toyc
