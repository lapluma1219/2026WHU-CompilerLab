#!/usr/bin/env python3
import random
import subprocess
import sys
import tempfile
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "compiler")
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 11
COUNT = int(sys.argv[3]) if len(sys.argv) > 3 else 30
CC = "riscv64-linux-gnu-gcc"
QEMU = "qemu-riscv64-static"
RT = os.path.join(ROOT, "tests", "runtime", "libtoyc.s")
rng = random.Random(SEED)


def gen_expr(vars, depth):
    if depth <= 0 or not vars or rng.random() < 0.4:
        if vars and rng.random() < 0.7:
            return rng.choice(vars)
        return str(rng.randint(-3, 5))
    op = rng.choice(["+", "-", "*", "<", ">", "<=", ">=", "==", "!=", "&&", "||"])
    if rng.random() < 0.25:
        op = rng.choice(["/", "%"])
        return f"({gen_expr(vars, depth - 1)} {op} {rng.choice([-2, -1, 1, 2, 3])})"
    return f"({gen_expr(vars, depth - 1)} {op} {gen_expr(vars, depth - 1)})"


def gen_block(vars, depth, in_loop):
    local = list(vars)
    parts = []
    for _ in range(rng.randint(1, 3)):
        parts.append(gen_stmt(local, depth, in_loop))
    return "{ " + " ".join(parts) + " }"


def gen_stmt(local, depth, in_loop):
    if depth <= 0:
        if not local:
            return ";"
        return f"{rng.choice(local)} = {gen_expr(local, 1)};"
    kind = rng.randrange(6 if not in_loop else 7)
    if kind == 0 and local:
        return f"{rng.choice(local)} = {gen_expr(local, 2)};"
    if kind == 1:
        name = f"v{len(local)}"
        local.append(name)
        return f"int {name} = {gen_expr(local[:-1] or ['0'], 2)};"
    if kind == 2:
        then_b = gen_block(local, depth - 1, in_loop)
        if rng.random() < 0.5:
            else_b = gen_block(local, depth - 1, in_loop)
            return f"if ({gen_expr(local, 2)}) {then_b} else {else_b}"
        return f"if ({gen_expr(local, 2)}) {then_b}"
    if kind == 3:
        body = gen_block(local, depth - 1, True)
        cond = gen_expr(local, 2)
        return (
            "{ int guard = 0; while (guard < 3) { if (!("
            + cond
            + ")) break; "
            + body
            + " guard = guard + 1; } }"
        )
    if kind == 4 and local:
        return f"putint({gen_expr(local, 2)});"
    if kind == 5:
        return gen_block(local, depth - 1, in_loop)
    return "break;"


def program(i):
    helpers = ""
    extra = ""
    if i % 2 == 0:
        helpers = "int inc(int x) { return x + 1; }\nint add(int x, int y) { return x + y; }\n"
        extra = "s = add(inc(a), b);"
    body = gen_block(["n", "a", "b", "s"], 2, False)
    return helpers + f"""
int main() {{
    int n = getint();
    int a = getint();
    int b = getint();
    int s = a;
    {extra}
    {body}
    putint(s + n);
    return a + b + s;
}}
"""


def compile_src(src, opt):
    cmd = [COMP, "-asm"] + (["-opt"] if opt else [])
    proc = subprocess.run(cmd, input=src.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    err = proc.stderr
    bad = proc.returncode != 0 or b"ERROR:" in err or b"runtime error" in err
    return (not bad) and proc.stdout.startswith(b".text"), proc.stdout, err


def execute(asm, data):
    with tempfile.NamedTemporaryFile(suffix=".s", delete=False) as handle:
        handle.write(asm)
        path = handle.name
    elf = path + ".elf"
    link = subprocess.run(
        [CC, "-march=rv64gc", "-mabi=lp64d", "-nostdlib", "-static", "-no-pie", "-Wl,-e,_start", path, RT, "-o", elf],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    os.unlink(path)
    if link.returncode != 0:
        return None
    try:
        proc = subprocess.run([QEMU, elf], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=2)
    except subprocess.TimeoutExpired:
        os.unlink(elf)
        return "TIMEOUT"
    os.unlink(elf)
    return (proc.returncode & 255, proc.stdout)


def main():
    checked = crash = mismatch = skip = 0
    for i in range(COUNT):
        src = program(i)
        ok, asm, err = compile_src(src, False)
        if not ok:
            skip += 1
            continue
        ok2, asm2, err2 = compile_src(src, True)
        if not ok2:
            crash += 1
            print(f"CRASH case {i}")
            print(err2.decode()[-400:])
            print(src[:500])
            continue
        data = f"{rng.randint(0, 5)} {rng.randint(-2, 4)} {rng.randint(-2, 4)}\n".encode()
        left = execute(asm, data)
        right = execute(asm2, data)
        if left is None or right is None:
            skip += 1
            continue
        checked += 1
        if left != right:
            mismatch += 1
            print(f"MISMATCH case {i} input {data!r}")
            print("base", left)
            print("opt", right)
            print(src)
    print(f"checked={checked} crash={crash} mismatch={mismatch} skip={skip}")
    return 1 if crash or mismatch else 0


if __name__ == "__main__":
    sys.exit(main())
