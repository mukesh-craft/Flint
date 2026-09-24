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

## D1 — C++ miscompiles `for` over array (segfault). OPEN, C++ bug.

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

## D2 — C++ rejects bare `len()` calls. OPEN, C++ gap.

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

## D3 — C++ method calls mangle to garbage (`fa_len`). OPEN, C++ bug.

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

## D4 — C++ crashes on string indexing (`s[1]`). OPEN, C++ bug.

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

## Policy for new divergences

1. Minimize repro, add row above + corpus file under `tests/differential/`.
2. Mark manifest allowance (`same` default; nothing else without a row).
3. Never "fix" by making self-hosted match a C++ miscompile.
