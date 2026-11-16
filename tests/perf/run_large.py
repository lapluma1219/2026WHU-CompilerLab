#!/usr/bin/env python3
"""Large constructed kernels. Not the hidden platform tests.

Trip counts are runtime getint values. Each case is meant to keep a loop:
if -opt or GCC deletes the back edge, the row is marked closed and is not
compared as a 100ms race. Five repeats. Times are qemu-riscv64-static
milliseconds, including startup.
"""

import os
import statistics
import subprocess
import textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COMP = os.path.join(ROOT, "compiler")
RT = os.path.join(ROOT, "tests", "runtime", "libtoyc.s")
GCC = "riscv64-linux-gnu-gcc"
QEMU = "qemu-riscv64-static"
OUT = os.path.join(ROOT, "tests", "perf", "large")
os.makedirs(OUT, exist_ok=True)
LINK = [GCC, "-march=rv64gc", "-mabi=lp64d", "-nostdlib", "-static", "-no-pie", "-Wl,-e,_start"]

def pad(n):
    return " ".join(["t = t + 0;" for _ in range(n)])

CASES = [
    (
        "pre_mul",
        "same i*a computed in a side block and again after the join",
        "30000000\n17\n3\n1\n",
        """
        int main() {
            int n = getint();
            int a = getint();
            int k = getint();
            int m = getint();
            int i = 0;
            int s = 0;
            int t = 0;
            while (i < n) {
                if (k) {
                    s = s + (i * a);
                }
                t = t + (i * a) + (i % m);
                i = i + 1;
            }
            putint(s);
            putint(t);
            return 0;
        }
        """,
    ),
    (
        "inline_rem",
        "small helper of i, inlined, remainder blocks a closed form",
        "20000000\n19\n7\n",
        """
        int mix(int x, int y, int m) {
            int t = x * y;
            t = t + (x % m);
            t = t + y;
            return t;
        }
        int main() {
            int n = getint();
            int y = getint();
            int m = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                s = s + mix(i, y, m);
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "call_rem",
        "same helper padded past the inline limit, so the call stays",
        "4000000\n19\n7\n",
        """
        int mix(int x, int y, int m) {
            int t = x * y;
            t = t + (x % m);
            t = t + y;
            """ + pad(180) + """
            return t;
        }
        int main() {
            int n = getint();
            int y = getint();
            int m = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                s = s + mix(i, y, m);
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
    (
        "tail_mod",
        "tail recursion whose body is acc + (n % 5), not a pure multiply",
        "15000000\n5\n3\n",
        """
        int walk(int n, int acc, int step) {
            if (n == 0) {
                return acc;
            }
            return walk(n - 1, acc + (n % step), step);
        }
        int main() {
            int n = getint();
            int step = getint();
            int bias = getint();
            putint(walk(n, bias, step));
            return 0;
        }
        """,
    ),
    (
        "live_many",
        "eight independent accumulators, each uses i % 3, summed after the loop",
        "12000000\n9\n",
        """
        int main() {
            int n = getint();
            int m = getint();
            int i = 0;
            int s0 = 1;
            int s1 = 2;
            int s2 = 3;
            int s3 = 4;
            int s4 = 5;
            int s5 = 6;
            int s6 = 7;
            int s7 = 8;
            while (i < n) {
                int r = i % m;
                s0 = s0 + r + 1;
                s1 = s1 + r + 2;
                s2 = s2 + r + 3;
                s3 = s3 + r + 4;
                s4 = s4 + r + 5;
                s5 = s5 + r + 6;
                s6 = s6 + r + 7;
                s7 = s7 + r + 8;
                i = i + 1;
            }
            putint(s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7);
            return 0;
        }
        """,
    ),
    (
        "split_loop",
        "multi-block loop, branch depends on i so it cannot be lifted out",
        "20000000\n5\n9\n",
        """
        int main() {
            int n = getint();
            int a = getint();
            int b = getint();
            int i = 0;
            int s = 0;
            while (i < n) {
                if (i % 2) {
                    s = s + a + (i % 3);
                } else {
                    s = s + b + (i % 3);
                }
                i = i + 1;
            }
            putint(s);
            return 0;
        }
        """,
    ),
]


def loops_of(asm):
    lines = asm.splitlines()
    labels = {}
    for i, line in enumerate(lines):
        s = line.strip()
        if s.endswith(":") and " " not in s and not s.startswith("."):
            labels[s[:-1]] = i
        elif s.endswith(":") and s.startswith("."):
            labels[s[:-1]] = i
    found = []
    for i, line in enumerate(lines):
        parts = line.strip().replace(",", " ").split()
        if len(parts) < 2:
            continue
        if parts[0] not in {"j", "bge", "blt", "beq", "bne", "ble", "bgt", "bgez", "bltz"}:
            continue
        target = parts[-1]
        if target not in labels or labels[target] >= i:
            continue
        body = [ln.strip() for ln in lines[labels[target]:i + 1] if ln.strip() and not ln.strip().endswith(":")]
        if any(ln.startswith("ret") or ln.startswith("ld ") for ln in body):
            continue
        found.append(body)
    return found


def hot(asm):
    found = loops_of(asm)
    if not found:
        return False, 0, [], 0, 0
    # The heavy loop is the longest back-edge body, not a short outer skip.
    body = max(found, key=len)
    mem = sum(1 for ln in body if ln.startswith(("lw ", "sw ", "ld ", "sd ")))
    calls = sum(1 for ln in body if ln.startswith(("call ", "jal ")))
    return True, len(body), body, mem, calls


def run_qemu(elf, data):
    import time
    t = time.perf_counter()
    proc = subprocess.run([QEMU, elf], input=data.encode(), capture_output=True)
    return (time.perf_counter() - t) * 1000, proc.returncode & 255, proc.stdout.decode()


def compile_toyc(name, src, opt):
    tag = "opt" if opt else "asm"
    asm_path = os.path.join(OUT, name + "." + tag + ".s")
    elf = os.path.join(OUT, name + "." + tag)
    proc = subprocess.run([COMP, "-asm"] + (["-opt"] if opt else []), input=src.encode(), capture_output=True)
    if proc.returncode != 0:
        return None, proc.stderr.decode()[-200:]
    open(asm_path, "w").write(proc.stdout.decode())
    link = subprocess.run(LINK + [asm_path, RT, "-o", elf], capture_output=True)
    if link.returncode != 0:
        return None, link.stderr.decode()[-200:]
    return elf, asm_path


def compile_gcc(name, src):
    asm_path = os.path.join(OUT, name + ".gcc.s")
    c_path = os.path.join(OUT, name + ".gcc.c")
    elf = os.path.join(OUT, name + ".gcc")
    open(c_path, "w").write(src)
    proc = subprocess.run([GCC, "-S", "-O2", "-fwrapv", "-march=rv64gc", "-mabi=lp64d", c_path, "-o", asm_path],
                          capture_output=True)
    if proc.returncode != 0:
        return None, proc.stderr.decode()[-300:]
    link = subprocess.run(LINK + ["-O2", "-fwrapv", c_path, RT, "-o", elf], capture_output=True)
    if link.returncode != 0:
        return None, link.stderr.decode()[-300:]
    return elf, asm_path


def summarize(samples):
    return round(statistics.median(samples), 1), round(min(samples), 1), round(max(samples), 1)


def main():
    rows = []
    for name, note, data, src in CASES:
        src = textwrap.dedent(src).strip() + "\n"
        open(os.path.join(OUT, name + ".c"), "w").write(src)
        rec = {"name": name, "note": note}
        for key, built in {
            "opt": compile_toyc(name, src, True),
            "asm": compile_toyc(name, src, False),
            "gcc": compile_gcc(name, src),
        }.items():
            elf, info = built
            if elf is None:
                rec[key] = {"error": info}
                print(f"  {key} FAIL {info}")
                continue
            times = []
            code = out = None
            for _ in range(5):
                ms, code, out = run_qemu(elf, data)
                times.append(ms)
            asm = open(info).read()
            loop = hot(asm)
            med, lo, hi = summarize(times)
            rec[key] = {
                "times": [round(x, 1) for x in times],
                "median": med,
                "lo": lo,
                "hi": hi,
                "exit": code,
                "stdout": out,
                "closed": not loop[0],
                "loop_n": loop[1] if loop[0] else 0,
                "mem": loop[3] if loop[0] else 0,
                "calls": loop[4] if loop[0] else 0,
                "loop": loop[2] if loop[0] else [],
            }
        rows.append(rec)
        o, g = rec.get("opt", {}), rec.get("gcc", {})
        print(
            f"{name:12} opt {o.get('median')} closed={o.get('closed')} n={o.get('loop_n')} "
            f"mem={o.get('mem')} call={o.get('calls')} | gcc {g.get('median')} closed={g.get('closed')} "
            f"n={g.get('loop_n')} | out {o.get('stdout')!r} / {g.get('stdout')!r}"
        )
        if o.get("loop"):
            print("  opt:", " | ".join(o["loop"][:18]))
        if g.get("loop"):
            print("  gcc:", " | ".join(g["loop"][:18]))
        open(os.path.join(OUT, name + ".times.txt"), "w").write(repr(rec) + "\n")


if __name__ == "__main__":
    main()
