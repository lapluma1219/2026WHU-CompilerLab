# 2026WHU-CompilerLab

武汉大学编译原理课程的 ToyC 编译器。`master` 实现前两阶段：手写词法分析、递归下降语法分析、抽象语法树和语义分析。代码生成在 `final`。

源文件从标准输入读入，结果写到标准输出。`accept` 和 `reject` 都返回 0。无法识别的参数写到标准错误，标准输出为空，返回 2。

## 当前能力

| 模式 | 输出 |
| --- | --- |
| `-token` | 词法结果。没有诊断时只有 token 流。有诊断时先写 `reject` 和 0-based 行号，空一行，再写恢复后的 token。 |
| `-ast` | 语法树建在内存中，没有语法错误时接着做语义检查。没有诊断时输出 `accept`，否则输出 `reject` 和 0-based 行号。标准输出不含语法树。 |

一次运行指定 `-token` 或 `-ast`。`-asm` 和 `-opt` 在 `final`。

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
| `src/lexer/` | 手写扫描、关键字、token 文本 |
| `src/parser/` | 递归下降、表达式层次、语法恢复 |
| `src/ast/ast.hpp` | 编译单元、函数、语句和表达式 |
| `src/sema/` | 符号表、类型、常量折叠、返回路径 |
| `src/common/diagnostic.hpp` | 0-based 行号、列号和说明 |
| `tests/cases/` | `-token` 输入与期望输出 |
| `tests/ast/` | `-ast` 输入与期望输出 |
| `tests/lexer_locations.cpp` | 行号和列号 |
| `tests/ast_shape.cpp` | 优先级、一元负号、`else` 归属 |
| `tests/run_tests.sh` | 回归脚本 |

## 构建

在仓库根目录执行，使用 C++17。

```bash
make
```

```bash
cmake -S . -B build
cmake --build build
```

两种方式都得到根目录的 `./compiler`。未指定编译器时，CMake 使用 `g++`。

## 用法

每个 token 一行：`<序号>:<类型>:<内容>`。序号从 0 连续编号。关键字、运算符和标点的类型是单引号中的字面量，标识符是 `Ident`，整数是 `IntConst`。内容是双引号中的原文。行号从 0 起，写在说明前面，中间一个空格。

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

`-ast` 的多条诊断按行号、列号排在同一次 `reject` 里。

## 分析规则

### 词法

- 标识符为 `[_A-Za-z][_A-Za-z0-9]*`。关键字全文匹配并区分大小写：`int`、`void`、`const`、`if`、`else`、`while`、`break`、`continue`、`return`。`getint` 和 `putint` 是标识符。
- 整数取最长连续十进制数字，文本保持原文。`-` 单独作为运算符。
- `==`、`!=`、`<=`、`>=`、`&&`、`||` 取最长匹配。单独的 `&` 和 `|` 是非法字符。
- 空白是空格、制表符和换行。`\r\n` 与单独的 `\r` 记为一个换行。
- `//` 注释到该行换行前，`/*` 注释到最近的 `*/`，不嵌套。`/*/` 不闭合。
- 非法字符不进入 token 流，扫描从下一个字符继续。

### 表达式

从低到高。同一层左结合，一元运算符右结合：

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

### 名字

- 全局只有函数定义。预置 `int getint()` 和 `void putint(int)`。
- 调用写在被调函数定义之后。函数可以调用自己。`main` 是无参数的 `int main()`。
- 形参与函数体最外层共用一个作用域。嵌套的 `{}` 新开一层，内层名称可以遮蔽外层。同一层重复定义会拒绝。

### 语义

- `int` 可以没有初值。`const int` 必须有初值，之后不能再赋值。不检查普通变量在读取前是否已经赋值。
- `break` 和 `continue` 只出现在循环里。
- `int` 函数的每条路径都要返回一个值。`while` 的条件折成非零、循环体返回且没有 `break` 时，这条路径已经返回。
- `void` 函数可以没有 `return`。带值的 `return` 会拒绝。`void` 调用可以单独作为语句。
- 除数或 `%` 的右操作数是常量 0 时拒绝。非常量除数不拒绝。
- `&&` 和 `||` 的左侧已经折成能够短路的常量时，不分析右侧。折叠来自字面量和可折叠的运算，变量引用不折叠。
- 存在语法错误时不做语义分析。

## 测试

```bash
make test
```

`make test` 生成 `./compiler`，比较 `tests/cases` 中 32 个 `-token` 样例和 `tests/ast` 中 63 个 `-ast` 样例，并检查标准输入、缺少模式、`-asm`、行号和语法树形状。结果为 `101 passed, 0 failed`。

课程平台对 [`36ebafb18432ed18e1fea60cf0b431333df6af7c`](https://github.com/lapluma1219/2026WHU-CompilerLab/commit/36ebafb18432ed18e1fea60cf0b431333df6af7c) 的成绩是词法 100/100（30/30），语法与语义 100/100（25/25）。

## 仓库

- GitHub：<https://github.com/lapluma1219/2026WHU-CompilerLab>
- Gitee：<https://gitee.com/lapluma1219/2026-whu-compiler-lab>
- `master` 是前两阶段。
- [`final`](https://github.com/lapluma1219/2026WHU-CompilerLab/commit/9e262fdcb5b42aaa717f696c4f8a78bbc6bd8e32) 在此之上实现 RISC-V 代码生成和优化，使用 `-asm` 与 `-asm -opt`。中期答辩后合入 `master`。
