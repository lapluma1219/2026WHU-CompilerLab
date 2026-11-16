#!/usr/bin/env python3
"""Same-machine QEMU comparison of constructed ToyC kernels.

These programs are not the hidden platform tests. Names follow the public
sample categories. Trip counts come from getint so neither compiler can fold
the loop away just by seeing a literal bound. C is compiled with -fwrapv so
signed overflow matches ToyC's 32-bit wrap. A second GCC build without
-fwrapv is disassembled only, and its stdout is reported separately.
"""

import os
import statistics
import subprocess
import sys
import textwrap
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COMP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "compiler")
RT = os.path.join(ROOT, "tests", "runtime", "libtoyc.s")
GCC = "riscv64-linux-gnu-gcc"
QEMU = "qemu-riscv64-static"
OUT = os.path.join(ROOT, "tests", "perf", "out")
os.makedirs(OUT, exist_ok=True)

LINK = [
    GCC,
    "-march=rv64gc",
    "-mabi=lp64d",
    "-nostdlib",
    "-static",
    "-no-pie",
    "-Wl,-e,_start",
]


CASES = [
    (
        "dead_affine",
        "dead stores around an affine update the opt pipeline can delete or close",
        "4000000\n317\n19\n",
        """
        int main() {
            int n = getint();
            int a = getint();
            int b = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                int dead = i * i + a * b;
                s = s + a * i + b;
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "dead_square",
        "dead multiply removed, live sum of squares kept as a loop",
        "4000000\n317\n19\n",
        """
        int main() {
            int n = getint();
            int a = getint();
            int b = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                int dead = a * b + i;
                s = s + i * i;
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "cse_cross",
        "same a*b+c computed on both sides of an invariant branch",
        "3000000\n173\n23\n5\n",
        """
        int main() {
            int n = getint();
            int a = getint();
            int b = getint();
            int k = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                if (k) {
                    int t = a * b + k;
                    s = s + t;
                } else {
                    int t = a * b + k;
                    s = s + t + 1;
                }
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "licm_nested",
        "nested loops, inner addend invariant in the inner loop",
        "1800\n1800\n317\n11\n",
        """
        int main() {
            int n = getint();
            int m = getint();
            int a = getint();
            int b = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                int j = 0;
                while (j < m) {
                    s = s + a * b + i;
                    j = j + 1;
                }
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "induction",
        "s = s + i * k, k runtime; strength reduction or a closed form may apply",
        "4000000\n317\n37\n",
        """
        int main() {
            int n = getint();
            int k = getint();
            int bias = getint();
            int i = 0;
            int s = bias;
            while (i < n) {
                s = s + i * k;
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "unswitch",
        "invariant if inside a loop, both arms are live updates",
        "3000000\n493\n13\n2\n",
        """
        int main() {
            int n = getint();
            int a = getint();
            int b = getint();
            int k = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                if (a < b) {
                    s = s + i * k;
                } else {
                    s = s + k;
                }
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "inline_call",
        "call of a straight-line helper inside a counted loop",
        "4000000\n877\n193\n",
        """
        int work(int x, int y) {
            int t = x + y;
            t = t + x;
            t = t + y;
            t = t + 1;
            t = t + x;
            t = t + y;
            return t;
        }
        int main() {
            int n = getint();
            int a = getint();
            int b = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                s = s + work(a, b);
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "tail_rec",
        "tail recursion that can become a loop",
        "2000000\n703\n19\n",
        """
        int down(int n, int acc, int step) {
            if (n == 0) {
                return acc;
            }
            return down(n - 1, acc + step, step);
        }
        int main() {
            int n = getint();
            int step = getint();
            int bias = getint();
            putint(down(n, bias, step));
            return 0;
        }
        """,
    ),
    (
        "reg_pressure",
        "many values live across one loop iteration",
        "2500000\n911\n13\n",
        """
        int main() {
            int n = getint();
            int a = getint();
            int b = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                int v0 = s + a;
                int v1 = v0 + b;
                int v2 = v1 + i;
                int v3 = v2 + a;
                int v4 = v3 + b;
                int v5 = v4 + 1;
                int v6 = v5 + a;
                int v7 = v6 + b;
                int v8 = v7 + i;
                int v9 = v8 + a;
                int v10 = v9 + b;
                int v11 = v10 + 3;
                s = v11 + v0;
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "short_circuit",
        "short-circuit chain with no calls, both sides live",
        "4000000\n317\n193\n",
        """
        int main() {
            int n = getint();
            int a = getint();
            int b = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                if (a && b && i < n) {
                    s = s + a;
                } else {
                    s = s + b;
                }
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "div_rem",
        "variable dividend, constant divisor and modulus",
        "2000000\n271\n12345\n",
        """
        int main() {
            int n = getint();
            int m = getint();
            int d = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                s = s + (i / d) + (i % m) + (i / 3);
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "startup",
        "empty main, measures qemu and runtime startup",
        "",
        """
        int main() {
            return 0;
        }
        """,
    ),
]


def write_src(name, src):
    path = os.path.join(OUT, name + ".c")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(textwrap.dedent(src).strip() + "\n")
    return path


def run_qemu(elf, data):
    start = time.perf_counter()
    proc = subprocess.run([QEMU, elf], input=data.encode(), capture_output=True)
    elapsed = (time.perf_counter() - start) * 1000
    return elapsed, proc.returncode & 0xFF, proc.stdout.decode()


def compile_toyc(name, src_path, opt):
    flag = ["-asm"] + (["-opt"] if opt else [])
    asm_path = os.path.join(OUT, name + (".opt.s" if opt else ".asm.s"))
    elf_path = os.path.join(OUT, name + (".opt" if opt else ".asm"))
    with open(src_path, "rb") as fh:
        proc = subprocess.run([COMP] + flag, stdin=fh, capture_output=True)
    if proc.returncode != 0:
        return None, "toyc " + proc.stderr.decode()[-200:]
    with open(asm_path, "w", encoding="utf-8") as fh:
        fh.write(proc.stdout.decode())
    link = subprocess.run(LINK + [asm_path, RT, "-o", elf_path], capture_output=True)
    if link.returncode != 0:
        return None, "link " + link.stderr.decode()[-200:]
    return elf_path, asm_path


def compile_gcc(name, src_path, wrap):
    tag = "gcc-wrap" if wrap else "gcc-nowrap"
    asm_path = os.path.join(OUT, name + "." + tag + ".s")
    elf_path = os.path.join(OUT, name + "." + tag)
    cmd = [
        GCC,
        "-S",
        "-O2",
        "-march=rv64gc",
        "-mabi=lp64d",
    ]
    if wrap:
        cmd.append("-fwrapv")
    cmd += [src_path, "-o", asm_path]
    proc = subprocess.run(cmd, capture_output=True)
    if proc.returncode != 0:
        return None, proc.stderr.decode()[-200:]
    link = subprocess.run(
        LINK + ["-O2"] + (["-fwrapv"] if wrap else []) + [src_path, RT, "-o", elf_path],
        capture_output=True,
    )
    if link.returncode != 0:
        return None, link.stderr.decode()[-200:]
    return elf_path, asm_path


def hot_loop(asm_text):
    lines = asm_text.splitlines()
    labels = {}
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.endswith(":") and not stripped.startswith("."):
            labels[stripped[:-1]] = index
        elif stripped.endswith(":") and stripped.startswith("."):
            labels[stripped[:-1]] = index
    best = None
    for index, line in enumerate(lines):
        stripped = line.strip()
        parts = stripped.replace(",", " ").split()
        if len(parts) < 2:
            continue
        op = parts[0]
        if op not in {"j", "jal", "bge", "blt", "beq", "bne", "bgez", "bltz", "ble", "bgt"}:
            continue
        target = parts[-1]
        if target not in labels or labels[target] >= index:
            continue
        body = lines[labels[target] : index + 1]
        insts = [item.strip() for item in body if item.strip() and not item.strip().endswith(":")]
        if best is None or len(insts) < best[0]:
            best = (len(insts), target, insts)
    if best is None:
        return []
    return best[2]


def median(samples):
    return statistics.median(samples)


def main():
    rows = []
    for name, note, data, src in CASES:
        src_path = write_src(name, src)
        builds = {}
        builds["opt"] = compile_toyc(name, src_path, True)
        builds["asm"] = compile_toyc(name, src_path, False)
        builds["gcc"] = compile_gcc(name, src_path, True)
        builds["gcc-ub"] = compile_gcc(name, src_path, False)
        record = {"name": name, "note": note, "input": data}
        for key, built in builds.items():
            elf, info = built
            if elf is None:
                record[key] = {"error": info}
                continue
            times = []
            code = None
            out = None
            for _ in range(3):
                elapsed, code, out = run_qemu(elf, data)
                times.append(elapsed)
            asm_text = open(info, encoding="utf-8").read()
            loop = hot_loop(asm_text)
            record[key] = {
                "ms": [round(item, 2) for item in times],
                "median": round(median(times), 2),
                "min": round(min(times), 2),
                "max": round(max(times), 2),
                "exit": code,
                "stdout": out,
                "loop_n": len(loop),
                "loop": loop,
            }
        rows.append(record)
        opt = record.get("opt", {})
        gcc = record.get("gcc", {})
        bare = record.get("asm", {})
        print(
            f"{name:16} opt {opt.get('median')} ms loop {opt.get('loop_n')} "
            f"asm {bare.get('median')} gcc {gcc.get('median')} loop {gcc.get('loop_n')} "
            f"out opt={opt.get('stdout')!r} gcc={gcc.get('stdout')!r} "
            f"ub={record.get('gcc-ub', {}).get('stdout')!r}"
        )
        if opt.get("loop"):
            print("  opt:", " | ".join(opt["loop"]))
        if gcc.get("loop"):
            print("  gcc:", " | ".join(gcc["loop"]))

    startup = next(row for row in rows if row["name"] == "startup")
    base = startup["opt"]["median"]
    print(f"startup_median_ms {base}")
    for row in rows:
        if row["name"] == "startup":
            continue
        opt = row["opt"]["median"] - base
        gcc = row["gcc"]["median"] - base
        ratio = (gcc / opt * 100) if opt > 1 else None
        print(
            f"net {row['name']:16} opt {opt:.1f} gcc {gcc:.1f} "
            f"ratio {None if ratio is None else round(ratio, 1)}"
        )


if __name__ == "__main__":
    main()
