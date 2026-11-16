#pragma once

#include "lexer/token.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace toyc {

enum class ValueKind { Invalid, Int, Void };

enum class ExprKind { Bad, IntLit, Var, Unary, Binary, Call };

struct Expr {
  ExprKind kind = ExprKind::Bad;
  int line = 0;
  int column = 0;
  ValueKind type = ValueKind::Invalid;
  bool has_const = false;
  int32_t const_value = 0;
  virtual ~Expr() = default;
};

struct BadExpr : Expr {
  BadExpr() { kind = ExprKind::Bad; }
};

struct IntLitExpr : Expr {
  std::string lexeme;
  IntLitExpr() { kind = ExprKind::IntLit; }
};

struct VarExpr : Expr {
  std::string name;
  VarExpr() { kind = ExprKind::Var; }
};

struct UnaryExprNode : Expr {
  TokenKind op = TokenKind::Plus;
  std::unique_ptr<Expr> rhs;
  UnaryExprNode() { kind = ExprKind::Unary; }
};

struct BinaryExprNode : Expr {
  TokenKind op = TokenKind::Plus;
  std::unique_ptr<Expr> lhs;
  std::unique_ptr<Expr> rhs;
  BinaryExprNode() { kind = ExprKind::Binary; }
};

struct CallExprNode : Expr {
  std::string name;
  std::vector<std::unique_ptr<Expr>> args;
  CallExprNode() { kind = ExprKind::Call; }
};

enum class StmtKind {
  Block,
  Empty,
  ExprStmt,
  Assign,
  VarDecl,
  If,
  While,
  Break,
  Continue,
  Return
};

struct Stmt {
  StmtKind kind;
  int line = 0;
  int column = 0;
  explicit Stmt(StmtKind k) : kind(k) {}
  virtual ~Stmt() = default;
};

struct BlockStmt : Stmt {
  std::vector<std::unique_ptr<Stmt>> stmts;
  BlockStmt() : Stmt(StmtKind::Block) {}
};

struct EmptyStmt : Stmt {
  EmptyStmt() : Stmt(StmtKind::Empty) {}
};

struct ExprStmtNode : Stmt {
  std::unique_ptr<Expr> expr;
  ExprStmtNode() : Stmt(StmtKind::ExprStmt) {}
};

struct AssignStmt : Stmt {
  std::string name;
  std::unique_ptr<Expr> expr;
  AssignStmt() : Stmt(StmtKind::Assign) {}
};

struct VarDeclStmt : Stmt {
  std::string name;
  bool is_const = false;
  bool has_init = false;
  std::unique_ptr<Expr> init;
  VarDeclStmt() : Stmt(StmtKind::VarDecl) {}
};

struct IfStmt : Stmt {
  std::unique_ptr<Expr> cond;
  std::unique_ptr<Stmt> then_stmt;
  std::unique_ptr<Stmt> else_stmt;
  IfStmt() : Stmt(StmtKind::If) {}
};

struct WhileStmt : Stmt {
  std::unique_ptr<Expr> cond;
  std::unique_ptr<Stmt> body;
  WhileStmt() : Stmt(StmtKind::While) {}
};

struct BreakStmt : Stmt {
  BreakStmt() : Stmt(StmtKind::Break) {}
};

struct ContinueStmt : Stmt {
  ContinueStmt() : Stmt(StmtKind::Continue) {}
};

struct ReturnStmt : Stmt {
  std::unique_ptr<Expr> expr;
  ReturnStmt() : Stmt(StmtKind::Return) {}
};

struct Param {
  std::string name;
  int line = 0;
  int column = 0;
};

struct FuncDef {
  bool returns_int = true;
  std::string name;
  int line = 0;
  int column = 0;
  std::vector<Param> params;
  std::unique_ptr<BlockStmt> body;
};

struct CompUnit {
  std::vector<std::unique_ptr<FuncDef>> funcs;
};

}  // namespace toyc
