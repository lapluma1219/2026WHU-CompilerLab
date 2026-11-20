# 2026WHU-CompilerLab

武汉大学编译原理课程的 ToyC 编译器。`master` 完成前两阶段：手写词法分析器、递归下降语法分析、内存中的抽象语法树，以及语法通过之后的语义分析。第三阶段的目标代码在 `final`，中期答辩后再合入 `master`。

程序从标准输入读入整份源文件，评测看标准输出。词法、语法和语义的 `accept` 与 `reject` 都返回 0。命令行无法识别时，说明写到标准错误，标准输出为空，返回 2。

## 当前能力

| 模式 | 结果 |
| --- | --- |
| `-token` | 词法分析。没有诊断时只输出 token 流。有诊断时先输出 `reject` 和 0-based 行号，空一行，再输出恢复后的 token。 |
| `-ast` | 建立语法树；没有语法错误时再做语义检查。没有诊断时输出 `accept`。有诊断时输出 `reject` 和 0-based 行号。语法树留在内存里，标准输出不含树。 |

`master` 上的 `-asm` 和 `-opt` 尚未实现。一次运行只接受 `-token` 或 `-ast` 其中一个。

## 结构

```text
标准输入
  └─ Lexer::tokenize
       ├─ -token → 打印 token
       └─ Parser::parse → CompUnit
            └─ 没有语法错误 → analyze
                 └─ -ast → accept 或 reject
```

| 路径 | 职责 |
| --- | --- |
| `src/main.cpp` | 读入源文件，选择模式，合并诊断并写出标准输出 |
| `src/lexer/` | 手写扫描、关键字识别、token 文本 |
| `src/parser/` | 递归下降、表达式层次、语法恢复 |
| `src/ast/ast.hpp` | 编译单元、函数、语句和表达式 |
| `src/sema/` | 符号表、类型、常量折叠、返回路径 |
| `src/common/diagnostic.hpp` | 0-based 行号、列号和说明 |
| `tests/cases/` | `-token` 的输入与期望输出 |
| `tests/ast/` | `-ast` 的输入与期望输出 |
| `tests/lexer_locations.cpp` | 行号和列号 |
| `tests/ast_shape.cpp` | 优先级、一元负号、`else` 归属 |
| `tests/run_tests.sh` | 回归脚本 |

## 构建

在仓库根目录执行。语言标准是 C++17。本次核对使用 Linux 上的 g++ 13.3.0 和 CMake 3.28.3，两种构建都没有警告。

```bash
make
```

`Makefile` 使用 `-std=c++17 -Wall -Wextra -Wpedantic -O2`，生成根目录的 `./compiler`。

```bash
cmake -S . -B build
cmake --build build
```

未指定编译器时，`CMakeLists.txt` 选择 `g++`。`build/compiler` 会再复制到仓库根目录。CMake 不追加 `-O2`，二进制不必与 `make` 的产物相同。本次用两份程序核对了下面的 `-token` 和 `-ast` 样例，标准输出一致。

## 用法

每个 token 占一行：`<序号>:<类型>:<内容>`。序号从 0 连续编号。关键字、运算符和标点的类型是单引号中的字面量，标识符是 `Ident`，整数是 `IntConst`。内容是双引号中的原文。行号从 0 起，写在说明前面，中间一个空格。

```bash
echo "int a = 1;" | ./compiler -token
```

```text
0:'int':"int"
1:Ident:"a"
2:'=':"="
3:IntConst:"1"
4:';':";"
```

```bash
printf 'int main(){\nreturn $1;\n}\n' | ./compiler -token
```

```text
reject
1 illegal character '$'

0:'int':"int"
1:Ident:"main"
2:'(':"("
3:')':")"
4:'{':"{"
5:'return':"return"
6:IntConst:"1"
7:';':";"
8:'}':"}"
```

```bash
echo "int main() { int a; a = 1; return a; }" | ./compiler -ast
```

```text
accept
```

```bash
echo "int main(){ return 1/0; }" | ./compiler -ast
```

```text
reject
0 division by zero
```

```bash
echo 'int main() { return 0;' | ./compiler -ast
```

```text
reject
0 Lack of '}'
```

`-ast` 的多条诊断按行号、列号排在同一次 `reject` 中，诊断和结果之间没有 `-token` 那种空行。

## 分析规则

标识符是 `[_A-Za-z][_A-Za-z0-9]*`。下列单词全文匹配、区分大小写时是关键字：`int`、`void`、`const`、`if`、`else`、`while`、`break`、`continue`、`return`。`getint` 和 `putint` 是标识符。

整数取最长连续十进制数字，文本保持原文。`-` 始终单独作为运算符。双字符运算符只在第二个字符能够组成 `==`、`!=`、`<=`、`>=`、`&&`、`||` 时取长匹配。单独的 `&` 和 `|` 是非法字符。空白是空格、制表符和换行；`\r\n` 与单独的 `\r` 记为一个换行。`//` 注释到该行换行前，`/*` 注释到最近的 `*/`，不嵌套；`/*/` 不闭合。非法字符不进入 token 流，扫描从它后面继续。

表达式从低到高如下。同一层左结合，一元运算符右结合：

```text
||
&&
<  >  <=  >=  ==  !=
+  -
*  /  %
一元 +  -  !
整数字面量、变量、函数调用、括号
```

赋值是语句 `标识符 = 表达式 ;`。`else` 属于最近的 `if`。

全局只有函数定义。语义分析预置 `int getint()` 和 `void putint(int)`。调用出现在被调函数定义之后；函数可以调用自己。`main` 是无参数的 `int main()`。形参和函数体最外层共用一个作用域，嵌套的 `{}` 再开一层，内层名称可以遮蔽外层。同一层重复定义会拒绝。

`int` 声明可以没有初值。`const int` 必须有初值，之后不能再赋值。实现不判断普通变量在读取前是否已经赋过值。`break` 和 `continue` 只出现在循环里。`int` 函数的每条路径都要返回一个值；`while` 的条件能折成非零、循环体一定会返回且没有 `break` 时，这条路径视为已经返回。`void` 函数可以没有 `return`，带值的 `return` 会拒绝。`void` 调用可以单独作为语句。

除数或 `%` 的右操作数是常量 0 时拒绝，非常量除数接受。`&&` 和 `||` 的左侧已经折成能够短路的常量时，不分析右侧。参与折叠的常量来自字面量和可折叠的运算；变量引用不折叠。已经出现语法错误时不做语义分析。

## 测试

```bash
make test
```

`make test` 先生成 `./compiler`，再用 `cmp` 比较 `tests/cases` 中 32 个 `-token` 样例和 `tests/ast` 中 63 个 `-ast` 样例，并检查标准输入示例、缺少模式、`-asm`、行号程序和语法树形状程序。

本次执行结果：`101 passed, 0 failed`。环境是 g++ 13.3.0，选项与 `Makefile` 相同。这是本地夹具和当前程序的对照。这次提交只改 README，没有新的课程平台评测。

## 仓库

- 主仓库：<https://github.com/lapluma1219/2026WHU-CompilerLab>
- 课程评测使用 Gitee 上的副本，由维护者从主仓库同步。Gitee 地址和访问令牌不在本仓库。
- `master` 是前两阶段。课程平台对 [`36ebafb18432ed18e1fea60cf0b431333df6af7c`](https://github.com/lapluma1219/2026WHU-CompilerLab/commit/36ebafb18432ed18e1fea60cf0b431333df6af7c) 的成绩是词法 100/100（30/30），语法与语义 100/100（25/25）。
- [`final`](https://github.com/lapluma1219/2026WHU-CompilerLab/tree/final) 的 `9e262fdcb5b42aaa717f696c4f8a78bbc6bd8e32` 在前两阶段之上包含第三阶段的 RISC-V 代码生成和优化，提供 `-asm` 与 `-asm -opt`。中期答辩后再合入 `master`。
- 默认分支是 `master`。`main` 只有初始提交。
