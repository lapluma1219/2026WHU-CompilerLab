#include "codegen/codegen.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toyc {
namespace {

struct VarInfo {
  int slot = 0;
  bool folded = false;
  int32_t value = 0;
};

int scratch_need(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::Bad:
    case ExprKind::IntLit:
    case ExprKind::Var:
      return 0;
    case ExprKind::Unary: {
      const auto& node = static_cast<const UnaryExprNode&>(expr);
      return node.rhs ? scratch_need(*node.rhs) : 0;
    }
    case ExprKind::Binary: {
      const auto& node = static_cast<const BinaryExprNode&>(expr);
      const int left = node.lhs ? scratch_need(*node.lhs) : 0;
      const int right = node.rhs ? scratch_need(*node.rhs) : 0;
      return std::max(left, 1 + right);
    }
    case ExprKind::Call: {
      const auto& node = static_cast<const CallExprNode&>(expr);
      int need = 0;
      for (const auto& arg : node.args) {
        if (arg) {
          need = std::max(need, scratch_need(*arg));
        }
      }
      return need;
    }
  }
  return 0;
}

bool has_side_effect(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::Call:
      return true;
    case ExprKind::Unary: {
      const auto& node = static_cast<const UnaryExprNode&>(expr);
      return node.rhs && has_side_effect(*node.rhs);
    }
    case ExprKind::Binary: {
      const auto& node = static_cast<const BinaryExprNode&>(expr);
      return (node.lhs && has_side_effect(*node.lhs)) || (node.rhs && has_side_effect(*node.rhs));
    }
    default:
      return false;
  }
}

void walk_expr_stats(const Expr& expr, int& max_args, int& max_scratch) {
  max_scratch = std::max(max_scratch, scratch_need(expr));
  if (expr.kind == ExprKind::Call) {
    const auto& call = static_cast<const CallExprNode&>(expr);
    max_args = std::max(max_args, static_cast<int>(call.args.size()));
    for (const auto& arg : call.args) {
      if (arg) {
        walk_expr_stats(*arg, max_args, max_scratch);
      }
    }
  } else if (expr.kind == ExprKind::Unary) {
    const auto& node = static_cast<const UnaryExprNode&>(expr);
    if (node.rhs) {
      walk_expr_stats(*node.rhs, max_args, max_scratch);
    }
  } else if (expr.kind == ExprKind::Binary) {
    const auto& node = static_cast<const BinaryExprNode&>(expr);
    if (node.lhs) {
      walk_expr_stats(*node.lhs, max_args, max_scratch);
    }
    if (node.rhs) {
      walk_expr_stats(*node.rhs, max_args, max_scratch);
    }
  }
}

void walk_stmt_stats(const Stmt& stmt, int& slots, int& max_args, int& max_scratch) {
  switch (stmt.kind) {
    case StmtKind::Block:
      for (const auto& child : static_cast<const BlockStmt&>(stmt).stmts) {
        walk_stmt_stats(*child, slots, max_args, max_scratch);
      }
      break;
    case StmtKind::ExprStmt:
      if (static_cast<const ExprStmtNode&>(stmt).expr) {
        walk_expr_stats(*static_cast<const ExprStmtNode&>(stmt).expr, max_args, max_scratch);
      }
      break;
    case StmtKind::Assign:
      if (static_cast<const AssignStmt&>(stmt).expr) {
        walk_expr_stats(*static_cast<const AssignStmt&>(stmt).expr, max_args, max_scratch);
      }
      break;
    case StmtKind::VarDecl: {
      ++slots;
      const auto& decl = static_cast<const VarDeclStmt&>(stmt);
      if (decl.init) {
        walk_expr_stats(*decl.init, max_args, max_scratch);
      }
      break;
    }
    case StmtKind::If: {
      const auto& node = static_cast<const IfStmt&>(stmt);
      if (node.cond) {
        walk_expr_stats(*node.cond, max_args, max_scratch);
      }
      if (node.then_stmt) {
        walk_stmt_stats(*node.then_stmt, slots, max_args, max_scratch);
      }
      if (node.else_stmt) {
        walk_stmt_stats(*node.else_stmt, slots, max_args, max_scratch);
      }
      break;
    }
    case StmtKind::While: {
      const auto& node = static_cast<const WhileStmt&>(stmt);
      if (node.cond) {
        walk_expr_stats(*node.cond, max_args, max_scratch);
      }
      if (node.body) {
        walk_stmt_stats(*node.body, slots, max_args, max_scratch);
      }
      break;
    }
    case StmtKind::Return:
      if (static_cast<const ReturnStmt&>(stmt).expr) {
        walk_expr_stats(*static_cast<const ReturnStmt&>(stmt).expr, max_args, max_scratch);
      }
      break;
    default:
      break;
  }
}

class AsmEmitter {
 public:
  explicit AsmEmitter(bool optimize) : optimize_(optimize) {}

  std::string emit(const CompUnit& unit) {
    for (const auto& func : unit.funcs) {
      emit_func(*func);
    }
    return out_.str();
  }

 private:
  bool optimize_;
  std::ostringstream out_;
  int label_id_ = 0;
  int scratch_base_ = 0;
  int scratch_depth_ = 0;
  int local_base_ = 0;
  int frame_size_ = 0;
  std::vector<std::unordered_map<std::string, VarInfo>> scopes_;
  std::vector<std::pair<std::string, std::string>> loops_;

  std::string fresh(const std::string& prefix) {
    return prefix + std::to_string(label_id_++);
  }

  void line(const std::string& text) { out_ << text << '\n'; }

  void inst(const std::string& text) { out_ << '\t' << text << '\n'; }

  void push_scope() { scopes_.emplace_back(); }

  void pop_scope() { scopes_.pop_back(); }

  VarInfo* lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  void bind(const std::string& name, VarInfo info) { scopes_.back().emplace(name, info); }

  void push_t0() {
    inst("sw t0, " + std::to_string(scratch_base_ + scratch_depth_ * 8) + "(s0)");
    ++scratch_depth_;
  }

  void pop_t0() {
    --scratch_depth_;
    inst("lw t0, " + std::to_string(scratch_base_ + scratch_depth_ * 8) + "(s0)");
  }

  void emit_func(const FuncDef& func) {
    int slots = static_cast<int>(func.params.size());
    int max_args = 0;
    int max_scratch = 0;
    if (func.body) {
      for (const auto& stmt : func.body->stmts) {
        walk_stmt_stats(*stmt, slots, max_args, max_scratch);
      }
    }
    if (max_scratch < 1) {
      max_scratch = 1;
    }
    const int out_bytes = std::max(0, max_args - 8) * 8;
    const int arg_bytes = max_args * 8;
    const int scratch_bytes = max_scratch * 8;
    const int local_bytes = slots * 8;
    const int fixed = out_bytes + arg_bytes + scratch_bytes + local_bytes;
    const int ra_off = fixed;
    const int s0_off = fixed + 8;
    frame_size_ = (fixed + 16 + 15) & ~15;
    scratch_base_ = out_bytes + arg_bytes;
    local_base_ = scratch_base_ + scratch_bytes;
    scratch_depth_ = 0;

    line(".text");
    line(".globl " + func.name);
    line(func.name + ":");
    inst("addi sp, sp, -" + std::to_string(frame_size_));
    inst("sd ra, " + std::to_string(ra_off) + "(sp)");
    inst("sd s0, " + std::to_string(s0_off) + "(sp)");
    inst("addi s0, sp, 0");

    push_scope();
    for (int i = 0; i < static_cast<int>(func.params.size()); ++i) {
      const int slot = local_base_ + i * 8;
      if (i < 8) {
        inst("sw a" + std::to_string(i) + ", " + std::to_string(slot) + "(s0)");
      } else {
        const int incoming = frame_size_ + (i - 8) * 8;
        inst("lw t0, " + std::to_string(incoming) + "(s0)");
        inst("sw t0, " + std::to_string(slot) + "(s0)");
      }
      bind(func.params[static_cast<std::size_t>(i)].name, VarInfo{slot, false, 0});
    }
    int local_index = static_cast<int>(func.params.size());
    const std::string epilogue = func.name + "_epilogue";
    if (func.body) {
      emit_stmts(func.body->stmts, epilogue, local_index, false);
    }
    line(epilogue + ":");
    inst("ld ra, " + std::to_string(ra_off) + "(sp)");
    inst("ld s0, " + std::to_string(s0_off) + "(sp)");
    inst("addi sp, sp, " + std::to_string(frame_size_));
    inst("ret");
    line("");
    pop_scope();
  }

  void emit_stmts(const std::vector<std::unique_ptr<Stmt>>& stmts, const std::string& epilogue,
                  int& local_index, bool in_dead) {
    bool dead = in_dead;
    for (const auto& stmt : stmts) {
      if (dead && optimize_) {
        continue;
      }
      emit_stmt(*stmt, epilogue, local_index);
      if (stmt->kind == StmtKind::Return) {
        dead = true;
      }
    }
  }

  void emit_stmt(const Stmt& stmt, const std::string& epilogue, int& local_index) {
    switch (stmt.kind) {
      case StmtKind::Block: {
        push_scope();
        emit_stmts(static_cast<const BlockStmt&>(stmt).stmts, epilogue, local_index, false);
        pop_scope();
        break;
      }
      case StmtKind::Empty:
        break;
      case StmtKind::ExprStmt:
        if (static_cast<const ExprStmtNode&>(stmt).expr) {
          eval(*static_cast<const ExprStmtNode&>(stmt).expr);
        }
        break;
      case StmtKind::Assign: {
        const auto& node = static_cast<const AssignStmt&>(stmt);
        if (node.expr) {
          eval(*node.expr);
        }
        if (VarInfo* info = lookup(node.name)) {
          inst("sw t0, " + std::to_string(info->slot) + "(s0)");
          info->folded = false;
        }
        break;
      }
      case StmtKind::VarDecl: {
        const auto& node = static_cast<const VarDeclStmt&>(stmt);
        const int slot = local_base_ + local_index * 8;
        ++local_index;
        VarInfo info{slot, false, 0};
        if (node.init) {
          eval(*node.init);
          inst("sw t0, " + std::to_string(slot) + "(s0)");
          if (optimize_ && node.is_const && node.init->has_const) {
            info.folded = true;
            info.value = node.init->const_value;
          }
        }
        bind(node.name, info);
        break;
      }
      case StmtKind::If:
        emit_if(static_cast<const IfStmt&>(stmt), epilogue, local_index);
        break;
      case StmtKind::While:
        emit_while(static_cast<const WhileStmt&>(stmt), epilogue, local_index);
        break;
      case StmtKind::Break:
        if (!loops_.empty()) {
          inst("j " + loops_.back().second);
        }
        break;
      case StmtKind::Continue:
        if (!loops_.empty()) {
          inst("j " + loops_.back().first);
        }
        break;
      case StmtKind::Return: {
        const auto& node = static_cast<const ReturnStmt&>(stmt);
        if (node.expr) {
          eval(*node.expr);
          inst("mv a0, t0");
        }
        inst("j " + epilogue);
        break;
      }
    }
  }

  void emit_if(const IfStmt& stmt, const std::string& epilogue, int& local_index) {
    const std::string else_label = fresh(".Lelse_");
    const std::string end_label = fresh(".Lend_");
    if (stmt.cond) {
      eval(*stmt.cond);
    } else {
      inst("li t0, 0");
    }
    inst("beq t0, zero, " + (stmt.else_stmt ? else_label : end_label));
    if (stmt.then_stmt) {
      emit_stmt(*stmt.then_stmt, epilogue, local_index);
    }
    if (stmt.else_stmt) {
      inst("j " + end_label);
      line(else_label + ":");
      emit_stmt(*stmt.else_stmt, epilogue, local_index);
    }
    line(end_label + ":");
  }

  void emit_while(const WhileStmt& stmt, const std::string& epilogue, int& local_index) {
    const std::string head = fresh(".Lwhile_");
    const std::string end = fresh(".Lwend_");
    loops_.push_back({head, end});
    line(head + ":");
    if (stmt.cond) {
      eval(*stmt.cond);
    } else {
      inst("li t0, 0");
    }
    inst("beq t0, zero, " + end);
    if (stmt.body) {
      emit_stmt(*stmt.body, epilogue, local_index);
    }
    inst("j " + head);
    line(end + ":");
    loops_.pop_back();
  }

  void eval(const Expr& expr) {
    if (optimize_ && expr.has_const && !has_side_effect(expr)) {
      inst("li t0, " + std::to_string(expr.const_value));
      return;
    }
    switch (expr.kind) {
      case ExprKind::IntLit: {
        const auto& node = static_cast<const IntLitExpr&>(expr);
        if (node.has_const) {
          inst("li t0, " + std::to_string(node.const_value));
        } else {
          inst("li t0, " + node.lexeme);
        }
        break;
      }
      case ExprKind::Var: {
        const auto& node = static_cast<const VarExpr&>(expr);
        if (VarInfo* info = lookup(node.name)) {
          if (optimize_ && info->folded) {
            inst("li t0, " + std::to_string(info->value));
          } else {
            inst("lw t0, " + std::to_string(info->slot) + "(s0)");
          }
        } else {
          inst("li t0, 0");
        }
        break;
      }
      case ExprKind::Unary:
        eval_unary(static_cast<const UnaryExprNode&>(expr));
        break;
      case ExprKind::Binary:
        eval_binary(static_cast<const BinaryExprNode&>(expr));
        break;
      case ExprKind::Call:
        eval_call(static_cast<const CallExprNode&>(expr));
        break;
      case ExprKind::Bad:
        inst("li t0, 0");
        break;
    }
  }

  void eval_unary(const UnaryExprNode& expr) {
    if (expr.rhs) {
      eval(*expr.rhs);
    }
    if (expr.op == TokenKind::Minus) {
      inst("negw t0, t0");
    } else if (expr.op == TokenKind::Not) {
      inst("seqz t0, t0");
    }
  }

  bool strength_reduce(const BinaryExprNode& expr) {
    if (!optimize_ || !expr.lhs || !expr.rhs) {
      return false;
    }
    const bool left_const = expr.lhs->has_const && !has_side_effect(*expr.lhs);
    const bool right_const = expr.rhs->has_const && !has_side_effect(*expr.rhs);
    const int32_t left = expr.lhs->const_value;
    const int32_t right = expr.rhs->const_value;
    if (expr.op == TokenKind::Plus && right_const && right == 0) {
      eval(*expr.lhs);
      return true;
    }
    if (expr.op == TokenKind::Plus && left_const && left == 0) {
      eval(*expr.rhs);
      return true;
    }
    if (expr.op == TokenKind::Minus && right_const && right == 0) {
      eval(*expr.lhs);
      return true;
    }
    if ((expr.op == TokenKind::Star || expr.op == TokenKind::Slash) && right_const && right == 1) {
      eval(*expr.lhs);
      return true;
    }
    if (expr.op == TokenKind::Star && left_const && left == 1) {
      eval(*expr.rhs);
      return true;
    }
    if (expr.op == TokenKind::Star && right_const && right == 0 && !has_side_effect(*expr.lhs)) {
      inst("li t0, 0");
      return true;
    }
    if (expr.op == TokenKind::Star && left_const && left == 0 && !has_side_effect(*expr.rhs)) {
      inst("li t0, 0");
      return true;
    }
    return false;
  }

  void eval_binary(const BinaryExprNode& expr) {
    if (!expr.lhs || !expr.rhs) {
      inst("li t0, 0");
      return;
    }
    if (expr.op == TokenKind::AndAnd || expr.op == TokenKind::OrOr) {
      eval_logical(expr);
      return;
    }
    if (strength_reduce(expr)) {
      return;
    }
    eval(*expr.lhs);
    push_t0();
    eval(*expr.rhs);
    inst("mv t1, t0");
    pop_t0();
    switch (expr.op) {
      case TokenKind::Plus:
        inst("addw t0, t0, t1");
        break;
      case TokenKind::Minus:
        inst("subw t0, t0, t1");
        break;
      case TokenKind::Star:
        inst("mulw t0, t0, t1");
        break;
      case TokenKind::Slash:
        inst("divw t0, t0, t1");
        break;
      case TokenKind::Percent:
        inst("remw t0, t0, t1");
        break;
      case TokenKind::Lt:
        inst("slt t0, t0, t1");
        break;
      case TokenKind::Gt:
        inst("slt t0, t1, t0");
        break;
      case TokenKind::Le:
        inst("slt t2, t1, t0");
        inst("xori t0, t2, 1");
        break;
      case TokenKind::Ge:
        inst("slt t2, t0, t1");
        inst("xori t0, t2, 1");
        break;
      case TokenKind::Eq:
        inst("xor t0, t0, t1");
        inst("seqz t0, t0");
        break;
      case TokenKind::NotEq:
        inst("xor t0, t0, t1");
        inst("snez t0, t0");
        break;
      default:
        inst("li t0, 0");
        break;
    }
  }

  void eval_logical(const BinaryExprNode& expr) {
    const std::string skip = fresh(".Lskip_");
    const std::string end = fresh(".Llogic_");
    eval(*expr.lhs);
    if (expr.op == TokenKind::AndAnd) {
      inst("beq t0, zero, " + skip);
    } else {
      inst("bne t0, zero, " + skip);
    }
    eval(*expr.rhs);
    inst("beq t0, zero, " + skip);
    inst("li t0, 1");
    inst("j " + end);
    line(skip + ":");
    if (expr.op == TokenKind::OrOr) {
      inst("snez t0, t0");
    } else {
      inst("li t0, 0");
    }
    line(end + ":");
  }

  void eval_call(const CallExprNode& expr) {
    const int nargs = static_cast<int>(expr.args.size());
    for (int i = 0; i < nargs; ++i) {
      if (expr.args[static_cast<std::size_t>(i)]) {
        eval(*expr.args[static_cast<std::size_t>(i)]);
      } else {
        inst("li t0, 0");
      }
      const int temp = (std::max(0, nargs - 8) * 8) + i * 8;
      inst("sw t0, " + std::to_string(temp) + "(s0)");
    }
    for (int i = 0; i < nargs && i < 8; ++i) {
      const int temp = (std::max(0, nargs - 8) * 8) + i * 8;
      inst("lw a" + std::to_string(i) + ", " + std::to_string(temp) + "(s0)");
    }
    for (int i = 8; i < nargs; ++i) {
      const int temp = (std::max(0, nargs - 8) * 8) + i * 8;
      inst("lw t0, " + std::to_string(temp) + "(s0)");
      inst("sw t0, " + std::to_string((i - 8) * 8) + "(sp)");
    }
    inst("call " + expr.name);
    if (expr.type != ValueKind::Void) {
      inst("mv t0, a0");
    }
  }
};

}  // namespace

std::string emit_asm(const CompUnit& unit, bool optimize) {
  AsmEmitter emitter(optimize);
  return emitter.emit(unit);
}

}  // namespace toyc
