#pragma once

#include "ast/ast.hpp"
#include "common/diagnostic.hpp"

#include <vector>

namespace toyc {

void analyze(CompUnit& unit, std::vector<Diagnostic>& errors);

}  // namespace toyc
