#pragma once

#include "ast/ast.hpp"

#include <string>

namespace toyc {

// 由已经通过语义分析的语法树生成 RV64GC / LP64D 汇编。
// optimize 为假时沿用已经通过功能测试的朴素栈槽后端。
// optimize 为真时先建 SSA，再做常量传播、死代码删除、GVN、循环不变量外提、
// 归纳强度削减、简单内联和尾递归，最后做寄存器分配。
std::string emit_asm(const CompUnit& unit, bool optimize);

std::string emit_optimized_asm(const CompUnit& unit);

}  // namespace toyc
