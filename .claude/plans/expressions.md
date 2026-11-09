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

## Several values (design, implemented after catch option B)

The reference gives a block all the values of its exit: `val a, b = do
:X { escape(:X, 10, 20) }` is `10, 20`, `do { (1, 2) }` is `1, 2`
(manual: "the whole block being escaped evaluates to the other
arguments"). Here every exit truncated, keeping the *last* extra value
(`break(1, 2)` gave `2`). Design, with a static arity per exit and no
runtime count:

- Each exit of a compound statement (`do`/`loop`/`if`/`ifs`/`match`/
  `test`: the normal end of the body, `break`/`until`/`while` values,
  `escape` values, branch ends) yields k values, k known at compile
  time: 0 (`escape(:X)`, `break()`, a loop's normal exit, `if` without
  `else`), 1, or the n values of a parenthesized tuple or of an
  argument list. The values are evaluated first (above the block's
  locals) and then moved to `reg..reg+k-1`, `reg` being the value
  register of the construct; the construct records the arities seen
  (`ResDesc.arities`, a bit set; at most 15 values).
- Exits are grouped by arity: `break`/`escape` jump to a label named
  per arity (`break#k`, `tag#k`, created at the block end as the
  single `break`/tag labels were) and branch ends keep one jump list
  per arity. At the end of the construct the maximum arity `max` is
  known (at least 1); the labels/lists are resolved in increasing arity
  and each one is followed by `OP_LOADNIL reg+k .. reg+max-1` (nil
  padding), falling through into the next (re-padding is harmless), so
  that every exit leaves exactly `max` values. A construct whose exits
  are all single-valued (the common case) gets no padding and stays a
  `VNONRELOC reg` expression, exactly as before.
- With `max > 1` the construct ends with `OP_BLOCKRES reg B C` (`B-1 =
  max` values available at `reg..`, `C-1` wanted, `C = 0` open: `L->top
  = reg + max`), and the expression is `VCALL`-like, so `val a, b = do
  ...`, `print(do ...)` and `f(loop ...)` get all values through the
  usual `luaK_setreturns`/`luaK_setoneret`; a single-value context takes
  `reg`. (`catch` keeps its own `OP_CATCHRES`: its error path has a
  dynamic count.)
- Statement position discards (`discard` sets `C = 1`). A function body
  ending in a multi-valued block returns all its values (`tailreturn`
  truncates calls, not blocks: a block's arity is explicit).
- Jumps out of task bodies carry one value to the owner's loop/block
  (the reference's `break(v)`), unchanged.

Status: implemented (`ResDesc`, `storevalues`, `finishexits`,
`finishres`, `OP_BLOCKRES` in `lparser.c`/`lvm.c`); tests in
`testes/exprs.atm` ("several values") and the reference `do 2`/`do 3`.
Notes: an exit of the same arity as the normal end joins it before its
padding; an arrow branch `=> (a, b)` yields the tuple (`branchexpr`);
a block in a non-final argument position gives one value, like a call;
`(...)` alone as a function body is one value (parenthesized).

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
  tuple or a block with several values yields several. The reference
  does the same (`return (e)`).
- `match` and `where` are not implemented yet; both are expressions.
- `defer` and `catch` must also produce values when implemented
  (`catch` evaluates to the caught error or the block value).
- Pipes (`->`, `-->`) are expressions as well.

## Compound expressions with live temporaries

A compound statement used as an expression (`f(do { ... })`,
`x() + if c { a } else { b }`, items of a constructor, indices) is
parsed while temporaries are live (the function and arguments being
collected, operands already in registers). Its value register and the
locals declared inside are allocated at the level of the active locals
(`luaY_nvarstack`), which is below those temporaries. `compoundexp`
(`lparser.c`) covers the temporaries with hidden locals named `(temp)`
for the duration of the statement, so that `freereg == nvarstack` holds
inside and everything is allocated above them; the hidden locals are
removed afterwards and the value is left just above the temporaries.
They are visible to `debug.getlocal` while the statement runs, like
Lua's `(for state)`, and count toward the 200-locals limit.
Tests: `testes/exprs.atm`.

## Multi-value block results (open)

The reference gives `val a, b = do :X { (10, 20) }` and `escape(:X, 10,
20)` both values (exec.lua "do 2"/"do 3"). Here a block value is one
register, so these keep the first value only. Supporting it needs
results with a run-time count (values at the top of the stack as a
call leaves them), since different paths of a block can produce
different counts. Also, `do { 10 ; 20 }` is a syntax error in the
reference (a bare literal is not a statement there); here it is an
expression statement. Both cases stay disabled in `testes/ref/exec.atm`.
