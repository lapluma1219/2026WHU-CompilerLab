#!/usr/bin/env bash
# 本地微基准：同一段 ToyC 分别用 -asm 和 -asm -opt 生成，再和等价的 gcc -O2 对照。
# 这不是平台隐藏用例，只用来看优化前后的运行时间。
set -u
cd "$(dirname "$0")/.."
CC=riscv64-linux-gnu-gcc
QEMU=qemu-riscv64-static
RT=tests/runtime/libtoyc.s
N="${1:-5000000}"

cat > /tmp/bench_dead.c << EOF
int main() {
    int n = $N;
    int x = 3;
    int y = 5;
    int i = 0;
    int s = 0;
    while (i < n) {
        int d1 = i * x + y;
        int d2 = d1 * d1;
        int d3 = d2 + i;
        s = s + x;
        i = i + 1;
    }
    putint(s);
    return 0;
}
EOF

cat > /tmp/bench_dead_gcc.c << EOF
#include <stdio.h>
int main() {
    int n = $N;
    int x = 3;
    int y = 5;
    int i = 0;
    int s = 0;
    while (i < n) {
        int d1 = i * x + y;
        int d2 = d1 * d1;
        int d3 = d2 + i;
        s = s + x;
        i = i + 1;
    }
    printf("%d", s);
    return 0;
}
EOF

time_one() {
  local label="$1"
  shift
  local start end
  start=$(date +%s%3N)
  "$@" >/tmp/bench.out
  end=$(date +%s%3N)
  echo "$label elapsed_ms=$((end - start)) stdout=$(cat /tmp/bench.out)"
}

./compiler -asm < /tmp/bench_dead.c > /tmp/bench_base.s
./compiler -asm -opt < /tmp/bench_dead.c > /tmp/bench_opt.s
"$CC" -march=rv64gc -mabi=lp64d -nostdlib -static -no-pie -Wl,-e,_start \
  /tmp/bench_base.s "$RT" -o /tmp/bench_base.elf
"$CC" -march=rv64gc -mabi=lp64d -nostdlib -static -no-pie -Wl,-e,_start \
  /tmp/bench_opt.s "$RT" -o /tmp/bench_opt.elf
"$CC" -O2 -march=rv64gc -mabi=lp64d -static /tmp/bench_dead_gcc.c -o /tmp/bench_gcc.elf

echo "n=$N"
time_one base "$QEMU" /tmp/bench_base.elf
time_one opt "$QEMU" /tmp/bench_opt.elf
time_one gcc "$QEMU" /tmp/bench_gcc.elf
