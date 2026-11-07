# Expression-based language

Atmos is expression based: "a sequence of expressions evaluates to its
last expression" (manual, Chunks/Blocks). `if`, `ifs`, `match`, `do`,
`loop`, blocks and function bodies all have values:

```
val v = do { print(:ok); 1 }       ;; 1
val m = if x > y => x => y
val r = loop { ... until(f()) }    ;; value of f()
func add (x, y) { x + y }          ;; returns x + y
```

## Status (branch `second`)

Implemented in `lparser.c`, on top of Lua's unchanged code generator:

- every statement parser returns an `expdesc` for its value; `statlist`
  discards all but the last one (`discard`: calls get no results, other
  expressions are evaluated);
- function bodies and chunks return the value of their last statement
  (`tailreturn`): one value, or all values of a parenthesized tuple
  (`(a, b)`); explicit `return(...)` keeps Lua's semantics (tail calls,
  multiple results);
- `if`/`ifs`/`do`/`loop` deliver their value through a *value register*:
  `reg = luaY_nvarstack(fs)` at statement start, the same slot the first
  local of the inner block reuses. Branch tails, `break(v)`, `until(c)`,
  `while(c)` and `escape(:tag, v)` store into it at their very end
  (`store_value`), after `closeupto` emitted an `OP_CLOSE` if an enclosing
  block captured upvalues or holds to-be-closed variables (so closures do
  not see the overwritten slot). `value_result` then hands the register to
  the caller as an ordinary temporary (`VNONRELOC reg`, `freereg = reg+1`).
  A loop writes `nil` into the register on its normal exit; an `if`
  without `else` stores `nil`;
- declarations (`val`/`var`/`pin`), `set`, `func NAME` statements,
  `return`, `break`, `escape` evaluate to `nil` (void).

Design goals met: no hidden local variables (an earlier attempt used a
hidden `(value)` local; it broke `debug.getlocal`, which maps the n-th
visible local name to register n-1), no closures for blocks (the Lua
reference implementation compiles blocks into closures, which breaks
`...`, `debug.getlocal`, tail calls and `return` semantics), no new
opcodes or runtime support. About 40 lines of Atmos-specific code.

## Relation to Lua's "results at the top of the stack"

The value register is the same mechanism Lua uses for call results: a call
leaves its results at the slot of the called function, which is `freereg`
at the call, and `reg = luaY_nvarstack(fs)` at statement start is exactly
that slot. The block's locals live above it and die at its end, and the
value stays at `reg`, like a callee frame vanishing and leaving results
at the func slot; `value_result` describes it as `VNONRELOC reg`, as the
parser describes a call adjusted to one result.

What is not supported is more than one value:

- fixed count (`do { (1, 2) }`, `break(a, b)`): feasible by storing n
  values at `reg..reg+n-1` and reporting n, if every exit of the block
  agrees on n (or pads with nils to the maximum); small extension of
  `store_value`/`value_result`. Not done because the reference truncates
  to one value too.
- open count (`break(f())` with all results of `f`): only `OP_CALL`,
  `OP_VARARG`, `OP_RETURN`, `OP_SETLIST` set or read `L->top`; an exit in
  the middle of a block cannot set `L->top`, so this needs a new opcode
  or a runtime helper. Left out.

## Open points

- Only one test in the converted Lua suite uses a compound statement as a
  value (`testes/math.atm:24`, written during the conversion); the real
  coverage will come from the Atmos tests (`atmos-lang/atmos/tst`).
- A function whose body ends in a call returns that call's first value
  (`func () { assert(x) }` returns `true`), unlike Lua where such a
  function returns nothing. Iterator functions and callbacks that must
  return nothing need an explicit `nil`. Alternatives to discuss:
  (a) keep as is (reference behaviour); (b) make the tail call a real tail
  call returning all values (`return(f())` semantics) — changes arity;
  (c) treat a statement-like last expression as void.
- Truncation: a tail expression yields one value; only a parenthesized
  tuple yields several. The reference does the same (`return (e)`).
- `match` and `where` are not implemented yet; both are expressions.
- `defer` and `catch` must also produce values when implemented
  (`catch` evaluates to the caught error or the block value).
- Pipes (`->`, `-->`) are expressions as well.
