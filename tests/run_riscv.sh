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

cat > "$tmp/iv.c" << 'EOF'
int main() {
    int i = 0;
    int s = 0;
    while (i < 10) {
        s = s + i * 3;
        i = i + 1;
    }
    return s;
}
EOF
run_case iv "$tmp/iv.c" /dev/null "" 135

cat > "$tmp/licm.c" << 'EOF'
int main() {
    int n = 20;
    int a = 6;
    int b = 7;
    int i = 0;
    int s = 0;
    while (i < n) {
        s = s + a * b;
        i = i + 1;
    }
    putint(s);
    return 0;
}
EOF
run_case licm "$tmp/licm.c" /dev/null "840" 0

cat > "$tmp/dead.c" << 'EOF'
int main() {
    int n = 50;
    int x = 3;
    int y = 5;
    int i = 0;
    int s = 0;
    while (i < n) {
        int d1 = i * x + y;
        int d2 = d1 * d1;
        s = s + x;
        i = i + 1;
    }
    putint(s);
    return 0;
}
EOF
run_case dead "$tmp/dead.c" /dev/null "150" 0

cat > "$tmp/tail.c" << 'EOF'
int down(int n, int acc) {
    if (n <= 0) return acc;
    return down(n - 1, acc + 1);
}
int main() {
    putint(down(20, 0));
    return 0;
}
EOF
run_case tail "$tmp/tail.c" /dev/null "20" 0

cat > "$tmp/inl.c" << 'EOF'
int inc(int x) { return x + 1; }
int main() {
    int i = 0;
    int s = 0;
    while (i < 5) {
        s = inc(s);
        i = i + 1;
    }
    return s;
}
EOF
run_case inl "$tmp/inl.c" /dev/null "" 5

cat > "$tmp/order.c" << 'EOF'
int a() { putint(1); return 1; }
int b() { putint(2); return 2; }
int add(int x, int y) { return x + y; }
int main() { putint(add(a(), b())); return 0; }
EOF
run_case order "$tmp/order.c" /dev/null "123" 0

cat > "$tmp/short2.c" << 'EOF'
int side() { putint(9); return 1; }
int main() {
    if (0 && side()) putint(8);
    if (1 || side()) putint(4);
    return 0;
}
EOF
run_case short2 "$tmp/short2.c" /dev/null "4" 0

cat > "$tmp/unsw.c" << 'EOF'
int main() {
    int flag = 0;
    int i = 0;
    while (i < 3) {
        if (flag) putint(1);
        else putint(2);
        i = i + 1;
    }
    return 0;
}
EOF
run_case unsw "$tmp/unsw.c" /dev/null "222" 0

cat > "$tmp/many.c" << 'EOF'
int main() {
    int a1=1; int a2=2; int a3=3; int a4=4; int a5=5;
    int a6=6; int a7=7; int a8=8; int a9=9; int a10=10;
    int b1=11; int b2=12; int b3=13; int b4=14; int b5=15;
    int b6=16; int b7=17; int b8=18; int b9=19; int b10=20;
    int c1=21; int c2=22; int c3=23; int c4=24; int c5=25;
    int c6=26; int c7=27; int c8=28; int c9=29; int c10=30;
    return a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+b1+b2+b3+b4+b5+b6+b7+b8+b9+b10+c1+c2+c3+c4+c5+c6+c7+c8+c9+c10;
}
EOF
run_case many "$tmp/many.c" /dev/null "" 209

# break 在循环里时，后面的赋值必须还能带到循环出口。
cat > "$tmp/breakloop.c" << 'EOF'
int main() {
    int n = getint();
    int a = getint();
    int guard = 0;
    while (guard < 3) {
        if (!a) break;
        n = -3 + a;
        guard = guard + 1;
    }
    putint(n);
    return 0;
}
EOF
printf '3 1\n' > "$tmp/breakloop.in"
run_case breakloop "$tmp/breakloop.c" "$tmp/breakloop.in" "-2" 0

# 循环条件和分支里的短路不能让编译器崩溃，并且要保留副作用。
cat > "$tmp/shortloop.c" << 'EOF'
int main() {
    int n = getint();
    int a = getint();
    int b = getint();
    int i = 0;
    int s = 0;
    while (i < n && (a > 0 || b < i)) {
        if (i < a && b > 0 || s < i && a > 0) {
            s = s + i + b;
        } else {
            s = s + a;
        }
        i = i + 1;
    }
    putint(s);
    return a;
}
EOF
printf '8 3 4\n' > "$tmp/shortloop.in"
run_case shortloop "$tmp/shortloop.c" "$tmp/shortloop.in" "30" 3
printf '8 0 4\n' > "$tmp/shortzero.in"
run_case shortzero "$tmp/shortloop.c" "$tmp/shortzero.in" "0" 0

# 循环内不变的 if 走两个方向，结果都要跟着迭代次数变。
cat > "$tmp/unsw2.c" << 'EOF'
int main() {
    int n = getint();
    int a = getint();
    int b = getint();
    int i = 0;
    int s = 0;
    while (i < n) {
        if (a < b) {
            s = s + i;
        } else {
            s = s + 1;
        }
        i = i + 1;
    }
    putint(s);
    return s;
}
EOF
printf '6 1 4\n' > "$tmp/unsw2a.in"
run_case unswa "$tmp/unsw2.c" "$tmp/unsw2a.in" "15" 15
printf '6 9 2\n' > "$tmp/unsw2b.in"
run_case unswb "$tmp/unsw2.c" "$tmp/unsw2b.in" "6" 6

# 步长为 1、初值非负、循环里只做累加时，-opt 可以收成乘法。结果必须和 -asm 一样。
cat > "$tmp/closed.c" << 'EOF'
int main() {
    int n = getint();
    int i = 0;
    int s = 0;
    while (i < n) {
        int d = i * i;
        s = s + 3;
        i = i + 1;
    }
    putint(s);
    return i;
}
EOF
printf '5\n' > "$tmp/closed5.in"
run_case closed5 "$tmp/closed.c" "$tmp/closed5.in" "15" 5
printf '0\n' > "$tmp/closed0.in"
run_case closed0 "$tmp/closed.c" "$tmp/closed0.in" "0" 0
cat > "$tmp/closedneg.c" << 'EOF'
int main() {
    int n = -4;
    int i = 0;
    int s = 7;
    while (i < n) {
        s = s + 3;
        i = i + 1;
    }
    putint(s);
    return i;
}
EOF
run_case closedneg "$tmp/closedneg.c" /dev/null "7" 0

rm -rf "$tmp"
if ! python3 tests/fuzz_opt.py ./compiler 11 30; then
  echo "FAIL opt-fuzz"
  fail=1
else
  echo "OK opt-fuzz"
fi
if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
exit 0
