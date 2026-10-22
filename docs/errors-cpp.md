# C++ compiler diagnostic catalog

Every diagnostic the shipped compiler (`flintc`, `src/main.cpp`) can print,
as a CODE and a PATTERN: the code is stable across releases (the docs
and the tools link to it), and the pattern is the literal text with
`<...>` where a value is interpolated. A user searching for a message
finds the pattern that starts like their message.

The codes live in the source, next to the diagnostic (`codegenError("E0042",
"...")`) — the same shape as Clang's DiagnosticIDs.td. The table is
`docs/error-codes.csv`, and `tests/test_diagnostic_codes.sh` keeps the two
in sync and checks that a code never changes its meaning.

This catalog is about the C++ compiler. The self-hosted emitter's
diagnostics are catalogued separately in [errors.md](errors.md), and
`tests/test_errors.sh` keeps THAT list in sync. `tests/test_errors_cpp.sh`
keeps this one in sync.

Regenerate: `python3 tools/errors_cpp.py --format md` (or
`bash tests/test_errors_cpp.sh --fix`).

## Fix guidance by family

- **undefined / unknown / undeclared X**: the name is not in scope. Declare
  it first; note that C++ rejects shadowing, so does self-hosted
  resolution. A *function* used in value position needs a lambda or an
  explicit type argument.
- **expects N argument(s), got M**: arity mismatch. Defaults fill trailing
  parameters only.
- **argument K of 'f' has type A, but 'f' expects B**: the argument type
  does not match the parameter. i64 promotes to f64 implicitly; nothing
  else converts. Cast explicitly (`x as f64`).
- **cannot infer type parameter 'T'**: the compiler could not work out the
  type from the argument. State it: `f::<T>(...)`.
- **cannot access field 'x' on non-struct type**: the value is not a
  struct, or the field does not exist.
- **cannot return/store a function value ... would escape its defining
  frame**: a closure (or a named-function value) is bound to the frame that
  created it. Pass it to a callee, do not return it or put it in a global.
- **use of moved variable / cannot move while borrowed**: ownership moved
  at the assignment, or the value is borrowed by a live reference.
- **use of uninitialized variable / not declared**: the binding does not
  exist in this scope.
- **as-cast**: only `i64` and `f64` casts exist; f64 -> i64 truncates
  toward zero and can overflow (use the checked helpers).
- **array literals must hold i64 values**: arrays are i64-only today;
  float literals belong in the tensor/`flt_get` world.
- **a lambda has no '-> Ret' form**: write an expression body
  (`|x| x + 1`); use a named `fn` when a return annotation is needed.
- **'parallel for' was removed**: it could not capture variables and had no
  result channel. Use a plain `for` loop, or move the work into a Rust
  runtime function such as `flint_parallel_reduce`.

## Complete message list

- `E0001  ' <...> ' expects <...> argument(s), got <...>`
- `E0002  ' <...> ' expects <...> type argument(s), got <...>`
- `E0003  ' <...> ' is not defined for structs or enums; use == or !=`
- `E0004  'parallel for' was removed: it could not capture variables and had no result channel. Use a plain 'for' loop, or move the work into a Rust runtime function such as flint_parallel_reduce`
- `E0005  <...> () needs one string arg`
- `E0006  a fn type needs an explicit return type (fn(...) -> Ret); without it an indirect call cannot know what the callee returns`
- `E0007  a lambda has no '-> Ret' form: write an expression body (|x| x + 1); use a named fn when you need a return annotation`
- `E0008  argument <...> of ' <...> ' has type <...> <...> <...>`
- `E0009  array index needs i64 (got str; map keys are strings)`
- `E0010  array literals must hold i64 values, but element <...> is <...> (arrays of other types are not supported yet)`
- `E0011  as f64 needs an i64 or f64 value`
- `E0012  as i64 needs an i64 or f64 value`
- `E0013  as-cast target must be 'i64' or 'f64'`
- `E0014  as-cast to ' <...> ' is not yet supported (only 'i64' and 'f64'; f64->i64 truncates toward zero)`
- `E0015  as-casts support only 'i64' and 'f64' (got ' <...> ')`
- `E0016  break outside loop`
- `E0017  call to unknown function ' <...> '`
- `E0018  can only reference variables`
- `E0019  cannot access field ' <...> ' on non-struct type`
- `E0020  cannot assign to ' <...> ' while borrowed`
- `E0021  cannot assign to immutable ' <...> '`
- `E0022  cannot assign to immutable global ' <...> '`
- `E0023  cannot bind a function value to a non-fn type`
- `E0024  cannot bind function ' <...> ': declared signature differs from the function's signature`
- `E0025  cannot bind function value ' <...> ': declared signature differs from the value's fn type`
- `E0026  cannot borrow lambda ' <...> '`
- `E0027  cannot borrow moved variable ' <...> '`
- `E0028  cannot call lambda ' <...> ' from a different function`
- `E0029  cannot capture ' <...> '`
- `E0030  cannot capture ' <...> ' while borrowed`
- `E0031  cannot capture lambda ' <...> '`
- `E0032  cannot capture moved variable ' <...> '`
- `E0033  cannot capture unknown variable ' <...> '`
- `E0034  cannot compare enum ' <...> ' with a value of a different type`
- `E0035  cannot infer type parameter ' <...> ' for ' <...> '; the argument's type could not be determined`
- `E0036  cannot move ' <...> ' while borrowed`
- `E0037  cannot move while borrowed`
- `E0038  cannot pass a lambda to parameter ' <...> ' of ' <...> ': the parameter is not a fn type`
- `E0039  cannot rebind ' <...> ' as lambda`
- `E0040  cannot rebind lambda ' <...> '`
- `E0041  cannot return a function value: it would escape its defining frame`
- `E0042  cannot return a lambda: it would escape its defining frame`
- `E0043  cannot store a function value into global ' <...> ': it would escape its defining frame`
- `E0044  cannot store a lambda into global ' <...> ': it would escape its defining frame`
- `E0045  continue outside loop`
- `E0046  empty lambda body`
- `E0047  expected '<' after '::' before explicit type arguments`
- `E0048  expected '\"C\"' after extern`
- `E0049  expected 'fn' or 'def' in trait ' <...> ' body`
- `E0050  expected a top-level statement or a 'fn name(params) { body }' definition; '(name) (' here does not look like either`
- `E0051  expected a top-level statement or a 'fn name(params) { body }' definition; 'name name' here does not look like either`
- `E0052  expected expression`
- `E0053  expected expression after '='`
- `E0054  expected module path (string literal or identifier)`
- `E0055  expected type (i64, str, bool, [T], &T, struct name, or enum name)`
- `E0056  expected value in map literal`
- `E0057  explicit type arguments are only allowed on a generic function; ' <...> ' is not generic`
- `E0058  failed to specialize ' <...> '`
- `E0059  fao_matmul not linked`
- `E0060  flint_array_data needs 1 arg`
- `E0061  flint_array_get needs 2 args`
- `E0062  flint_array_get needs an array`
- `E0063  flint_array_get_ptr needs 2 args`
- `E0064  flint_array_set needs 3 args`
- `E0065  flint_array_set needs an array`
- `E0066  flint_array_set_ptr needs 3 args`
- `E0067  flint_array_slice not linked`
- `E0068  flint_map_len not linked`
- `E0069  flint_str_concat not linked`
- `E0070  flint_str_concat/flint_f64_to_string not linked`
- `E0071  flint_str_concat/flint_i64_to_string not linked`
- `E0072  flint_str_length not linked`
- `E0073  flint_thread_spawn first arg must be a string literal`
- `E0074  flint_thread_spawn first arg must be a string literal (function name)`
- `E0075  flint_thread_spawn needs 2 args: func_name, arg`
- `E0076  flint_thread_spawn: runtime function 'flint_thread_create' not found`
- `E0077  flint_thread_spawn: runtime function not found`
- `E0078  flint_thread_spawn: unknown function ' <...> '`
- `E0079  for-in needs an array or str collection`
- `E0080  function ' <...> ' cannot be used as a value here; bind a lambda or pass it in a direct call`
- `E0081  function value ' <...> ' argument type mismatch (parameter <...> )`
- `E0082  function value ' <...> ' expects <...> argument(s), got <...>`
- `E0083  generic function ' <...> ' cannot be used as a value yet; instantiate it first`
- `E0084  if branches must yield the same type`
- `E0085  index assignment requires an array variable`
- `E0086  lambda ' <...> ' argument type mismatch`
- `E0087  lambda ' <...> ' expects <...> argument(s), got <...>`
- `E0088  lambda body type mismatch`
- `E0089  lambda does not match the fn type of parameter ' <...> ' of ' <...> '`
- `E0090  lambdas are not supported in generic functions`
- `E0091  len needs array, str or map`
- `E0092  len() needs 1 argument`
- `E0093  map index needs a str key (map keys are strings)`
- `E0094  map runtime not registered`
- `E0095  map values must be i64 (got f64)`
- `E0096  match arms must yield the same type`
- `E0097  method ' <...> ' needs <...> argument(s)`
- `E0098  missing field ' <...> ' in struct literal`
- `E0099  no type in scope implements ' <...> '`
- ``E0100  numeric dtype suffix ' <...> ' is not yet supported (tensor literals are f64-only; convert with `as f64` / `as i64`)``
- `E0101  pattern <...> . <...> binds <...> value(s) but the variant has <...>`
- `E0102  print() needs an arg`
- `E0103  print() of unsupported type`
- `E0104  py_eval() needs one string arg`
- `E0105  py_eval_int not declared`
- `E0106  ragged tensor literal: all rows must have the same length`
- `E0107  range() needs 1 or 2 arguments`
- `E0108  range() takes 1 or 2 arguments: range(n) or range(start, end)`
- `E0109  right side of '|>' must be a function call or identifier`
- `E0110  struct ' <...> ' has no field ' <...> '`
- `E0111  struct type not found ' <...> '`
- `E0112  tensor literal needs at least one f64 element (add a decimal point); nested i64 arrays are not supported`
- `E0113  tensor literals support only rank 1 and 2 (f64)`
- `E0114  tensor runtime not registered`
- `E0115  tensor values cannot be indexed with []; use flt_get(t, i)`
- `E0116  tensor-shaped array literal is neither a flat f64 list nor equal-length f64 rows (use bare f64 literals, or build dynamic tensors with flint_create_1d/2d + flt_set)`
- `E0117  the explicit type argument for ' <...> ' must be a concrete type`
- `E0118  trait ' <...> ' is already declared`
- `E0119  trait method ' <...> ' is declared twice in ' <...> '`
- `E0120  undefined function ' <...> '`
- `E0121  undefined var ' <...> '`
- `E0122  undefined variable ' <...> '`
- `E0123  unknown enum ' <...> '`
- `E0124  unknown enum in match`
- `E0125  unknown method ' <...> ' for map (available: get, set, has, len, keys)`
- `E0126  unknown method ' <...> ' for str (available: len, upper, lower, trim, reverse, get, repeat, startswith, endswith, indexof, replace)`
- `E0127  unknown operator ' <...> '`
- `E0128  unknown struct ' <...> '`
- `E0129  unknown type ' <...> '`
- `E0130  unknown variable ' <...> ' in lambda body`
- `E0131  unknown variant ' <...> '`
- `E0132  unrecognized top-level construct`
- `E0133  unsupported f64 operator ' <...> '`
- `E0134  use of moved variable ' <...> '`
- `E0135  variable ' <...> ' already declared`
- `E0136  variable ' <...> ' not declared`

Total: 136 diagnostics.
