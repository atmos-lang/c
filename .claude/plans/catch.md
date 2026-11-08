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

Status: (A) was implemented first (`luaB_catch` in `lbaselib.c`,
`catchexp` in `lparser.c`); (B) replaces it: VM try regions (design
below), `watching` inline, jumps across task bodies. `defer` is
unchanged (function values closable in `lfunc.c`).

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

### (B) try regions in the VM (implemented design)

Scope: `catch` as an inline block; `watching P { }` with an inline body;
`break`/`until`/`while`/`escape`/`return` crossing the closures that
are task bodies (`spawn { }`, `par`/`par :any`/`par :all` branches,
`toggle on` bodies) to the owner task's loop, `do :X` block or
function, as the reference does for all closures (there by making every
jump a throw; here only at the task boundary). `defer` bodies stay
functions: no jumps out of them.

Regions. A per-thread stack `L->tryregions` of records {`ci`, `handler`
(pc), `level` (stack index of the first error value), `oldtop`,
`nCcalls`, `allowhook`, `errfunc`, `nested`}. Frames with regions carry
`CIST_TRY`. New opcodes (`lopcodes`, `lvm.c`):

- `OP_TRY A sBx`: pushes a region for this frame with handler `pc +
  sBx`, level `A+1`; saves and zeroes `L->errfunc` (a caught error must
  not run an outer `xpcall` handler). Two modes, decided at run time:
  *recovery* when the thread is yieldable (`yieldable(L)`: every C
  frame below has a continuation, which holds inside tasks), *nested*
  otherwise (main thread, REPL lines, inside callbacks of `sort`,
  `gsub`, metamethods called from C). In nested mode the opcode runs
  the rest of the frame under `luaD_rawrunprotected` with a recursive
  `luaV_execute` that returns at the matching `OP_ENDTRY`/`OP_CATCH`
  (region flag `nested`), as `pcall` does, so `catch` works anywhere
  `pcall` would; yields cannot happen there (non-yieldable).
- `OP_ENDTRY A sBx`: normal end: pops the region, restores `errfunc`,
  `R[A] = true`, `L->top = A+2` (the body value is at `A+1`), jumps to
  `pc + sBx` (the join).
- `OP_CATCH A B`: the handler. The error values are at `A+1..top-1`
  (placed by the recovery) and `L->trystatus` holds the status. B = 0
  (`catch`): the condition is `R[A]`; matching as today (`true`, a
  function called with the values whose other results replace them,
  else `e1 ?? CND`); jump throws (status `LUA_ERRJUMP`) never match;
  on match `R[A] = false`, values at `A+1..`, `L->top` after them; else
  rethrow all values with the original status (`luaD_thrown`). B = 1
  (jump handler of an owning block, see below): matches only jump
  throws; other errors are rethrown.
- `OP_CATCHRES A C`: the join: adjusts the values at `A..top-1` to
  `C-1` results (`C = 0`: keep `L->top`, open), so the parser treats
  the whole `catch` as a `VCALL`-like expression (`luaK_setreturns`
  patches `C`, `luaK_setoneret` gives `R[A]`).

Recovery (`ldo.c`). Errors inside a region in recovery mode unwind to
`lua_resume`'s `luaD_rawrunprotected`; `findpcall` now also stops at a
frame with `CIST_TRY` (whichever is nearer the top: an inner `pcall`
wins over an outer region and vice versa); `precover` sets the status
in the frame (`setcistrecst`) and `unroll` recovers it before
re-entering `luaV_execute`: pop the region, `L->trystatus = status`,
`luaF_close` the to-be-closed variables and upvalues down to `level`
(an error in a `defer` there becomes the new error, found by the next
iteration of `precover` with the next region), move the error values to
`level` (`luaD_seterrorobj`, n values), restore `nCcalls`/`allowhook`/
`errfunc`, set `savedpc` to the handler, `luaD_shrinkstack`. A region
is recoverable only if no C frame without continuation lies between
its frame and the recovery point; `OP_TRY` guarantees it by choosing
nested mode otherwise, so `findpcall` needs no check. Nested mode does
the same recovery in place when `luaD_rawrunprotected` returns an
error. `luaE_resetthread`/`lua_closethread` drop the regions of the
thread; a region is also dropped when its frame returns (the parser
always emits `OP_ENDTRY`/`OP_CATCH` before leaving it, and jumps out of
a region (`break` etc.) pop it with `OP_ENDTRY` emitted by the jump
code, like `OP_CLOSE` for `pin`).

Parser, `catch CND { body }` (register layout: `A` = CND then result,
`A+1` = body value, hidden locals `(cnd)`/`(val)` as `compoundexp`
does, so body locals sit above):

    CND -> R[A]
    OP_TRY A -> handler
    body (block with resreg A+1; its locals >= A+2)
    OP_ENDTRY A -> join
    handler: OP_CATCH A 0
    join: OP_CATCHRES A C      (VCALL-like expression at A)

A `break`/`until`/`while`/`escape`/`return` inside the body is a plain
jump: before it, `OP_ENDTRY` pops the regions of the blocks being left
(tracked in `BlockCnt.region`), and `OP_CLOSE`/`OP_RETURN k` close the
to-be-closed variables as usual. `...` inside the body is now allowed.

`watching P { body }` (inline): `A` = watcher/result, `A+1` = body
value, `A+2` = hidden pin `(watcher)`:

    OP_TRY A -> handler
    R[A] = _ENV["watching.spawn"](P)   (a transparent task: await(P),
                                       then throw(itself, results...))
    (watcher) <close> = R[A]           (aborted when the block ends)
    body (resreg A+1)
    OP_ENDTRY A -> join
    handler: OP_CATCH A 0              (CND = the watcher: e1 == watcher)
    join: OP_CATCHRES A 4; value = R[A] ? R[A+1] : R[A+2]

The watcher's throw reaches the body as an error at its pending `await`
(child error, `lua_resumeerror`) or synchronously during an `emit` of
the body; the region catches it, the body's pins and the watcher are
closed by the unwinding; the value is the event. A `break`/`return`/
`escape` in the body is a plain jump out of the region.

Task-crossing jumps. Inside a task-body closure (`spawn { }`, `par`
branches, `toggle on` bodies; kinds `CB_SPAWN`/`CB_PAR`/`CB_TOGGLE`),
a `break`/`until`/`while` with no enclosing loop in the closure, an
`escape :X` with no enclosing `do :X`, and a `return` compile to a
throw of status `LUA_ERRJUMP` with the values `(kind, [tag,] vals...)`
(`OP_JUMPOUT A B` with `kind` in B, the values at `A..top`), and the
jump is registered in the *owning block*: the block of the enclosing
function where the spawn/par statement is (the block that pins the
task). That block gets a region opened right before the spawn/par call
(`OP_TRY` with `A` below the call, so the hidden pin at `A+1` is closed
by the unwinding) and closed at the block end, with a jump handler
(`OP_CATCH A 1`) that dispatches on `kind` (and the tag for escapes) to
the registered targets: the loop break (values to its result
register, `goto break`), the `do :X` escape, or `OP_RETURN` of the
values; an unmatched jump is rethrown (an outer owning block may own the
target). The error reaches the owner synchronously (a jump before the
task's first `await`, raised by `task_result` in the spawn call) or at
the owner's `await` (`lua_resumeerror`), both inside the region. The
same owning-block region serves every spawn of the block (opened by the
first one). `pcall`/`catch` never intercept jump throws (`catch` by
status; `pcall` passes `LUA_ERRJUMP` on: `finishpcall` rethrows it).
Jumps from a child of a child propagate the same way, level by level.
`escape :X` also accepts the juxtaposed tag form (`escape :X [..]`), as
`throw :X` does.

Not covered, by design: `break` inside a plain function breaking the
caller's loop and `escape` to a missing block as a catchable error (the
reference's "dynamic scope" bugs: compile errors here); jumps out of
`defer` bodies; a `catch` nested in a non-yieldable C callback inside a
task still works (nested mode) but cannot `await` (as any code there).

Decision: (A) was implemented first, matching the reference; (B)
replaces it (this design) to allow jumps across `catch`, `watching`
and task bodies, to remove the closure per `catch`, and because the
stackless coroutines of coroutine.md (c) need inline blocks.
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
