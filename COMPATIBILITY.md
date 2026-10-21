# Flint Divergence Log (P0 — correctness before speed)

Two compilers accept Flint source: the C++ reference (`src/main.cpp`,
`flintc`) and the self-hosted pipeline (`stage1-3/*.fl` via
`tests/test_selfhost.sh`). They MUST agree on valid programs. This file
logs every observed divergence: repro, both behaviors, owner, policy.

Rules (Lux/Traverse precedent adapted):
- Every entry needs: minimal repro, C++ behavior, self-hosted behavior,
  verdict (who is right), and disposition (fix-ref vs pin-limitation).
- `tests/test_differential.sh` encodes these entries and fails on any
  UNLISTED divergence. No silent miscompiles, ever.
- Perf work (P1) is gated on this log: no listed P0 open.

## D1 — C++ miscompiles `for` over array (segfault). FIXED 2026-10-15 (hoisted data+len desugar + bounds-checked index; both print 6).

```flint
fn main() -> i64 {
    a = [1, 2, 3]
    mut s: i64 = 0
    for v in a {
        s = s + v
    }
    print(s)
    0
}
```

- C++ (`flintc file.fl --emit-llvm`): emits IR, linked binary segfaults
  (exit 139). Root cause unknown (likely bounds/data-ptr codegen).
- Self-hosted: prints `6`, exit 0. Correct.
- Verdict: self-hosted is right. Disposition: FIX-REF (C++ must not
  miscompile) or at minimum emit a loud error instead of a bad binary.
  Differential status: `cpp_may_differ` (see manifest).

## D2 — C++ rejects bare `len()` calls. FIXED 2026-10-15 (bare len() dispatches array→extractvalue, str→flint_str_length, map→flint_map_len; both print 3).

```flint
fn main() -> i64 {
    a = [1, 2, 3]
    print(len(a))
    0
}
```

- C++: `codegen: undefined function 'len'` (emission failed).
- Self-hosted: prints `3`, exit 0. `len()` over array/str/map is specified
  G3/G6 behavior with ladder coverage (`g3_arrays`, `g3_stridx`, `g6_maps`).
- Verdict: self-hosted is right. Disposition: FIX-REF (add builtin) or
  document `len()` as self-hosted-only. Differential: `cpp_may_differ`.

## D3 — C++ method calls mangle to garbage (`fa_len`). FIXED 2026-10-15 (clean unknown-method rejection converges with self-host G7 error; both reject).

```flint
fn main() -> i64 {
    a = [1, 2, 3]
    print(a.len())
    0
}
```

- C++: `codegen: undefined function 'fa_len'` (name-mangling bug).
- Self-hosted: loud `emit error: method needs G7+` (only map
  has/get/set supported). Both reject; messages differ.
- Verdict: convergent rejection (acceptable), but C++ message is corrupt.
  Disposition: PIN-LIMITATION (methods beyond map has/get/set are G7) +
  FIX-REF message. Differential: `both_may_reject`.

## D4 — C++ crashes on string indexing (`s[1]`). FIXED 2026-10-15 (str index via i8 GEP+zext with bounds check; both print 101).

```flint
fn main() -> i64 {
    s = "hello"
    print(s[1])
    0
}
```

- C++: LLVM assertion `checkGEPType` — compiler crash (no binary).
- Self-hosted: prints `101` (byte value), exit 0, bounds-checked.
- Verdict: self-hosted is right. Disposition: FIX-REF. Differential:
  `cpp_may_differ`.

## D5 — The self-hosted pipeline lacks the features the C++ compiler gained after 2026-10-15. OPEN (feature gap, not a miscompile).

The two compilers do not disagree on the programs both can compile: the
differential gate is green (12/12, 0 known-divergence). What the gate does
NOT cover is the FEATURE GAP — the C++ compiler accepts programs the
self-hosted pipeline cannot parse or emit at all. Those programs are not in
the differential manifest, so the gate stays green while the gap is real.

Features in the C++ compiler that the self-hosted pipeline does not have
(as of commit 4214082; the generic row was closed in 2E.2):

| Feature | C++ compiler | Self-hosted (stage2/stage3) |
|---|---|---|
| `trait` declarations and name+arity method dispatch (M3, `1219665`) | supported | absent — `trait` is not even in the parser |
| Lambda-as-a-value with the env-box protocol (M4, `a6b89e6`) | supported | partial — the emitter has lambda shapes, the parser is behind |
| Generic functions with by-parameter inference (M0-M2) | supported | **supported** (2E.2) — the emitter monomorphizes, with the C++ compiler's own mangled instance names (`idc.i64`, `@"idc.s:Inner"`); the differential gate now covers a generic program |
| Explicit type arguments `f::<T>(...)` (M2b, `3d3d914`) | supported | absent — `::` is not lexed by the self-host parser |
| Enum equality by per-variant payload compare (P0-NEW-2, `d1c58cf`) | supported | absent — no aggregate equality in stage3 |
| A named function used as a value (`let h = inc`, `fd8cf09`) | supported | absent |

- C++: compiles and runs these programs.
- Self-hosted: rejects them (parse error), or would not emit them.
- Verdict: the C++ compiler is right; the self-hosted pipeline is behind.
- Disposition: PIN-LIMITATION for now, with the gap recorded here. Porting
  the features to the Flint pipeline is a project of its own (stage2 is
  4.1k lines, stage3 is 8.3k); it is NOT a correctness bug in either
  compiler, and the differential gate correctly does not pretend otherwise.
  The first feature to be ported was generics (2E.2): the emitter
  monomorphizes, and the instance names are the C++ compiler's own, so
  the two frontends agree symbol for symbol.
- Follow-up: every feature added to the C++ compiler from now on should
  either land in the self-host pipeline too, or be listed in the table
  above. The table is the checklist.

## Policy for new divergences

1. Minimize repro, add row above + corpus file under `tests/differential/`.
2. Mark manifest allowance (`same` default; nothing else without a row).
3. Never "fix" by making self-hosted match a C++ miscompile.
