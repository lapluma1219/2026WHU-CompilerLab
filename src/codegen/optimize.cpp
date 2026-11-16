#include "codegen/codegen.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toyc {
namespace {

enum class Op {
  Nop,
  Arg,
  Li,
  Mov,
  Add,
  Sub,
  Mul,
  Div,
  Rem,
  Addi,
  Shl,
  And,
  Sra,
  Slt,
  Sgt,
  Sle,
  Sge,
  Eq,
  Ne,
  Neg,
  Seqz,
  Snez,
  Call,
  Ret,
  Br,
  Jmp,
  CBr
};

struct Inst {
  Op op = Op::Nop;
  int dst = -1;
  int a = -1;
  int b = -1;
  int imm = 0;
  int true_bb = -1;
  int false_bb = -1;
  std::string name;
  std::vector<int> args;
  bool dead = false;
};

struct Phi {
  int dst = -1;
  int var = -1;
  int block = -1;
  std::vector<std::pair<int, int>> incoming;
  bool dead = false;
};

struct Block {
  std::vector<Inst> insts;
  std::vector<int> preds;
  std::vector<int> succs;
  std::vector<int> phis;
  bool sealed = false;
};

struct DefKey {
  int var = 0;
  int bb = 0;
  bool operator==(const DefKey& other) const { return var == other.var && bb == other.bb; }
};

struct DefHash {
  std::size_t operator()(const DefKey& key) const {
    return (static_cast<std::size_t>(key.var) << 32) ^ static_cast<std::size_t>(key.bb);
  }
};

int32_t add32(int32_t lhs, int32_t rhs) {
  return static_cast<int32_t>(static_cast<uint32_t>(lhs) + static_cast<uint32_t>(rhs));
}

int32_t sub32(int32_t lhs, int32_t rhs) {
  return static_cast<int32_t>(static_cast<uint32_t>(lhs) - static_cast<uint32_t>(rhs));
}

int32_t mul32(int32_t lhs, int32_t rhs) {
  return static_cast<int32_t>(static_cast<uint32_t>(lhs) * static_cast<uint32_t>(rhs));
}

int32_t div32(int32_t lhs, int32_t rhs) {
  if (rhs == 0 || (lhs == INT32_MIN && rhs == -1)) {
    return lhs;
  }
  return static_cast<int32_t>(static_cast<int64_t>(lhs) / static_cast<int64_t>(rhs));
}

int32_t rem32(int32_t lhs, int32_t rhs) {
  if (rhs == 0 || (lhs == INT32_MIN && rhs == -1)) {
    return 0;
  }
  return static_cast<int32_t>(static_cast<int64_t>(lhs) % static_cast<int64_t>(rhs));
}

int32_t shl32(int32_t lhs, int amount) {
  return static_cast<int32_t>(static_cast<uint32_t>(lhs) << (amount & 31));
}

bool fits12(int32_t value) { return value >= -2048 && value <= 2047; }

bool is_term(Op op) { return op == Op::Br || op == Op::Jmp || op == Op::Ret || op == Op::CBr; }

bool expr_side_effect(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::Call:
      return true;
    case ExprKind::Unary: {
      const auto& node = static_cast<const UnaryExprNode&>(expr);
      return node.rhs && expr_side_effect(*node.rhs);
    }
    case ExprKind::Binary: {
      const auto& node = static_cast<const BinaryExprNode&>(expr);
      return (node.lhs && expr_side_effect(*node.lhs)) || (node.rhs && expr_side_effect(*node.rhs));
    }
    default:
      return false;
  }
}

int stmt_nodes(const Stmt& stmt) {
  int count = 1;
  switch (stmt.kind) {
    case StmtKind::Block:
      for (const auto& child : static_cast<const BlockStmt&>(stmt).stmts) {
        count += stmt_nodes(*child);
      }
      break;
    case StmtKind::If: {
      const auto& node = static_cast<const IfStmt&>(stmt);
      if (node.then_stmt) {
        count += stmt_nodes(*node.then_stmt);
      }
      if (node.else_stmt) {
        count += stmt_nodes(*node.else_stmt);
      }
      break;
    }
    case StmtKind::While: {
      const auto& node = static_cast<const WhileStmt&>(stmt);
      if (node.body) {
        count += stmt_nodes(*node.body);
      }
      break;
    }
    default:
      break;
  }
  return count;
}

void collect_assigned(const Stmt& stmt, std::unordered_set<std::string>& names) {
  switch (stmt.kind) {
    case StmtKind::Assign:
      names.insert(static_cast<const AssignStmt&>(stmt).name);
      break;
    case StmtKind::VarDecl:
      names.insert(static_cast<const VarDeclStmt&>(stmt).name);
      break;
    case StmtKind::Block:
      for (const auto& child : static_cast<const BlockStmt&>(stmt).stmts) {
        collect_assigned(*child, names);
      }
      break;
    case StmtKind::If: {
      const auto& node = static_cast<const IfStmt&>(stmt);
      if (node.then_stmt) {
        collect_assigned(*node.then_stmt, names);
      }
      if (node.else_stmt) {
        collect_assigned(*node.else_stmt, names);
      }
      break;
    }
    case StmtKind::While:
      if (static_cast<const WhileStmt&>(stmt).body) {
        collect_assigned(*static_cast<const WhileStmt&>(stmt).body, names);
      }
      break;
    default:
      break;
  }
}

bool expr_invariant(const Expr& expr, const std::unordered_set<std::string>& dirty) {
  switch (expr.kind) {
    case ExprKind::IntLit:
      return true;
    case ExprKind::Var:
      return dirty.find(static_cast<const VarExpr&>(expr).name) == dirty.end();
    case ExprKind::Unary: {
      const auto& node = static_cast<const UnaryExprNode&>(expr);
      return node.rhs && expr_invariant(*node.rhs, dirty);
    }
    case ExprKind::Binary: {
      const auto& node = static_cast<const BinaryExprNode&>(expr);
      return node.lhs && node.rhs && expr_invariant(*node.lhs, dirty) &&
             expr_invariant(*node.rhs, dirty);
    }
    default:
      return false;
  }
}

const IfStmt* find_unswitch_if(const WhileStmt& loop) {
  if (!loop.body || stmt_nodes(*loop.body) > 80) {
    return nullptr;
  }
  std::unordered_set<std::string> dirty;
  collect_assigned(*loop.body, dirty);
  const IfStmt* found = nullptr;
  auto consider = [&](const IfStmt& item) {
    if (found || !item.cond || expr_side_effect(*item.cond)) {
      return;
    }
    if (!expr_invariant(*item.cond, dirty)) {
      return;
    }
    found = &item;
  };
  if (loop.body->kind == StmtKind::If) {
    consider(static_cast<const IfStmt&>(*loop.body));
  } else if (loop.body->kind == StmtKind::Block) {
    for (const auto& child : static_cast<const BlockStmt&>(*loop.body).stmts) {
      if (child->kind == StmtKind::If) {
        consider(static_cast<const IfStmt&>(*child));
      }
    }
  }
  return found;
}

const CallExprNode* as_call(const Expr& expr) {
  if (expr.kind != ExprKind::Call) {
    return nullptr;
  }
  return &static_cast<const CallExprNode&>(expr);
}

int32_t literal_value(const IntLitExpr& expr) {
  if (expr.has_const) {
    return expr.const_value;
  }
  try {
    const long long value = std::stoll(expr.lexeme);
    return static_cast<int32_t>(value);
  } catch (...) {
    return 0;
  }
}

const char* k_regs[] = {"s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
                        "t3", "t4", "t5", "t6", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
constexpr int k_callee_regs = 11;
constexpr int k_all_regs = 23;

struct Binding {
  int var = -1;
};

class FunctionCompiler {
 public:
  FunctionCompiler(const FuncDef& func, const std::unordered_map<std::string, const FuncDef*>& funcs)
      : func_(func), funcs_(funcs) {}

  void compile_to(std::ostringstream& out) {
    build();
    optimize();
    eliminate_phis();
    fuse_compares();
    // 头上只剩比较时，把同一条比较放到回边，热路径少一次无条件跳转。
    sink_loop_branch();
    allocate();
    emit_asm(out);
  }

 private:
  const FuncDef& func_;
  const std::unordered_map<std::string, const FuncDef*>& funcs_;
  std::vector<Block> blocks_;
  std::vector<Phi> phis_;
  mutable std::vector<int> alias_;
  std::unordered_map<DefKey, int, DefHash> current_def_;
  std::unordered_map<DefKey, int, DefHash> incomplete_;
  std::vector<std::unordered_map<std::string, Binding>> scopes_;
  std::vector<std::pair<int, int>> loops_;
  std::vector<int> param_vars_;
  int cur_ = -1;
  int entry_bb_ = -1;
  int zero_vreg_ = -1;
  int n_vars_ = 0;
  int inline_exit_ = -1;
  int inline_ret_var_ = -1;
  int inline_depth_ = 0;
  std::unordered_set<std::string> inline_stack_;
  const IfStmt* unswitch_skip_ = nullptr;
  bool unswitch_then_ = true;
  std::vector<uint8_t> known_;
  std::vector<int32_t> kval_;
  std::vector<int> home_;
  std::vector<int> spill_off_;
  std::vector<uint8_t> crosses_;
  int frame_size_ = 0;
  int argsave_off_ = 0;
  int shuffle_off_ = 0;
  int spill_base_ = 0;
  int ra_off_ = 0;
  int s0_off_ = 0;
  int max_call_args_ = 0;
  std::vector<int> saved_sregs_;

  int new_vreg() {
    const int id = static_cast<int>(alias_.size());
    alias_.push_back(id);
    return id;
  }

  int new_var() { return n_vars_++; }

  int new_block() {
    blocks_.push_back(Block{});
    return static_cast<int>(blocks_.size()) - 1;
  }

  int resolve(int vreg) const {
    if (vreg < 0 || vreg >= static_cast<int>(alias_.size())) {
      return vreg;
    }
    int root = vreg;
    while (alias_[root] != root) {
      root = alias_[root];
    }
    while (vreg != root) {
      const int next = alias_[vreg];
      alias_[vreg] = root;
      vreg = next;
    }
    return root;
  }

  void push_scope() { scopes_.emplace_back(); }

  void pop_scope() { scopes_.pop_back(); }

  void bind(const std::string& name, int var) { scopes_.back()[name] = Binding{var}; }

  int lookup_var(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return found->second.var;
      }
    }
    return -1;
  }

  Inst* terminator(int block) {
    for (int i = static_cast<int>(blocks_[block].insts.size()) - 1; i >= 0; --i) {
      if (!blocks_[block].insts[static_cast<std::size_t>(i)].dead) {
        Inst& inst = blocks_[block].insts[static_cast<std::size_t>(i)];
        if (is_term(inst.op)) {
          return &inst;
        }
        return nullptr;
      }
    }
    return nullptr;
  }

  void add_edge(int from, int to) {
    if (from < 0 || to < 0) {
      return;
    }
    blocks_[from].succs.push_back(to);
    blocks_[to].preds.push_back(from);
  }

  void emit_inst(Inst inst) {
    if (cur_ < 0 || (terminator(cur_) != nullptr)) {
      return;
    }
    blocks_[cur_].insts.push_back(std::move(inst));
  }

  int emit_dst(Inst inst) {
    inst.dst = new_vreg();
    emit_inst(inst);
    return inst.dst;
  }

  int emit_li(int32_t value) {
    if (cur_ < 0) {
      return zero_vreg_;
    }
    Inst inst;
    inst.op = Op::Li;
    inst.imm = value;
    return emit_dst(inst);
  }

  int emit_binop(Op op, int lhs, int rhs) {
    Inst inst;
    inst.op = op;
    inst.a = lhs;
    inst.b = rhs;
    return emit_dst(inst);
  }

  void seal(int block);

  int read_variable(int var, int block);
  int read_recursive(int var, int block);
  int add_phi_operands(int var, int block, int phi_dst);
  int try_remove_phi(int phi_index);
  void simplify_phis();
  void write_variable(int var, int block, int value) {
    if (block < 0) {
      return;
    }
    current_def_[DefKey{var, block}] = value;
  }

  void emit_jump(int target) {
    if (cur_ < 0 || terminator(cur_) != nullptr) {
      return;
    }
    Inst inst;
    inst.op = Op::Jmp;
    inst.true_bb = target;
    emit_inst(inst);
    add_edge(cur_, target);
    cur_ = -1;
  }

  void emit_branch(int cond, int true_bb, int false_bb) {
    if (cur_ < 0 || terminator(cur_) != nullptr) {
      return;
    }
    Inst inst;
    inst.op = Op::Br;
    inst.a = cond;
    inst.true_bb = true_bb;
    inst.false_bb = false_bb;
    emit_inst(inst);
    add_edge(cur_, true_bb);
    if (false_bb != true_bb) {
      add_edge(cur_, false_bb);
    }
    cur_ = -1;
  }

  void emit_ret(int value) {
    if (cur_ < 0 || terminator(cur_) != nullptr) {
      return;
    }
    Inst inst;
    inst.op = Op::Ret;
    inst.a = value;
    emit_inst(inst);
    cur_ = -1;
  }

  int eval_expr(const Expr& expr);
  void emit_stmt(const Stmt& stmt);
  void emit_stmts(const std::vector<std::unique_ptr<Stmt>>& stmts);
  void emit_if(const IfStmt& stmt);
  void emit_while(const WhileStmt& stmt, bool allow_unswitch);
  int emit_call(const CallExprNode& call, bool tail);
  int emit_inline(const FuncDef& callee, const CallExprNode& call);

  void build() {
    push_scope();
    cur_ = new_block();
    blocks_[cur_].sealed = true;
    zero_vreg_ = emit_li(0);
    for (const Param& param : func_.params) {
      const int var = new_var();
      param_vars_.push_back(var);
      bind(param.name, var);
      Inst arg;
      arg.op = Op::Arg;
      arg.imm = static_cast<int>(param_vars_.size()) - 1;
      const int value = emit_dst(arg);
      write_variable(var, cur_, value);
    }
    entry_bb_ = new_block();
    emit_jump(entry_bb_);
    cur_ = entry_bb_;
    if (func_.body) {
      emit_stmts(func_.body->stmts);
    }
    if (cur_ >= 0) {
      emit_ret(func_.returns_int ? zero_vreg_ : -1);
    }
    seal(entry_bb_);
    simplify_phis();
    pop_scope();
    if (std::getenv("TOYC_DUMP")) {
      dump_ir("after-build");
    }
  }

  void dump_ir(const char* tag) const {
    std::cerr << "---- " << func_.name << ' ' << tag << " ----\n";
    for (int i = 0; i < static_cast<int>(phis_.size()); ++i) {
      const Phi& phi = phis_[static_cast<std::size_t>(i)];
      if (phi.dead) {
        continue;
      }
      std::cerr << "phi b" << phi.block << " v" << resolve(phi.dst) << " =";
      for (const auto& incoming : phi.incoming) {
        std::cerr << " b" << incoming.first << ":v" << resolve(incoming.second);
      }
      std::cerr << '\n';
    }
    for (int block = 0; block < static_cast<int>(blocks_.size()); ++block) {
      std::cerr << "b" << block << " preds";
      for (int pred : blocks_[static_cast<std::size_t>(block)].preds) {
        std::cerr << ' ' << pred;
      }
      std::cerr << '\n';
      for (const Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
        if (inst.dead) {
          continue;
        }
        std::cerr << "  op" << static_cast<int>(inst.op) << " d" << inst.dst << " a" << inst.a
                  << " b" << inst.b << " imm" << inst.imm << " t" << inst.true_bb << " f"
                  << inst.false_bb << '\n';
      }
    }
  }

  void rebuild_cfg() {
    for (Block& block : blocks_) {
      block.preds.clear();
      block.succs.clear();
    }
    if (blocks_.empty()) {
      return;
    }
    std::vector<uint8_t> seen(blocks_.size(), 0);
    std::vector<int> stack{0};
    while (!stack.empty()) {
      const int block = stack.back();
      stack.pop_back();
      if (block < 0 || block >= static_cast<int>(blocks_.size()) || seen[static_cast<std::size_t>(block)]) {
        continue;
      }
      seen[static_cast<std::size_t>(block)] = 1;
      Inst* term = terminator(block);
      if (!term) {
        continue;
      }
      if (term->op == Op::Jmp) {
        add_edge(block, term->true_bb);
        stack.push_back(term->true_bb);
      } else if (term->op == Op::Br || term->op == Op::CBr) {
        add_edge(block, term->true_bb);
        stack.push_back(term->true_bb);
        if (term->false_bb != term->true_bb) {
          add_edge(block, term->false_bb);
          stack.push_back(term->false_bb);
        }
      }
    }
    for (Phi& phi : phis_) {
      if (phi.dead) {
        continue;
      }
      std::vector<std::pair<int, int>> kept;
      for (const auto& incoming : phi.incoming) {
        const auto& preds = blocks_[static_cast<std::size_t>(phi.block)].preds;
        if (std::find(preds.begin(), preds.end(), incoming.first) != preds.end()) {
          kept.push_back({incoming.first, resolve(incoming.second)});
        }
      }
      phi.incoming = std::move(kept);
    }
  }

  void flatten() {
    for (Block& block : blocks_) {
      for (Inst& inst : block.insts) {
        inst.a = resolve(inst.a);
        inst.b = resolve(inst.b);
        for (int& arg : inst.args) {
          arg = resolve(arg);
        }
      }
    }
    for (Phi& phi : phis_) {
      phi.dst = resolve(phi.dst);
      for (auto& incoming : phi.incoming) {
        incoming.second = resolve(incoming.second);
      }
    }
  }

  void resize_const() {
    known_.assign(alias_.size(), 0);
    kval_.assign(alias_.size(), 0);
  }

  bool get_const(int vreg, int32_t& value) const {
    vreg = resolve(vreg);
    if (vreg < 0 || vreg >= static_cast<int>(known_.size()) || !known_[static_cast<std::size_t>(vreg)]) {
      return false;
    }
    value = kval_[static_cast<std::size_t>(vreg)];
    return true;
  }

  void set_const(int vreg, int32_t value) {
    vreg = resolve(vreg);
    if (vreg < 0) {
      return;
    }
    if (known_.size() < alias_.size()) {
      resize_const();
    }
    known_[static_cast<std::size_t>(vreg)] = 1;
    kval_[static_cast<std::size_t>(vreg)] = value;
  }

  void rewrite_li(Inst& inst, int32_t value) {
    inst.op = Op::Li;
    inst.imm = value;
    inst.a = -1;
    inst.b = -1;
    inst.args.clear();
    set_const(inst.dst, value);
  }

  void propagate_constants() {
    resize_const();
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 64) {
      changed = false;
      for (Block& block : blocks_) {
        for (Inst& inst : block.insts) {
          if (inst.dead || inst.dst < 0) {
            continue;
          }
          int32_t lhs = 0;
          int32_t rhs = 0;
          const bool has_lhs = get_const(inst.a, lhs);
          const bool has_rhs = get_const(inst.b, rhs);
          if (inst.op == Op::Li) {
            if (!known_[static_cast<std::size_t>(resolve(inst.dst))]) {
              set_const(inst.dst, inst.imm);
              changed = true;
            }
          } else if (inst.op == Op::Mov && has_lhs) {
            rewrite_li(inst, lhs);
            changed = true;
          } else if (inst.op == Op::Add && has_lhs && has_rhs) {
            rewrite_li(inst, add32(lhs, rhs));
            changed = true;
          } else if (inst.op == Op::Sub && has_lhs && has_rhs) {
            rewrite_li(inst, sub32(lhs, rhs));
            changed = true;
          } else if (inst.op == Op::Mul && has_lhs && has_rhs) {
            rewrite_li(inst, mul32(lhs, rhs));
            changed = true;
          } else if (inst.op == Op::Div && has_lhs && has_rhs && rhs != 0) {
            rewrite_li(inst, div32(lhs, rhs));
            changed = true;
          } else if (inst.op == Op::Rem && has_lhs && has_rhs && rhs != 0) {
            rewrite_li(inst, rem32(lhs, rhs));
            changed = true;
          } else if (inst.op == Op::Addi && has_lhs) {
            rewrite_li(inst, add32(lhs, inst.imm));
            changed = true;
          } else if (inst.op == Op::Shl && has_lhs) {
            rewrite_li(inst, shl32(lhs, inst.imm));
            changed = true;
          } else if (inst.op == Op::Neg && has_lhs) {
            rewrite_li(inst, sub32(0, lhs));
            changed = true;
          } else if (inst.op == Op::Seqz && has_lhs) {
            rewrite_li(inst, lhs == 0 ? 1 : 0);
            changed = true;
          } else if (inst.op == Op::Snez && has_lhs) {
            rewrite_li(inst, lhs != 0 ? 1 : 0);
            changed = true;
          } else if (has_lhs && has_rhs && inst.op == Op::Slt) {
            rewrite_li(inst, lhs < rhs ? 1 : 0);
            changed = true;
          } else if (has_lhs && has_rhs && inst.op == Op::Sgt) {
            rewrite_li(inst, lhs > rhs ? 1 : 0);
            changed = true;
          } else if (has_lhs && has_rhs && inst.op == Op::Sle) {
            rewrite_li(inst, lhs <= rhs ? 1 : 0);
            changed = true;
          } else if (has_lhs && has_rhs && inst.op == Op::Sge) {
            rewrite_li(inst, lhs >= rhs ? 1 : 0);
            changed = true;
          } else if (has_lhs && has_rhs && inst.op == Op::Eq) {
            rewrite_li(inst, lhs == rhs ? 1 : 0);
            changed = true;
          } else if (has_lhs && has_rhs && inst.op == Op::Ne) {
            rewrite_li(inst, lhs != rhs ? 1 : 0);
            changed = true;
          }
          if (inst.op == Op::Br) {
            int32_t cond = 0;
            if (get_const(inst.a, cond)) {
              inst.op = Op::Jmp;
              inst.true_bb = cond != 0 ? inst.true_bb : inst.false_bb;
              inst.a = -1;
              changed = true;
            }
          }
        }
      }
      for (Phi& phi : phis_) {
        if (phi.dead || phi.incoming.empty()) {
          continue;
        }
        int32_t common = 0;
        bool all = true;
        bool any = false;
        for (const auto& incoming : phi.incoming) {
          int32_t value = 0;
          if (!get_const(incoming.second, value)) {
            all = false;
            break;
          }
          if (!any) {
            common = value;
            any = true;
          } else if (value != common) {
            all = false;
            break;
          }
        }
        if (all && any && !known_[static_cast<std::size_t>(resolve(phi.dst))]) {
          set_const(phi.dst, common);
          changed = true;
        }
      }
    }
  }

  void strength_reduce_ops() {
    for (Block& block : blocks_) {
      for (Inst& inst : block.insts) {
        if (inst.dead) {
          continue;
        }
        int32_t lhs = 0;
        int32_t rhs = 0;
        const bool has_lhs = get_const(inst.a, lhs);
        const bool has_rhs = get_const(inst.b, rhs);
        if (inst.op == Op::Add && has_rhs && fits12(rhs)) {
          inst.op = Op::Addi;
          inst.imm = rhs;
          inst.b = -1;
        } else if (inst.op == Op::Add && has_lhs && fits12(lhs)) {
          inst.op = Op::Addi;
          inst.imm = lhs;
          inst.a = inst.b;
          inst.b = -1;
        } else if (inst.op == Op::Sub && has_rhs && rhs != INT32_MIN && fits12(-rhs)) {
          inst.op = Op::Addi;
          inst.imm = -rhs;
          inst.b = -1;
        } else if (inst.op == Op::Mul && has_rhs && rhs == 0) {
          rewrite_li(inst, 0);
        } else if (inst.op == Op::Mul && has_lhs && lhs == 0) {
          rewrite_li(inst, 0);
        } else if (inst.op == Op::Mul && has_rhs && rhs == 1) {
          inst.op = Op::Mov;
          inst.b = -1;
        } else if (inst.op == Op::Mul && has_lhs && lhs == 1) {
          inst.op = Op::Mov;
          inst.a = inst.b;
          inst.b = -1;
        } else if (inst.op == Op::Mul && has_rhs && rhs == -1) {
          inst.op = Op::Neg;
          inst.b = -1;
        } else if (inst.op == Op::Mul && has_lhs && lhs == -1) {
          inst.op = Op::Neg;
          inst.a = inst.b;
          inst.b = -1;
        } else if (inst.op == Op::Mul && (has_lhs || has_rhs)) {
          const int32_t factor = has_rhs ? rhs : lhs;
          const int other = has_rhs ? inst.a : inst.b;
          if (factor > 1 && (factor & (factor - 1)) == 0) {
            int shift = 0;
            int32_t cursor = factor;
            while (cursor > 1) {
              cursor >>= 1;
              ++shift;
            }
            inst.op = Op::Shl;
            inst.a = other;
            inst.b = -1;
            inst.imm = shift;
          }
        } else if ((inst.op == Op::Div || inst.op == Op::Rem) && has_rhs && rhs == 1) {
          if (inst.op == Op::Div) {
            inst.op = Op::Mov;
            inst.b = -1;
          } else {
            rewrite_li(inst, 0);
          }
        }
      }
    }
  }

  std::vector<int> postorder_;
  std::vector<int> idom_;
  std::vector<int> rpo_index_;
  std::vector<std::vector<int>> dom_children_;

  void compute_dom() {
    const int n = static_cast<int>(blocks_.size());
    idom_.assign(static_cast<std::size_t>(n), -1);
    rpo_index_.assign(static_cast<std::size_t>(n), -1);
    postorder_.clear();
    std::vector<uint8_t> seen(static_cast<std::size_t>(n), 0);
    std::function<void(int)> dfs = [&](int block) {
      seen[static_cast<std::size_t>(block)] = 1;
      for (int succ : blocks_[static_cast<std::size_t>(block)].succs) {
        if (succ >= 0 && !seen[static_cast<std::size_t>(succ)]) {
          dfs(succ);
        }
      }
      postorder_.push_back(block);
    };
    if (n > 0) {
      dfs(0);
    }
    std::vector<int> rpo(postorder_.rbegin(), postorder_.rend());
    for (int i = 0; i < static_cast<int>(rpo.size()); ++i) {
      rpo_index_[static_cast<std::size_t>(rpo[static_cast<std::size_t>(i)])] = i;
    }
    if (n > 0) {
      idom_[0] = 0;
    }
    auto intersect = [&](int lhs, int rhs) {
      int guard = n + 2;
      while (lhs != rhs && guard-- > 0) {
        while (lhs >= 0 && rhs >= 0 && rpo_index_[static_cast<std::size_t>(lhs)] >
                                            rpo_index_[static_cast<std::size_t>(rhs)]) {
          lhs = idom_[static_cast<std::size_t>(lhs)];
        }
        while (lhs >= 0 && rhs >= 0 && rpo_index_[static_cast<std::size_t>(rhs)] >
                                            rpo_index_[static_cast<std::size_t>(lhs)]) {
          rhs = idom_[static_cast<std::size_t>(rhs)];
        }
      }
      return lhs;
    };
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < n + 2) {
      changed = false;
      for (int block : rpo) {
        if (block == 0) {
          continue;
        }
        int chosen = -1;
        for (int pred : blocks_[static_cast<std::size_t>(block)].preds) {
          if (pred < 0 || idom_[static_cast<std::size_t>(pred)] < 0) {
            continue;
          }
          if (chosen < 0) {
            chosen = pred;
          } else {
            chosen = intersect(chosen, pred);
          }
        }
        if (chosen >= 0 && idom_[static_cast<std::size_t>(block)] != chosen) {
          idom_[static_cast<std::size_t>(block)] = chosen;
          changed = true;
        }
      }
    }
    dom_children_.assign(static_cast<std::size_t>(n), {});
    for (int block = 1; block < n; ++block) {
      const int parent = idom_[static_cast<std::size_t>(block)];
      if (parent >= 0 && rpo_index_[static_cast<std::size_t>(block)] >= 0) {
        dom_children_[static_cast<std::size_t>(parent)].push_back(block);
      }
    }
  }

  bool dominates(int parent, int child) const {
    int guard = static_cast<int>(blocks_.size()) + 1;
    while (child >= 0 && guard-- > 0) {
      if (child == parent) {
        return true;
      }
      const int next = idom_[static_cast<std::size_t>(child)];
      if (next < 0 || next == child) {
        return false;
      }
      child = next;
    }
    return false;
  }

  struct GvnKey {
    int op = 0;
    int a = 0;
    int b = 0;
    int imm = 0;
    bool operator==(const GvnKey& other) const {
      return op == other.op && a == other.a && b == other.b && imm == other.imm;
    }
  };

  struct GvnHash {
    std::size_t operator()(const GvnKey& key) const {
      return (static_cast<std::size_t>(key.op) * 131u) ^ (static_cast<std::size_t>(key.a) << 8) ^
             (static_cast<std::size_t>(key.b) << 16) ^ static_cast<std::size_t>(key.imm);
    }
  };

  bool pure_op(Op op) const {
    switch (op) {
      case Op::Li:
      case Op::Mov:
      case Op::Add:
      case Op::Sub:
      case Op::Mul:
      case Op::Div:
      case Op::Rem:
      case Op::Addi:
      case Op::Shl:
      case Op::And:
      case Op::Sra:
      case Op::Slt:
      case Op::Sgt:
      case Op::Sle:
      case Op::Sge:
      case Op::Eq:
      case Op::Ne:
      case Op::Neg:
      case Op::Seqz:
      case Op::Snez:
        return true;
      default:
        return false;
    }
  }

  void global_value_numbering() {
    if (blocks_.empty()) {
      return;
    }
    std::unordered_map<GvnKey, int, GvnHash> table;
    std::function<void(int)> walk = [&](int block) {
      std::vector<GvnKey> added;
      for (Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
        if (inst.dead || inst.dst < 0 || !pure_op(inst.op)) {
          continue;
        }
        GvnKey key;
        key.op = static_cast<int>(inst.op);
        key.a = resolve(inst.a);
        key.b = resolve(inst.b);
        key.imm = inst.imm;
        if (inst.op == Op::Add || inst.op == Op::Mul || inst.op == Op::Eq || inst.op == Op::Ne) {
          if (key.a > key.b) {
            std::swap(key.a, key.b);
          }
        }
        const auto found = table.find(key);
        if (found != table.end() && found->second != inst.dst) {
          alias_[inst.dst] = found->second;
          inst.dead = true;
        } else if (found == table.end()) {
          table.emplace(key, resolve(inst.dst));
          added.push_back(key);
        }
      }
      for (int child : dom_children_[static_cast<std::size_t>(block)]) {
        walk(child);
      }
      for (const GvnKey& key : added) {
        table.erase(key);
      }
    };
    walk(0);
    flatten();
  }

  std::vector<int> def_block_;

  void index_defs() {
    def_block_.assign(alias_.size(), -1);
    for (int block = 0; block < static_cast<int>(blocks_.size()); ++block) {
      for (int phi_index : blocks_[static_cast<std::size_t>(block)].phis) {
        Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
        if (!phi.dead && phi.dst >= 0 && phi.dst < static_cast<int>(def_block_.size())) {
          def_block_[static_cast<std::size_t>(phi.dst)] = block;
        }
      }
      for (const Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
        if (!inst.dead && inst.dst >= 0 && inst.dst < static_cast<int>(def_block_.size())) {
          def_block_[static_cast<std::size_t>(inst.dst)] = block;
        }
      }
    }
  }

  bool defined_outside(int vreg, const std::unordered_set<int>& loop) const {
    vreg = resolve(vreg);
    if (vreg < 0) {
      return true;
    }
    // 常量也要等它的 li 已经在循环外，不能先把用到它的除法提出去。
    if (vreg >= static_cast<int>(def_block_.size())) {
      return false;
    }
    const int block = def_block_[static_cast<std::size_t>(vreg)];
    return block < 0 || loop.find(block) == loop.end();
  }

  void hoist_invariants() {
    index_defs();
    const int n = static_cast<int>(blocks_.size());
    for (int header = 0; header < n; ++header) {
      if (idom_[static_cast<std::size_t>(header)] < 0 && header != 0) {
        continue;
      }
      std::vector<int> latches;
      for (int pred : blocks_[static_cast<std::size_t>(header)].preds) {
        if (dominates(header, pred)) {
          latches.push_back(pred);
        }
      }
      if (latches.empty()) {
        continue;
      }
      std::unordered_set<int> loop;
      std::vector<int> work = latches;
      loop.insert(header);
      while (!work.empty()) {
        const int block = work.back();
        work.pop_back();
        if (block < 0 || !loop.insert(block).second) {
          continue;
        }
        for (int pred : blocks_[static_cast<std::size_t>(block)].preds) {
          if (pred != header) {
            work.push_back(pred);
          }
        }
      }
      loop.insert(header);
      int preheader = -1;
      bool unique = true;
      for (int pred : blocks_[static_cast<std::size_t>(header)].preds) {
        if (loop.find(pred) == loop.end()) {
          if (preheader < 0) {
            preheader = pred;
          } else {
            unique = false;
          }
        }
      }
      if (!unique || preheader < 0) {
        continue;
      }
      for (int block : loop) {
        if (block == preheader) {
          continue;
        }
        for (Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
          if (inst.dead || inst.dst < 0) {
            continue;
          }
          const bool arithmetic = inst.op == Op::Add || inst.op == Op::Sub || inst.op == Op::Mul ||
                                   inst.op == Op::Addi || inst.op == Op::Shl || inst.op == Op::And ||
                                   inst.op == Op::Sra || inst.op == Op::Neg ||
                                   inst.op == Op::Li || inst.op == Op::Slt || inst.op == Op::Sgt ||
                                   inst.op == Op::Sle || inst.op == Op::Sge || inst.op == Op::Eq ||
                                   inst.op == Op::Ne || inst.op == Op::Seqz || inst.op == Op::Snez;
          const bool divrem = inst.op == Op::Div || inst.op == Op::Rem;
          if (!arithmetic && !divrem) {
            continue;
          }
          if (inst.a >= 0 && !defined_outside(inst.a, loop)) {
            continue;
          }
          if (inst.b >= 0 && !defined_outside(inst.b, loop)) {
            continue;
          }
          if (divrem) {
            int32_t divisor = 0;
            const bool const_nz = get_const(inst.b, divisor) && divisor != 0;
            bool every_iter = !latches.empty();
            for (int latch : latches) {
              if (!dominates(block, latch)) {
                every_iter = false;
              }
            }
            if (!const_nz && !every_iter) {
              continue;
            }
          }
          Inst moved = inst;
          inst.dead = true;
          Block& dest = blocks_[static_cast<std::size_t>(preheader)];
          auto place = dest.insts.end();
          if (!dest.insts.empty() && is_term(dest.insts.back().op)) {
            place = dest.insts.end() - 1;
          }
          dest.insts.insert(place, std::move(moved));
          if (moved.dst >= 0 && moved.dst < static_cast<int>(def_block_.size())) {
            def_block_[static_cast<std::size_t>(moved.dst)] = preheader;
          }
        }
      }
    }
  }

  const Inst* def_inst(int vreg) const {
    vreg = resolve(vreg);
    for (const Block& block : blocks_) {
      for (const Inst& inst : block.insts) {
        if (!inst.dead && inst.dst == vreg) {
          return &inst;
        }
      }
    }
    return nullptr;
  }

  struct InductionMul {
    int mul_dst = -1;
    int phi_dst = -1;
    int init_v = -1;
    int32_t step = 0;
    int32_t factor = 0;
    int preheader = -1;
    int latch = -1;
    int header = -1;
  };

  void reduce_induction() {
    index_defs();
    std::vector<InductionMul> jobs;
    const int n = static_cast<int>(blocks_.size());
    for (int header = 0; header < n; ++header) {
      std::vector<int> latches;
      std::unordered_set<int> loop;
      for (int pred : blocks_[static_cast<std::size_t>(header)].preds) {
        if (dominates(header, pred)) {
          latches.push_back(pred);
        }
      }
      if (latches.size() != 1) {
        continue;
      }
      std::vector<int> work = latches;
      while (!work.empty()) {
        const int block = work.back();
        work.pop_back();
        if (!loop.insert(block).second) {
          continue;
        }
        for (int pred : blocks_[static_cast<std::size_t>(block)].preds) {
          if (pred != header) {
            work.push_back(pred);
          }
        }
      }
      loop.insert(header);
      int preheader = -1;
      for (int pred : blocks_[static_cast<std::size_t>(header)].preds) {
        if (loop.count(pred) == 0) {
          if (preheader >= 0) {
            preheader = -2;
            break;
          }
          preheader = pred;
        }
      }
      if (preheader < 0) {
        continue;
      }
      const int latch = latches[0];
      for (int phi_index : blocks_[static_cast<std::size_t>(header)].phis) {
        Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
        if (phi.dead || phi.incoming.size() != 2) {
          continue;
        }
        int init_v = -1;
        int next_v = -1;
        for (const auto& incoming : phi.incoming) {
          if (incoming.first == latch) {
            next_v = resolve(incoming.second);
          } else if (incoming.first == preheader) {
            init_v = resolve(incoming.second);
          }
        }
        if (init_v < 0 || next_v < 0) {
          continue;
        }
        const Inst* update = def_inst(next_v);
        if (!update || update->dead) {
          continue;
        }
        int32_t step = 0;
        bool step_known = false;
        if (update->op == Op::Addi && resolve(update->a) == resolve(phi.dst)) {
          step = update->imm;
          step_known = true;
        } else if (update->op == Op::Add) {
          const int ua = resolve(update->a);
          const int ub = resolve(update->b);
          if (ua == resolve(phi.dst) && get_const(ub, step)) {
            step_known = true;
          } else if (ub == resolve(phi.dst) && get_const(ua, step)) {
            step_known = true;
          }
        }
        if (!step_known || step == 0) {
          continue;
        }
        const int phi_dst = resolve(phi.dst);
        for (int block : loop) {
          for (const Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
            if (inst.dead || inst.op != Op::Mul || inst.dst < 0) {
              continue;
            }
            const int ma = resolve(inst.a);
            const int mb = resolve(inst.b);
            int32_t factor = 0;
            if (ma == phi_dst && get_const(mb, factor)) {
            } else if (mb == phi_dst && get_const(ma, factor)) {
            } else {
              continue;
            }
            InductionMul job;
            job.mul_dst = inst.dst;
            job.phi_dst = phi_dst;
            job.init_v = init_v;
            job.step = step;
            job.factor = factor;
            job.preheader = preheader;
            job.latch = latch;
            job.header = header;
            jobs.push_back(job);
          }
        }
      }
    }
    for (const InductionMul& job : jobs) {
      Inst* mul = nullptr;
      for (Block& block : blocks_) {
        for (Inst& inst : block.insts) {
          if (!inst.dead && inst.dst == job.mul_dst) {
            mul = &inst;
          }
        }
      }
      if (!mul) {
        continue;
      }
      mul->dead = true;
      mul = nullptr;
      const int32_t delta = mul32(job.step, job.factor);
      int init_m = zero_vreg_;
      int32_t init_const = 0;
      if (get_const(job.init_v, init_const)) {
        Inst seed;
        seed.op = Op::Li;
        seed.imm = mul32(init_const, job.factor);
        seed.dst = new_vreg();
        init_m = seed.dst;
        insert_before_term(job.preheader, seed);
        set_const(init_m, seed.imm);
      } else {
        Inst factor_li;
        factor_li.op = Op::Li;
        factor_li.imm = job.factor;
        factor_li.dst = new_vreg();
        insert_before_term(job.preheader, factor_li);
        Inst seed;
        seed.op = Op::Mul;
        seed.a = job.init_v;
        seed.b = factor_li.dst;
        seed.dst = new_vreg();
        init_m = seed.dst;
        insert_before_term(job.preheader, seed);
      }
      const int derived_dst = new_vreg();
      Inst step_add;
      step_add.dst = new_vreg();
      step_add.a = derived_dst;
      if (fits12(delta)) {
        step_add.op = Op::Addi;
        step_add.imm = delta;
      } else {
        Inst delta_li;
        delta_li.op = Op::Li;
        delta_li.imm = delta;
        delta_li.dst = new_vreg();
        insert_before_term(job.latch, delta_li);
        step_add.op = Op::Add;
        step_add.b = delta_li.dst;
      }
      const int step_dst = step_add.dst;
      insert_before_term(job.latch, step_add);
      Phi derived;
      derived.dst = derived_dst;
      derived.block = job.header;
      derived.var = -1;
      derived.incoming.push_back({job.preheader, init_m});
      derived.incoming.push_back({job.latch, step_dst});
      blocks_[static_cast<std::size_t>(job.header)].phis.push_back(static_cast<int>(phis_.size()));
      phis_.push_back(derived);
      alias_[job.mul_dst] = derived_dst;
    }
    flatten();
  }

  void insert_before_term(int block, Inst inst) {
    Block& dest = blocks_[static_cast<std::size_t>(block)];
    auto place = dest.insts.end();
    if (!dest.insts.empty() && is_term(dest.insts.back().op) && !dest.insts.back().dead) {
      place = dest.insts.end() - 1;
    }
    dest.insts.insert(place, std::move(inst));
  }

  bool lookup_const(int vreg, int32_t& value) {
    if (get_const(vreg, value)) {
      return true;
    }
    const Inst* def = def_inst(vreg);
    if (def && !def->dead && def->op == Op::Li) {
      value = def->imm;
      return true;
    }
    return false;
  }

  // 把 i = i0; while (i < bound) { acc = acc + (A * i + B); i = i + 1; } 收成乘加。
  // 步长为 1，i0 是非负常数。A、B 是循环不变量。求和按 32 位回绕：
  // trip*(trip-1)/2 用「偶数的那个因子先除 2」再做 32 位乘法，避免先乘后除丢掉高位。
  void close_counted_loops() {
    // 一次只收一个循环，收完重建支配，避免后面的循环用到过期的前驱。
    for (;;) {
    rebuild_cfg();
    compute_dom();
    index_defs();
    const int n = static_cast<int>(blocks_.size());
    bool closed_one = false;
    for (int header = 0; header < n && !closed_one; ++header) {
      Inst* branch = terminator(header);
      if (!branch || branch->op != Op::Br) {
        continue;
      }
      const int latch = branch->true_bb;
      const int exit_bb = branch->false_bb;
      // 出口通常仍被循环头支配，因为它只能从头上的分支到达。不能据此把循环判掉。
      if (latch < 0 || exit_bb < 0 || latch == header || !dominates(header, latch)) {
        continue;
      }
      const Inst* compare = nullptr;
      for (const Inst& inst : blocks_[static_cast<std::size_t>(header)].insts) {
        if (inst.dead || is_term(inst.op)) {
          continue;
        }
        if (resolve(inst.dst) == resolve(branch->a) && inst.op == Op::Slt) {
          compare = &inst;
        } else {
          compare = nullptr;
          break;
        }
      }
      if (!compare) {
        continue;
      }
      std::vector<int> latches;
      int preheader = -1;
      bool bad_pred = false;
      for (int pred : blocks_[static_cast<std::size_t>(header)].preds) {
        if (dominates(header, pred)) {
          latches.push_back(pred);
        } else if (preheader < 0) {
          preheader = pred;
        } else {
          bad_pred = true;
        }
      }
      if (bad_pred || preheader < 0 || latches.size() != 1 || latches[0] != latch) {
        continue;
      }
      Inst* back = terminator(latch);
      if (!back || back->op != Op::Jmp || back->true_bb != header) {
        continue;
      }
      int iv_phi = -1;
      int32_t iv_init = 0;
      bool iv_ok = false;
      struct Term {
        int32_t imm = 0;
        std::vector<std::pair<int32_t, int>> vars;
      };
      struct Series {
        int phi = -1;
        int init = -1;
        Term coeff;
        Term add;
      };
      struct PhiInfo {
        int dst = -1;
        int init_v = -1;
        int next_v = -1;
        const Inst* update = nullptr;
      };
      std::vector<PhiInfo> infos;
      bool shape_ok = true;
      for (int phi_index : blocks_[static_cast<std::size_t>(header)].phis) {
        Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
        if (phi.dead) {
          continue;
        }
        if (phi.incoming.size() != 2) {
          shape_ok = false;
          break;
        }
        PhiInfo info;
        info.dst = resolve(phi.dst);
        for (const auto& incoming : phi.incoming) {
          if (incoming.first == latch) {
            info.next_v = resolve(incoming.second);
          } else if (incoming.first == preheader) {
            info.init_v = resolve(incoming.second);
          }
        }
        if (info.init_v < 0 || info.next_v < 0) {
          shape_ok = false;
          break;
        }
        info.update = def_inst(info.next_v);
        if (!info.update || info.update->dead) {
          shape_ok = false;
          break;
        }
        if (info.update->op == Op::Addi && resolve(info.update->a) == info.dst && info.update->imm == 1 &&
            resolve(compare->a) == info.dst) {
          int32_t init_c = 0;
          if (!lookup_const(info.init_v, init_c) || init_c < 0) {
            shape_ok = false;
            break;
          }
          iv_phi = info.dst;
          iv_init = init_c;
          iv_ok = true;
        }
        infos.push_back(info);
      }
      if (!shape_ok || !iv_ok) {
        continue;
      }
      std::unordered_map<int, std::pair<int32_t, int32_t>> linear;
      for (const PhiInfo& info : infos) {
        if (info.dst == iv_phi) {
          continue;
        }
        int32_t step = 0;
        bool self_step = false;
        if (info.update->op == Op::Addi && resolve(info.update->a) == info.dst) {
          step = info.update->imm;
          self_step = true;
        } else if (info.update->op == Op::Add) {
          const int lhs = resolve(info.update->a);
          const int rhs = resolve(info.update->b);
          if (lhs == info.dst && lookup_const(rhs, step)) {
            self_step = true;
          } else if (rhs == info.dst && lookup_const(lhs, step)) {
            self_step = true;
          }
        }
        int32_t init_c = 0;
        if (self_step && lookup_const(info.init_v, init_c)) {
          linear[info.dst] = {step, sub32(init_c, mul32(step, iv_init))};
        }
      }
      std::unordered_set<int> expr_dst;
      std::vector<Series> accs;
      auto term_add = [](Term& dst, const Term& src) {
        dst.imm = add32(dst.imm, src.imm);
        dst.vars.insert(dst.vars.end(), src.vars.begin(), src.vars.end());
      };
      auto term_scale = [](Term& dst, int32_t factor) {
        dst.imm = mul32(dst.imm, factor);
        for (auto& var : dst.vars) {
          var.first = mul32(var.first, factor);
        }
      };
      for (const PhiInfo& info : infos) {
        if (info.dst == iv_phi) {
          continue;
        }
        struct Poly {
          int32_t acc_imm = 0;
          Term acc;
          Term coeff;
          Term add;
          bool bad = false;
        };
        std::unordered_set<int> visiting;
        auto parse = [&](auto& self, int vreg, int acc_phi, Poly& out) -> void {
          vreg = resolve(vreg);
          if (vreg == acc_phi) {
            out.acc_imm = add32(out.acc_imm, 1);
            return;
          }
          if (vreg == iv_phi) {
            out.coeff.imm = add32(out.coeff.imm, 1);
            return;
          }
          int32_t constant = 0;
          if (lookup_const(vreg, constant)) {
            out.add.imm = add32(out.add.imm, constant);
            return;
          }
          const auto lin = linear.find(vreg);
          if (lin != linear.end() && vreg != acc_phi) {
            out.coeff.imm = add32(out.coeff.imm, lin->second.first);
            out.add.imm = add32(out.add.imm, lin->second.second);
            return;
          }
          const int home = vreg >= 0 && vreg < static_cast<int>(def_block_.size())
                               ? def_block_[static_cast<std::size_t>(vreg)]
                               : -1;
          if (home != latch) {
            if (home < 0 || home == header || dominates(header, home)) {
              out.bad = true;
              return;
            }
            out.add.vars.push_back({1, vreg});
            return;
          }
          if (!visiting.insert(vreg).second) {
            out.bad = true;
            return;
          }
          const Inst* def = def_inst(vreg);
          if (!def || def->dead) {
            out.bad = true;
            visiting.erase(vreg);
            return;
          }
          expr_dst.insert(vreg);
          auto finish = [&]() { visiting.erase(vreg); };
          if (def->op == Op::Add || def->op == Op::Sub) {
            Poly left;
            Poly right;
            self(self, def->a, acc_phi, left);
            self(self, def->b, acc_phi, right);
            if (left.bad || right.bad) {
              out.bad = true;
              finish();
              return;
            }
            if (def->op == Op::Sub) {
              term_scale(right.acc, -1);
              term_scale(right.coeff, -1);
              term_scale(right.add, -1);
              right.acc_imm = mul32(right.acc_imm, -1);
            }
            out.acc_imm = add32(left.acc_imm, right.acc_imm);
            term_add(out.acc, left.acc);
            term_add(out.acc, right.acc);
            term_add(out.coeff, left.coeff);
            term_add(out.coeff, right.coeff);
            term_add(out.add, left.add);
            term_add(out.add, right.add);
            finish();
            return;
          }
          if (def->op == Op::Addi) {
            self(self, def->a, acc_phi, out);
            out.add.imm = add32(out.add.imm, def->imm);
            finish();
            return;
          }
          if (def->op == Op::Neg) {
            self(self, def->a, acc_phi, out);
            term_scale(out.acc, -1);
            term_scale(out.coeff, -1);
            term_scale(out.add, -1);
            out.acc_imm = mul32(out.acc_imm, -1);
            finish();
            return;
          }
          if (def->op == Op::Shl) {
            if (def->imm < 0 || def->imm > 31) {
              out.bad = true;
              finish();
              return;
            }
            self(self, def->a, acc_phi, out);
            const int32_t factor = static_cast<int32_t>(1u << def->imm);
            term_scale(out.acc, factor);
            term_scale(out.coeff, factor);
            term_scale(out.add, factor);
            out.acc_imm = mul32(out.acc_imm, factor);
            finish();
            return;
          }
          if (def->op == Op::Mov) {
            self(self, def->a, acc_phi, out);
            finish();
            return;
          }
          if (def->op == Op::Mul) {
            Poly left;
            Poly right;
            self(self, def->a, acc_phi, left);
            self(self, def->b, acc_phi, right);
            if (left.bad || right.bad) {
              out.bad = true;
              finish();
              return;
            }
            auto uses_iv = [](const Poly& poly) {
              return poly.coeff.imm != 0 || !poly.coeff.vars.empty();
            };
            auto uses_acc = [](const Poly& poly) {
              return poly.acc_imm != 0 || !poly.acc.vars.empty();
            };
            if ((uses_iv(left) || uses_acc(left)) && (uses_iv(right) || uses_acc(right))) {
              out.bad = true;
              finish();
              return;
            }
            Poly scaled = uses_iv(left) || uses_acc(left) ? left : right;
            Poly factor = uses_iv(left) || uses_acc(left) ? right : left;
            const bool factor_const = factor.acc_imm == 0 && factor.acc.vars.empty() &&
                                       factor.coeff.imm == 0 && factor.coeff.vars.empty() &&
                                       factor.add.vars.empty();
            const bool factor_var = factor.acc_imm == 0 && factor.acc.vars.empty() &&
                                     factor.coeff.imm == 0 && factor.coeff.vars.empty() &&
                                     factor.add.imm == 0 && factor.add.vars.size() == 1 &&
                                     factor.add.vars[0].first == 1;
            if (factor_const) {
              term_scale(scaled.acc, factor.add.imm);
              term_scale(scaled.coeff, factor.add.imm);
              term_scale(scaled.add, factor.add.imm);
              scaled.acc_imm = mul32(scaled.acc_imm, factor.add.imm);
            } else if (factor_var && scaled.acc.vars.empty() && scaled.coeff.vars.empty() &&
                       scaled.add.vars.empty()) {
              const int var = factor.add.vars[0].second;
              if (scaled.acc_imm != 0) {
                scaled.acc.vars.push_back({scaled.acc_imm, var});
                scaled.acc_imm = 0;
              }
              if (scaled.coeff.imm != 0) {
                scaled.coeff.vars.push_back({scaled.coeff.imm, var});
                scaled.coeff.imm = 0;
              }
              if (scaled.add.imm != 0) {
                scaled.add.vars.push_back({scaled.add.imm, var});
                scaled.add.imm = 0;
              }
            } else {
              out.bad = true;
              finish();
              return;
            }
            out = scaled;
            finish();
            return;
          }
          out.bad = true;
          finish();
        };
        Poly parsed;
        parse(parse, info.next_v, info.dst, parsed);
        if (parsed.bad || parsed.acc_imm != 1 || !parsed.acc.vars.empty()) {
          shape_ok = false;
          break;
        }
        Series series;
        series.phi = info.dst;
        series.init = info.init_v;
        series.coeff = parsed.coeff;
        series.add = parsed.add;
        accs.push_back(std::move(series));
      }
      if (!shape_ok) {
        continue;
      }
      bool extra_inst = false;
      for (const Inst& inst : blocks_[static_cast<std::size_t>(latch)].insts) {
        if (inst.dead || is_term(inst.op)) {
          continue;
        }
        bool explained = expr_dst.count(resolve(inst.dst)) != 0;
        for (int phi_index : blocks_[static_cast<std::size_t>(header)].phis) {
          const Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
          if (phi.dead) {
            continue;
          }
          for (const auto& incoming : phi.incoming) {
            if (incoming.first == latch && resolve(incoming.second) == resolve(inst.dst)) {
              explained = true;
            }
          }
        }
        if (!explained) {
          extra_inst = true;
        }
      }
      if (extra_inst) {
        continue;
      }
      const int bound = resolve(compare->b);
      if (bound < 0) {
        continue;
      }
      const int bound_block = bound < static_cast<int>(def_block_.size())
                                  ? def_block_[static_cast<std::size_t>(bound)]
                                  : header;
      if (bound_block == header || bound_block == latch || dominates(header, bound_block)) {
        continue;
      }
      if (!terminator(preheader) || terminator(preheader)->op != Op::Jmp) {
        continue;
      }
      // 循环里算出来、又在循环外被用到的值，只能是归纳变量和累加量。
      // 指令用必须在循环头支配的块里；phi 用必须来自头本身或头支配的前驱。
      std::unordered_set<int> exported;
      exported.insert(iv_phi);
      for (const Series& acc : accs) {
        exported.insert(acc.phi);
      }
      auto defined_in_loop = [&](int vreg) {
        vreg = resolve(vreg);
        if (vreg < 0 || vreg >= static_cast<int>(def_block_.size())) {
          return false;
        }
        const int block = def_block_[static_cast<std::size_t>(vreg)];
        return block == header || block == latch;
      };
      bool use_ok = true;
      for (int block = 0; block < n && use_ok; ++block) {
        if (block == header || block == latch) {
          continue;
        }
        for (const Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
          if (inst.dead) {
            continue;
          }
          auto check_use = [&](int slot) {
            const int cur = resolve(slot);
            if (!defined_in_loop(cur)) {
              return;
            }
            if (!exported.count(cur) || !dominates(header, block)) {
              use_ok = false;
            }
          };
          check_use(inst.a);
          check_use(inst.b);
          for (int arg : inst.args) {
            check_use(arg);
          }
        }
        for (int phi_index : blocks_[static_cast<std::size_t>(block)].phis) {
          const Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
          if (phi.dead) {
            continue;
          }
          for (const auto& incoming : phi.incoming) {
            const int cur = resolve(incoming.second);
            if (!defined_in_loop(cur)) {
              continue;
            }
            const bool edge_from_loop = incoming.first == header || dominates(header, incoming.first);
            if (!exported.count(cur) || !edge_from_loop) {
              use_ok = false;
            }
          }
        }
      }
      if (!use_ok) {
        continue;
      }
      const int init_li = new_vreg();
      Inst init_inst;
      init_inst.op = Op::Li;
      init_inst.dst = init_li;
      init_inst.imm = iv_init;
      insert_before_term(preheader, init_inst);
      const int enter = new_block();
      const int skip = new_block();
      const int join = new_block();
      const int cmp_dst = new_vreg();
      Inst cmp;
      cmp.op = Op::Slt;
      cmp.dst = cmp_dst;
      cmp.a = init_li;
      cmp.b = bound;
      insert_before_term(preheader, cmp);
      Inst* pre_term = terminator(preheader);
      pre_term->op = Op::Br;
      pre_term->a = cmp_dst;
      pre_term->true_bb = enter;
      pre_term->false_bb = skip;
      auto emit_in = [&](int block, Inst inst) {
        blocks_[static_cast<std::size_t>(block)].insts.push_back(std::move(inst));
      };
      const int trip = new_vreg();
      Inst trip_sub;
      trip_sub.op = Op::Sub;
      trip_sub.dst = trip;
      trip_sub.a = bound;
      trip_sub.b = init_li;
      emit_in(enter, trip_sub);
      std::vector<std::pair<int, int>> finals;
      finals.push_back({iv_phi, -1});
      const int iv_final = new_vreg();
      Inst iv_add;
      iv_add.op = Op::Add;
      iv_add.dst = iv_final;
      iv_add.a = init_li;
      iv_add.b = trip;
      emit_in(enter, iv_add);
      finals.back().second = iv_final;
      auto emit_term = [&](const Term& term) {
        Inst base;
        base.op = Op::Li;
        base.dst = new_vreg();
        base.imm = term.imm;
        emit_in(enter, base);
        int cur = base.dst;
        for (const auto& var : term.vars) {
          int piece = var.second;
          if (var.first != 1) {
            Inst scale;
            scale.op = Op::Li;
            scale.dst = new_vreg();
            scale.imm = var.first;
            emit_in(enter, scale);
            Inst mul;
            mul.op = Op::Mul;
            mul.dst = new_vreg();
            mul.a = scale.dst;
            mul.b = var.second;
            emit_in(enter, mul);
            piece = mul.dst;
          }
          Inst add;
          add.op = Op::Add;
          add.dst = new_vreg();
          add.a = cur;
          add.b = piece;
          emit_in(enter, add);
          cur = add.dst;
        }
        return cur;
      };
      auto emit_binop = [&](Op op, int lhs, int rhs) {
        Inst inst;
        inst.op = op;
        inst.dst = new_vreg();
        inst.a = lhs;
        inst.b = rhs;
        emit_in(enter, inst);
        return inst.dst;
      };
      // trip >= 1。偶数因子先除 2，32 位乘积等于数学值的低 32 位。
      const int two = new_vreg();
      Inst two_li;
      two_li.op = Op::Li;
      two_li.dst = two;
      two_li.imm = 2;
      emit_in(enter, two_li);
      Inst trip_m1_inst;
      trip_m1_inst.op = Op::Addi;
      trip_m1_inst.dst = new_vreg();
      trip_m1_inst.a = trip;
      trip_m1_inst.imm = -1;
      emit_in(enter, trip_m1_inst);
      const int trip_m1 = trip_m1_inst.dst;
      const int q = emit_binop(Op::Div, trip, two);
      const int q2 = emit_binop(Op::Div, trip_m1, two);
      const int prod_even = emit_binop(Op::Mul, q, trip_m1);
      const int prod_odd = emit_binop(Op::Mul, trip, q2);
      const int twice_q = emit_binop(Op::Mul, q, two);
      const int rem = emit_binop(Op::Sub, trip, twice_q);
      const int delta = emit_binop(Op::Sub, prod_odd, prod_even);
      const int rem_delta = emit_binop(Op::Mul, rem, delta);
      const int half = emit_binop(Op::Add, prod_even, rem_delta);
      int sum_i = half;
      if (iv_init != 0) {
        Inst init_scale;
        init_scale.op = Op::Li;
        init_scale.dst = new_vreg();
        init_scale.imm = iv_init;
        emit_in(enter, init_scale);
        const int base = emit_binop(Op::Mul, trip, init_scale.dst);
        sum_i = emit_binop(Op::Add, base, half);
      }
      for (const Series& acc : accs) {
        const bool coeff_zero = acc.coeff.imm == 0 && acc.coeff.vars.empty();
        const bool add_zero = acc.add.imm == 0 && acc.add.vars.empty();
        int combined = acc.init;
        if (!coeff_zero) {
          const int aval = emit_term(acc.coeff);
          const int part = emit_binop(Op::Mul, aval, sum_i);
          combined = emit_binop(Op::Add, combined, part);
        }
        if (!add_zero) {
          const int bval = emit_term(acc.add);
          const int part = emit_binop(Op::Mul, bval, trip);
          combined = emit_binop(Op::Add, combined, part);
        }
        finals.push_back({acc.phi, combined});
      }
      Inst jmp_join;
      jmp_join.op = Op::Jmp;
      jmp_join.true_bb = join;
      emit_in(enter, jmp_join);
      std::vector<std::pair<int, int>> skip_vals;
      skip_vals.push_back({iv_phi, init_li});
      for (const Series& acc : accs) {
        skip_vals.push_back({acc.phi, acc.init});
      }
      Inst jmp_skip;
      jmp_skip.op = Op::Jmp;
      jmp_skip.true_bb = join;
      emit_in(skip, jmp_skip);
      std::unordered_map<int, int> closed;
      for (std::size_t i = 0; i < finals.size(); ++i) {
        Phi phi;
        phi.dst = new_vreg();
        phi.block = join;
        phi.var = -1;
        phi.incoming.push_back({enter, finals[i].second});
        phi.incoming.push_back({skip, skip_vals[i].second});
        closed[finals[i].first] = phi.dst;
        blocks_[static_cast<std::size_t>(join)].phis.push_back(static_cast<int>(phis_.size()));
        phis_.push_back(phi);
      }
      Inst jmp_exit;
      jmp_exit.op = Op::Jmp;
      jmp_exit.true_bb = exit_bb;
      emit_in(join, jmp_exit);
      auto rewrite_slot = [&](int& slot) {
        const auto found = closed.find(resolve(slot));
        if (found != closed.end()) {
          slot = found->second;
        }
      };
      // 只改循环头支配的块。汇合点上另一条路的指令不能换成这条循环的结果。
      for (int block = 0; block < static_cast<int>(blocks_.size()); ++block) {
        if (block == header || block == latch || block == enter || block == skip || block == join) {
          continue;
        }
        if (!dominates(header, block)) {
          continue;
        }
        for (Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
          rewrite_slot(inst.a);
          rewrite_slot(inst.b);
          for (int& arg : inst.args) {
            rewrite_slot(arg);
          }
        }
      }
      for (Phi& phi : phis_) {
        if (phi.dead || phi.block == header || phi.block == latch || phi.block == enter ||
            phi.block == skip || phi.block == join) {
          continue;
        }
        for (auto& incoming : phi.incoming) {
          rewrite_slot(incoming.second);
          // 头上的出口边改接到 join，phi 的前驱要跟着改。
          if (incoming.first == header) {
            incoming.first = join;
          }
        }
      }
      // new_block 可能让原来的 branch 指针失效，这里重新取。
      // 头不再连到出口，死循环留在 latch 上，重建后从入口不可达。
      Inst* header_term = terminator(header);
      header_term->op = Op::Jmp;
      header_term->true_bb = latch;
      header_term->a = -1;
      closed_one = true;
    }
    if (!closed_one) {
      return;
    }
    }
  }

  void eliminate_dead() {
    const int n = static_cast<int>(alias_.size());
    std::vector<uint8_t> vreg_needed(static_cast<std::size_t>(n), 0);
    std::vector<uint8_t> inst_needed_mark(blocks_.size(), 0);
    std::vector<std::vector<int>> def_inst(static_cast<std::size_t>(n));
    std::vector<std::vector<int>> def_phi(static_cast<std::size_t>(n));
    for (int block = 0; block < static_cast<int>(blocks_.size()); ++block) {
      for (int index = 0; index < static_cast<int>(blocks_[static_cast<std::size_t>(block)].insts.size());
           ++index) {
        Inst& inst = blocks_[static_cast<std::size_t>(block)].insts[static_cast<std::size_t>(index)];
        if (inst.dead) {
          continue;
        }
        if (inst.dst >= 0 && inst.dst < n) {
          def_inst[static_cast<std::size_t>(inst.dst)].push_back(block * 100000 + index);
        }
      }
      for (int phi_index : blocks_[static_cast<std::size_t>(block)].phis) {
        Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
        if (!phi.dead && phi.dst >= 0 && phi.dst < n) {
          def_phi[static_cast<std::size_t>(phi.dst)].push_back(phi_index);
        }
      }
    }
    std::vector<int> stack;
    auto need_vreg = [&](int vreg) {
      vreg = resolve(vreg);
      if (vreg < 0 || vreg >= n || vreg_needed[static_cast<std::size_t>(vreg)]) {
        return;
      }
      vreg_needed[static_cast<std::size_t>(vreg)] = 1;
      stack.push_back(vreg);
    };
    for (Block& block : blocks_) {
      for (Inst& inst : block.insts) {
        if (inst.dead) {
          continue;
        }
        if (inst.op == Op::Call || inst.op == Op::Ret || inst.op == Op::Br || inst.op == Op::Jmp ||
            inst.op == Op::CBr) {
          need_vreg(inst.a);
          need_vreg(inst.b);
          for (int arg : inst.args) {
            need_vreg(arg);
          }
        }
      }
    }
    while (!stack.empty()) {
      const int vreg = stack.back();
      stack.pop_back();
      for (int coded : def_inst[static_cast<std::size_t>(vreg)]) {
        const int block = coded / 100000;
        const int index = coded % 100000;
        Inst& inst = blocks_[static_cast<std::size_t>(block)].insts[static_cast<std::size_t>(index)];
        need_vreg(inst.a);
        need_vreg(inst.b);
        for (int arg : inst.args) {
          need_vreg(arg);
        }
      }
      for (int phi_index : def_phi[static_cast<std::size_t>(vreg)]) {
        for (const auto& incoming : phis_[static_cast<std::size_t>(phi_index)].incoming) {
          need_vreg(incoming.second);
        }
      }
    }
    for (Block& block : blocks_) {
      for (Inst& inst : block.insts) {
        if (inst.dead) {
          continue;
        }
        if (inst.op == Op::Call || inst.op == Op::Ret || inst.op == Op::Br || inst.op == Op::Jmp ||
            inst.op == Op::CBr) {
          if (inst.op == Op::Call && inst.dst >= 0 && !vreg_needed[static_cast<std::size_t>(resolve(inst.dst))]) {
            inst.dst = -1;
          }
          continue;
        }
        if (inst.dst < 0 || !vreg_needed[static_cast<std::size_t>(resolve(inst.dst))]) {
          inst.dead = true;
        }
      }
    }
    for (Phi& phi : phis_) {
      if (!phi.dead && (phi.dst < 0 || !vreg_needed[static_cast<std::size_t>(resolve(phi.dst))])) {
        phi.dead = true;
      }
    }
    (void)inst_needed_mark;
  }

  struct LinForm {
    int32_t imm = 0;
    std::vector<std::pair<int, int32_t>> terms;
  };

  static int log2_floor_u32(uint32_t value) {
    int shift = 0;
    while (value > 1u) {
      value >>= 1;
      ++shift;
    }
    return shift;
  }

  // 把系数变成指令条数。2 的幂用移位，2^k±1 用移位加减，其余用 li+mul。负数直接乘，不再多一条 neg。
  static int scale_cost(int32_t coeff) {
    if (coeff == 1) {
      return 0;
    }
    if (coeff == -1 || coeff == INT32_MIN) {
      return 1;
    }
    if (coeff > 1 && (coeff & (coeff - 1)) == 0) {
      return 1;
    }
    if (coeff < 0) {
      const int32_t mag = static_cast<int32_t>(-static_cast<int64_t>(coeff));
      if (mag > 0 && (mag & (mag - 1)) == 0) {
        return 2;
      }
    }
    if (coeff > 1 && ((coeff - 1) & (coeff - 2)) == 0) {
      return 2;
    }
    if (coeff > 1 && (coeff & (coeff + 1)) == 0) {
      return 2;
    }
    return 2;
  }

  static int form_cost(const LinForm& form) {
    if (form.terms.empty()) {
      return 1;
    }
    int cost = 0;
    bool first = true;
    for (const auto& term : form.terms) {
      cost += scale_cost(term.second);
      if (!first) {
        ++cost;
      }
      first = false;
    }
    if (form.imm != 0) {
      ++cost;
    }
    if (cost == 0) {
      return 1;
    }
    return cost;
  }

  void add_lin_term(LinForm& form, int base, int32_t coeff) const {
    if (coeff == 0 || base < 0) {
      return;
    }
    for (auto& term : form.terms) {
      if (term.first == base) {
        term.second = add32(term.second, coeff);
        if (term.second == 0) {
          term = form.terms.back();
          form.terms.pop_back();
        }
        return;
      }
    }
    form.terms.push_back({base, coeff});
  }

  void add_lin_form(LinForm& dst, const LinForm& src, int32_t scale) const {
    dst.imm = add32(dst.imm, mul32(src.imm, scale));
    for (const auto& term : src.terms) {
      add_lin_term(dst, term.first, mul32(term.second, scale));
    }
  }

  // x % ±2^k 按向零截断展开。负数余数的符号跟被除数，不能直接 and。
  void reduce_pow2_rem() {
    for (Block& block : blocks_) {
      std::vector<Inst> rebuilt;
      for (const Inst& inst : block.insts) {
        int32_t divisor = 0;
        if (inst.dead || inst.op != Op::Rem || !get_const(inst.b, divisor) || divisor == 0 ||
            divisor == 1 || divisor == -1) {
          rebuilt.push_back(inst);
          continue;
        }
        int32_t mag = divisor;
        if (mag < 0) {
          if (mag == INT32_MIN) {
            rebuilt.push_back(inst);
            continue;
          }
          mag = -mag;
        }
        if (mag < 2 || (mag & (mag - 1)) != 0) {
          rebuilt.push_back(inst);
          continue;
        }
        const int32_t mask_bits = mag - 1;
        auto push = [&](Op op, int dst, int a, int b, int imm) {
          Inst made;
          made.op = op;
          made.dst = dst;
          made.a = a;
          made.b = b;
          made.imm = imm;
          rebuilt.push_back(made);
        };
        const int mask_li = new_vreg();
        push(Op::Li, mask_li, -1, -1, mask_bits);
        const int sign = new_vreg();
        push(Op::Sra, sign, inst.a, -1, 31);
        const int mask = new_vreg();
        push(Op::And, mask, sign, mask_li, 0);
        const int biased = new_vreg();
        push(Op::Add, biased, inst.a, mask, 0);
        const int low = new_vreg();
        push(Op::And, low, biased, mask_li, 0);
        push(Op::Sub, inst.dst, low, mask, 0);
      }
      block.insts = std::move(rebuilt);
    }
  }

  // 同一基本块里，把只被加法使用的链收成系数和。模 2^32，只有指令变少才替换。
  void fold_linear_adds() {
    for (int block_index = 0; block_index < static_cast<int>(blocks_.size()); ++block_index) {
      Block& block = blocks_[static_cast<std::size_t>(block_index)];
      std::unordered_map<int, int> def_at;
      std::unordered_set<int> linear_at;
      for (int index = 0; index < static_cast<int>(block.insts.size()); ++index) {
        Inst& inst = block.insts[static_cast<std::size_t>(index)];
        if (inst.dead || inst.dst < 0) {
          continue;
        }
        const int dst = resolve(inst.dst);
        def_at[dst] = index;
        int32_t dummy = 0;
        const bool mul_const = inst.op == Op::Mul && (get_const(inst.a, dummy) || get_const(inst.b, dummy));
        if (inst.op == Op::Add || inst.op == Op::Addi || inst.op == Op::Sub || inst.op == Op::Neg ||
            inst.op == Op::Mov || inst.op == Op::Shl || mul_const) {
          linear_at.insert(dst);
        }
      }
      if (linear_at.size() < 2) {
        continue;
      }
      std::unordered_map<int, std::vector<int>> users;
      auto note_use = [&](int vreg, int user_dst) {
        vreg = resolve(vreg);
        if (linear_at.count(vreg) != 0) {
          users[vreg].push_back(user_dst);
        }
      };
      for (const Inst& inst : block.insts) {
        if (inst.dead) {
          continue;
        }
        const int user = inst.dst >= 0 ? resolve(inst.dst) : -1;
        note_use(inst.a, user);
        note_use(inst.b, user);
        for (int arg : inst.args) {
          note_use(arg, user);
        }
      }
      for (const Phi& phi : phis_) {
        if (phi.dead) {
          continue;
        }
        for (const auto& incoming : phi.incoming) {
          const int vreg = resolve(incoming.second);
          if (linear_at.count(vreg) != 0) {
            users[vreg].push_back(-2);
          }
        }
      }
      std::unordered_set<int> fold;
      for (int dst : linear_at) {
        bool all_linear = !users[dst].empty();
        for (int user : users[dst]) {
          if (user < 0 || linear_at.count(user) == 0 || def_at.count(user) == 0) {
            all_linear = false;
          }
        }
        if (all_linear) {
          fold.insert(dst);
        }
      }
      bool promoted = true;
      while (promoted) {
        promoted = false;
        std::vector<int> drop;
        for (int dst : fold) {
          std::unordered_set<int> sinks;
          std::vector<int> stack = users[dst];
          std::unordered_set<int> seen;
          while (!stack.empty()) {
            const int user = stack.back();
            stack.pop_back();
            if (user < 0 || !seen.insert(user).second) {
              continue;
            }
            if (fold.count(user) != 0) {
              stack.insert(stack.end(), users[user].begin(), users[user].end());
            } else {
              sinks.insert(user);
            }
          }
          if (sinks.size() != 1) {
            drop.push_back(dst);
          }
        }
        for (int dst : drop) {
          fold.erase(dst);
          promoted = true;
        }
      }
      if (fold.empty()) {
        continue;
      }
      std::unordered_map<int, LinForm> forms;
      auto form_of = [&](int vreg) {
        LinForm form;
        vreg = resolve(vreg);
        int32_t constant = 0;
        if (lookup_const(vreg, constant)) {
          form.imm = constant;
          return form;
        }
        if (fold.count(vreg) != 0 && forms.count(vreg) != 0) {
          return forms[vreg];
        }
        form.terms.push_back({vreg, 1});
        return form;
      };
      for (const Inst& inst : block.insts) {
        if (inst.dead || inst.dst < 0 || linear_at.count(resolve(inst.dst)) == 0) {
          continue;
        }
        LinForm form;
        int32_t lhs_c = 0;
        int32_t rhs_c = 0;
        const bool lhs_const = get_const(inst.a, lhs_c);
        const bool rhs_const = get_const(inst.b, rhs_c);
        if (inst.op == Op::Add) {
          form = form_of(inst.a);
          add_lin_form(form, form_of(inst.b), 1);
        } else if (inst.op == Op::Addi) {
          form = form_of(inst.a);
          form.imm = add32(form.imm, inst.imm);
        } else if (inst.op == Op::Sub) {
          form = form_of(inst.a);
          add_lin_form(form, form_of(inst.b), -1);
        } else if (inst.op == Op::Neg) {
          add_lin_form(form, form_of(inst.a), -1);
        } else if (inst.op == Op::Mov) {
          form = form_of(inst.a);
        } else if (inst.op == Op::Shl && inst.imm >= 0 && inst.imm <= 31) {
          add_lin_form(form, form_of(inst.a), static_cast<int32_t>(1u << inst.imm));
        } else if (inst.op == Op::Mul && (lhs_const || rhs_const)) {
          const int32_t factor = rhs_const ? rhs_c : lhs_c;
          const int other = rhs_const ? inst.a : inst.b;
          add_lin_form(form, form_of(other), factor);
        } else {
          form.terms.push_back({resolve(inst.dst), 1});
        }
        forms[resolve(inst.dst)] = form;
      }
      std::unordered_map<int, std::vector<Inst>> replace;
      for (int dst : linear_at) {
        if (fold.count(dst) != 0 || forms.count(dst) == 0) {
          continue;
        }
        const LinForm& form = forms[dst];
        std::unordered_set<int> cone;
        for (int folded : fold) {
          std::unordered_set<int> sinks;
          std::vector<int> walk = users[folded];
          std::unordered_set<int> seen;
          while (!walk.empty()) {
            const int user = walk.back();
            walk.pop_back();
            if (user < 0 || !seen.insert(user).second) {
              continue;
            }
            if (fold.count(user) != 0) {
              walk.insert(walk.end(), users[user].begin(), users[user].end());
            } else {
              sinks.insert(user);
            }
          }
          if (sinks.size() == 1 && sinks.count(dst) != 0) {
            cone.insert(folded);
          }
        }
        const int cone_size = static_cast<int>(cone.size()) + 1;
        if (form_cost(form) >= cone_size || cone.empty()) {
          continue;
        }
        std::vector<Inst> seq;
        int current = -1;
        auto emit = [&](Inst inst) {
          seq.push_back(inst);
          return inst.dst;
        };
        auto piece = [&](int base, int32_t coeff) {
          if (coeff == 1) {
            return base;
          }
          if (coeff == -1) {
            Inst neg;
            neg.op = Op::Neg;
            neg.a = base;
            neg.dst = new_vreg();
            return emit(neg);
          }
          auto shl_to = [&](int src, int amount) {
            Inst shifted;
            shifted.op = Op::Shl;
            shifted.a = src;
            shifted.imm = amount;
            shifted.dst = new_vreg();
            return emit(shifted);
          };
          if (coeff == INT32_MIN) {
            return shl_to(base, 31);
          }
          if (coeff > 1 && (coeff & (coeff - 1)) == 0) {
            return shl_to(base, log2_floor_u32(static_cast<uint32_t>(coeff)));
          }
          if (coeff < -1) {
            const int32_t mag = static_cast<int32_t>(-static_cast<int64_t>(coeff));
            if (mag > 0 && (mag & (mag - 1)) == 0) {
              const int shifted = shl_to(base, log2_floor_u32(static_cast<uint32_t>(mag)));
              Inst neg;
              neg.op = Op::Neg;
              neg.a = shifted;
              neg.dst = new_vreg();
              return emit(neg);
            }
          }
          if (coeff > 1 && ((coeff - 1) & (coeff - 2)) == 0) {
            const int shifted = shl_to(base, log2_floor_u32(static_cast<uint32_t>(coeff - 1)));
            Inst add;
            add.op = Op::Add;
            add.a = shifted;
            add.b = base;
            add.dst = new_vreg();
            return emit(add);
          }
          if (coeff > 1 && (coeff & (coeff + 1)) == 0) {
            const int shifted = shl_to(base, log2_floor_u32(static_cast<uint32_t>(coeff + 1)));
            Inst sub;
            sub.op = Op::Sub;
            sub.a = shifted;
            sub.b = base;
            sub.dst = new_vreg();
            return emit(sub);
          }
          Inst scale;
          scale.op = Op::Li;
          scale.imm = coeff;
          scale.dst = new_vreg();
          emit(scale);
          Inst mul;
          mul.op = Op::Mul;
          mul.a = base;
          mul.b = scale.dst;
          mul.dst = new_vreg();
          return emit(mul);
        };
        for (const auto& term : form.terms) {
          const int part = piece(term.first, term.second);
          if (current < 0) {
            current = part;
            continue;
          }
          Inst add;
          add.op = Op::Add;
          add.a = current;
          add.b = part;
          add.dst = new_vreg();
          current = emit(add);
        }
        if (form.imm != 0) {
          if (current < 0) {
            Inst load;
            load.op = Op::Li;
            load.imm = form.imm;
            load.dst = new_vreg();
            current = emit(load);
          } else if (fits12(form.imm)) {
            Inst add;
            add.op = Op::Addi;
            add.a = current;
            add.imm = form.imm;
            add.dst = new_vreg();
            current = emit(add);
          } else {
            Inst load;
            load.op = Op::Li;
            load.imm = form.imm;
            load.dst = new_vreg();
            emit(load);
            Inst add;
            add.op = Op::Add;
            add.a = current;
            add.b = load.dst;
            add.dst = new_vreg();
            current = emit(add);
          }
        }
        if (current < 0) {
          Inst load;
          load.op = Op::Li;
          load.imm = 0;
          load.dst = new_vreg();
          current = emit(load);
        }
        if (current != dst) {
          if (!seq.empty() && seq.back().dst == current) {
            seq.back().dst = dst;
          } else {
            Inst mov;
            mov.op = Op::Mov;
            mov.a = current;
            mov.dst = dst;
            emit(mov);
          }
        }
        if (static_cast<int>(seq.size()) < cone_size) {
          replace[dst] = std::move(seq);
        }
      }
      if (replace.empty()) {
        continue;
      }
      std::vector<Inst> rebuilt;
      for (Inst& inst : block.insts) {
        if (!inst.dead && inst.dst >= 0 && replace.count(resolve(inst.dst)) != 0) {
          const auto& seq = replace[resolve(inst.dst)];
          rebuilt.insert(rebuilt.end(), seq.begin(), seq.end());
          continue;
        }
        rebuilt.push_back(std::move(inst));
      }
      block.insts = std::move(rebuilt);
    }
  }

  // 分支走到的后继上，条件值是常量。只改被该后继支配的使用，不移动调用。
  void propagate_edge_facts() {
    index_defs();
    rebuild_cfg();
    compute_dom();
    const int n = static_cast<int>(blocks_.size());
    struct Fact {
      int cond = -1;
      int value = 0;
      int block = -1;
      int li = -1;
    };
    std::vector<Fact> facts;
    for (int block = 0; block < n; ++block) {
      if (blocks_[static_cast<std::size_t>(block)].preds.size() != 1) {
        continue;
      }
      const int pred = blocks_[static_cast<std::size_t>(block)].preds[0];
      Inst* term = terminator(pred);
      if (!term || term->op != Op::Br || term->true_bb == term->false_bb) {
        continue;
      }
      int taken = -1;
      if (term->true_bb == block) {
        taken = 1;
      } else if (term->false_bb == block) {
        taken = 0;
      }
      if (taken < 0) {
        continue;
      }
      const int cond = resolve(term->a);
      if (cond < 0) {
        continue;
      }
      // 分支成立只说明值非零。只有比较指令的结果才一定是 0 或 1。
      const Inst* cond_def = def_inst(cond);
      if (!cond_def || !is_cmp(cond_def->op)) {
        continue;
      }
      const int home = cond < static_cast<int>(def_block_.size()) ? def_block_[static_cast<std::size_t>(cond)] : -1;
      if (home >= 0 && dominates(block, home)) {
        continue;
      }
      Fact fact;
      fact.cond = cond;
      fact.value = taken;
      fact.block = block;
      fact.li = new_vreg();
      Inst load;
      load.op = Op::Li;
      load.dst = fact.li;
      load.imm = taken;
      blocks_[static_cast<std::size_t>(block)].insts.insert(blocks_[static_cast<std::size_t>(block)].insts.begin(),
                                                             load);
      facts.push_back(fact);
    }
    for (const Fact& fact : facts) {
      for (int block = 0; block < n; ++block) {
        if (!dominates(fact.block, block)) {
          continue;
        }
        for (Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
          if (inst.dst == fact.li) {
            continue;
          }
          auto rewrite = [&](int& slot) {
            if (resolve(slot) == fact.cond) {
              slot = fact.li;
            }
          };
          rewrite(inst.a);
          rewrite(inst.b);
          for (int& arg : inst.args) {
            rewrite(arg);
          }
        }
        for (int phi_index : blocks_[static_cast<std::size_t>(block)].phis) {
          Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
          for (auto& incoming : phi.incoming) {
            if (resolve(incoming.second) == fact.cond) {
              incoming.second = fact.li;
            }
          }
        }
      }
    }
  }

  void unswitch_invariant_branches() {
    for (int round = 0; round < 3; ++round) {
      rebuild_cfg();
      compute_dom();
      index_defs();
      const int n = static_cast<int>(blocks_.size());
      int header = -1;
      int preheader = -1;
      int branch_block = -1;
      int branch_index = -1;
      std::unordered_set<int> loop;
      for (int candidate = 0; candidate < n && header < 0; ++candidate) {
        std::vector<int> latches;
        for (int pred : blocks_[static_cast<std::size_t>(candidate)].preds) {
          if (dominates(candidate, pred)) {
            latches.push_back(pred);
          }
        }
        if (latches.size() != 1) {
          continue;
        }
        int outside = -1;
        bool two_outside = false;
        for (int pred : blocks_[static_cast<std::size_t>(candidate)].preds) {
          if (!dominates(candidate, pred)) {
            if (outside < 0) {
              outside = pred;
            } else {
              two_outside = true;
            }
          }
        }
        Inst* pre_term = outside >= 0 ? terminator(outside) : nullptr;
        if (two_outside || outside < 0 || !pre_term || pre_term->op != Op::Jmp || pre_term->true_bb != candidate) {
          continue;
        }
        std::unordered_set<int> body;
        std::vector<int> stack = latches;
        body.insert(candidate);
        while (!stack.empty()) {
          const int block = stack.back();
          stack.pop_back();
          if (block < 0 || !dominates(candidate, block) || !body.insert(block).second) {
            continue;
          }
          for (int pred : blocks_[static_cast<std::size_t>(block)].preds) {
            stack.push_back(pred);
          }
        }
        if (body.size() > 24) {
          continue;
        }
        for (int block : body) {
          const auto& insts = blocks_[static_cast<std::size_t>(block)].insts;
          for (int index = 0; index < static_cast<int>(insts.size()); ++index) {
            const Inst& inst = insts[static_cast<std::size_t>(index)];
            if (inst.dead || inst.op != Op::Br || inst.true_bb == inst.false_bb) {
              continue;
            }
            if (body.count(inst.true_bb) == 0 || body.count(inst.false_bb) == 0) {
              continue;
            }
            const int cond = resolve(inst.a);
            int32_t ignore = 0;
            if (lookup_const(cond, ignore)) {
              continue;
            }
            const int home = cond >= 0 && cond < static_cast<int>(def_block_.size())
                                 ? def_block_[static_cast<std::size_t>(cond)]
                                 : -1;
            if (home >= 0 && body.count(home) != 0) {
              continue;
            }
            if (home >= 0 && !dominates(home, outside)) {
              continue;
            }
            header = candidate;
            preheader = outside;
            branch_block = block;
            branch_index = index;
            loop = std::move(body);
            break;
          }
          if (header >= 0) {
            break;
          }
        }
      }
      if (header < 0) {
        return;
      }
      bool extra_entry = false;
      for (int block : loop) {
        for (int pred : blocks_[static_cast<std::size_t>(block)].preds) {
          if (loop.count(pred) == 0 && pred != preheader) {
            extra_entry = true;
          }
        }
      }
      if (extra_entry) {
        return;
      }
      std::unordered_set<int> loop_defs;
      std::unordered_set<int> exits;
      for (int block : loop) {
        for (const Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
          if (!inst.dead && inst.dst >= 0) {
            loop_defs.insert(resolve(inst.dst));
          }
          if (!inst.dead && is_term(inst.op)) {
            if (inst.true_bb >= 0 && loop.count(inst.true_bb) == 0) {
              exits.insert(inst.true_bb);
            }
            if (inst.false_bb >= 0 && loop.count(inst.false_bb) == 0) {
              exits.insert(inst.false_bb);
            }
          }
        }
        for (int phi_index : blocks_[static_cast<std::size_t>(block)].phis) {
          const Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
          if (!phi.dead) {
            loop_defs.insert(resolve(phi.dst));
          }
        }
      }
      bool escaped_far = false;
      for (int block = 0; block < static_cast<int>(blocks_.size()); ++block) {
        if (loop.count(block) != 0) {
          continue;
        }
        auto uses_loop = [&](int vreg) { return loop_defs.count(resolve(vreg)) != 0; };
        bool used = false;
        for (const Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
          if (inst.dead) {
            continue;
          }
          if (uses_loop(inst.a) || uses_loop(inst.b)) {
            used = true;
          }
          for (int arg : inst.args) {
            if (uses_loop(arg)) {
              used = true;
            }
          }
        }
        for (int phi_index : blocks_[static_cast<std::size_t>(block)].phis) {
          const Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
          for (const auto& incoming : phi.incoming) {
            if (uses_loop(incoming.second)) {
              used = true;
            }
          }
        }
        if (used && exits.count(block) == 0) {
          escaped_far = true;
        }
      }
      if (escaped_far) {
        return;
      }
      std::unordered_map<int, int> map_true;
      std::unordered_map<int, int> map_false;
      for (int block : loop) {
        map_true[block] = new_block();
        map_false[block] = new_block();
      }
      std::unordered_map<int, int> vtrue;
      std::unordered_map<int, int> vfalse;
      auto remember = [&](int dst, std::unordered_map<int, int>& map) {
        dst = resolve(dst);
        if (dst >= 0 && map.count(dst) == 0) {
          map[dst] = new_vreg();
        }
      };
      for (int block : loop) {
        for (const Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
          if (!inst.dead && inst.dst >= 0) {
            remember(inst.dst, vtrue);
            remember(inst.dst, vfalse);
          }
        }
        for (int phi_index : blocks_[static_cast<std::size_t>(block)].phis) {
          const Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
          if (!phi.dead) {
            remember(phi.dst, vtrue);
            remember(phi.dst, vfalse);
          }
        }
      }
      const Inst& source_branch = blocks_[static_cast<std::size_t>(branch_block)].insts[static_cast<std::size_t>(branch_index)];
      const int cond = resolve(source_branch.a);
      const int true_target = source_branch.true_bb;
      const int false_target = source_branch.false_bb;
      auto clone_side = [&](const std::unordered_map<int, int>& blocks_map, const std::unordered_map<int, int>& vmap,
                            int taken_target) {
        auto map_block = [&](int block) {
          const auto found = blocks_map.find(block);
          return found == blocks_map.end() ? block : found->second;
        };
        auto map_value = [&](int vreg) {
          vreg = resolve(vreg);
          const auto found = vmap.find(vreg);
          return found == vmap.end() ? vreg : found->second;
        };
        for (int block : loop) {
          const int cloned = blocks_map.at(block);
          for (int phi_index : blocks_[static_cast<std::size_t>(block)].phis) {
            const Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
            if (phi.dead) {
              continue;
            }
            Phi copy = phi;
            copy.dst = map_value(phi.dst);
            copy.block = cloned;
            for (auto& incoming : copy.incoming) {
              incoming.first = map_block(incoming.first);
              incoming.second = map_value(incoming.second);
            }
            blocks_[static_cast<std::size_t>(cloned)].phis.push_back(static_cast<int>(phis_.size()));
            phis_.push_back(std::move(copy));
          }
          for (int index = 0; index < static_cast<int>(blocks_[static_cast<std::size_t>(block)].insts.size()); ++index) {
            Inst inst = blocks_[static_cast<std::size_t>(block)].insts[static_cast<std::size_t>(index)];
            if (inst.dead) {
              continue;
            }
            auto rewrite = [&](int& slot) { slot = map_value(slot); };
            rewrite(inst.a);
            rewrite(inst.b);
            for (int& arg : inst.args) {
              rewrite(arg);
            }
            if (inst.dst >= 0) {
              inst.dst = map_value(inst.dst);
            }
            inst.true_bb = map_block(inst.true_bb);
            inst.false_bb = map_block(inst.false_bb);
            if (block == branch_block && index == branch_index) {
              inst.op = Op::Jmp;
              inst.a = -1;
              inst.true_bb = map_block(taken_target);
              inst.false_bb = -1;
            }
            blocks_[static_cast<std::size_t>(cloned)].insts.push_back(std::move(inst));
          }
        }
      };
      clone_side(map_true, vtrue, true_target);
      clone_side(map_false, vfalse, false_target);
      for (Phi& phi : phis_) {
        if (phi.dead || loop.count(phi.block) != 0) {
          continue;
        }
        std::vector<std::pair<int, int>> incoming;
        for (const auto& edge : phi.incoming) {
          if (loop.count(edge.first) == 0) {
            incoming.push_back(edge);
            continue;
          }
          const int value = resolve(edge.second);
          const int true_value = vtrue.count(value) != 0 ? vtrue[value] : value;
          const int false_value = vfalse.count(value) != 0 ? vfalse[value] : value;
          incoming.push_back({map_true[edge.first], true_value});
          incoming.push_back({map_false[edge.first], false_value});
        }
        phi.incoming = std::move(incoming);
      }
      for (int exit_block : exits) {
        std::unordered_set<int> needed;
        auto consider = [&](int vreg) {
          vreg = resolve(vreg);
          if (loop_defs.count(vreg) != 0) {
            needed.insert(vreg);
          }
        };
        for (const Inst& inst : blocks_[static_cast<std::size_t>(exit_block)].insts) {
          if (inst.dead) {
            continue;
          }
          consider(inst.a);
          consider(inst.b);
          for (int arg : inst.args) {
            consider(arg);
          }
        }
        for (int value : needed) {
          Phi gate;
          gate.dst = new_vreg();
          gate.block = exit_block;
          gate.var = -1;
          for (int block : loop) {
            const Inst* term = terminator(block);
            if (!term) {
              continue;
            }
            if (term->true_bb != exit_block && term->false_bb != exit_block) {
              continue;
            }
            const int true_value = vtrue.count(value) != 0 ? vtrue[value] : value;
            const int false_value = vfalse.count(value) != 0 ? vfalse[value] : value;
            gate.incoming.push_back({map_true[block], true_value});
            gate.incoming.push_back({map_false[block], false_value});
          }
          const int gate_dst = gate.dst;
          blocks_[static_cast<std::size_t>(exit_block)].phis.push_back(static_cast<int>(phis_.size()));
          phis_.push_back(std::move(gate));
          for (Inst& inst : blocks_[static_cast<std::size_t>(exit_block)].insts) {
            auto rewrite = [&](int& slot) {
              if (resolve(slot) == value) {
                slot = gate_dst;
              }
            };
            rewrite(inst.a);
            rewrite(inst.b);
            for (int& arg : inst.args) {
              rewrite(arg);
            }
          }
        }
      }
      Inst* pre_term = terminator(preheader);
      pre_term->op = Op::Br;
      pre_term->a = cond;
      pre_term->true_bb = map_true[header];
      pre_term->false_bb = map_false[header];
    }
  }

  void optimize() {
    simplify_phis();
    flatten();
    propagate_constants();
    rebuild_cfg();
    simplify_phis();
    strength_reduce_ops();
    propagate_constants();
    rebuild_cfg();
    compute_dom();
    global_value_numbering();
    propagate_constants();
    reduce_pow2_rem();
    rebuild_cfg();
    compute_dom();
    for (int pass = 0; pass < 4; ++pass) {
      reassociate();
      propagate_constants();
      strength_reduce_ops();
      hoist_invariants();
    }
    reduce_induction();
    propagate_constants();
    strength_reduce_ops();
    fold_linear_adds();
    propagate_constants();
    rebuild_cfg();
    compute_dom();
    propagate_edge_facts();
    propagate_constants();
    eliminate_dead();
    simplify_phis();
    unswitch_invariant_branches();
    propagate_constants();
    rebuild_cfg();
    compute_dom();
    eliminate_dead();
    simplify_phis();
    close_counted_loops();
    rebuild_cfg();
    eliminate_dead();
    simplify_phis();
    flatten();
    if (!blocks_.empty() && terminator(0) == nullptr) {
      cur_ = 0;
      emit_ret(func_.returns_int ? zero_vreg_ : -1);
      cur_ = -1;
    }
    rebuild_cfg();
    if (std::getenv("TOYC_DUMP")) {
      dump_ir("after-opt");
    }
  }

  void sequentialize(std::vector<std::pair<int, int>> moves, int block) {
    while (!moves.empty()) {
      int picked = -1;
      for (int i = 0; i < static_cast<int>(moves.size()); ++i) {
        if (moves[static_cast<std::size_t>(i)].first == moves[static_cast<std::size_t>(i)].second) {
          moves.erase(moves.begin() + i);
          picked = -2;
          break;
        }
        bool used = false;
        for (int j = 0; j < static_cast<int>(moves.size()); ++j) {
          if (j != i && moves[static_cast<std::size_t>(j)].second == moves[static_cast<std::size_t>(i)].first) {
            used = true;
          }
        }
        if (!used) {
          picked = i;
          break;
        }
      }
      if (picked == -2) {
        continue;
      }
      if (picked >= 0) {
        Inst mov;
        mov.op = Op::Mov;
        mov.dst = moves[static_cast<std::size_t>(picked)].first;
        mov.a = moves[static_cast<std::size_t>(picked)].second;
        insert_before_term(block, mov);
        moves.erase(moves.begin() + picked);
        continue;
      }
      const int source = moves.back().second;
      const int temp = new_vreg();
      Inst mov;
      mov.op = Op::Mov;
      mov.dst = temp;
      mov.a = source;
      insert_before_term(block, mov);
      moves.back().second = temp;
    }
  }

  void retarget(int pred, int old_succ, int neu) {
    for (Inst& inst : blocks_[static_cast<std::size_t>(pred)].insts) {
      if (inst.dead || !is_term(inst.op)) {
        continue;
      }
      if (inst.true_bb == old_succ) {
        inst.true_bb = neu;
      }
      if (inst.false_bb == old_succ) {
        inst.false_bb = neu;
      }
    }
  }

  void eliminate_phis() {
    std::unordered_map<std::uint64_t, std::vector<std::pair<int, int>>> copies;
    for (const Phi& phi : phis_) {
      if (phi.dead) {
        continue;
      }
      for (const auto& incoming : phi.incoming) {
        if (resolve(incoming.second) == resolve(phi.dst)) {
          continue;
        }
        const std::uint64_t key = (static_cast<std::uint64_t>(incoming.first) << 32) |
                                   static_cast<std::uint32_t>(phi.block);
        copies[key].push_back({phi.dst, incoming.second});
      }
    }
    for (auto& entry : copies) {
      const int pred = static_cast<int>(entry.first >> 32);
      const int succ = static_cast<int>(entry.first & 0xffffffffu);
      int home = pred;
      int succ_count = 0;
      if (Inst* term = terminator(pred)) {
        if ((term->op == Op::Br || term->op == Op::CBr) && term->true_bb != term->false_bb) {
          succ_count = 2;
        } else {
          succ_count = 1;
        }
      }
      if (succ_count > 1) {
        home = new_block();
        retarget(pred, succ, home);
        Inst jump;
        jump.op = Op::Jmp;
        jump.true_bb = succ;
        blocks_[static_cast<std::size_t>(home)].insts.push_back(jump);
      }
      sequentialize(entry.second, home);
    }
    for (Phi& phi : phis_) {
      phi.dead = true;
    }
    for (Block& block : blocks_) {
      block.phis.clear();
    }
    rebuild_cfg();
  }

  static bool is_cmp(Op op) {
    return op == Op::Slt || op == Op::Sgt || op == Op::Sle || op == Op::Sge || op == Op::Eq ||
           op == Op::Ne || op == Op::Seqz || op == Op::Snez;
  }

  static int cmp_code(Op op) {
    switch (op) {
      case Op::Slt:
        return 0;
      case Op::Sgt:
        return 1;
      case Op::Sle:
        return 2;
      case Op::Sge:
        return 3;
      case Op::Eq:
        return 4;
      case Op::Ne:
        return 5;
      case Op::Seqz:
        return 6;
      case Op::Snez:
        return 7;
      default:
        return -1;
    }
  }

  void reassociate() {
    for (int guard = 0; guard < 32; ++guard) {
      std::vector<int> uses(alias_.size(), 0);
      std::vector<std::pair<int, int>> def_at(alias_.size(), {-1, -1});
      auto touch = [&](int vreg) {
        vreg = resolve(vreg);
        if (vreg >= 0 && vreg < static_cast<int>(uses.size())) {
          ++uses[static_cast<std::size_t>(vreg)];
        }
      };
      for (int block = 0; block < static_cast<int>(blocks_.size()); ++block) {
        for (int index = 0; index < static_cast<int>(blocks_[static_cast<std::size_t>(block)].insts.size());
             ++index) {
          Inst& inst = blocks_[static_cast<std::size_t>(block)].insts[static_cast<std::size_t>(index)];
          if (inst.dead) {
            continue;
          }
          touch(inst.a);
          touch(inst.b);
          for (int arg : inst.args) {
            touch(arg);
          }
          if (inst.dst >= 0) {
            const int dst = resolve(inst.dst);
            if (dst >= 0 && dst < static_cast<int>(def_at.size())) {
              def_at[static_cast<std::size_t>(dst)] = {block, index};
            }
          }
        }
      }
      bool folded = false;
      for (Block& block : blocks_) {
        for (Inst& inst : block.insts) {
          if (inst.dead || (inst.op != Op::Add && inst.op != Op::Addi)) {
            continue;
          }
          const int left = resolve(inst.a);
          const int right = inst.op == Op::Add ? resolve(inst.b) : -1;
          auto try_fold = [&](int mid, int other, bool other_imm, int32_t other_const) -> bool {
            if (mid < 0 || mid >= static_cast<int>(uses.size()) || uses[static_cast<std::size_t>(mid)] != 1) {
              return false;
            }
            const auto at = def_at[static_cast<std::size_t>(mid)];
            if (at.first < 0) {
              return false;
            }
            Inst& src = blocks_[static_cast<std::size_t>(at.first)].insts[static_cast<std::size_t>(at.second)];
            if (src.dead || (src.op != Op::Add && src.op != Op::Addi) || resolve(src.dst) != mid) {
              return false;
            }
            const int base = resolve(src.a);
            if (base < 0 || base == inst.dst) {
              return false;
            }
            if (src.op == Op::Addi && other_imm) {
              const int32_t sum = add32(src.imm, other_const);
              if (!fits12(sum)) {
                return false;
              }
              inst.op = Op::Addi;
              inst.a = base;
              inst.b = -1;
              inst.imm = sum;
              src.dead = true;
              return true;
            }
            int extra = src.op == Op::Addi ? -1 : resolve(src.b);
            int32_t extra_imm = src.op == Op::Addi ? src.imm : 0;
            const bool extra_is_imm = src.op == Op::Addi;
            Inst sum_inst;
            sum_inst.dst = new_vreg();
            if (extra_is_imm && other_imm) {
              return false;
            }
            if (extra_is_imm && !other_imm) {
              sum_inst.op = Op::Addi;
              sum_inst.a = other;
              sum_inst.imm = extra_imm;
            } else if (!extra_is_imm && other_imm) {
              sum_inst.op = Op::Addi;
              sum_inst.a = extra;
              sum_inst.imm = other_const;
            } else {
              sum_inst.op = Op::Add;
              sum_inst.a = extra;
              sum_inst.b = other;
            }
            if (!fits12(sum_inst.imm) && sum_inst.op == Op::Addi) {
              return false;
            }
            const int sum_dst = sum_inst.dst;
            int inst_index = 0;
            for (int i = 0; i < static_cast<int>(block.insts.size()); ++i) {
              if (&block.insts[static_cast<std::size_t>(i)] == &inst) {
                inst_index = i;
                break;
              }
            }
            const int dest_block = static_cast<int>(&block - blocks_.data());
            int src_index = at.second;
            block.insts.insert(block.insts.begin() + inst_index, sum_inst);
            if (at.first == dest_block && src_index >= inst_index) {
              ++src_index;
            }
            Inst& rewritten = block.insts[static_cast<std::size_t>(inst_index + 1)];
            rewritten.op = Op::Add;
            rewritten.a = base;
            rewritten.b = sum_dst;
            rewritten.imm = 0;
            blocks_[static_cast<std::size_t>(at.first)].insts[static_cast<std::size_t>(src_index)].dead = true;
            return true;
          };
          if (inst.op == Op::Addi && try_fold(left, -1, true, inst.imm)) {
            folded = true;
            break;
          }
          if (inst.op == Op::Add) {
            int32_t lhs_c = 0;
            int32_t rhs_c = 0;
            const bool lhs_imm = get_const(left, lhs_c);
            const bool rhs_imm = get_const(right, rhs_c);
            if (try_fold(left, right, rhs_imm, rhs_c) || try_fold(right, left, lhs_imm, lhs_c)) {
              folded = true;
              break;
            }
          }
          if (folded) {
            break;
          }
        }
        if (folded) {
          break;
        }
      }
      if (!folded) {
        return;
      }
    }
  }

  void rotate_loops() {
    compute_dom();
    const int n = static_cast<int>(blocks_.size());
    for (int header = 0; header < n; ++header) {
      Inst* branch = terminator(header);
      if (!branch || branch->op != Op::Br) {
        continue;
      }
      const int cond = resolve(branch->a);
      const int true_bb = branch->true_bb;
      const int false_bb = branch->false_bb;
      const Inst* compare = nullptr;
      int others = 0;
      for (const Inst& inst : blocks_[static_cast<std::size_t>(header)].insts) {
        if (inst.dead || is_term(inst.op)) {
          continue;
        }
        if (resolve(inst.dst) == cond && is_cmp(inst.op)) {
          compare = &inst;
        } else {
          ++others;
        }
      }
      if (!compare || others != 0) {
        continue;
      }
      std::vector<int> latches;
      bool simple = true;
      bool outside = false;
      for (int pred : blocks_[static_cast<std::size_t>(header)].preds) {
        if (!dominates(header, pred)) {
          outside = true;
          continue;
        }
        Inst* term = terminator(pred);
        if (!term || term->op != Op::Jmp || term->true_bb != header) {
          simple = false;
          break;
        }
        latches.push_back(pred);
      }
      if (!simple || latches.empty() || !outside) {
        continue;
      }
      const Inst saved = *compare;
      for (int latch : latches) {
        Inst cloned = saved;
        cloned.dst = new_vreg();
        cloned.dead = false;
        const int dst = cloned.dst;
        insert_before_term(latch, cloned);
        Inst* term = terminator(latch);
        if (!term) {
          continue;
        }
        term->op = Op::Br;
        term->a = dst;
        term->true_bb = true_bb;
        term->false_bb = false_bb;
      }
    }
    rebuild_cfg();
  }

  void sink_loop_branch() {
    rebuild_cfg();
    compute_dom();
    const int n = static_cast<int>(blocks_.size());
    for (int header = 0; header < n; ++header) {
      Inst* branch = terminator(header);
      if (!branch || branch->op != Op::CBr) {
        continue;
      }
      bool only_branch = true;
      for (const Inst& inst : blocks_[static_cast<std::size_t>(header)].insts) {
        if (!inst.dead && !is_term(inst.op)) {
          only_branch = false;
        }
      }
      if (!only_branch || branch->true_bb < 0 || branch->false_bb < 0) {
        continue;
      }
      for (int pred : blocks_[static_cast<std::size_t>(header)].preds) {
        if (!dominates(header, pred)) {
          continue;
        }
        Inst* back = terminator(pred);
        // 只处理单块循环。多块循环把比较挪走后，布局常常要再补一条跳转。
        if (!back || back->op != Op::Jmp || back->true_bb != header || branch->true_bb != pred) {
          continue;
        }
        back->op = Op::CBr;
        back->imm = branch->imm;
        back->a = branch->a;
        back->b = branch->b;
        back->true_bb = branch->true_bb;
        back->false_bb = branch->false_bb;
      }
    }
    rebuild_cfg();
  }

  void fuse_compares() {
    std::vector<int> uses(alias_.size(), 0);
    std::vector<std::pair<int, int>> def_at(alias_.size(), {-1, -1});
    auto touch = [&](int vreg) {
      vreg = resolve(vreg);
      if (vreg >= 0 && vreg < static_cast<int>(uses.size())) {
        ++uses[static_cast<std::size_t>(vreg)];
      }
    };
    for (int block = 0; block < static_cast<int>(blocks_.size()); ++block) {
      auto& insts = blocks_[static_cast<std::size_t>(block)].insts;
      for (int index = 0; index < static_cast<int>(insts.size()); ++index) {
        Inst& inst = insts[static_cast<std::size_t>(index)];
        if (inst.dead) {
          continue;
        }
        touch(inst.a);
        touch(inst.b);
        for (int arg : inst.args) {
          touch(arg);
        }
        if (inst.dst >= 0) {
          const int dst = resolve(inst.dst);
          if (dst >= 0 && dst < static_cast<int>(def_at.size())) {
            def_at[static_cast<std::size_t>(dst)] = {block, index};
          }
        }
      }
    }
    for (Block& block : blocks_) {
      for (Inst& inst : block.insts) {
        if (inst.dead || inst.op != Op::Br) {
          continue;
        }
        const int cond = resolve(inst.a);
        if (cond < 0 || cond >= static_cast<int>(uses.size()) || uses[static_cast<std::size_t>(cond)] != 1) {
          continue;
        }
        const auto at = def_at[static_cast<std::size_t>(cond)];
        if (at.first < 0) {
          continue;
        }
        Inst& compare = blocks_[static_cast<std::size_t>(at.first)].insts[static_cast<std::size_t>(at.second)];
        const int code = cmp_code(compare.op);
        if (compare.dead || code < 0) {
          continue;
        }
        inst.op = Op::CBr;
        inst.imm = code;
        inst.a = compare.a;
        inst.b = compare.b;
        compare.dead = true;
      }
    }
  }

  void allocate() {
    const int nreg_v = static_cast<int>(alias_.size());
    const int nblock = static_cast<int>(blocks_.size());
    std::vector<std::unordered_set<int>> upward(static_cast<std::size_t>(nblock));
    std::vector<std::unordered_set<int>> defs(static_cast<std::size_t>(nblock));
    auto add_use = [&](int, int vreg, const std::unordered_set<int>& defined,
                       std::unordered_set<int>& exposed) {
      vreg = resolve(vreg);
      if (vreg < 0) {
        return;
      }
      if (defined.find(vreg) == defined.end()) {
        exposed.insert(vreg);
      }
    };
    for (int block = 0; block < nblock; ++block) {
      std::unordered_set<int> defined;
      for (const Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
        if (inst.dead) {
          continue;
        }
        add_use(block, inst.a, defined, upward[static_cast<std::size_t>(block)]);
        add_use(block, inst.b, defined, upward[static_cast<std::size_t>(block)]);
        for (int arg : inst.args) {
          add_use(block, arg, defined, upward[static_cast<std::size_t>(block)]);
        }
        if (inst.dst >= 0) {
          defined.insert(resolve(inst.dst));
        }
      }
      defs[static_cast<std::size_t>(block)] = std::move(defined);
    }
    std::vector<std::unordered_set<int>> live_in(static_cast<std::size_t>(nblock));
    std::vector<std::unordered_set<int>> live_out(static_cast<std::size_t>(nblock));
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < nblock * 4 + 4) {
      changed = false;
      for (int block = nblock - 1; block >= 0; --block) {
        std::unordered_set<int> out;
        for (int succ : blocks_[static_cast<std::size_t>(block)].succs) {
          if (succ < 0 || succ >= nblock) {
            continue;
          }
          out.insert(live_in[static_cast<std::size_t>(succ)].begin(),
                     live_in[static_cast<std::size_t>(succ)].end());
        }
        std::unordered_set<int> in = upward[static_cast<std::size_t>(block)];
        for (int vreg : out) {
          if (defs[static_cast<std::size_t>(block)].count(vreg) == 0) {
            in.insert(vreg);
          }
        }
        if (in != live_in[static_cast<std::size_t>(block)] || out != live_out[static_cast<std::size_t>(block)]) {
          live_in[static_cast<std::size_t>(block)] = std::move(in);
          live_out[static_cast<std::size_t>(block)] = std::move(out);
          changed = true;
        }
      }
    }
    std::vector<std::unordered_set<int>> adj(static_cast<std::size_t>(nreg_v));
    crosses_.assign(static_cast<std::size_t>(nreg_v), 0);
    std::vector<int> cost(static_cast<std::size_t>(nreg_v), 0);
    auto interfere = [&](int lhs, int rhs) {
      lhs = resolve(lhs);
      rhs = resolve(rhs);
      if (lhs < 0 || rhs < 0 || lhs == rhs || lhs >= nreg_v || rhs >= nreg_v) {
        return;
      }
      adj[static_cast<std::size_t>(lhs)].insert(rhs);
      adj[static_cast<std::size_t>(rhs)].insert(lhs);
    };
    for (int block = 0; block < nblock; ++block) {
      const int weight = blocks_[static_cast<std::size_t>(block)].preds.size() > 1 ? 8 : 1;
      std::unordered_set<int> live = live_out[static_cast<std::size_t>(block)];
      for (int index = static_cast<int>(blocks_[static_cast<std::size_t>(block)].insts.size()) - 1; index >= 0;
           --index) {
        Inst& inst = blocks_[static_cast<std::size_t>(block)].insts[static_cast<std::size_t>(index)];
        if (inst.dead) {
          continue;
        }
        if (inst.op == Op::Call) {
          for (int vreg : live) {
            if (vreg != resolve(inst.dst) && vreg >= 0 && vreg < nreg_v) {
              crosses_[static_cast<std::size_t>(vreg)] = 1;
            }
          }
        }
        if (inst.dst >= 0) {
          const int dst = resolve(inst.dst);
          for (int vreg : live) {
            interfere(dst, vreg);
          }
          live.erase(dst);
          if (dst >= 0 && dst < nreg_v) {
            cost[static_cast<std::size_t>(dst)] += weight;
          }
        }
        auto touch = [&](int vreg) {
          vreg = resolve(vreg);
          if (vreg < 0) {
            return;
          }
          live.insert(vreg);
          if (vreg < nreg_v) {
            cost[static_cast<std::size_t>(vreg)] += weight;
          }
        };
        touch(inst.a);
        touch(inst.b);
        for (int arg : inst.args) {
          touch(arg);
        }
        std::vector<int> clique(live.begin(), live.end());
        for (std::size_t i = 0; i < clique.size(); ++i) {
          for (std::size_t j = i + 1; j < clique.size(); ++j) {
            interfere(clique[i], clique[j]);
          }
        }
      }
    }
    home_.assign(static_cast<std::size_t>(nreg_v), -1);
    std::vector<int> order;
    for (int vreg = 0; vreg < nreg_v; ++vreg) {
      if (cost[static_cast<std::size_t>(vreg)] > 0 || !adj[static_cast<std::size_t>(vreg)].empty()) {
        order.push_back(vreg);
      }
    }
    std::sort(order.begin(), order.end(), [&](int lhs, int rhs) {
      if (crosses_[static_cast<std::size_t>(lhs)] != crosses_[static_cast<std::size_t>(rhs)]) {
        return crosses_[static_cast<std::size_t>(lhs)] > crosses_[static_cast<std::size_t>(rhs)];
      }
      return cost[static_cast<std::size_t>(lhs)] > cost[static_cast<std::size_t>(rhs)];
    });
    std::unordered_map<std::uint64_t, int> prefer;
    for (const Block& block : blocks_) {
      for (const Inst& inst : block.insts) {
        if (!inst.dead && inst.op == Op::Mov && inst.dst >= 0 && inst.a >= 0) {
          prefer[(static_cast<std::uint64_t>(resolve(inst.dst)) << 32) |
                 static_cast<std::uint32_t>(resolve(inst.a))] = 1;
        }
      }
    }
    for (int vreg : order) {
      std::unordered_set<int> used;
      for (int other : adj[static_cast<std::size_t>(vreg)]) {
        if (other >= 0 && other < nreg_v && home_[static_cast<std::size_t>(other)] >= 0) {
          used.insert(home_[static_cast<std::size_t>(other)]);
        }
      }
      const int limit = crosses_[static_cast<std::size_t>(vreg)] ? k_callee_regs : k_all_regs;
      int chosen = -1;
      for (const auto& pref : prefer) {
        const int dst = static_cast<int>(pref.first >> 32);
        const int src = static_cast<int>(pref.first & 0xffffffffu);
        int other = -1;
        if (dst == vreg) {
          other = src;
        } else if (src == vreg) {
          other = dst;
        }
        if (other < 0 || other >= nreg_v) {
          continue;
        }
        const int color = home_[static_cast<std::size_t>(other)];
        if (color >= 0 && color < limit && used.count(color) == 0) {
          chosen = color;
          break;
        }
      }
      if (chosen < 0) {
        for (int reg = 0; reg < limit; ++reg) {
          if (used.count(reg) == 0) {
            chosen = reg;
            break;
          }
        }
      }
      home_[static_cast<std::size_t>(vreg)] = chosen;
    }
    spill_off_.assign(static_cast<std::size_t>(nreg_v), -1);
    int spills = 0;
    for (int vreg = 0; vreg < nreg_v; ++vreg) {
      if (home_[static_cast<std::size_t>(vreg)] < 0 &&
          (cost[static_cast<std::size_t>(vreg)] > 0 || !adj[static_cast<std::size_t>(vreg)].empty())) {
        spill_off_[static_cast<std::size_t>(vreg)] = spills++;
      }
    }
    max_call_args_ = 0;
    for (const Block& block : blocks_) {
      for (const Inst& inst : block.insts) {
        if (!inst.dead && inst.op == Op::Call) {
          max_call_args_ = std::max(max_call_args_, static_cast<int>(inst.args.size()));
        }
      }
    }
    saved_sregs_.clear();
    for (int reg = 0; reg < k_callee_regs; ++reg) {
      for (int color : home_) {
        if (color == reg) {
          saved_sregs_.push_back(reg);
          break;
        }
      }
    }
    const int nargs = static_cast<int>(func_.params.size());
    const int out_bytes = std::max(0, max_call_args_ - 8) * 8;
    const int shuffle_bytes = std::min(std::max(max_call_args_, 0), 8) * 8;
    const int argsave_bytes = nargs * 8;
    const int spill_bytes = spills * 8;
    const int save_bytes = 8 * (2 + static_cast<int>(saved_sregs_.size()));
    const int raw = out_bytes + shuffle_bytes + argsave_bytes + spill_bytes + save_bytes;
    frame_size_ = (raw + 15) & ~15;
    shuffle_off_ = out_bytes;
    argsave_off_ = shuffle_off_ + shuffle_bytes;
    spill_base_ = argsave_off_ + argsave_bytes;
    ra_off_ = spill_base_ + spill_bytes;
    s0_off_ = ra_off_ + 8;
  }

  bool in_reg(int vreg) const {
    vreg = resolve(vreg);
    return vreg >= 0 && vreg < static_cast<int>(home_.size()) && home_[static_cast<std::size_t>(vreg)] >= 0;
  }

  const char* reg_name(int vreg) const {
    vreg = resolve(vreg);
    return k_regs[home_[static_cast<std::size_t>(vreg)]];
  }

  int mem_off(int vreg) const {
    vreg = resolve(vreg);
    const int slot = spill_off_[static_cast<std::size_t>(vreg)];
    return spill_base_ + slot * 8;
  }

  std::string load_vreg(std::ostringstream& out, int vreg, const char* tmp) const {
    vreg = resolve(vreg);
    if (vreg < 0) {
      out << '\t' << "li " << tmp << ", 0\n";
      return tmp;
    }
    if (in_reg(vreg)) {
      return reg_name(vreg);
    }
    if (vreg < static_cast<int>(spill_off_.size()) && spill_off_[static_cast<std::size_t>(vreg)] >= 0) {
      out << '\t' << "lw " << tmp << ", " << mem_off(vreg) << "(s0)\n";
      return tmp;
    }
    out << '\t' << "li " << tmp << ", 0\n";
    return tmp;
  }

  void store_tmp(std::ostringstream& out, int vreg, const char* tmp) const {
    if (in_reg(vreg)) {
      if (std::string(reg_name(vreg)) != tmp) {
        out << '\t' << "mv " << reg_name(vreg) << ", " << tmp << '\n';
      }
      return;
    }
    out << '\t' << "sw " << tmp << ", " << mem_off(vreg) << "(s0)\n";
  }

  void emit_move(std::ostringstream& out, int dst, int src) const {
    if (resolve(dst) == resolve(src)) {
      return;
    }
    const std::string left = load_vreg(out, src, "t0");
    if (in_reg(dst)) {
      if (left != reg_name(dst)) {
        out << '\t' << "mv " << reg_name(dst) << ", " << left << '\n';
      }
    } else if (left == "t0") {
      out << '\t' << "sw t0, " << mem_off(dst) << "(s0)\n";
    } else {
      out << '\t' << "sw " << left << ", " << mem_off(dst) << "(s0)\n";
    }
  }

  void emit_to_a0(std::ostringstream& out, int vreg) const {
    const std::string held = load_vreg(out, vreg, "t0");
    if (held != "a0") {
      out << '\t' << "mv a0, " << held << '\n';
    }
  }

  void emit_asm(std::ostringstream& out) {
    out << ".text\n.globl " << func_.name << '\n' << func_.name << ":\n";
    out << '\t' << "addi sp, sp, -" << frame_size_ << '\n';
    out << '\t' << "sd ra, " << ra_off_ << "(sp)\n";
    out << '\t' << "sd s0, " << s0_off_ << "(sp)\n";
    for (int i = 0; i < static_cast<int>(saved_sregs_.size()); ++i) {
      out << '\t' << "sd " << k_regs[saved_sregs_[static_cast<std::size_t>(i)]] << ", "
          << (s0_off_ + 8 + i * 8) << "(sp)\n";
    }
    out << '\t' << "addi s0, sp, 0\n";
    const int nargs = static_cast<int>(func_.params.size());
    for (int i = 0; i < nargs && i < 8; ++i) {
      out << '\t' << "sw a" << i << ", " << (argsave_off_ + i * 8) << "(s0)\n";
    }
    for (int i = 8; i < nargs; ++i) {
      out << '\t' << "lw t0, " << (frame_size_ + (i - 8) * 8) << "(s0)\n";
      out << '\t' << "sw t0, " << (argsave_off_ + i * 8) << "(s0)\n";
    }
    std::vector<int> order;
    std::vector<uint8_t> seen(blocks_.size(), 0);
    std::function<void(int)> dfs = [&](int block) {
      if (block < 0 || block >= static_cast<int>(blocks_.size()) || seen[static_cast<std::size_t>(block)]) {
        return;
      }
      seen[static_cast<std::size_t>(block)] = 1;
      order.push_back(block);
      for (int succ : blocks_[static_cast<std::size_t>(block)].succs) {
        dfs(succ);
      }
    };
    dfs(0);
    auto label = [&](int block) { return func_.name + "_b" + std::to_string(block); };
    for (int index = 0; index < static_cast<int>(order.size()); ++index) {
      const int block = order[static_cast<std::size_t>(index)];
      const int next = index + 1 < static_cast<int>(order.size()) ? order[static_cast<std::size_t>(index + 1)] : -1;
      out << label(block) << ":\n";
      for (const Inst& inst : blocks_[static_cast<std::size_t>(block)].insts) {
        if (inst.dead) {
          continue;
        }
        emit_one(out, inst, next, label);
      }
    }
    out << func_.name << "_epi:\n";
    for (int i = static_cast<int>(saved_sregs_.size()) - 1; i >= 0; --i) {
      out << '\t' << "ld " << k_regs[saved_sregs_[static_cast<std::size_t>(i)]] << ", "
          << (s0_off_ + 8 + i * 8) << "(sp)\n";
    }
    out << '\t' << "ld s0, " << s0_off_ << "(sp)\n";
    out << '\t' << "ld ra, " << ra_off_ << "(sp)\n";
    out << '\t' << "addi sp, sp, " << frame_size_ << '\n';
    out << '\t' << "ret\n\n";
  }

  void emit_result(std::ostringstream& out, const std::string& opcode, int dst, const std::string& rest) const {
    if (in_reg(dst)) {
      out << '\t' << opcode << ' ' << reg_name(dst) << rest << '\n';
      return;
    }
    out << '\t' << opcode << " t2" << rest << '\n';
    out << '\t' << "sw t2, " << mem_off(dst) << "(s0)\n";
  }

  void emit_one(std::ostringstream& out, const Inst& inst, int next,
                const std::function<std::string(int)>& label) {
    switch (inst.op) {
      case Op::Nop:
        break;
      case Op::Arg: {
        const int off = argsave_off_ + inst.imm * 8;
        if (in_reg(inst.dst)) {
          out << '\t' << "lw " << reg_name(inst.dst) << ", " << off << "(s0)\n";
        } else {
          out << '\t' << "lw t0, " << off << "(s0)\n";
          out << '\t' << "sw t0, " << mem_off(inst.dst) << "(s0)\n";
        }
        break;
      }
      case Op::Li:
        emit_result(out, "li", inst.dst, ", " + std::to_string(inst.imm));
        break;
      case Op::Mov:
        emit_move(out, inst.dst, inst.a);
        break;
      case Op::Add:
      case Op::Sub:
      case Op::Mul:
      case Op::Div:
      case Op::Rem:
      case Op::Slt:
      case Op::Sgt: {
        const char* op = "addw";
        if (inst.op == Op::Sub) op = "subw";
        if (inst.op == Op::Mul) op = "mulw";
        if (inst.op == Op::Div) op = "divw";
        if (inst.op == Op::Rem) op = "remw";
        if (inst.op == Op::Slt) op = "slt";
        const std::string lhs = load_vreg(out, inst.a, "t0");
        const std::string rhs = load_vreg(out, inst.b, "t1");
        if (inst.op == Op::Sgt) {
          emit_result(out, "slt", inst.dst, ", " + rhs + ", " + lhs);
        } else {
          emit_result(out, op, inst.dst, ", " + lhs + ", " + rhs);
        }
        break;
      }
      case Op::Addi: {
        const std::string lhs = load_vreg(out, inst.a, "t0");
        emit_result(out, "addiw", inst.dst, ", " + lhs + ", " + std::to_string(inst.imm));
        break;
      }
      case Op::Shl: {
        const std::string lhs = load_vreg(out, inst.a, "t0");
        emit_result(out, "slliw", inst.dst, ", " + lhs + ", " + std::to_string(inst.imm));
        break;
      }
      case Op::And: {
        const std::string lhs = load_vreg(out, inst.a, "t0");
        const std::string rhs = load_vreg(out, inst.b, "t1");
        emit_result(out, "and", inst.dst, ", " + lhs + ", " + rhs);
        break;
      }
      case Op::Sra: {
        const std::string lhs = load_vreg(out, inst.a, "t0");
        emit_result(out, "sraiw", inst.dst, ", " + lhs + ", " + std::to_string(inst.imm));
        break;
      }
      case Op::Sle:
      case Op::Sge:
      case Op::Eq:
      case Op::Ne: {
        const std::string lhs = load_vreg(out, inst.a, "t0");
        const std::string rhs = load_vreg(out, inst.b, "t1");
        if (inst.op == Op::Sle) {
          out << '\t' << "slt t2, " << rhs << ", " << lhs << '\n';
          emit_result(out, "xori", inst.dst, ", t2, 1");
        } else if (inst.op == Op::Sge) {
          out << '\t' << "slt t2, " << lhs << ", " << rhs << '\n';
          emit_result(out, "xori", inst.dst, ", t2, 1");
        } else {
          out << '\t' << "xor t2, " << lhs << ", " << rhs << '\n';
          emit_result(out, inst.op == Op::Eq ? "seqz" : "snez", inst.dst, ", t2");
        }
        break;
      }
      case Op::Neg: {
        const std::string lhs = load_vreg(out, inst.a, "t0");
        emit_result(out, "negw", inst.dst, ", " + lhs);
        break;
      }
      case Op::Seqz:
      case Op::Snez: {
        const std::string lhs = load_vreg(out, inst.a, "t0");
        emit_result(out, inst.op == Op::Seqz ? "seqz" : "snez", inst.dst, ", " + lhs);
        break;
      }
      case Op::Call: {
        const int nargs = static_cast<int>(inst.args.size());
        struct ArgSrc {
          bool mem = false;
          std::string reg;
          int off = 0;
          bool in_a = false;
        };
        std::vector<ArgSrc> srcs;
        int a_sources = 0;
        for (int arg : inst.args) {
          ArgSrc src;
          const int vreg = resolve(arg);
          if (in_reg(vreg)) {
            src.reg = reg_name(vreg);
            src.in_a = src.reg.size() == 2 && src.reg[0] == 'a' && src.reg[1] >= '0' && src.reg[1] <= '7';
            if (src.in_a) {
              ++a_sources;
            }
          } else if (vreg >= 0 && vreg < static_cast<int>(spill_off_.size()) &&
                     spill_off_[static_cast<std::size_t>(vreg)] >= 0) {
            src.mem = true;
            src.off = mem_off(vreg);
          } else {
            src.reg = "zero";
          }
          srcs.push_back(std::move(src));
        }
        const bool direct = nargs <= 8 && a_sources <= 3;
        if (direct) {
          const char* tmps[] = {"t0", "t1", "t2"};
          int tmp_i = 0;
          std::vector<std::string> held(static_cast<std::size_t>(nargs));
          for (int i = 0; i < nargs; ++i) {
            const std::string dest = "a" + std::to_string(i);
            if (srcs[static_cast<std::size_t>(i)].in_a && srcs[static_cast<std::size_t>(i)].reg != dest) {
              held[static_cast<std::size_t>(i)] = tmps[tmp_i];
              out << '\t' << "mv " << tmps[tmp_i] << ", " << srcs[static_cast<std::size_t>(i)].reg << '\n';
              ++tmp_i;
            } else if (!srcs[static_cast<std::size_t>(i)].mem) {
              held[static_cast<std::size_t>(i)] = srcs[static_cast<std::size_t>(i)].reg;
            }
          }
          for (int i = 0; i < nargs; ++i) {
            const std::string dest = "a" + std::to_string(i);
            if (srcs[static_cast<std::size_t>(i)].mem) {
              out << '\t' << "lw " << dest << ", " << srcs[static_cast<std::size_t>(i)].off << "(s0)\n";
            } else if (held[static_cast<std::size_t>(i)] != dest) {
              out << '\t' << "mv " << dest << ", " << held[static_cast<std::size_t>(i)] << '\n';
            }
          }
        } else {
          for (int i = 0; i < nargs; ++i) {
            const std::string held = load_vreg(out, inst.args[static_cast<std::size_t>(i)], "t0");
            const int off = i < 8 ? shuffle_off_ + i * 8 : (i - 8) * 8;
            out << '\t' << "sw " << held << ", " << off << "(s0)\n";
          }
          for (int i = 0; i < nargs && i < 8; ++i) {
            out << '\t' << "lw a" << i << ", " << (shuffle_off_ + i * 8) << "(s0)\n";
          }
        }
        out << '\t' << "call " << inst.name << '\n';
        if (inst.dst >= 0) {
          if (in_reg(inst.dst)) {
            if (std::string(reg_name(inst.dst)) != "a0") {
              out << '\t' << "mv " << reg_name(inst.dst) << ", a0\n";
            }
          } else {
            out << '\t' << "sw a0, " << mem_off(inst.dst) << "(s0)\n";
          }
        }
        break;
      }
      case Op::Ret:
        if (inst.a >= 0) {
          emit_to_a0(out, inst.a);
        }
        out << '\t' << "j " << func_.name << "_epi\n";
        break;
      case Op::Jmp:
        if (inst.true_bb != next) {
          out << '\t' << "j " << label(inst.true_bb) << '\n';
        }
        break;
      case Op::Br: {
        const std::string cond = load_vreg(out, inst.a, "t0");
        if (next == inst.true_bb) {
          out << '\t' << "beq " << cond << ", zero, " << label(inst.false_bb) << '\n';
        } else if (next == inst.false_bb) {
          out << '\t' << "bne " << cond << ", zero, " << label(inst.true_bb) << '\n';
        } else {
          out << '\t' << "beq " << cond << ", zero, " << label(inst.false_bb) << '\n';
          out << '\t' << "j " << label(inst.true_bb) << '\n';
        }
        break;
      }
      case Op::CBr: {
        const std::string lhs = load_vreg(out, inst.a, "t0");
        const std::string rhs = inst.imm >= 6 ? std::string("zero") : load_vreg(out, inst.b, "t1");
        const char* when_true = "blt";
        const char* when_false = "bge";
        std::string true_l = lhs;
        std::string true_r = rhs;
        std::string false_l = lhs;
        std::string false_r = rhs;
        switch (inst.imm) {
          case 0:
            break;
          case 1:
            true_l = rhs;
            true_r = lhs;
            false_l = rhs;
            false_r = lhs;
            break;
          case 2:
            when_true = "bge";
            when_false = "blt";
            true_l = rhs;
            true_r = lhs;
            false_l = rhs;
            false_r = lhs;
            break;
          case 3:
            when_true = "bge";
            when_false = "blt";
            break;
          case 4:
            when_true = "beq";
            when_false = "bne";
            break;
          case 5:
            when_true = "bne";
            when_false = "beq";
            break;
          case 6:
            when_true = "beq";
            when_false = "bne";
            true_r = "zero";
            false_r = "zero";
            break;
          default:
            when_true = "bne";
            when_false = "beq";
            true_r = "zero";
            false_r = "zero";
            break;
        }
        if (next == inst.false_bb) {
          out << '\t' << when_true << ' ' << true_l << ", " << true_r << ", " << label(inst.true_bb) << '\n';
        } else if (next == inst.true_bb) {
          out << '\t' << when_false << ' ' << false_l << ", " << false_r << ", " << label(inst.false_bb)
              << '\n';
        } else {
          out << '\t' << when_true << ' ' << true_l << ", " << true_r << ", " << label(inst.true_bb) << '\n';
          out << '\t' << "j " << label(inst.false_bb) << '\n';
        }
        break;
      }
    }
  }
};

int FunctionCompiler::read_variable(int var, int block) {
  const auto found = current_def_.find(DefKey{var, block});
  if (found != current_def_.end()) {
    return resolve(found->second);
  }
  return read_recursive(var, block);
}

int FunctionCompiler::read_recursive(int var, int block) {
  if (block < 0) {
    return zero_vreg_;
  }
  int value = zero_vreg_;
  if (!blocks_[static_cast<std::size_t>(block)].sealed) {
    value = new_vreg();
    Phi phi;
    phi.dst = value;
    phi.var = var;
    phi.block = block;
    const int index = static_cast<int>(phis_.size());
    phis_.push_back(phi);
    blocks_[static_cast<std::size_t>(block)].phis.push_back(index);
    incomplete_[DefKey{var, block}] = value;
  } else if (blocks_[static_cast<std::size_t>(block)].preds.empty()) {
    value = zero_vreg_;
  } else if (blocks_[static_cast<std::size_t>(block)].preds.size() == 1) {
    value = read_variable(var, blocks_[static_cast<std::size_t>(block)].preds[0]);
  } else {
    value = new_vreg();
    Phi phi;
    phi.dst = value;
    phi.var = var;
    phi.block = block;
    const int index = static_cast<int>(phis_.size());
    phis_.push_back(phi);
    blocks_[static_cast<std::size_t>(block)].phis.push_back(index);
    write_variable(var, block, value);
    value = add_phi_operands(var, block, value);
  }
  write_variable(var, block, value);
  return resolve(value);
}

int FunctionCompiler::add_phi_operands(int var, int block, int phi_dst) {
  int phi_index = -1;
  for (int index : blocks_[static_cast<std::size_t>(block)].phis) {
    if (phis_[static_cast<std::size_t>(index)].dst == phi_dst) {
      phi_index = index;
      break;
    }
  }
  if (phi_index < 0) {
    return phi_dst;
  }
  // 读前驱时会递归创建新的 phi。不能拿 phis_ 里的引用跨过这次调用，
  // 否则 vector 扩容后写入的是已释放的 incoming。
  const std::vector<int> preds = blocks_[static_cast<std::size_t>(block)].preds;
  std::vector<std::pair<int, int>> incoming;
  incoming.reserve(preds.size());
  for (int pred : preds) {
    incoming.push_back({pred, read_variable(var, pred)});
  }
  phis_[static_cast<std::size_t>(phi_index)].incoming = std::move(incoming);
  return try_remove_phi(phi_index);
}

int FunctionCompiler::try_remove_phi(int phi_index) {
  Phi& phi = phis_[static_cast<std::size_t>(phi_index)];
  int same = -1;
  for (const auto& incoming : phi.incoming) {
    const int value = resolve(incoming.second);
    if (value == phi.dst || value == same) {
      continue;
    }
    if (same != -1) {
      return phi.dst;
    }
    same = value;
  }
  if (same < 0) {
    same = zero_vreg_;
  }
  if (same != phi.dst) {
    alias_[phi.dst] = same;
    phi.dead = true;
  }
  return resolve(same);
}

void FunctionCompiler::simplify_phis() {
  bool changed = true;
  int guard = 0;
  while (changed && guard++ < 32) {
    changed = false;
    for (int i = 0; i < static_cast<int>(phis_.size()); ++i) {
      if (phis_[static_cast<std::size_t>(i)].dead) {
        continue;
      }
      const int before = phis_[static_cast<std::size_t>(i)].dst;
      const int after = try_remove_phi(i);
      if (after != before || phis_[static_cast<std::size_t>(i)].dead) {
        changed = true;
      }
    }
  }
}

void FunctionCompiler::seal(int block) {
  if (block < 0 || blocks_[static_cast<std::size_t>(block)].sealed) {
    return;
  }
  blocks_[static_cast<std::size_t>(block)].sealed = true;
  std::vector<std::pair<int, int>> pending;
  for (const auto& entry : incomplete_) {
    if (entry.first.bb == block) {
      pending.push_back({entry.first.var, entry.second});
    }
  }
  for (const auto& item : pending) {
    const int replaced = add_phi_operands(item.first, block, item.second);
    write_variable(item.first, block, replaced);
  }
}

void FunctionCompiler::emit_stmts(const std::vector<std::unique_ptr<Stmt>>& stmts) {
  for (const auto& stmt : stmts) {
    if (cur_ < 0) {
      return;
    }
    emit_stmt(*stmt);
  }
}

void FunctionCompiler::emit_stmt(const Stmt& stmt) {
  if (cur_ < 0) {
    return;
  }
  switch (stmt.kind) {
    case StmtKind::Block: {
      push_scope();
      emit_stmts(static_cast<const BlockStmt&>(stmt).stmts);
      pop_scope();
      break;
    }
    case StmtKind::Empty:
      break;
    case StmtKind::ExprStmt:
      if (static_cast<const ExprStmtNode&>(stmt).expr) {
        eval_expr(*static_cast<const ExprStmtNode&>(stmt).expr);
      }
      break;
    case StmtKind::Assign: {
      const auto& node = static_cast<const AssignStmt&>(stmt);
      const int var = lookup_var(node.name);
      const int value = node.expr ? eval_expr(*node.expr) : zero_vreg_;
      if (var >= 0) {
        write_variable(var, cur_, value);
      }
      break;
    }
    case StmtKind::VarDecl: {
      const auto& node = static_cast<const VarDeclStmt&>(stmt);
      const int var = new_var();
      bind(node.name, var);
      const int value = node.init ? eval_expr(*node.init) : emit_li(0);
      write_variable(var, cur_, value);
      break;
    }
    case StmtKind::If:
      emit_if(static_cast<const IfStmt&>(stmt));
      break;
    case StmtKind::While:
      emit_while(static_cast<const WhileStmt&>(stmt), true);
      break;
    case StmtKind::Break:
      if (!loops_.empty()) {
        emit_jump(loops_.back().second);
      }
      break;
    case StmtKind::Continue:
      if (!loops_.empty()) {
        emit_jump(loops_.back().first);
      }
      break;
    case StmtKind::Return: {
      const auto& node = static_cast<const ReturnStmt&>(stmt);
      if (node.expr && !inline_stack_.count(func_.name)) {
        if (const CallExprNode* call = as_call(*node.expr)) {
          if (inline_exit_ < 0 && call->name == func_.name &&
              call->args.size() == func_.params.size()) {
            emit_call(*call, true);
            break;
          }
        }
      }
      const int value = node.expr ? eval_expr(*node.expr) : -1;
      if (inline_exit_ >= 0) {
        if (value >= 0 && inline_ret_var_ >= 0) {
          write_variable(inline_ret_var_, cur_, value);
        }
        emit_jump(inline_exit_);
      } else {
        emit_ret(value);
      }
      break;
    }
  }
}

void FunctionCompiler::emit_if(const IfStmt& stmt) {
  if (unswitch_skip_ == &stmt) {
    if (unswitch_then_) {
      if (stmt.then_stmt) {
        emit_stmt(*stmt.then_stmt);
      }
    } else if (stmt.else_stmt) {
      emit_stmt(*stmt.else_stmt);
    }
    return;
  }
  const int cond = stmt.cond ? eval_expr(*stmt.cond) : zero_vreg_;
  const int then_bb = new_block();
  const int else_bb = stmt.else_stmt ? new_block() : -1;
  const int join = new_block();
  emit_branch(cond, then_bb, stmt.else_stmt ? else_bb : join);
  seal(then_bb);
  cur_ = then_bb;
  if (stmt.then_stmt) {
    emit_stmt(*stmt.then_stmt);
  }
  if (cur_ >= 0) {
    emit_jump(join);
  }
  if (stmt.else_stmt) {
    seal(else_bb);
    cur_ = else_bb;
    emit_stmt(*stmt.else_stmt);
    if (cur_ >= 0) {
      emit_jump(join);
    }
  }
  if (!blocks_[static_cast<std::size_t>(join)].preds.empty()) {
    seal(join);
    cur_ = join;
  } else {
    cur_ = -1;
  }
}

void FunctionCompiler::emit_while(const WhileStmt& stmt, bool allow_unswitch) {
  if (allow_unswitch) {
    if (const IfStmt* split = find_unswitch_if(stmt)) {
      const int cond = split->cond ? eval_expr(*split->cond) : zero_vreg_;
      const int then_bb = new_block();
      const int else_bb = new_block();
      const int join = new_block();
      emit_branch(cond, then_bb, else_bb);
      const IfStmt* saved = unswitch_skip_;
      const bool saved_side = unswitch_then_;
      auto emit_side = [&](int block, bool take_then) {
        seal(block);
        cur_ = block;
        unswitch_skip_ = split;
        unswitch_then_ = take_then;
        emit_while(stmt, false);
        unswitch_skip_ = saved;
        unswitch_then_ = saved_side;
        if (cur_ >= 0) {
          emit_jump(join);
        }
      };
      // 没有 else 时也要留下另一份循环，只是那个 if 变成空语句。
      // 否则 if (不变条件) break; 后面的语句会在条件为假时被整段丢掉。
      emit_side(then_bb, true);
      emit_side(else_bb, false);
      if (!blocks_[static_cast<std::size_t>(join)].preds.empty()) {
        seal(join);
        cur_ = join;
      } else {
        cur_ = -1;
      }
      return;
    }
  }
  const int header = new_block();
  const int body = new_block();
  const int exit_bb = new_block();
  if (cur_ >= 0) {
    emit_jump(header);
  }
  cur_ = header;
  const int cond = stmt.cond ? eval_expr(*stmt.cond) : zero_vreg_;
  emit_branch(cond, body, exit_bb);
  seal(body);
  cur_ = body;
  loops_.push_back({header, exit_bb});
  if (stmt.body) {
    emit_stmt(*stmt.body);
  }
  loops_.pop_back();
  if (cur_ >= 0) {
    emit_jump(header);
  }
  seal(header);
  if (!blocks_[static_cast<std::size_t>(exit_bb)].preds.empty()) {
    seal(exit_bb);
    cur_ = exit_bb;
  } else {
    cur_ = -1;
  }
}

int FunctionCompiler::emit_call(const CallExprNode& call, bool tail) {
  if (tail) {
    std::vector<int> args;
    for (const auto& arg : call.args) {
      args.push_back(arg ? eval_expr(*arg) : zero_vreg_);
    }
    for (int i = 0; i < static_cast<int>(param_vars_.size()) && i < static_cast<int>(args.size()); ++i) {
      write_variable(param_vars_[static_cast<std::size_t>(i)], cur_, args[static_cast<std::size_t>(i)]);
    }
    emit_jump(entry_bb_);
    return zero_vreg_;
  }
  const auto found = funcs_.find(call.name);
  if (found != funcs_.end() && inline_depth_ < 4 && inline_stack_.count(call.name) == 0 &&
      found->second->body && stmt_nodes(*found->second->body) <= 160) {
    return emit_inline(*found->second, call);
  }
  std::vector<int> args;
  for (const auto& arg : call.args) {
    args.push_back(arg ? eval_expr(*arg) : zero_vreg_);
  }
  Inst inst;
  inst.op = Op::Call;
  inst.name = call.name;
  inst.args = std::move(args);
  if (call.type == ValueKind::Void) {
    inst.dst = -1;
    emit_inst(inst);
    return zero_vreg_;
  }
  return emit_dst(inst);
}

int FunctionCompiler::emit_inline(const FuncDef& callee, const CallExprNode& call) {
  if (!callee.body) {
    return zero_vreg_;
  }
  std::vector<int> args;
  for (const auto& arg : call.args) {
    args.push_back(arg ? eval_expr(*arg) : zero_vreg_);
  }
  const int exit_bb = new_block();
  const int saved_exit = inline_exit_;
  const int saved_ret = inline_ret_var_;
  const int ret_var = new_var();
  inline_exit_ = exit_bb;
  inline_ret_var_ = ret_var;
  ++inline_depth_;
  inline_stack_.insert(callee.name);
  push_scope();
  for (int i = 0; i < static_cast<int>(callee.params.size()); ++i) {
    const int var = new_var();
    bind(callee.params[static_cast<std::size_t>(i)].name, var);
    const int value = i < static_cast<int>(args.size()) ? args[static_cast<std::size_t>(i)] : zero_vreg_;
    write_variable(var, cur_, value);
  }
  emit_stmts(callee.body->stmts);
  if (cur_ >= 0) {
    emit_jump(exit_bb);
  }
  pop_scope();
  inline_stack_.erase(callee.name);
  --inline_depth_;
  inline_exit_ = saved_exit;
  inline_ret_var_ = saved_ret;
  if (!blocks_[static_cast<std::size_t>(exit_bb)].preds.empty()) {
    seal(exit_bb);
    cur_ = exit_bb;
  } else {
    cur_ = -1;
    return zero_vreg_;
  }
  if (callee.returns_int) {
    return read_variable(ret_var, cur_);
  }
  return zero_vreg_;
}

int FunctionCompiler::eval_expr(const Expr& expr) {
  if (cur_ < 0) {
    return zero_vreg_;
  }
  if (expr.has_const) {
    return emit_li(expr.const_value);
  }
  switch (expr.kind) {
    case ExprKind::IntLit:
      return emit_li(literal_value(static_cast<const IntLitExpr&>(expr)));
    case ExprKind::Var: {
      const int var = lookup_var(static_cast<const VarExpr&>(expr).name);
      if (var < 0) {
        return emit_li(0);
      }
      return read_variable(var, cur_);
    }
    case ExprKind::Unary: {
      const auto& node = static_cast<const UnaryExprNode&>(expr);
      const int value = node.rhs ? eval_expr(*node.rhs) : zero_vreg_;
      if (node.op == TokenKind::Minus) {
        return emit_binop(Op::Neg, value, -1);
      }
      if (node.op == TokenKind::Not) {
        return emit_binop(Op::Seqz, value, -1);
      }
      return value;
    }
    case ExprKind::Binary: {
      const auto& node = static_cast<const BinaryExprNode&>(expr);
      if (!node.lhs || !node.rhs) {
        return emit_li(0);
      }
      if (node.op == TokenKind::AndAnd || node.op == TokenKind::OrOr) {
        const int result = new_var();
        const int lhs = eval_expr(*node.lhs);
        const int right_bb = new_block();
        const int skip_bb = new_block();
        const int join = new_block();
        if (node.op == TokenKind::AndAnd) {
          emit_branch(lhs, right_bb, skip_bb);
        } else {
          emit_branch(lhs, skip_bb, right_bb);
        }
        seal(skip_bb);
        cur_ = skip_bb;
        if (node.op == TokenKind::AndAnd) {
          write_variable(result, cur_, emit_li(0));
        } else {
          write_variable(result, cur_, emit_li(1));
        }
        emit_jump(join);
        seal(right_bb);
        cur_ = right_bb;
        const int rhs = eval_expr(*node.rhs);
        write_variable(result, cur_, emit_binop(Op::Snez, rhs, -1));
        emit_jump(join);
        seal(join);
        cur_ = join;
        return read_variable(result, cur_);
      }
      const int lhs = eval_expr(*node.lhs);
      const int rhs = eval_expr(*node.rhs);
      Op op = Op::Add;
      switch (node.op) {
        case TokenKind::Plus:
          op = Op::Add;
          break;
        case TokenKind::Minus:
          op = Op::Sub;
          break;
        case TokenKind::Star:
          op = Op::Mul;
          break;
        case TokenKind::Slash:
          op = Op::Div;
          break;
        case TokenKind::Percent:
          op = Op::Rem;
          break;
        case TokenKind::Lt:
          op = Op::Slt;
          break;
        case TokenKind::Gt:
          op = Op::Sgt;
          break;
        case TokenKind::Le:
          op = Op::Sle;
          break;
        case TokenKind::Ge:
          op = Op::Sge;
          break;
        case TokenKind::Eq:
          op = Op::Eq;
          break;
        case TokenKind::NotEq:
          op = Op::Ne;
          break;
        default:
          return emit_li(0);
      }
      return emit_binop(op, lhs, rhs);
    }
    case ExprKind::Call:
      return emit_call(static_cast<const CallExprNode&>(expr), false);
    default:
      return emit_li(0);
  }
}

}  // namespace

std::string emit_optimized_asm(const CompUnit& unit) {
  std::unordered_map<std::string, const FuncDef*> funcs;
  for (const auto& func : unit.funcs) {
    funcs[func->name] = func.get();
  }
  std::ostringstream out;
  for (const auto& func : unit.funcs) {
    FunctionCompiler compiler(*func, funcs);
    compiler.compile_to(out);
  }
  return out.str();
}

}  // namespace toyc
