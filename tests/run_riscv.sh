#!/usr/bin/env bash
# 用课程规定的 RV64GC / LP64D 链接方式和 QEMU 跑编译器自己生成的汇编。
# -asm 与 -asm -opt 必须得到相同的标准输出和退出码。
set -u
cd "$(dirname "$0")/.."

CC=riscv64-linux-gnu-gcc
QEMU=qemu-riscv64-static
RT=tests/runtime/libtoyc.s
fail=0

run_case() {
  local name="$1"
  local src="$2"
  local input="$3"
  local expected="$4"
  local exp_exit="$5"
  local mode
  for mode in off on; do
    local asm elf out ec optflag
    asm="$(mktemp --suffix=.s)"
    elf="$(mktemp)"
    out="$(mktemp)"
    optflag=()
    if [[ "$mode" == on ]]; then
      optflag=(-opt)
    fi
    if ! ./compiler -asm "${optflag[@]}" < "$src" > "$asm"; then
      echo "FAIL $name compile opt=$mode"
      fail=1
      rm -f "$asm" "$elf" "$out"
      continue
    fi
    if ! "$CC" -march=rv64gc -mabi=lp64d -nostdlib -static -no-pie -Wl,-e,_start \
        "$asm" "$RT" -o "$elf" >/tmp/toyc-link.log 2>&1; then
      echo "FAIL $name link opt=$mode"
      cat /tmp/toyc-link.log
      fail=1
      rm -f "$asm" "$elf" "$out"
      continue
    fi
    set +e
    "$QEMU" "$elf" < "$input" > "$out"
    ec=$?
    set -e
    if [[ "$ec" -ne "$exp_exit" ]] || ! cmp -s "$out" <(printf '%s' "$expected"); then
      echo "FAIL $name opt=$mode exit=$ec expected_exit=$exp_exit"
      echo "stdout=[$(cat "$out")] expected=[$expected]"
      fail=1
    else
      echo "OK $name opt=$mode"
    fi
    rm -f "$asm" "$elf" "$out"
  done
}

tmp="$(mktemp -d)"

cat > "$tmp/ret.c" << 'EOF'
int main() { return 42; }
EOF
run_case ret "$tmp/ret.c" /dev/null "" 42

cat > "$tmp/put.c" << 'EOF'
int main() { putint(7); return 0; }
EOF
run_case put "$tmp/put.c" /dev/null "7" 0

cat > "$tmp/echo.c" << 'EOF'
int main() {
    int x = getint();
    putint(x + 1);
    return 0;
}
EOF
printf '41\n' > "$tmp/echo.in"
run_case echo "$tmp/echo.c" "$tmp/echo.in" "42" 0

cat > "$tmp/div.c" << 'EOF'
int main() {
    int a = -5;
    putint(a / 2);
    putint(a % 2);
    return 0;
}
EOF
run_case div "$tmp/div.c" /dev/null "-2-1" 0

cat > "$tmp/cmp.c" << 'EOF'
int main() {
    int a = -1;
    if (a < 0 && a != 0 || a > 1) return 1;
    return 0;
}
EOF
run_case cmp "$tmp/cmp.c" /dev/null "" 1

cat > "$tmp/fact.c" << 'EOF'
int fact(int n) {
    if (n <= 1) return 1;
    return n * fact(n - 1);
}
int main() { return fact(5); }
EOF
run_case fact "$tmp/fact.c" /dev/null "" 120

cat > "$tmp/args.c" << 'EOF'
int pick(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) {
    return a + j;
}
int main() { return pick(1, 2, 3, 4, 5, 6, 7, 8, 9, 10); }
EOF
run_case args "$tmp/args.c" /dev/null "" 11

cat > "$tmp/short.c" << 'EOF'
int side() { putint(1); return 1; }
int main() {
    if (0 && side()) putint(2);
    if (1 || side()) putint(3);
    return 0;
}
EOF
run_case short "$tmp/short.c" /dev/null "3" 0

cat > "$tmp/loop.c" << 'EOF'
int main() {
    int i = 0;
    int s = 0;
    while (i < 3) {
        int j = 0;
        i = i + 1;
        while (j < 3) {
            j = j + 1;
            if (j == 2) break;
            s = s + 1;
        }
        if (i == 2) continue;
        s = s + 10;
    }
    return s;
}
EOF
run_case loop "$tmp/loop.c" /dev/null "" 23

cat > "$tmp/wrap.c" << 'EOF'
int main() {
    int a = 2147483647;
    putint(a + 1);
    return 0;
}
EOF
run_case wrap "$tmp/wrap.c" /dev/null "-2147483648" 0

cat > "$tmp/sample.c" << 'EOF'
int sum(int n, int from) {
    int s = 0;
    while (from <= n) {
        if (from == 2) {
            from = from + 1;
            continue;
        }
        s = s + from;
        from = from + 1;
    }
    return s;
}
void show(int v) {
    putint(v);
    ;
}
int main() {
    const int LIMIT = 3;
    const int STEP = 1;
    int a = 5;
    int b;
    b = +a - -1;
    int c = (a + b) * 2 / 3 % 4;
    {
        int a = 1;
        c = c + a;
    }
    if (a >= b && b != 0 || !(a == LIMIT)) {
        c = c + sum(a, STEP);
    } else {
        c = c - 1;
    }
    while (c > 0) {
        c = c - 1;
        if (c == 5) continue;
        if (c < 2) break;
        show(c);
    }
    return c;
}
EOF
# (5+6)*2/3%4 = 7%4 = 3，加内层 a 后为 4，再加 sum(5,1)=13，循环从 17 递减。
# putint 不换行，依次输出 16..6、4、3、2，最后 c 为 1。
run_case sample "$tmp/sample.c" /dev/null "161514131211109876432" 1

rm -rf "$tmp"
if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
exit 0
