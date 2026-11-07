# catch / throw / defer

Reference (atmos-lang/atmos, `doc/manual.md` "Exceptions", "Defer";
runtime `lua-atmos/atmos/atmos/run.lua` `M.catch`, `M.throw`, `M.defer`;
tests `tst/exec.lua` "CATCH / THROW", "DEFER").

## Semantics (from the reference)

- `throw(e1, ..., en)` aborts enclosing blocks up to a matching `catch`.
  In the reference it packs the values in a table with a metatable; here
  the values travel on the stack (see errors.md): `throw` is the base
  function already implemented, `throw :X` and `throw :X [..]` are plain
  juxtaposed calls. Nothing to do in the parser.
- `catch CND { BODY }` evaluates to `true, body values` when nothing is
  thrown, and to `false, e1, ..., en` when it catches; otherwise the
  error propagates unchanged (all its values).
- `CND` is evaluated before the body. Matching, on the error values:
  - `true`: catches anything (including Lua `error` strings, memory
    errors);
  - `false`: catches nothing;
  - a function: called with the error values; matches if its first
    result is `true`, and the *other results* replace the error values;
  - otherwise: matches if `e1 ?? CND`: `e1 == CND`, or `type(e1) == CND`,
    or (`e1` a string tag and `CND` a tag that is `e1` or a prefix of it
    at a `.` boundary: `X` catches `X.Y`), or (`e1` a table whose `tag`
    field matches that way). Tags are strings without the colon
    (`:X` is `"X"`).
- `defer { BODY }` runs `BODY` when the enclosing block terminates,
  normally or by error/break/escape/return; several `defer`s run in
  reverse order; nested blocks first. The body is a function (no
  `break`/`return` out of it). An error inside a `defer` replaces the
  error in flight (Lua `__close` semantics).

Not in scope here: `??`/`!?` as operators (later, same `is` function),
the `catch` stack-trace output of the reference, tasks interactions
(`abort`, errors crossing `await`).

## Mechanism for `catch`

Status: implemented with option (A) (`luaB_catch` in `lbaselib.c`,
`catchexp`/`deferstat` in `lparser.c`, function values closable in
`lfunc.c`); (B) stays as the planned replacement when `break`/`escape`/
`return` across `catch` are needed. `testes/catch.atm` passes; its
"jumps across catch" cases are compile errors for now.

Two bugs of the Atmos front end surfaced with `defer` in value blocks
and were fixed on the way: (1) `val x = <expr>` predeclared `x` before
parsing the expression (as Lua does), so locals declared by a block
inside the expression got shifted registers; the names are now
declared after the initializer. (2) `OP_CLOSE` placed the `__close`
call right above the closed variable, clobbering the temporary that
holds a block's value; it now closes "at top" like `OP_RETURN`.
The debug build (`ltests.h`, which stresses register allocation with
`MAXINDEXRK 1`) found two more: (3) the value register of a compound
expression was not reserved (`value_result`), so a function ending in
`if`/`do`/`loop` could have a frame one slot too small; (4) tagged
constructors did not free the register of the `tag` key when it did not
fit as a constant. Still open (TODO.md): a compound expression used
while temporaries are live (`print(do { 1 })`, `f() + do { ... }`)
takes its value register at the local level, below the temporaries.

Two options.

### (A) closure + `pcall` (what the reference does)

`catch CND { BODY }` compiles to a call of a hidden C function
`catch(CND, function () BODY end)` that does `pcall` and the matching.

- Small: parser only (~80 lines) plus one C function; uses `lua_pcallk`
  so it is yieldable inside coroutines (tasks).
- Costs a closure per execution; the body can read/write outer locals
  as upvalues (fine, `val`/`var` work).
- `break`, `until`/`while`, `escape`, `return` inside the body cannot
  cross the closure: they are compiled as jumps here. The reference
  solves it by turning them into throws (`atm-loop`, `atm-func`,
  `atm-do`) caught by the loop/function/`do`, which it documents as a
  known "dynamic scope" bug. We would either reject them inside `catch`
  (compile error) or reproduce the throw scheme.
- Message handlers of an enclosing `xpcall` do not see caught errors
  (`pcall` resets `errfunc`), as expected.

### (B) try regions in the VM (recommended)

Three opcodes and a per-thread stack of active regions.

- `OP_TRY A Bx`: push region {ci, handler pc, stack level A, saved
  `errfunc`, `nCcalls`, `allowhook`}; set `L->errfunc = 0`. The region
  records that frame `ci` is in a `catch`.
- `OP_ENDTRY A`: pop the region (normal end). Also emitted before a
  `break`/`escape`/`return` that leaves the block, like `OP_CLOSE` is
  for `pin`; a `return` from the frame pops all regions of the frame.
- `OP_CATCH A`: at the handler, with `CND` in `R[A]` and the error
  values at `R[A+1] .. top` (open, like call results): apply the
  matching rules; on match set `R[A] = false` and keep the values; else
  re-throw all the values (`luaD_thrown`). Calling a function condition
  uses `luaD_call` (yieldable).
- Catching: `luaV_execute` keeps one `lua_longjmp` per C invocation. The
  first `OP_TRY` of an invocation chains it in `L->errorJmp` with a
  `setjmp` right there (the opcode runs inside the invocation's C frame,
  so the buffer is valid); the last `OP_ENDTRY` unchains it. On a
  longjmp into it: scan regions from the top down to the invocation's
  fresh frame; the first one found gives the frame to continue in:
  close upvalues/tbc down to its level with the error status
  (`luaD_closeprotected`, which may run `defer`s and replace the error),
  move the error values to `R[A+1]` (`luaD_seterrorobj`), set
  `L->ci`, `savedpc = handler`, restore `nCcalls`/`allowhook`/`errfunc`,
  `luaD_shrinkstack`, reload `base`/`pc` and continue the loop. No
  region in this invocation: unchain and re-throw to the previous
  handler (an outer invocation's buffer or a `pcall`/`resume`).
- Yields: a yield inside the body longjmps with `LUA_YIELD` into our
  buffer: unchain and re-throw it; the regions stay recorded in the
  thread. When `luaV_execute` is re-entered for a frame that has
  regions (flag `CIST_TRY` in `callstatus`), it chains the buffer again
  before running. So `await` inside `catch` works in tasks.
- C boundaries: a `catch` inside a callback called from C (`sort`,
  `gsub`, iterators) works, because the buffer belongs to the
  callback's own `luaV_execute` invocation, which is still alive; the
  C frames above it are discarded exactly as `pcall` does.
- `break`/`escape`/`return`/`until`/`while` work across `catch`
  (same frame, plain jumps plus `OP_ENDTRY`).
- Cost: a `setjmp` per `catch` entry (as `pcall` has), no closure, no
  table. Size: ~300 lines (`lopcodes`, `lvm.c`, `ldo.c`, `lstate.h`,
  `lparser.c`, `ldebug.c` for the new opcodes).
- Caveats: locals of `luaV_execute` are reloaded from `ci` after the
  longjmp (`base`, `pc`, `k`, `cl`, `trap`), the same way the `trap`
  paths do; the region stack is per thread (`L->tryregions`, grown on
  demand) and popped when frames are unwound by errors or
  `coroutine.close` (`luaE_resetthread` drops all regions).

Recommendation: (B). `catch` becomes a real block like `do :X {}`:
`break`/`escape`/`return` work through it, no allocation, and the
error values stay on the stack end to end. (A) is a quick fallback
with the known reference limitation.

## `defer`

`defer { BODY }` compiles to a closure stored in a hidden to-be-closed
register (`OP_TBC`, as `pin`). Core change (two lines in `lfunc.c`): a
to-be-closed value that is a *function* is called on close (as
`f(nil, e1)` with the Lua `__close` convention, or with no arguments),
instead of requiring a `__close` metamethod. No table or metatable per
`defer`; the closure is the only allocation (as in the reference).
`debug.getlocal` does not see the hidden slot (register without a
name, like the value registers), and `break`/`escape`/`return` and
errors close it through the existing `OP_CLOSE`/`luaF_close` paths, in
reverse order with nested blocks first.

Open point: should the defer body see the error values (`e1` as in
`__close`, all of them, or none, as in the reference)? The tests assume
none are needed (the body takes no parameters; extra arguments are
ignored by a lambda with no parameters, so passing `e1` costs nothing).

## Tests

`testes/catch.atm`: the reference "CATCH / THROW" and "DEFER" cases
(output captured through a local `print`), plus: tag hierarchy, values
through `catch` (n values, nil holes), function conditions replacing
the values, re-throw of non-matching errors with all values, `catch`
around Lua `error` strings and runtime errors, `defer` on
`break`/`escape`/`return`/error and its order, `defer` errors replacing
the error, `pin` and `defer` interleaving, `catch` with yields inside
coroutines, `catch` inside a C callback, and `break`/`escape`/`return`
crossing `catch` (the last group only holds with option (B)).
