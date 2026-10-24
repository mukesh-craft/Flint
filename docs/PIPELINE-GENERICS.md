# The self-host pipeline: generics and traits

**Status (2026-10-24): the monomorphizer, the traits, and the query mode
are IMPLEMENTED.** This note is the specification, written down so it does
not have to be re-derived. The measurements below are from this device.
What landed:

- **2E.1** the pipeline files were renamed to their stage roles
  (`stage1/lexer.fl`, `stage2/parser.fl`, `stage3/emitter.fl`).
- **2E.2** the monomorphizer: the pipeline compiles generic functions and
  produces the **C++ compiler's own mangled instance names**
  (`idc.i64`, `idc.f64`, `idc.str`, `@"idc.s:Inner"`, `pair.f64.str`) —
  verified symbol for symbol against the C++ IR, and the differential gate
  now covers a generic program (`tests/differential/t_generic.fl`).
- **2E.5** the C++ query mode (`flintc --emit-signatures`) and the
  pipeline's overlay consumer — the pipeline now reads the C++ compiler's
  resolved signatures instead of re-deriving inference (§5).

What is still open: the explicit `::<T>` call syntax (a stage2 gap: the
*declaration* form works ✓).

**The traits landed too (2E.3/2E.4):** the parser reads
`trait Name { fn m(self) -> T ... }` into
`(trait NAME ( (m (params) ret) ... ))` ✓ — the parameter list and the
return type are KEPT, because the pretty-printer has to reproduce the
source's token stream exactly (the fixpoint gate compares it ✓) ✓. A
trait emits no IR; it only records the method names, so a call to an
unimplemented one can say "no type in scope implements 'm'" ✓. A method
call on a user type resolves by NAME + ARITY: the implementation's first
parameter is the receiver ✓, so the implementation must take exactly one
more parameter than the call has arguments ✓.

**The one limitation:** the pipeline's function table holds ONE entry per
name ✓, so two implementations of the same method name cannot coexist ✓
— the C++ compiler keeps a multimap and filters the candidates on the
receiver's type ✓. The fix is a table change, not an algorithm change ✓.

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
3. **Instantiate.** Copy the template's S-expression and bind each type
   parameter to its concrete type in a **type environment** that the
   emitter's type resolution consults (`e_ann_code`, and the named-type
   lookup). The environment, NOT a textual substitution: a textual pass
   over the body would also rewrite a string literal or a variable that
   happens to be spelled `T`. (This was the one design change from the
   first draft of this note — the implementation is the structural
   version of the same idea.)
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

### 5.1 The query mode and the overlay — DONE (2E.5)

**The producer.** `flintc --emit-signatures in.fl out.sig` parses the WHOLE
file (the untyped-parameter fixpoint needs the call sites), runs the
declaration pass (`inferUntypedParams` + `generateDeclarations`, the exact
view codegen uses), and writes:

    flint-signatures 1
    fn NAME (TPARAMS) (PARAM_TYPES) -> RET

Type words are `i64 f64 str bool void ptr map [T]`, a struct/enum name, or
a type parameter; groups are comma-separated and `()` is empty; `#` starts
a comment. Definitions win over forward declarations; one line per name,
in first-occurrence order. No artifact and no linking: the mode exits after
writing the file.

**The consumer.** `stage3/emitter.fl` takes
`emit.0 prog.sexp out.ll --signatures FILE`; the driver passes it through
with `flintc.fl ... --signatures FILE`. The file is an **overlay**: a
function it does not list keeps its scanned signature, so a partial file is
safe. It overrides parameter words, the return word, and a template's type
parameters. A shape mismatch is loud (`E0287` parameter count, `E0288` type
parameters) and an unsupported type word is loud (`E0289`), never a guess.

**What it fixed (measured).** `fn show(x) { print(x) }` called as
`show("hi")`: the S-expression says the parameter is untyped (`-`), so the
pipeline defaulted it to i64 and printed the pointer's decimal value
(`379811845520`); the C++ compiler resolves it to `str` from the call
site. With the query file the pipeline emits
`define i64 @show(ptr %x.arg)` and prints `hi`. `tests/test_signatures.sh`
is the gate: the file's content and determinism, the end-to-end fix, the
no-file control (still i64 — proof the overlay is what changes the
answer), a partial file, a C++-vs-pipeline differential pair, and the six
error codes.

**Limits, recorded rather than hidden.**

- The query covers ONE file. In a multi-file build the driver passes the
  main file's signatures; imported functions keep their scanned signatures
  (safe because the file is an overlay, but they can still drift when
  their own untyped parameters are resolved from cross-file calls). Run
  the query per file when that matters.
- `ref` and first-class function types print as `ref`/`fn`, and an array
  other than `[i64]` prints its element type: the pipeline rejects those
  words with `E0289` because it cannot emit them. The file never lies to
  keep the consumer happy.
- A `void` return maps back to the source's `-` marker, so the existing
  loud `E0241` (the pipeline cannot compile void helper functions) is
  unchanged.
- Diagnostic codes in the pipeline's space: `E0284` unreadable file,
  `E0285` wrong header/version, `E0286` malformed line, `E0287` parameter
  count disagrees, `E0288` type parameters disagree, `E0289` type not
  supported by the pipeline.

## 6. Order of work

1. **DONE (2E.2): stage3 monomorphization** — with the C++ mangling names
   matching, so the differential gate compares the two frontends symbol for
   symbol.
2. **DONE (2E.3): stage2 the trait form + method signatures.**
3. **DONE (2E.4): stage3 trait resolution** (by name + arity).
4. **DONE (2E.5): the query mode** (`flintc --emit-signatures`) and the
   pipeline overlay, so the two frontends stop re-deriving types (§5.1).

### 6.1 What the monomorphizer learned (worth keeping)

- **A generic call must pass every argument.** The C++ compiler does not
  fill a generic's default arguments ("'f' expects 2 argument(s), got 1"),
  so the pipeline refuses it too. The pipeline being MORE permissive than
  the reference implementation is a divergence, not an improvement.
- **A type parameter that no parameter mentions cannot be inferred** — an
  error, never a guess (`fn countdown<T>(n: i64)` is rejected by both
  frontends).
- **The instance is registered before its body is emitted**, so a generic
  that calls itself terminates.
- **`bool` has no type code in the pipeline** (it is i64 in this
  codegen), so a bool instantiation mangles as `.i64` where the C++
  compiler says `.bool`: the same behaviour, a different symbol name.

Each step is gated by the same suites as the C++ compiler
(`tests/test_differential.sh` is the oracle: the two frontends must produce
the same output for the same program).
