#pragma once

#include "ast/ast.hpp"

#include <string>

namespace toyc {

// 由已经通过语义分析的语法树生成 RV64GC / LP64D 汇编。
// optimize 为真时做常量折叠、强度削减，以及 return 之后的直线死代码删除。
std::string emit_asm(const CompUnit& unit, bool optimize);

}  // namespace toyc
