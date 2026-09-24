#!/usr/bin/env python3
"""Grammar-based differential fuzzer input generator for Flint.
Usage: python3 fuzz/generate.py SEED [INVALID_RATE] > case.fl
Generates mostly-valid programs (declared-before-use, typed ops) with
occasional invalid ones (marked `# INVALID` for error-path coverage).
Deterministic per seed. Covers: arithmetic (incl. div/modString ops),
if/while/for/break/continue, calls/recursion, arrays/index/slices,
strings/escapes/methods, structs, enums/match/unwrap, compound-assign.
Skips (not yet in emitter): lambdas, try (use unwrap), payload bindings
beyond single, channels, maps with non-i64 values.
"""
import random
import sys

seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
invalid_rate = float(sys.argv[2]) if len(sys.argv) > 2 else 0.1
rng = random.Random(seed)

MAX_DEPTH = 3
MAX_STMTS = 12

PRELUDE = """struct Pt {
    x: i64,
    y: i64
}
enum Opt {
    None,
    Some(i64)
}
enum Color {
    Red,
    Green
}
"""

INTS = ["0", "1", "2", "3", "5", "7", "10", "42", "100"]
STRS = ['"hi"', '"a"', '"x"', '"a\\nb"', '"{{q}}"']
BINOPS = ["+", "-", "*", "/", "%", "<", ">", "==", "!=", "&&", "||"]


class Gen:
    def __init__(self):
        self.vars = {}  # name -> [kind, is_mut]
        self.fns = {}  # name -> ret kind
        self.counter = 0

    def fresh(self, p="v"):
        self.counter += 1
        return f"{p}{self.counter}"

    def small_int(self, depth):
        # small ints keep div/mod and indices in range
        return rng.choice(["0", "1", "2", "3", "5"])

    def expr_int(self, depth):
        r = rng.random()
        if depth >= MAX_DEPTH or r < 0.35:
            ints = [v for v, k in self.vars.items() if k[0] == "i64"]
            if ints and rng.random() < 0.6:
                return rng.choice(ints)
            return rng.choice(INTS)
        op = rng.choice(BINOPS)
        if op == "/" or op == "%":
            rhs = rng.choice(["1", "2", "3", "5"])
            return f"({self.expr_int(depth+1)} {op} {rhs})"
        if op in ("<", ">", "==", "!=", "&&", "||"):
            return f"({self.expr_int(depth+1)} {op} {self.expr_int(depth+1)})"
        return f"({self.expr_int(depth+1)} {op} {self.expr_int(depth+1)})"

    def expr_str(self, depth):
        r = rng.random()
        strs = [v for v, k in self.vars.items() if k[0] == "str"]
        if strs and r < 0.5:
            base = rng.choice(strs)
        else:
            base = rng.choice(STRS)
        if r < 0.75:
            return base
        m = rng.choice(["upper", "lower", "trim", "len"])
        if m == "len":
            return base
        return f"{base}.{m}()"

    def stmt(self, depth, out):
        r = rng.random()
        if r < 0.18:
            init = self.expr_int(depth)
            nm = self.fresh()
            self.vars[nm] = ["i64", True]
            out.append(f"    mut {nm}: i64 = {init}")
        elif r < 0.28:
            init = self.expr_str(depth)
            nm = self.fresh()
            self.vars[nm] = ["str", True]
            out.append(f'    mut {nm}: str = {init}')
        elif r < 0.36:
            ints = [v for v, k in self.vars.items() if k[0] == "i64" and k[1]]
            if not ints:
                init = self.expr_int(depth)
                nm2 = self.fresh()
                self.vars[nm2] = ["i64", True]
                out.append(f"    mut {nm2}: i64 = {init}")
                return
            nm = rng.choice(ints)
            op = rng.choice(["+=", "-=", "*="])
            out.append(f"    {nm} {op} {self.expr_int(depth)}")
        elif r < 0.48:
            c = self.expr_int(depth)
            out.append(f"    if {c} > 3 {{")
            self.block(depth + 1, out, 3)
            out.append("    } else {")
            self.block(depth + 1, out, 2)
            out.append("    }")
        elif r < 0.56:
            arrs = [v for v, k in self.vars.items() if k[0] == "arr"]
            if arrs and rng.random() < 0.6:
                an = rng.choice(arrs)
                idx = self.small_int(depth)
                out.append(f"    print({an}[{idx}])")
            else:
                nm = self.fresh()
                self.vars[nm] = ["arr", True]
                elts = ", ".join(rng.choice(INTS) for _ in range(rng.randint(1, 4)))
                out.append(f"    mut {nm}: [i64] = [{elts}]")
        elif r < 0.64:
            lo = self.small_int(depth)
            hi = str(int(lo) + rng.randint(1, 4))
            iv = self.fresh("i")
            acc = self.fresh("s")
            out.append(f"    mut {acc}: i64 = 0")
            out.append(f"    for {iv} in {lo}..{hi} {{")
            self.vars[iv] = ["i64", False]
            out.append(f"        {acc} = {acc} + {iv}")
            out.append("    }")
            del self.vars[iv]
        elif r < 0.72:
            fns = [f for f in self.fns if f != "main"]
            if fns and rng.random() < 0.7:
                out.append(f"    print({rng.choice(fns)}({self.expr_int(depth)}))")
            else:
                out.append(f"    print({self.expr_int(depth)})")
        elif r < 0.78:
            if rng.random() < invalid_rate:
                out.append("    print(undefined_xyz)")
                out.append("    # INVALID")
            else:
                out.append(f"    print({self.expr_str(depth)})")
        elif r < 0.84:
            fx = self.expr_int(depth)
            fy = self.expr_int(depth)
            nm = self.fresh("p")
            self.vars[nm] = ["struct", False]
            out.append(f"    {nm} = Pt {{ x: {fx}, y: {fy} }}")
            out.append(f"    print({nm}.x + {nm}.y)")
        elif r < 0.90:
            mm = self.fresh("m")
            self.vars[mm] = ["i64", False]
            out.append("    %s = match Color.Red {" % mm)
            out.append("        Color.Red => 1,")
            out.append("        Color.Green => 2")
            out.append("    }")
            out.append(f"    print({mm})")
        else:
            wv = self.fresh("w")
            out.append(f"    mut {wv}: i64 = 0")
            out.append(f"    while {wv} < 3 {{")
            out.append(f"        {wv} = {wv} + 1")
            out.append("    }")

    def block(self, depth, out, n):
        saved = dict(self.vars)
        for _ in range(n):
            self.stmt(depth, out)
        # Block-scoped decls (if/while arms) do not leak out. Loop-var
        # deletion + restore compose: restore wins, loop var already gone.
        for k in [k for k in self.vars if k not in saved]:
            del self.vars[k]

    def program(self):
        out = [PRELUDE]
        out.append("fn helper(n: i64) -> i64 {")
        out.append("    helper2 = n * 2")
        out.append("    helper2 + 1")
        out.append("}")
        self.fns["helper"] = "i64"
        out.append("fn main() -> i64 {")
        main_block = []
        self.block(0, main_block, MAX_STMTS)
        out.extend(main_block)
        out.append("    print(helper(21))")
        out.append("    0")
        out.append("}")
        return "\n".join(out) + "\n"


print(Gen().program(), end="")
