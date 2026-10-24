# Emitter error catalog

Every diagnostic the self-hosted emitter (`stage3/emitter.fl`) can
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

- `emit error[E0259]: too many args calling lambda`
- `emit error[E0221]: missing arg, no default calling lambda`
- `emit error[E0207]: lambdas cannot be passed as values`
- `emit error[E0206]: lambda must be bound`
- `emit error[E0205]: lambda cannot leave its function`
- `emit error[E0204]: lambda cannot capture unknown`
- `emit error[E0203]: lambda cannot assign to captured`
- `emit error[E0202]: lambda arg mismatch calling`
- `emit error[E0177]: default type mismatch calling lambda`
- `emit error[E0174]: cannot reassign lambda`
- `emit error[E0157]: bad lambda param type`
- `emit error[E0145]: arg type mismatch calling`
- `emit error[E0146]: arrays support only + in G3`
- `emit error[E0147]: as-cast needs i64 or f64 value`
- `emit error[E0148]: assign to undeclared`
- `emit error[E0149]: bad cast`
- `emit error[E0150]: bad cast type`
- `emit error[E0151]: bad compound operand`
- `emit error[E0152]: bad conversion`
- `emit error[E0153]: bad extern member`
- `emit error[E0154]: bad global type`
- `emit error[E0155]: bad handle conversion`
- `emit error[E0156]: bad int conversion`
- `emit error[E0157]: bad lambda param type`
- `emit error[E0158]: bad map conversion`
- `emit error[E0159]: bad match pattern`
- `emit error[E0160]: bad payload type`
- `emit error[E0161]: bad str conversion`
- `emit error[E0162]: bad struct field type`
- `emit error[E0163]: bad tensor negation`
- `emit error[E0164]: bad tensor number`
- `emit error[E0165]: bad tensor row`
- `emit error[E0166]: bad to-int conversion`
- `emit error[E0167]: bad top-level form`
- `emit error[E0168]: binop needs numeric/str/array`
- `emit error[E0169]: break outside loop`
- `emit error[E0170]: call needs a closing paren`
- `emit error[E0171]: cannot bind empty variant`
- `emit error[E0172]: cannot bind tag-only variant`
- `emit error[E0173]: cannot infer type parameter`
- `emit error[E0174]: cannot reassign lambda`
- `emit error[E0175]: continue outside loop`
- `emit error[E0176]: default type mismatch calling`
- `emit error[E0177]: default type mismatch calling lambda`
- `emit error[E0178]: deref needs a pointer`
- `emit error[E0179]: duplicate field`
- `emit error[E0180]: duplicate function`
- `emit error[E0181]: duplicate match arm`
- `emit error[E0182]: duplicate struct field`
- `emit error[E0183]: duplicate type`
- `emit error[E0184]: duplicate variant`
- `emit error[E0185]: field global init needs annotation`
- `emit error[E0186]: field needs a struct`
- `emit error[E0187]: field type mismatch`
- `emit error[E0188]: fn body must be a block`
- `emit error[E0189]: for needs an iterator`
- `emit error[E0190]: for needs range, array or str`
- `emit error[E0191]: generic call is missing arguments`
- `emit error[E0192]: generic parameter type is not a type`
- `emit error[E0193]: generic return type is not a type`
- `emit error[E0194]: global init mismatch`
- `emit error[E0195]: idx-set needs array (strings are immutable)`
- `emit error[E0196]: idx-set needs i64 index`
- `emit error[E0197]: idx-set needs i64 value in G3`
- `emit error[E0198]: if-expr arms must be single expressions in G1`
- `emit error[E0199]: if-expr arms must match types in G1`
- `emit error[E0200]: index needs array or str`
- `emit error[E0201]: index needs i64`
- `emit error[E0202]: lambda arg mismatch calling`
- `emit error[E0203]: lambda cannot assign to captured`
- `emit error[E0204]: lambda cannot capture unknown`
- `emit error[E0205]: lambda cannot leave its function`
- `emit error[E0206]: lambda must be bound`
- `emit error[E0207]: lambdas cannot be passed as values`
- `emit error[E0208]: len needs array, str or map`
- `emit error[E0209]: main must return i64 in G2`
- `emit error[E0210]: main takes no params in G2`
- `emit error[E0211]: map index needs a str key (map keys are strings)`
- `emit error[E0212]: map keys need str in G6`
- `emit error[E0213]: map values need i64 in G6`
- `emit error[E0214]: match arm enum mismatch`
- `emit error[E0215]: match arms must agree in type`
- `emit error[E0216]: match needs an enum in G5`
- `emit error[E0217]: match needs arms`
- `emit error[E0218]: method arg mismatch`
- `emit error[E0219]: method receiver type mismatch`
- `emit error[E0220]: missing arg, no default calling`
- `emit error[E0221]: missing arg, no default calling lambda`
- `emit error[E0222]: mixed-type binop needs conversion`
- `emit error[E0223]: no type in scope implements`
- `emit error[E0224]: node needs a later group`
- `emit error[E0225]: non-exhaustive match`
- `emit error[E0226]: not a struct`
- `emit error[E0227]: not an enum`
- `emit error[E0228]: only extern C supported`
- `emit error[E0229]: only i64 arrays in G3`
- `emit error[E0230]: only i64 arrays in G7`
- `emit error[E0231]: param after variadic`
- `emit error[E0232]: param type needs G3+`
- `emit error[E0233]: payload count mismatch`
- `emit error[E0234]: payload type mismatch`
- `emit error[E0235]: print is reserved`
- `emit error[E0236]: print needs i64/f64/str in G3`
- `emit error[E0237]: ragged tensor literal: all rows must have the same length`
- `emit error[E0238]: range bounds need i64`
- `emit error[E0239]: ref needs a variable in G7`
- `emit error[E0240]: ref of undeclared`
- `emit error[E0241]: return annotation needed in G2`
- `emit error[E0242]: return type mismatch`
- `emit error[E0243]: return type needs G3+`
- `emit error[E0244]: runtime arg mismatch calling`
- `emit error[E0245]: slice bounds need i64`
- `emit error[E0246]: slice needs an array in G7`
- `emit error[E0247]: spread needs an array`
- `emit error[E0248]: strings support only +/==/!= in G6`
- `emit error[E0249]: struct literal missing fields`
- `emit error[E0250]: template parameter needs a closing paren`
- `emit error[E0251]: template parameter needs a name`
- `emit error[E0252]: template parameters need a closing paren`
- `emit error[E0253]: tensor literal is neither a flat f64 list nor equal-length f64 rows`
- `emit error[E0254]: tensor literal needs at least one f64 element`
- `emit error[E0255]: tensor literal rows must be non-empty all-numeric f64 literal lists`
- `emit error[E0256]: tensor literals support only f64 numbers and rank<=2 rows`
- `emit error[E0257]: too few args calling runtime`
- `emit error[E0258]: too many args calling`
- `emit error[E0259]: too many args calling lambda`
- `emit error[E0260]: too many args calling runtime`
- `emit error[E0261]: too many payload args`
- `emit error[E0262]: trait method needs a closing paren`
- `emit error[E0263]: trait method needs a name`
- `emit error[E0264]: undefined variable`
- `emit error[E0265]: unknown enum`
- `emit error[E0266]: unknown field`
- `emit error[E0267]: unknown function`
- `emit error[E0268]: unknown map method`
- `emit error[E0269]: unknown method`
- `emit error[E0270]: unknown name in global init`
- `emit error[E0271]: unknown str method`
- `emit error[E0272]: unknown struct`
- `emit error[E0273]: unknown variant`
- `emit error[E0274]: unscanned declaration`
- `emit error[E0275]: unscanned function`
- `emit error[E0276]: unwrap needs an enum`
- `emit error[E0277]: unwrap needs uniform single payload`
- `emit error[E0278]: variadic must be last param`
- `emit error[E0279]: variant takes no payload`
- `emit error[E0280]: void binop operand`
- `emit error[E0281]: void global init`
- `emit error[E0282]: void initializer`
- `emit error[E0283]: void variadic arg`
- `emit error[E0284]: cannot read the signature file`
- `emit error[E0285]: not a signature file (expected flint-signatures 1)`
- `emit error[E0286]: malformed signature line`
- `emit error[E0287]: signature file parameter count disagrees`
- `emit error[E0288]: signature file type parameters disagree`
- `emit error[E0289]: signature file type is not supported here`

Total: 145 messages.
