# 第 1 阶段测试记录

记录的是本仓库在 Linux 上实际跑过的命令。本地全部通过不等于课程平台 OJ 通过。

## 平台第一次评测

分支 `cursor/stage1-lexer-6b19`、提交 `f967b06e16ef1fd50d8186a8285774d4d0aa60e5`。平台 96.67/100，29/30 通过。唯一失败是 `l04_numbers`，结论为“错误输出”，3ms，没有输入、期望输出或构建日志。`l11_unary`、`l18_operator_adjacency`、`l10_mixed` 以及全部非法字符点都通过。

那一版把 `012` 切成 `IntConst:"0"` 和 `IntConst:"12"`，负号则按样例拆开。第二次修正只改连续数字：`012` 成为一个 `IntConst`。负号、运算符和非法字符的输出保持不变。

## 环境

- 系统：Linux
- 编译器：g++ 13.3.0，选项 `-std=c++17 -Wall -Wextra -Wpedantic -O2`
- 构建没有出现警告
- 默认的 `/usr/bin/c++` 是 Clang 18.1.3，链接阶段找不到 `-lstdc++`。`CMakeLists.txt` 因此在未指定编译器时选择 `g++`。Makefile 本来就调用 `g++`

## 命令和结果

```bash
make test
```

阶段 2 第一次平台结果是 92/100，`s04_unclosed_block` 和 `s05_while_without_condition` 为错误输出。修正错误恢复后重新执行 `make test`。词法夹具、示例一、示例二和原有语义夹具仍通过；新增未闭合块（有无末尾换行、嵌套、内层先闭合）和 `while` 缺条件（空括号、缺括号、缺右括号、后面还有别的错误）。具体行号规则见 `docs/stage2.md`。`s04_unclosed_block` 已在平台通过。针对 `s05_while_without_condition` 的回归之后，`make test` 曾为 `101 passed, 0 failed`。

第三阶段加入 `-asm` / `-opt` 后，`tests/run_riscv.sh` 用 `riscv64-linux-gnu-gcc -march=rv64gc -mabi=lp64d -nostdlib -static` 链接 `tests/runtime/libtoyc.s`，再用 `qemu-riscv64-static` 比较 `-asm` 与 `-asm -opt` 的标准输出和退出码。覆盖返回值、`putint`/`getint`、负数除模、比较短路、递归、10 个参数、嵌套 `break`/`continue`、32 位溢出和综合样例。本次完整 `make test`：`104 passed, 0 failed`。CMake 对同一批源码构建成功。

阶段 1 数字修正当时的 `make test` 是 `36 passed, 0 failed`。其中 32 个是 `tests/cases/*.in` 的 stdout 字节比较，另外 4 项是：

- `echo "int a = 1;" | ./compiler -token` 与 `stdin_example` 的期望一致
- 不带 `-token` 时返回非 0，stdout 为空
- `-ast` 返回非 0，stdout 为空
- `tests/lexer_locations.cpp` 的行号、列号和“注释中的 `$` 不报错”检查通过

CMake 使用另一组编译选项（默认没有 Makefile 的 `-O2`），所以两份二进制不必字节相同。行为用下面两条命令核对：

```bash
cmake -S . -B build
cmake --build build
echo "int a = 1;" | ./compiler -token
echo "b = +a - -1;" | ./build/compiler -token
```

`int a = 1;` 的实际输出：

```text
0:'int':"int"
1:Ident:"a"
2:'=':"="
3:IntConst:"1"
4:';':";"
```

`b = +a - -1;` 的实际输出：

```text
0:Ident:"b"
1:'=':"="
2:'+':"+"
3:Ident:"a"
4:'-':"-"
5:'-':"-"
6:IntConst:"1"
7:';':";"
```

这和综合样例 token 96–100 是同一条负号规则。

## 夹具对应的规则

| 夹具 | 锁定的行为 |
| --- | --- |
| `official_sample` | 页面综合样例，208 个 token，序号 0–207，含 `+a - -1` |
| `illegal_sample` | `$` `#` `?` 在 0-based 行 3、4、5；空行后恢复，`2` `3` `4` 仍在 |
| `comment_hides_illegal` | 页面 7.3，注释里的非法字符没有 `reject` |
| `number_minus_follows_sample` | `-1`、`1-2`、`--1`、`- 1`、`(-1)`、`+1`、`-0` 仍拆开负号；`-012` 是 `'-'` 加 `IntConst:"012"` |
| `numbers_and_idents_boundary` | `01`、`012`、`00`、`0001` 各为一个整数；`123abc`、`0x10`、`9_` 仍在数字和标识符边界断开 |
| `number_digit_run` | `0`、`007`、`08`、`2147483647`、`2147483648`、30 位数字、`1+2`、`1-2`、`+12`、`-12` |
| `keyword_boundary` | `int` 与 `integer`、`int0`、`Int`、`returnValue` |
| `operators_longest` | `<=` `>=` `==` `!=` `&&` `||` |
| `operator_boundaries` | `+++`、`===`、`<<=`、`>>=`、`&&&`、`\|\|\|`、单独 `&` `\|` |
| `comment_line_numbers` | 多行注释跨行后，注释外的非法字符行号仍是 0-based |
| `same_line_illegal_recover` | 同一行多个非法字符，后面的 token 都保留 |
| `empty`、`only_newline`、`only_whitespace` | 无 token 时 stdout 为空 |
| `eof_no_newline`、`line_comment_eof` | 最后没有换行 |
| `crlf_lines`、`cr_only_lines` | 每种换行只增加一个行号 |
| `unterminated_comment`、`unterminated_slash_star_slash` | 未闭合注释；`/*/` 不提前闭合。此项待平台核实 |
| `block_comment_closes_on_real_ender`、`division_vs_comment` | `*/`、`/**/`、除号和注释的边界 |
| `non_ascii`、`non_ascii_in_comment` | 注释外的 UTF-8 字符报告一次，注释内不报告 |
| `other_illegal_chars`、`controls_and_null`、`invalid_utf8` | 引号、反斜杠、`\v`、`\f`、NUL、孤立字节 `\xff` |
| `reject_only` | 只有非法字符时仍有诊断后的空行 |
| `putint_call`、`stdin_example` | 库函数名是标识符；补充格式中的 echo 示例 |

比较使用 `cmp`，包含最后的换行，不只看肉眼看起来像不像。
