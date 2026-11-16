#pragma once

#include <string>

namespace toyc {

// 行号、列号都是 0-based。词法诊断对外只打印行号。
struct Diagnostic {
  int line = 0;
  int column = 0;
  std::string message;
};

}  // namespace toyc
