# C++ compiler diagnostic catalog

Every diagnostic the shipped compiler (`flintc`, `src/main.cpp`) can print,
as a PATTERN: literal text with `<...>` where a value is interpolated. A
user searching for a message finds the pattern that starts like their
message.

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

- `' <...> ' expects <...> argument(s), got <...>`
- `' <...> ' expects <...> type argument(s), got <...>`
- `' <...> ' is not defined for structs or enums; use == or !=`
- `'parallel for' was removed: it could not capture variables and had no result channel. Use a plain 'for' loop, or move the work into a Rust runtime function such as flint_parallel_reduce`
- `<...> () needs one string arg`
- `<...> <...> '`
- `<...> <...> ' has no field ' <...> '`
- `<...> <...> ' in struct literal`
- `<...> <...> ' while borrowed`
- `a fn type needs an explicit return type (fn(...) -> Ret); without it an indirect call cannot know what the callee returns`
- `a lambda has no '-> Ret' form: write an expression body (|x| x + 1); use a named fn when you need a return annotation`
- `argument <...> of ' <...> ' has type <...> <...> <...>`
- `array index needs i64 (got str; map keys are strings)`
- `array literals must hold i64 values, but element <...> is <...> (arrays of other types are not supported yet)`
- `as f64 needs an i64 or f64 value`
- `as i64 needs an i64 or f64 value`
- `as-cast target must be 'i64' or 'f64'`
- `as-cast to ' <...> ' is not yet supported (only 'i64' and 'f64'; f64->i64 truncates toward zero)`
- `as-casts support only 'i64' and 'f64' (got ' <...> ')`
- `break outside loop`
- `call to unknown function ' <...> '`
- `can only reference variables`
- `cannot access field ' <...> ' on non-struct type`
- `cannot assign to ' <...> ' while borrowed`
- `cannot assign to immutable ' <...> '`
- `cannot assign to immutable global ' <...> '`
- `cannot bind a function value to a non-fn type`
- `cannot bind a lambda value to a non-fn type`
- `cannot bind function ' <...> ': declared signature differs from the function's signature`
- `cannot bind lambda ' <...> ': declared signature differs from the lambda's fn type`
- `cannot borrow moved variable ' <...> '`
- `cannot call lambda ' <...> ' from a different function`
- `cannot capture ' <...> '`
- `cannot capture ' <...> ' while borrowed`
- `cannot capture lambda ' <...> '`
- `cannot capture moved variable ' <...> '`
- `cannot capture unknown variable ' <...> '`
- `cannot compare enum ' <...> ' with a value of a different type`
- `cannot infer type parameter ' <...> ' for ' <...> '; the argument's type could not be determined`
- `cannot move ' <...> ' while borrowed`
- `cannot move while borrowed`
- `cannot pass a lambda to parameter ' <...> ' of ' <...> ': the parameter is not a fn type`
- `cannot rebind ' <...> ' as lambda`
- `cannot return a function value: it would escape its defining frame`
- `cannot return a lambda: it would escape its defining frame`
- `cannot store a function value into global ' <...> ': it would escape its defining frame`
- `cannot store a lambda into global ' <...> ': it would escape its defining frame`
- `continue outside loop`
- `empty lambda body`
- `expected '<' after '::' before explicit type arguments`
- `expected '\"C\"' after extern`
- `expected 'fn' or 'def' in trait ' <...> ' body`
- `expected a top-level statement or a 'fn name(params) { body }' definition; '(name) (' here does not look like either`
- `expected a top-level statement or a 'fn name(params) { body }' definition; 'name name' here does not look like either`
- `expected expression`
- `expected expression after '='`
- `expected module path (string literal or identifier)`
- `expected type (i64, str, bool, [T], &T, struct name, or enum name)`
- `expected value in map literal`
- `explicit type arguments are only allowed on a generic function; ' <...> ' is not generic`
- `failed to specialize ' <...> '`
- `fao_matmul not linked`
- `flint_array_data needs 1 arg`
- `flint_array_get needs 2 args`
- `flint_array_get needs an array`
- `flint_array_get_ptr needs 2 args`
- `flint_array_set needs 3 args`
- `flint_array_set needs an array`
- `flint_array_set_ptr needs 3 args`
- `flint_array_slice not linked`
- `flint_map_len not linked`
- `flint_str_concat not linked`
- `flint_str_concat/flint_f64_to_string not linked`
- `flint_str_concat/flint_i64_to_string not linked`
- `flint_str_length not linked`
- `flint_thread_spawn first arg must be a string literal`
- `flint_thread_spawn first arg must be a string literal (function name)`
- `flint_thread_spawn needs 2 args: func_name, arg`
- `flint_thread_spawn: runtime function 'flint_thread_create' not found`
- `flint_thread_spawn: runtime function not found`
- `flint_thread_spawn: unknown function ' <...> '`
- `for-in needs an array or str collection`
- `function ' <...> ' cannot be used as a value here; bind a lambda or pass it in a direct call`
- `function value ' <...> ' argument type mismatch (parameter <...> )`
- `function value ' <...> ' expects <...> argument(s), got <...>`
- `generic function ' <...> ' cannot be used as a value yet; instantiate it first`
- `if branches must yield the same type`
- `index assignment requires an array variable`
- `lambda ' <...> ' argument type mismatch`
- `lambda ' <...> ' expects <...> argument(s), got <...>`
- `lambda body type mismatch`
- `lambda does not match the fn type of parameter ' <...> ' of ' <...> '`
- `lambdas are not supported in generic functions`
- `len needs array, str or map`
- `len() needs 1 argument`
- `map index needs a str key (map keys are strings)`
- `map runtime not registered`
- `map values must be i64 (got f64)`
- `match arms must yield the same type`
- `method ' <...> ' needs <...> argument(s)`
- `no type in scope implements ' <...> '`
- ``numeric dtype suffix ' <...> ' is not yet supported (tensor literals are f64-only; convert with `as f64` / `as i64`)``
- `pattern <...> . <...> binds <...> value(s) but the variant has <...>`
- `print() needs an arg`
- `print() of unsupported type`
- `py_eval() needs one string arg`
- `py_eval_int not declared`
- `ragged tensor literal: all rows must have the same length`
- `range() needs 1 or 2 arguments`
- `range() takes 1 or 2 arguments: range(n) or range(start, end)`
- `right side of '|>' must be a function call or identifier`
- `struct ' <...> ' has no field ' <...> '`
- `struct type not found ' <...> '`
- `tensor literal needs at least one f64 element (add a decimal point); nested i64 arrays are not supported`
- `tensor literals support only rank 1 and 2 (f64)`
- `tensor runtime not registered`
- `tensor values cannot be indexed with []; use flt_get(t, i)`
- `tensor-shaped array literal is neither a flat f64 list nor equal-length f64 rows (use bare f64 literals, or build dynamic tensors with flint_create_1d/2d + flt_set)`
- `the explicit type argument for ' <...> ' must be a concrete type`
- `trait ' <...> ' is already declared`
- `trait method ' <...> ' is declared twice in ' <...> '`
- `undefined variable ' <...> '`
- `unknown enum ' <...> '`
- `unknown enum in match`
- `unknown method ' <...> ' for map (available: get, set, has, len, keys)`
- `unknown method ' <...> ' for str (available: len, upper, lower, trim, reverse, get, repeat, startswith, endswith, indexof, replace)`
- `unknown operator ' <...> '`
- `unknown struct ' <...> '`
- `unknown type ' <...> '`
- `unknown variable ' <...> ' in lambda body`
- `unknown variant ' <...> '`
- `unrecognized top-level construct`
- `use of moved variable ' <...> '`
- `variable ' <...> ' already declared`
- `variable ' <...> ' not declared`

Total: 135 diagnostics.
