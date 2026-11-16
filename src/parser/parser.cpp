#include "parser/parser.hpp"

namespace toyc {

Parser::Parser(const std::vector<Token>& tokens, std::vector<Diagnostic>& errors, int eof_line,
               int eof_column)
    : tokens_(tokens), errors_(errors), eof_line_(eof_line), eof_column_(eof_column) {}

bool Parser::eof() const { return pos_ >= tokens_.size(); }

const Token* Parser::peek(std::size_t ahead) const {
  if (pos_ + ahead >= tokens_.size()) {
    return nullptr;
  }
  return &tokens_[pos_ + ahead];
}

bool Parser::check(TokenKind kind) const {
  const Token* token = peek();
  return token != nullptr && token->kind == kind;
}

bool Parser::match(TokenKind kind) {
  if (!check(kind)) {
    return false;
  }
  ++pos_;
  return true;
}

const Token& Parser::previous() const { return tokens_[pos_ - 1]; }

int Parser::error_line() const {
  if (const Token* token = peek()) {
    return token->line;
  }
  return eof_line_;
}

int Parser::error_column() const {
  if (const Token* token = peek()) {
    return token->column;
  }
  return eof_column_;
}

void Parser::error_at(int line, int column, const std::string& message) {
  errors_.push_back(Diagnostic{line, column, message});
}

void Parser::error_here(const std::string& message) {
  error_at(error_line(), error_column(), message);
}

bool Parser::expect(TokenKind kind, const std::string& message) {
  if (match(kind)) {
    return true;
  }
  error_here(message);
  return false;
}

bool Parser::at_expr_sync() const {
  return eof() || check(TokenKind::Semi) || check(TokenKind::Comma) ||
         check(TokenKind::RParen) || check(TokenKind::RBrace);
}

bool Parser::starts_stmt() const {
  return check(TokenKind::LBrace) || check(TokenKind::Semi) || check(TokenKind::KwIf) ||
         check(TokenKind::KwWhile) || check(TokenKind::KwBreak) ||
         check(TokenKind::KwContinue) || check(TokenKind::KwReturn) ||
         check(TokenKind::Ident) || check(TokenKind::KwInt) || check(TokenKind::KwConst) ||
         check(TokenKind::IntConst) || check(TokenKind::LParen) || check(TokenKind::Plus) ||
         check(TokenKind::Minus) || check(TokenKind::Not);
}

std::unique_ptr<CompUnit> Parser::parse() {
  auto unit = std::make_unique<CompUnit>();
  while (!eof()) {
    if (check(TokenKind::KwInt) || check(TokenKind::KwVoid)) {
      unit->funcs.push_back(parse_func());
      continue;
    }
    error_here("unexpected token");
    ++pos_;
  }
  return unit;
}

std::unique_ptr<FuncDef> Parser::parse_func() {
  auto func = std::make_unique<FuncDef>();
  func->returns_int = check(TokenKind::KwInt);
  func->line = error_line();
  func->column = error_column();
  match(TokenKind::KwInt);
  match(TokenKind::KwVoid);
  if (check(TokenKind::Ident)) {
    func->name = peek()->lexeme;
    func->line = peek()->line;
    func->column = peek()->column;
    ++pos_;
  } else {
    error_here("expected function name");
  }
  expect(TokenKind::LParen, "Lack of '('");
  parse_params(func->params);
  if (!check(TokenKind::RParen)) {
    error_here("Lack of ')'");
  } else {
    ++pos_;
  }
  func->body = parse_block();
  return func;
}

void Parser::parse_params(std::vector<Param>& params) {
  if (check(TokenKind::RParen) || check(TokenKind::LBrace) || eof()) {
    return;
  }
  auto take_param = [&]() {
    if (!match(TokenKind::KwInt)) {
      error_here("expected 'int'");
      return;
    }
    Param param;
    if (check(TokenKind::Ident)) {
      param.name = peek()->lexeme;
      param.line = peek()->line;
      param.column = peek()->column;
      ++pos_;
    } else {
      error_here("expected parameter name");
    }
    params.push_back(std::move(param));
  };
  take_param();
  while (match(TokenKind::Comma)) {
    if (check(TokenKind::RParen) || check(TokenKind::LBrace) || eof()) {
      error_here("expected parameter");
      break;
    }
    take_param();
  }
}

std::unique_ptr<BlockStmt> Parser::parse_block() {
  auto block = std::make_unique<BlockStmt>();
  if (check(TokenKind::LBrace)) {
    block->line = peek()->line;
    block->column = peek()->column;
    ++pos_;
  } else {
    error_here("Lack of '{'");
    block->line = error_line();
    block->column = error_column();
    return block;
  }
  bool reported_close = false;
  while (!eof() && !check(TokenKind::RBrace)) {
    if (check(TokenKind::KwElse)) {
      error_here("unexpected 'else'");
      ++pos_;
      continue;
    }
    if (!starts_stmt()) {
      error_here("Lack of '}'");
      reported_close = true;
      break;
    }
    block->stmts.push_back(parse_stmt());
  }
  if (check(TokenKind::RBrace)) {
    ++pos_;
  } else if (!reported_close) {
    error_here("Lack of '}'");
  }
  return block;
}

std::unique_ptr<Stmt> Parser::parse_stmt() {
  if (eof() || check(TokenKind::RBrace) || check(TokenKind::KwElse)) {
    error_here("Lack of statement");
    auto stmt = std::make_unique<EmptyStmt>();
    stmt->line = error_line();
    stmt->column = error_column();
    return stmt;
  }
  if (check(TokenKind::LBrace)) {
    return parse_block();
  }
  if (match(TokenKind::Semi)) {
    auto stmt = std::make_unique<EmptyStmt>();
    stmt->line = previous().line;
    stmt->column = previous().column;
    return stmt;
  }
  if (check(TokenKind::KwIf)) {
    return parse_if();
  }
  if (check(TokenKind::KwWhile)) {
    return parse_while();
  }
  if (match(TokenKind::KwBreak)) {
    auto stmt = std::make_unique<BreakStmt>();
    stmt->line = previous().line;
    stmt->column = previous().column;
    expect(TokenKind::Semi, "Lack of ';'");
    return stmt;
  }
  if (match(TokenKind::KwContinue)) {
    auto stmt = std::make_unique<ContinueStmt>();
    stmt->line = previous().line;
    stmt->column = previous().column;
    expect(TokenKind::Semi, "Lack of ';'");
    return stmt;
  }
  if (check(TokenKind::KwReturn)) {
    return parse_return();
  }
  if (check(TokenKind::KwConst) || check(TokenKind::KwInt)) {
    const bool is_const = check(TokenKind::KwConst);
    return parse_var_decl(is_const);
  }
  return parse_assign_or_expr();
}

std::unique_ptr<Stmt> Parser::parse_if() {
  auto stmt = std::make_unique<IfStmt>();
  stmt->line = error_line();
  stmt->column = error_column();
  ++pos_;
  bool missing_condition = false;
  stmt->cond = parse_paren_condition(missing_condition);
  stmt->then_stmt = parse_stmt();
  if (match(TokenKind::KwElse)) {
    stmt->else_stmt = parse_stmt();
  }
  return stmt;
}

std::unique_ptr<Stmt> Parser::parse_while() {
  auto stmt = std::make_unique<WhileStmt>();
  stmt->line = error_line();
  stmt->column = error_column();
  ++pos_;
  bool missing_condition = false;
  stmt->cond = parse_paren_condition(missing_condition);
  // 条件已经缺失时，紧接着的 } 或文件结束不是另一条“缺少语句”。
  if (missing_condition && (eof() || check(TokenKind::RBrace))) {
    auto empty = std::make_unique<EmptyStmt>();
    empty->line = stmt->line;
    empty->column = stmt->column;
    stmt->body = std::move(empty);
  } else {
    stmt->body = parse_stmt();
  }
  return stmt;
}

bool Parser::can_start_expr() const {
  return check(TokenKind::Ident) || check(TokenKind::IntConst) || check(TokenKind::LParen) ||
         check(TokenKind::Plus) || check(TokenKind::Minus) || check(TokenKind::Not);
}

bool Parser::is_stmt_boundary() const {
  return eof() || check(TokenKind::LBrace) || check(TokenKind::Semi) || check(TokenKind::RBrace) ||
         check(TokenKind::KwIf) || check(TokenKind::KwWhile) || check(TokenKind::KwReturn) ||
         check(TokenKind::KwBreak) || check(TokenKind::KwContinue) || check(TokenKind::KwElse);
}

void Parser::sync_open_paren_to_rparen() {
  if (check(TokenKind::RParen)) {
    ++pos_;
    return;
  }
  // '{' 和语句关键字留给循环体。分号在尚未闭合的 '(' 里面是条件里的杂符，要跳过。
  if (check(TokenKind::LBrace) || check(TokenKind::RBrace) || check(TokenKind::KwIf) ||
      check(TokenKind::KwWhile) || check(TokenKind::KwReturn) || check(TokenKind::KwBreak) ||
      check(TokenKind::KwContinue) || check(TokenKind::KwElse) || eof()) {
    return;
  }
  while (!eof() && !check(TokenKind::RParen) && !check(TokenKind::LBrace) &&
         !check(TokenKind::RBrace) && !check(TokenKind::KwIf) && !check(TokenKind::KwWhile) &&
         !check(TokenKind::KwReturn) && !check(TokenKind::KwBreak) &&
         !check(TokenKind::KwContinue) && !check(TokenKind::KwElse)) {
    ++pos_;
  }
  if (check(TokenKind::RParen)) {
    ++pos_;
  }
}

std::unique_ptr<Expr> Parser::parse_paren_condition(bool& missing_condition) {
  missing_condition = false;
  auto bad_at = [&]() {
    auto expr = std::make_unique<BadExpr>();
    expr->line = error_line();
    expr->column = error_column();
    return expr;
  };
  if (!check(TokenKind::LParen)) {
    missing_condition = true;
    error_here("Lack of '('");
    if (!can_start_expr()) {
      return bad_at();
    }
    // 只有表达式后面紧跟着 ')'，才是示例二那种“缺左括号但条件和右括号都在”。
    // 否则不把后面的语句拆开当条件，例如 while a = 1;
    const std::size_t saved_pos = pos_;
    const std::size_t saved_errors = errors_.size();
    auto expr = parse_expr();
    if (check(TokenKind::RParen)) {
      ++pos_;
      missing_condition = false;
      return expr;
    }
    if (is_stmt_boundary() && errors_.size() == saved_errors) {
      missing_condition = false;
      return expr;
    }
    pos_ = saved_pos;
    errors_.resize(saved_errors);
    return bad_at();
  }
  ++pos_;
  if (!can_start_expr()) {
    missing_condition = true;
    error_here("Lack of expression");
    sync_open_paren_to_rparen();
    return bad_at();
  }
  auto expr = parse_expr();
  if (check(TokenKind::RParen)) {
    ++pos_;
  } else {
    error_here("Lack of ')'");
  }
  return expr;
}

std::unique_ptr<Stmt> Parser::parse_return() {
  auto stmt = std::make_unique<ReturnStmt>();
  stmt->line = error_line();
  stmt->column = error_column();
  ++pos_;
  if (!check(TokenKind::Semi)) {
    stmt->expr = parse_expr();
  }
  expect(TokenKind::Semi, "Lack of ';'");
  return stmt;
}

std::unique_ptr<Stmt> Parser::parse_var_decl(bool is_const) {
  auto stmt = std::make_unique<VarDeclStmt>();
  stmt->is_const = is_const;
  stmt->line = error_line();
  stmt->column = error_column();
  if (is_const) {
    ++pos_;
    expect(TokenKind::KwInt, "expected 'int'");
  } else {
    ++pos_;
  }
  if (check(TokenKind::Ident)) {
    stmt->name = peek()->lexeme;
    stmt->line = peek()->line;
    stmt->column = peek()->column;
    ++pos_;
  } else {
    error_here("expected name");
  }
  if (match(TokenKind::Assign)) {
    stmt->has_init = true;
    stmt->init = parse_expr();
  } else if (is_const) {
    error_here("const declaration requires an initializer");
    stmt->has_init = false;
  }
  expect(TokenKind::Semi, "Lack of ';'");
  return stmt;
}

std::unique_ptr<Stmt> Parser::parse_assign_or_expr() {
  if (check(TokenKind::Ident) && peek(1) != nullptr && peek(1)->kind == TokenKind::Assign) {
    auto stmt = std::make_unique<AssignStmt>();
    stmt->name = peek()->lexeme;
    stmt->line = peek()->line;
    stmt->column = peek()->column;
    ++pos_;
    ++pos_;
    stmt->expr = parse_expr();
    expect(TokenKind::Semi, "Lack of ';'");
    return stmt;
  }
  auto stmt = std::make_unique<ExprStmtNode>();
  stmt->line = error_line();
  stmt->column = error_column();
  stmt->expr = parse_expr();
  expect(TokenKind::Semi, "Lack of ';'");
  return stmt;
}

std::unique_ptr<Expr> Parser::parse_expr() { return parse_lor(); }

std::unique_ptr<Expr> Parser::parse_lor() {
  auto expr = parse_land();
  while (check(TokenKind::OrOr)) {
    const Token op = *peek();
    ++pos_;
    auto node = std::make_unique<BinaryExprNode>();
    node->op = op.kind;
    node->line = op.line;
    node->column = op.column;
    node->lhs = std::move(expr);
    node->rhs = parse_land();
    expr = std::move(node);
  }
  return expr;
}

std::unique_ptr<Expr> Parser::parse_land() {
  auto expr = parse_rel();
  while (check(TokenKind::AndAnd)) {
    const Token op = *peek();
    ++pos_;
    auto node = std::make_unique<BinaryExprNode>();
    node->op = op.kind;
    node->line = op.line;
    node->column = op.column;
    node->lhs = std::move(expr);
    node->rhs = parse_rel();
    expr = std::move(node);
  }
  return expr;
}

std::unique_ptr<Expr> Parser::parse_rel() {
  auto expr = parse_add();
  while (check(TokenKind::Lt) || check(TokenKind::Gt) || check(TokenKind::Le) ||
         check(TokenKind::Ge) || check(TokenKind::Eq) || check(TokenKind::NotEq)) {
    const Token op = *peek();
    ++pos_;
    auto node = std::make_unique<BinaryExprNode>();
    node->op = op.kind;
    node->line = op.line;
    node->column = op.column;
    node->lhs = std::move(expr);
    node->rhs = parse_add();
    expr = std::move(node);
  }
  return expr;
}

std::unique_ptr<Expr> Parser::parse_add() {
  auto expr = parse_mul();
  while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
    const Token op = *peek();
    ++pos_;
    auto node = std::make_unique<BinaryExprNode>();
    node->op = op.kind;
    node->line = op.line;
    node->column = op.column;
    node->lhs = std::move(expr);
    node->rhs = parse_mul();
    expr = std::move(node);
  }
  return expr;
}

std::unique_ptr<Expr> Parser::parse_mul() {
  auto expr = parse_unary();
  while (check(TokenKind::Star) || check(TokenKind::Slash) || check(TokenKind::Percent)) {
    const Token op = *peek();
    ++pos_;
    auto node = std::make_unique<BinaryExprNode>();
    node->op = op.kind;
    node->line = op.line;
    node->column = op.column;
    node->lhs = std::move(expr);
    node->rhs = parse_unary();
    expr = std::move(node);
  }
  return expr;
}

std::unique_ptr<Expr> Parser::parse_unary() {
  if (check(TokenKind::Plus) || check(TokenKind::Minus) || check(TokenKind::Not)) {
    const Token op = *peek();
    ++pos_;
    auto node = std::make_unique<UnaryExprNode>();
    node->op = op.kind;
    node->line = op.line;
    node->column = op.column;
    node->rhs = parse_unary();
    return node;
  }
  return parse_primary();
}

std::unique_ptr<Expr> Parser::parse_primary() {
  if (check(TokenKind::IntConst)) {
    auto node = std::make_unique<IntLitExpr>();
    node->lexeme = peek()->lexeme;
    node->line = peek()->line;
    node->column = peek()->column;
    ++pos_;
    return node;
  }
  if (check(TokenKind::Ident)) {
    const Token name = *peek();
    ++pos_;
    if (match(TokenKind::LParen)) {
      auto call = std::make_unique<CallExprNode>();
      call->name = name.lexeme;
      call->line = name.line;
      call->column = name.column;
      if (!check(TokenKind::RParen)) {
        do {
          if (at_expr_sync() && !check(TokenKind::Comma)) {
            error_here("Lack of expression");
            call->args.push_back(std::make_unique<BadExpr>());
          } else if (check(TokenKind::Comma)) {
            error_here("Lack of expression");
            call->args.push_back(std::make_unique<BadExpr>());
          } else {
            call->args.push_back(parse_expr());
          }
        } while (match(TokenKind::Comma));
      }
      if (check(TokenKind::RParen)) {
        ++pos_;
      } else {
        error_here("Lack of ')'");
      }
      return call;
    }
    auto var = std::make_unique<VarExpr>();
    var->name = name.lexeme;
    var->line = name.line;
    var->column = name.column;
    return var;
  }
  if (match(TokenKind::LParen)) {
    auto expr = parse_expr();
    if (check(TokenKind::RParen)) {
      ++pos_;
    } else {
      error_here("Lack of ')'");
    }
    return expr;
  }
  error_here("Lack of expression");
  auto bad = std::make_unique<BadExpr>();
  bad->line = error_line();
  bad->column = error_column();
  return bad;
}

}  // namespace toyc
