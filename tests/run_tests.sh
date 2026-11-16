#!/usr/bin/env bash
# 本地回归。通过只说明这些夹具与当前实现一致，不等于课程平台 OJ 通过。
set -u

cd "$(dirname "$0")/.."

if [[ ! -x ./compiler ]]; then
  echo "缺少 ./compiler，先在仓库根目录执行 make 或 cmake --build" >&2
  exit 1
fi

pass=0
fail=0

check_stdout() {
  local name="$1"
  local expected="$2"
  local actual="$3"
  local status="$4"
  if [[ "$status" -ne 0 ]]; then
    echo "FAIL $name exit=$status"
    fail=$((fail + 1))
    return
  fi
  if ! cmp -s "$expected" "$actual"; then
    echo "FAIL $name stdout mismatch"
    diff -u "$expected" "$actual" || true
    fail=$((fail + 1))
    return
  fi
  echo "PASS $name"
  pass=$((pass + 1))
}

shopt -s nullglob
for input in tests/cases/*.in; do
  name="$(basename "${input%.in}")"
  expected="${input%.in}.expected"
  actual="$(mktemp)"
  set +e
  ./compiler -token < "$input" > "$actual"
  status=$?
  set -e
  check_stdout "token:$name" "$expected" "$actual" "$status"
  rm -f "$actual"
done

for input in tests/ast/*.in; do
  name="$(basename "${input%.in}")"
  expected="${input%.in}.expected"
  actual="$(mktemp)"
  set +e
  ./compiler -ast < "$input" > "$actual"
  status=$?
  set -e
  check_stdout "ast:$name" "$expected" "$actual" "$status"
  rm -f "$actual"
done

actual="$(mktemp)"
set +e
echo 'int a = 1;' | ./compiler -token > "$actual"
status=$?
set -e
check_stdout "echo-stdin-example" tests/cases/stdin_example.expected "$actual" "$status"
rm -f "$actual"

actual="$(mktemp)"
err="$(mktemp)"
set +e
./compiler > "$actual" 2> "$err"
status=$?
set -e
if [[ "$status" -eq 0 || -s "$actual" ]]; then
  echo "FAIL missing -token should fail with empty stdout (exit=$status)"
  fail=$((fail + 1))
else
  echo "PASS cli-requires-token"
  pass=$((pass + 1))
fi
rm -f "$actual" "$err"

actual="$(mktemp)"
err="$(mktemp)"
set +e
echo 'int main(){return 0;}' | ./compiler -asm > "$actual" 2> "$err"
status=$?
set -e
if [[ "$status" -ne 0 ]] || ! grep -q '^\.globl main$' "$actual" || ! grep -q $'\tret$' "$actual"; then
  echo "FAIL -asm should emit RV assembly (exit=$status)"
  fail=$((fail + 1))
else
  echo "PASS cli-asm"
  pass=$((pass + 1))
fi
rm -f "$actual" "$err"

actual="$(mktemp)"
err="$(mktemp)"
set +e
echo 'int main(){return 1;}' | ./compiler -opt -asm > "$actual" 2> "$err"
status=$?
set -e
if [[ "$status" -ne 0 ]] || ! grep -Eq 'li[[:space:]]+[a-z0-9]+,[[:space:]]*1' "$actual"; then
  echo "FAIL -asm -opt should accept the optimization flag"
  fail=$((fail + 1))
else
  echo "PASS cli-asm-opt"
  pass=$((pass + 1))
fi
rm -f "$actual" "$err"

actual="$(mktemp)"
err="$(mktemp)"
set +e
echo 'int main(){return 0;}' | ./compiler -opt > "$actual" 2> "$err"
status=$?
set -e
if [[ "$status" -eq 0 || -s "$actual" ]]; then
  echo "FAIL -opt without -asm should fail"
  fail=$((fail + 1))
else
  echo "PASS cli-rejects-opt-alone"
  pass=$((pass + 1))
fi
rm -f "$actual" "$err"

actual="$(mktemp)"
set +e
echo 'int main() { int a; a = 1; return a; }' | ./compiler -ast > "$actual"
status=$?
set -e
printf 'accept\n' > "$err"
check_stdout "ast-echo-uninit" "$err" "$actual" "$status"
rm -f "$actual" "$err"

loc_bin="tests/lexer_locations_test"
g++ -std=c++17 -Wall -Wextra -Wpedantic -Isrc -o "$loc_bin" \
  tests/lexer_locations.cpp src/lexer/lexer.cpp src/lexer/token.cpp
set +e
"./$loc_bin"
status=$?
set -e
if [[ "$status" -ne 0 ]]; then
  echo "FAIL lexer-locations"
  fail=$((fail + 1))
else
  echo "PASS lexer-locations"
  pass=$((pass + 1))
fi

shape_bin="tests/ast_shape_test"
g++ -std=c++17 -Wall -Wextra -Wpedantic -Isrc -o "$shape_bin" \
  tests/ast_shape.cpp src/lexer/lexer.cpp src/lexer/token.cpp src/parser/parser.cpp src/sema/sema.cpp
set +e
"./$shape_bin"
status=$?
set -e
if [[ "$status" -ne 0 ]]; then
  echo "FAIL ast-shape"
  fail=$((fail + 1))
else
  echo "PASS ast-shape"
  pass=$((pass + 1))
fi

if bash tests/run_riscv.sh; then
  echo "PASS riscv-qemu"
  pass=$((pass + 1))
else
  echo "FAIL riscv-qemu"
  fail=$((fail + 1))
fi

echo "$pass passed, $fail failed"
if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
exit 0
