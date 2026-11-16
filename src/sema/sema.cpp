#include "sema/sema.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toyc {
namespace {

enum class SymbolKind { Var, Const, Func };

struct Symbol {
  SymbolKind kind = SymbolKind::Var;
  bool returns_int = true;
  int arity = 0;
  int line = 0;
};

struct Scope {
  std::unordered_map<std::string, Symbol> symbols;
};

struct Flow {
  bool returns = false;
  bool breaks = false;
};

class Analyzer {
 public:
  explicit Analyzer(std::vector<Diagnostic>& errors) : errors_(errors) {}

  void run(CompUnit& unit) {
    scopes_.push_back(Scope{});
    add_builtin("getint", true, 0);
    add_builtin("putint", false, 1);
    for (auto& func : unit.funcs) {
      declare_func(*func);
      analyze_func(*func);
    }
    if (!saw_main_) {
      errors_.push_back(Diagnostic{0, 0, "missing int main()"});
    }
  }

 private:
  std::vector<Diagnostic>& errors_;
  std::vector<Scope> scopes_;
  int loop_depth_ = 0;
  bool current_returns_int_ = true;
  bool saw_main_ = false;

  void error_at(int line, int column, const std::string& message) {
    errors_.push_back(Diagnostic{line, column, message});
  }

  void add_builtin(const std::string& name, bool returns_int, int arity) {
    Symbol symbol;
    symbol.kind = SymbolKind::Func;
    symbol.returns_int = returns_int;
    symbol.arity = arity;
    symbol.line = -1;
    scopes_[0].symbols.emplace(name, symbol);
  }

  Symbol* lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->symbols.find(name);
      if (found != it->symbols.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  bool declared_here(const std::string& name) const {
    return scopes_.back().symbols.find(name) != scopes_.back().symbols.end();
  }

  void declare_func(FuncDef& func) {
    if (func.name.empty()) {
      return;
    }
    if (declared_here(func.name)) {
      error_at(func.line, func.column, "redefinition of '" + func.name + "'");
      return;
    }
    Symbol symbol;
    symbol.kind = SymbolKind::Func;
    symbol.returns_int = func.returns_int;
    symbol.arity = static_cast<int>(func.params.size());
    symbol.line = func.line;
    scopes_.back().symbols.emplace(func.name, symbol);
    if (func.name == "main") {
      saw_main_ = true;
      if (!(func.returns_int && func.params.empty())) {
        error_at(func.line, func.column, "main must be 'int main()'");
      }
    }
  }

  void analyze_func(FuncDef& func) {
    current_returns_int_ = func.returns_int;
    scopes_.push_back(Scope{});
    for (const Param& param : func.params) {
      if (param.name.empty()) {
        continue;
      }
      if (declared_here(param.name)) {
        error_at(param.line, param.column, "redefinition of '" + param.name + "'");
        continue;
      }
      Symbol symbol;
      symbol.kind = SymbolKind::Var;
      symbol.line = param.line;
      scopes_.back().symbols.emplace(param.name, symbol);
    }
    Flow flow;
    // 形参和函数体最外层变量在同一作用域。函数体的大括号不再单独开一层。
    if (func.body) {
      flow = analyze_stmts(func.body->stmts);
    }
    if (func.returns_int && !flow.returns) {
      error_at(func.line, func.column, "missing return");
    }
    scopes_.pop_back();
  }

  Flow analyze_block(BlockStmt& block) {
    scopes_.push_back(Scope{});
    Flow flow = analyze_stmts(block.stmts);
    scopes_.pop_back();
    return flow;
  }

  Flow analyze_stmts(const std::vector<std::unique_ptr<Stmt>>& stmts) {
    Flow total;
    for (const auto& stmt : stmts) {
      if (total.returns) {
        break;
      }
      const Flow one = analyze_stmt(*stmt);
      total.returns = one.returns;
      total.breaks = total.breaks || one.breaks;
    }
    return total;
  }

  Flow analyze_stmt(Stmt& stmt) {
    switch (stmt.kind) {
      case StmtKind::Block:
        return analyze_block(static_cast<BlockStmt&>(stmt));
      case StmtKind::Empty:
        return {};
      case StmtKind::ExprStmt: {
        auto& node = static_cast<ExprStmtNode&>(stmt);
        if (node.expr) {
          analyze_expr(*node.expr, false);
        }
        return {};
      }
      case StmtKind::Assign: {
        auto& node = static_cast<AssignStmt&>(stmt);
        Symbol* symbol = lookup(node.name);
        if (symbol == nullptr) {
          error_at(node.line, node.column, "use of undeclared '" + node.name + "'");
        } else if (symbol->kind == SymbolKind::Func) {
          error_at(node.line, node.column, "function cannot be used as a value");
        } else if (symbol->kind == SymbolKind::Const) {
          error_at(node.line, node.column, "assignment to const '" + node.name + "'");
        }
        if (node.expr) {
          analyze_expr(*node.expr, true);
        }
        return {};
      }
      case StmtKind::VarDecl: {
        auto& node = static_cast<VarDeclStmt&>(stmt);
        if (node.init) {
          analyze_expr(*node.init, true);
        }
        if (node.name.empty()) {
          return {};
        }
        if (declared_here(node.name)) {
          error_at(node.line, node.column, "redefinition of '" + node.name + "'");
          return {};
        }
        Symbol symbol;
        symbol.kind = node.is_const ? SymbolKind::Const : SymbolKind::Var;
        symbol.line = node.line;
        scopes_.back().symbols.emplace(node.name, symbol);
        return {};
      }
      case StmtKind::If: {
        auto& node = static_cast<IfStmt&>(stmt);
        if (node.cond) {
          analyze_expr(*node.cond, true);
        }
        const Flow then_flow = node.then_stmt ? analyze_stmt(*node.then_stmt) : Flow{};
        if (!node.else_stmt) {
          return Flow{false, then_flow.breaks};
        }
        const Flow else_flow = analyze_stmt(*node.else_stmt);
        return Flow{then_flow.returns && else_flow.returns, then_flow.breaks || else_flow.breaks};
      }
      case StmtKind::While: {
        auto& node = static_cast<WhileStmt&>(stmt);
        if (node.cond) {
          analyze_expr(*node.cond, true);
        }
        ++loop_depth_;
        const Flow body = node.body ? analyze_stmt(*node.body) : Flow{};
        --loop_depth_;
        const bool always =
            node.cond && node.cond->has_const && node.cond->const_value != 0 && body.returns &&
            !body.breaks;
        return Flow{always, false};
      }
      case StmtKind::Break:
        if (loop_depth_ == 0) {
          error_at(stmt.line, stmt.column, "break outside loop");
        }
        return Flow{false, true};
      case StmtKind::Continue:
        if (loop_depth_ == 0) {
          error_at(stmt.line, stmt.column, "continue outside loop");
        }
        return {};
      case StmtKind::Return: {
        auto& node = static_cast<ReturnStmt&>(stmt);
        if (node.expr) {
          if (!current_returns_int_) {
            error_at(node.line, node.column, "void function cannot return a value");
            analyze_expr(*node.expr, false);
          } else {
            analyze_expr(*node.expr, true);
          }
        } else if (current_returns_int_) {
          error_at(node.line, node.column, "return value required");
        }
        return Flow{true, false};
      }
    }
    return {};
  }

  void analyze_expr(Expr& expr, bool need_value) {
    switch (expr.kind) {
      case ExprKind::Bad:
        expr.type = ValueKind::Invalid;
        return;
      case ExprKind::IntLit:
        analyze_int(static_cast<IntLitExpr&>(expr));
        break;
      case ExprKind::Var:
        analyze_var(static_cast<VarExpr&>(expr));
        break;
      case ExprKind::Unary:
        analyze_unary(static_cast<UnaryExprNode&>(expr));
        break;
      case ExprKind::Binary:
        analyze_binary(static_cast<BinaryExprNode&>(expr));
        break;
      case ExprKind::Call:
        analyze_call(static_cast<CallExprNode&>(expr), need_value);
        return;
    }
    if (need_value && expr.type == ValueKind::Void) {
      error_at(expr.line, expr.column, "void value cannot be used as a value");
    }
  }

  void analyze_int(IntLitExpr& expr) {
    expr.type = ValueKind::Int;
    int64_t value = 0;
    bool ok = true;
    for (char ch : expr.lexeme) {
      if (ch < '0' || ch > '9') {
        ok = false;
        break;
      }
      if (value > (INT64_C(9223372036854775807) - (ch - '0')) / 10) {
        ok = false;
        break;
      }
      value = value * 10 + (ch - '0');
    }
    if (ok && value <= INT64_C(2147483647)) {
      expr.has_const = true;
      expr.const_value = static_cast<int32_t>(value);
    }
  }

  void analyze_var(VarExpr& expr) {
    Symbol* symbol = lookup(expr.name);
    if (symbol == nullptr) {
      error_at(expr.line, expr.column, "use of undeclared '" + expr.name + "'");
      expr.type = ValueKind::Invalid;
      return;
    }
    if (symbol->kind == SymbolKind::Func) {
      error_at(expr.line, expr.column, "function cannot be used as a value");
      expr.type = ValueKind::Invalid;
      return;
    }
    expr.type = ValueKind::Int;
  }

  void analyze_unary(UnaryExprNode& expr) {
    if (!expr.rhs) {
      expr.type = ValueKind::Invalid;
      return;
    }
    analyze_expr(*expr.rhs, true);
    if (expr.rhs->type != ValueKind::Int) {
      expr.type = ValueKind::Invalid;
      return;
    }
    expr.type = ValueKind::Int;
    if (!expr.rhs->has_const) {
      if (expr.op == TokenKind::Minus && expr.rhs->kind == ExprKind::IntLit) {
        const auto& lit = static_cast<IntLitExpr&>(*expr.rhs);
        if (lit.lexeme == "2147483648") {
          expr.has_const = true;
          expr.const_value = static_cast<int32_t>(-2147483648);
        }
      }
      return;
    }
    const int32_t value = expr.rhs->const_value;
    if (expr.op == TokenKind::Plus) {
      expr.has_const = true;
      expr.const_value = value;
    } else if (expr.op == TokenKind::Not) {
      expr.has_const = true;
      expr.const_value = value == 0 ? 1 : 0;
    } else if (expr.op == TokenKind::Minus) {
      expr.has_const = true;
      expr.const_value = static_cast<int32_t>(0u - static_cast<uint32_t>(value));
    }
  }

  void analyze_binary(BinaryExprNode& expr) {
    if (!expr.lhs || !expr.rhs) {
      expr.type = ValueKind::Invalid;
      return;
    }
    const bool logical = expr.op == TokenKind::AndAnd || expr.op == TokenKind::OrOr;
    analyze_expr(*expr.lhs, true);
    bool skip_rhs = false;
    if (logical && expr.lhs->has_const) {
      if (expr.op == TokenKind::AndAnd && expr.lhs->const_value == 0) {
        skip_rhs = true;
      }
      if (expr.op == TokenKind::OrOr && expr.lhs->const_value != 0) {
        skip_rhs = true;
      }
    }
    if (!skip_rhs) {
      analyze_expr(*expr.rhs, true);
    }
    if (expr.lhs->type != ValueKind::Int ||
        (!skip_rhs && expr.rhs->type != ValueKind::Int && expr.rhs->kind != ExprKind::Bad)) {
      expr.type = ValueKind::Invalid;
      return;
    }
    if (!skip_rhs && (expr.op == TokenKind::Slash || expr.op == TokenKind::Percent) &&
        expr.rhs->has_const && expr.rhs->const_value == 0) {
      error_at(expr.line, expr.column, "division by zero");
    }
    expr.type = ValueKind::Int;
    if (skip_rhs && expr.lhs->has_const) {
      expr.has_const = true;
      expr.const_value = expr.op == TokenKind::AndAnd ? 0 : 1;
      return;
    }
    if (!expr.lhs->has_const || !expr.rhs->has_const) {
      return;
    }
    const int32_t left = expr.lhs->const_value;
    const int32_t right = expr.rhs->const_value;
    expr.has_const = true;
    switch (expr.op) {
      case TokenKind::Plus:
        expr.const_value = static_cast<int32_t>(static_cast<uint32_t>(left) +
                                                static_cast<uint32_t>(right));
        break;
      case TokenKind::Minus:
        expr.const_value = static_cast<int32_t>(static_cast<uint32_t>(left) -
                                                static_cast<uint32_t>(right));
        break;
      case TokenKind::Star:
        expr.const_value = static_cast<int32_t>(static_cast<uint32_t>(left) *
                                                static_cast<uint32_t>(right));
        break;
      case TokenKind::Slash:
        if (right == 0 || (left == static_cast<int32_t>(0x80000000u) && right == -1)) {
          expr.has_const = false;
        } else {
          expr.const_value = left / right;
        }
        break;
      case TokenKind::Percent:
        if (right == 0 || (left == static_cast<int32_t>(0x80000000u) && right == -1)) {
          expr.has_const = false;
        } else {
          expr.const_value = left % right;
        }
        break;
      case TokenKind::Lt:
        expr.const_value = left < right ? 1 : 0;
        break;
      case TokenKind::Gt:
        expr.const_value = left > right ? 1 : 0;
        break;
      case TokenKind::Le:
        expr.const_value = left <= right ? 1 : 0;
        break;
      case TokenKind::Ge:
        expr.const_value = left >= right ? 1 : 0;
        break;
      case TokenKind::Eq:
        expr.const_value = left == right ? 1 : 0;
        break;
      case TokenKind::NotEq:
        expr.const_value = left != right ? 1 : 0;
        break;
      case TokenKind::AndAnd:
        expr.const_value = (left != 0 && right != 0) ? 1 : 0;
        break;
      case TokenKind::OrOr:
        expr.const_value = (left != 0 || right != 0) ? 1 : 0;
        break;
      default:
        expr.has_const = false;
        break;
    }
  }

  void analyze_call(CallExprNode& expr, bool need_value) {
    Symbol* symbol = lookup(expr.name);
    if (symbol == nullptr || symbol->kind != SymbolKind::Func) {
      if (symbol == nullptr) {
        error_at(expr.line, expr.column, "call of undeclared '" + expr.name + "'");
      } else {
        error_at(expr.line, expr.column, "'" + expr.name + "' is not a function");
      }
      expr.type = ValueKind::Invalid;
      for (auto& arg : expr.args) {
        if (arg && arg->kind != ExprKind::Bad) {
          analyze_expr(*arg, true);
        }
      }
      return;
    }
    if (static_cast<int>(expr.args.size()) != symbol->arity) {
      error_at(expr.line, expr.column, "wrong number of arguments");
    }
    for (auto& arg : expr.args) {
      if (!arg || arg->kind == ExprKind::Bad) {
        continue;
      }
      analyze_expr(*arg, true);
      if (arg->type == ValueKind::Void) {
        error_at(arg->line, arg->column, "void value cannot be used as a value");
      }
    }
    expr.type = symbol->returns_int ? ValueKind::Int : ValueKind::Void;
    if (need_value && expr.type == ValueKind::Void) {
      error_at(expr.line, expr.column, "void value cannot be used as a value");
    }
  }
};

}  // namespace

void analyze(CompUnit& unit, std::vector<Diagnostic>& errors) {
  Analyzer analyzer(errors);
  analyzer.run(unit);
}

}  // namespace toyc
