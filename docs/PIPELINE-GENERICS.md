# The self-host pipeline: generics and traits

**Status (2026-10-20): MEASURED, NOT IMPLEMENTED.** This note is the
specification for the remaining work, written down so it does not have to be
re-derived. The measurements below are from this device.

## 1. What works today (measured)

| piece | state | evidence |
|---|---|---|
| stage2 (parser) accepts a **generic function** | **YES** | `parse.0` on `fn idc<T>(x: T) -> T { return x }` yields `(fn idc (tparams T) ( (x T -)) T (block (return (var x))))` — the type parameters are captured in the `tparams` group |
| stage3 (emitter) accepts a generic function | **NO** | `emit.0` prints `emit error: generics need G6` (`stage3/emitter.fl:6915`, and again at `:7350`) |
| stage2 accepts a **trait** | **NO** | `parse.0` on a `trait Shape { fn area(self) -> i64 }` reports `parse error at 2:7 (kind 14)` — a method SIGNATURE (no body) is not parseable; worse, the trait text is mis-parsed as a function named `trait` |
| stage3 emits a trait | **NO** | no trait form exists in the emitter |

The C++ compiler handles all of these (a generic program compiles and runs;
a trait program compiles and runs), so the gap is entirely in the pipeline.

## 2. The S-expression contract

The parser emits functions as:

```
(fn NAME (tparams T1 T2) ( (p1 T1) (p2 T2) ) RET (block ...))
```

`tparams` is an EMPTY group for an ordinary function and a non-empty one
for a generic function. That is the only difference — which is why the
emitter's check `if (tp != "tparams")` fires for generics: the check
compares the group's TEXT, and a generic function's group is `(tparams T)`.

**Proposed change (stage2)**: make the trait form explicit, so a trait is
never mistaken for a function:

```
(trait NAME ( (m1 (self T) RET) (m2 (self T) RET) ))
```

The parser needs a new rule for `trait NAME { ... }` where the members are
method SIGNATURES (no bodies) — the current grammar has no signature-only
form, which is why it errors.

## 3. Generics in stage3 (monomorphization)

The emitter already keeps a signature table (`e_sig_lookup`, `EFNN`/`EFP`/
`EFRET` ✓) and a set of text helpers for balanced groups
(`e_skip_balanced_fn` ✓). Monomorphization is therefore mostly table work
plus one substitution pass.

Steps, in order:

1. **Read the type parameters.** `(tparams T U)` → the names `T`, `U`.
   A function with a non-empty `tparams` is a TEMPLATE: it is not emitted
   as-is; only its instantiations are.
2. **Infer the type arguments at a call site.** For
   `(call NAME a1 a2 ...)`, where `NAME` is a template:
   - the argument's type is: a number → `i64`; a string → `str`; a
     variable → the declared type of that parameter in the ENCLOSING
     function's signature (the table already has it ✓); a call → the
     callee's return type (`EFRET` ✓); a struct literal → the struct
     name; a field access → the field's type (needs a struct-field table,
     which the emitter must build by scanning the `(struct NAME ( (f T) ))`
     forms the parser emits ✓).
   - the type ARGUMENTS of the template are then the distinct concrete
     types bound to `T`, `U` by the parameters that mention them (the
     by-parameter rule the C++ compiler uses ✓ — the same rule, so both
     frontends agree).
3. **Instantiate.** Copy the template's S-expression, substituting the
   type parameters by the concrete types in: the return type, the
   parameter types, and any type annotation inside the body (`as` casts).
   The substitution is textual over balanced groups, which the emitter's
   group helpers already support ✓.
4. **Name the instance** `<name>.<type>` — the C++ compiler's mangling
   style (`idc.arr`) ✓, so the two frontends produce the same symbol
   names and the differential gate can compare them directly ✓.
5. **Register before emitting** (a recursive generic calls itself: the
   instance must be in the signature table before its body is emitted ✓ —
   the same ordering bug the C++ compiler had in `specialize` and fixed by
   registering the cache entry before the body ✓).
6. **Rewrite the call site** to the instance name ✓.

**Type parameters that cannot be inferred** (a call that does not mention
`T` in any argument) must be an error, not a guess — the C++ compiler
reports `cannot infer type parameter 'T' for 'f'` ✓ and the pipeline should
say the same.

## 4. Traits in stage2 + stage3

The C++ compiler's rule (M3) is: a trait carries SIGNATURES only; an
implementation is an ordinary function with the same name and arity; the
call site resolves by NAME + ARITY: exactly one → call it; none → "no
type in scope implements 'm'"; several → "ambiguous (candidates: ...)".

The pipeline should implement the SAME rule, so the two frontends agree:

1. stage2: parse a trait into the `(trait NAME ( (m ...) ))` form ✓ (above).
2. stage3: collect the trait's method signatures; at a method call
   (`x.m(a)` or `m(x, a)`), resolve the method name + arity to an
   implementation function in the signature table ✓; rewrite the call to
   that implementation ✓; if there is no implementation → the same error
   the C++ compiler gives ✓; if several → the ambiguity error ✓.
3. The trait's own signatures are NOT emitted as functions ✓ (they have no
   bodies).

## 5. What "consume the shared semantics" means here

Both frontends must agree on: the type system (what `T` binds to), the
trait-resolution rule, the error messages, and the mangled names. The
cheapest way to guarantee that is for the pipeline to READ the C++
compiler's answers instead of re-deriving them:

- the C++ compiler gains a query mode that prints, for each function, its
  signature, its type parameters and its resolved parameter types — the
  same information the emitter needs for monomorphization;
- the pipeline consumes that file (like the existing `.flint.bc` interface
  mechanism ✓) instead of re-implementing inference in Flint.

That is the "one source of truth" the rest of the project is moving
towards (see `COMPATIBILITY.md` D5 and the Phase 2 work in `STATE.md`).

## 6. Order of work

1. stage2: the trait form + method signatures (a parser feature; the
   smallest piece, and it makes the S-expression contract explicit).
2. stage3: monomorphization (the big one) — with the C++ mangling names
   matching, so the differential gate compares the two frontends symbol for
   symbol.
3. stage3: trait resolution.
4. The query mode (above), so the two frontends stop re-deriving types.

Each step is gated by the same suites as the C++ compiler
(`tests/test_differential.sh` is the oracle: the two frontends must produce
the same output for the same program).
