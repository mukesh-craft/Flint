# Emitter error catalog

Every diagnostic the self-hosted emitter (`stage3/flint_emit.fl`) can
produce. Generated list; fix guidance curated. Stage-2 (parser) errors
are prefixed `parse error` with line/col and are not catalogued here.

## Fix guidance by family

- **undeclared / unknown X**: declare the name first (same function scope;
  C++ rejects shadowing, so does self-hosted resolution).
- **type mismatch / needs conversion**: insert an explicit conversion or
  matching literal; i64->f64 promotes, int<->ptr converts, else retype.
- **needs G*+**: construct not yet implemented (see G7 list in memory.md).
- **non-exhaustive / duplicate / missing**: complete the form (all struct
  fields, all enum variants, all call args or defaults).
- **bounds / overflow / divzero / OOB**: runtime panics, not compile
  errors — guard the index or range.
- **lambda ...**: lambdas are same-function only (outline to `lam.N`,
  captures frozen by value); annotate params or keep them i64, never
  assign to a captured name, never return or store a handle.

## Complete message list

- `emit error: too many args calling lambda`
- `emit error: missing arg, no default calling lambda`
- `emit error: lambdas cannot be passed as values`
- `emit error: lambda must be bound`
- `emit error: lambda cannot leave its function`
- `emit error: lambda cannot capture unknown`
- `emit error: lambda cannot assign to captured`
- `emit error: lambda arg mismatch calling`
- `emit error: default type mismatch calling lambda`
- `emit error: cannot reassign lambda`
- `emit error: bad lambda param type`
- `emit error: arg type mismatch calling`
- `emit error: arrays support only + in G3`
- `emit error: assign to undeclared`
- `emit error: bad compound operand`
- `emit error: bad conversion`
- `emit error: bad extern member`
- `emit error: bad global type`
- `emit error: bad handle conversion`
- `emit error: bad int conversion`
- `emit error: bad map conversion`
- `emit error: bad match pattern`
- `emit error: bad payload type`
- `emit error: bad str conversion`
- `emit error: bad struct field type`
- `emit error: bad to-int conversion`
- `emit error: bad top-level form`
- `emit error: binop needs numeric/str/array`
- `emit error: break outside loop`
- `emit error: cannot bind empty variant`
- `emit error: cannot bind tag-only variant`
- `emit error: continue outside loop`
- `emit error: default type mismatch calling`
- `emit error: deref needs a pointer`
- `emit error: duplicate field`
- `emit error: duplicate function`
- `emit error: duplicate match arm`
- `emit error: duplicate struct field`
- `emit error: duplicate type`
- `emit error: duplicate variant`
- `emit error: field global init needs annotation`
- `emit error: field needs a struct`
- `emit error: field type mismatch`
- `emit error: fn body must be a block`
- `emit error: for needs an iterator`
- `emit error: for needs range, array or str`
- `emit error: generics need G6`
- `emit error: global init mismatch`
- `emit error: idx-set needs array (strings are immutable)`
- `emit error: idx-set needs i64 index`
- `emit error: idx-set needs i64 value in G3`
- `emit error: if-expr arms must be single expressions in G1`
- `emit error: if-expr arms must match types in G1`
- `emit error: index needs array or str`
- `emit error: index needs i64`
- `emit error: len needs array, str or map`
- `emit error: main must return i64 in G2`
- `emit error: main takes no params in G2`
- `emit error: map keys need str in G6`
- `emit error: map values need i64 in G6`
- `emit error: match arm enum mismatch`
- `emit error: match arms must agree in type`
- `emit error: match needs an enum in G5`
- `emit error: match needs arms`
- `emit error: method arg mismatch`
- `emit error: methods need str/map in G7`
- `emit error: missing arg, no default calling`
- `emit error: mixed-type binop needs conversion`
- `emit error: node needs a later group`
- `emit error: non-exhaustive match`
- `emit error: not a struct`
- `emit error: not an enum`
- `emit error: only extern C supported`
- `emit error: only i64 arrays in G3`
- `emit error: only i64 arrays in G7`
- `emit error: param after variadic`
- `emit error: param type needs G3+`
- `emit error: payload count mismatch`
- `emit error: payload type mismatch`
- `emit error: print is reserved`
- `emit error: print needs i64/f64/str in G3`
- `emit error: range bounds need i64`
- `emit error: ref needs a variable in G7`
- `emit error: ref of undeclared`
- `emit error: return annotation needed in G2`
- `emit error: return type mismatch`
- `emit error: return type needs G3+`
- `emit error: runtime arg mismatch calling`
- `emit error: slice bounds need i64`
- `emit error: slice needs an array in G7`
- `emit error: spread needs an array`
- `emit error: strings support only +/==/!= in G6`
- `emit error: struct literal missing fields`
- `emit error: too few args calling runtime`
- `emit error: too many args calling`
- `emit error: too many args calling runtime`
- `emit error: too many payload args`
- `emit error: undefined variable`
- `emit error: unknown enum`
- `emit error: unknown field`
- `emit error: unknown function`
- `emit error: unknown map method`
- `emit error: unknown name in global init`
- `emit error: unknown str method`
- `emit error: unknown struct`
- `emit error: unknown variant`
- `emit error: unscanned declaration`
- `emit error: unscanned function`
- `emit error: unwrap needs an enum`
- `emit error: unwrap needs uniform single payload`
- `emit error: variadic must be last param`
- `emit error: variant takes no payload`
- `emit error: void binop operand`
- `emit error: void global init`
- `emit error: void initializer`
- `emit error: void variadic arg`

Total: 116 messages.
