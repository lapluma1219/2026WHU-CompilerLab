#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "sema/sema.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL " << message << '\n';
    ++failures;
  }
}

std::unique_ptr<toyc::CompUnit> parse_unit(const std::string& source,
                                           std::vector<toyc::Diagnostic>& errors) {
  toyc::Lexer lexer(source);
  const toyc::LexResult lex = lexer.tokenize();
  errors = lex.diagnostics;
  toyc::Parser parser(lex.tokens, errors);
  return parser.parse();
}

const toyc::AssignStmt* first_assign(const toyc::CompUnit& unit) {
  for (const auto& func : unit.funcs) {
    if (!func->body) {
      continue;
    }
    for (const auto& stmt : func->body->stmts) {
      if (stmt->kind == toyc::StmtKind::Assign) {
        return static_cast<const toyc::AssignStmt*>(stmt.get());
      }
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  {
    std::vector<toyc::Diagnostic> errors;
    auto unit = parse_unit("int main(){ int a; a = 1+2*3; return 0; }", errors);
    expect(errors.empty(), "precedence program has no syntax error");
    const toyc::AssignStmt* assign = first_assign(*unit);
    expect(assign != nullptr && assign->expr && assign->expr->kind == toyc::ExprKind::Binary,
           "a = 1+2*3 is binary");
    const auto* add = static_cast<const toyc::BinaryExprNode*>(assign->expr.get());
    expect(add->op == toyc::TokenKind::Plus, "outer op is +");
    expect(add->rhs && add->rhs->kind == toyc::ExprKind::Binary, "right of + is *");
    const auto* mul = static_cast<const toyc::BinaryExprNode*>(add->rhs.get());
    expect(mul->op == toyc::TokenKind::Star, "inner op is *");
  }
  {
    std::vector<toyc::Diagnostic> errors;
    auto unit = parse_unit("int main(){ int a; a = a- -1; return 0; }", errors);
    expect(errors.empty(), "unary minus program has no syntax error");
    const auto* assign = first_assign(*unit);
    const auto* sub = static_cast<const toyc::BinaryExprNode*>(assign->expr.get());
    expect(sub->op == toyc::TokenKind::Minus, "binary minus");
    expect(sub->rhs && sub->rhs->kind == toyc::ExprKind::Unary, "right is unary minus");
    const auto* neg = static_cast<const toyc::UnaryExprNode*>(sub->rhs.get());
    expect(neg->op == toyc::TokenKind::Minus && neg->rhs &&
               neg->rhs->kind == toyc::ExprKind::IntLit,
           "unary minus of 1");
  }
  {
    std::vector<toyc::Diagnostic> errors;
    auto unit = parse_unit("int main(){ return a||b&&c; }", errors);
    const auto& ret = static_cast<const toyc::ReturnStmt&>(*unit->funcs[0]->body->stmts[0]);
    const auto* lor = static_cast<const toyc::BinaryExprNode*>(ret.expr.get());
    expect(lor->op == toyc::TokenKind::OrOr, "|| is outer");
    expect(lor->rhs && lor->rhs->kind == toyc::ExprKind::Binary, "&& is inner");
    expect(static_cast<const toyc::BinaryExprNode*>(lor->rhs.get())->op == toyc::TokenKind::AndAnd,
           "&& binds tighter than ||");
  }
  {
    std::vector<toyc::Diagnostic> errors;
    auto unit = parse_unit("int main(){ if (1) if (0) return 1; else return 0; }", errors);
    const auto* outer = static_cast<const toyc::IfStmt*>(unit->funcs[0]->body->stmts[0].get());
    expect(outer->else_stmt == nullptr, "outer if has no else");
    expect(outer->then_stmt && outer->then_stmt->kind == toyc::StmtKind::If, "inner if");
    const auto* inner = static_cast<const toyc::IfStmt*>(outer->then_stmt.get());
    expect(inner->else_stmt != nullptr, "else binds to nearest if");
  }
  {
    std::vector<toyc::Diagnostic> errors;
    auto unit = parse_unit("int main(){ int a; a = (1+2)*3; return a; }", errors);
    toyc::analyze(*unit, errors);
    expect(errors.empty(), "parenthesized expression is semantically valid");
    const auto* assign = first_assign(*unit);
    expect(assign->expr->kind == toyc::ExprKind::Binary &&
               static_cast<const toyc::BinaryExprNode*>(assign->expr.get())->op ==
                   toyc::TokenKind::Star,
           "(1+2)*3 multiplies outside");
  }
  if (failures != 0) {
    std::cerr << failures << " ast shape checks failed\n";
    return 1;
  }
  std::cout << "ast shape checks passed\n";
  return 0;
}
