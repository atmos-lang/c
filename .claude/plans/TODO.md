# TODO

## 1. Changes to coroutines

See [coroutine.md](coroutine.md) for the full items and discussion.

- a. `coroutine.error(co, ...)`: done (raises at the pending yield).
- b. Close a normal coroutine: not needed (see coroutine.md).
- c. Bounded stacks; "stackless" coroutines and tasks (yield only at the
  prototype level, shallow frame kept while suspended).

## 2. Dropped from the Lua test suite (testes/)

The Lua 5.5 test suite was converted to Atmos syntax (`testes/*.atm`) and
the `.lua` originals were removed. Dropped, with the reason:

- `goto.lua`: Atmos has no `goto`/labels. Fragments elsewhere were
  rewritten with `do :tag { escape(:tag) }`, `loop { until() }`, etc.
- `bitwise.lua`, `bwcoercion.lua`: Atmos has no bitwise operators
  (`& | ~ << >> ~`). Bitwise fragments in other files (metamethod yields
  in coroutine, constant folding in code, `& 0xFF` masks) were dropped or
  rewritten with `//`, `%` and `math.tointeger(2 ** n)`.
- Lua-only syntax tests inside other files: `<const>`/`<close>` attribute
  errors, `global` declarations (Lua 5.5), long brackets `[[ ]]`/`[==[`,
  named varargs `...t` (became `table.pack(...)`), `local a, b <close>`
  "multiple to-be-closed" error, `return;;`, label errors.
- Tests that depend on Lua 5.5 reference-implementation details that
  Atmos changes: `string.dump` of functions with upvalues is kept (works
  natively), but one `b.aloc == 3` memory-block count for the implicit
  vararg table was dropped.
- `all.atm` no longer lists goto/bitwise; `_VERSION` is still "Lua 5.5".
- The main chunk runs as a task (a coroutine under `atmos.loop`), so
  `coroutine.atm` no longer asserts that the top level is the main
  thread / not yieldable, nor that `coroutine.close(main)` fails;
  `errors.atm` dropped "yield outside a coroutine" (a raw yield at the
  top level is an `await` nothing wakes); `main.atm` checks `__tostring`
  of error objects without `debug.getinfo(4)` (the chunk's frames are
  in another thread).
- Debug build (`ltests.h`) failures that predate tasks (checked with a
  build of the previous commit), not investigated: `db.atm:669` (count
  hook counts 1018 > 1012 instructions), `coroutine.atm` "yields in line
  hook" (`T.sethook("setglobal X; yield 0", "l", 0)`), `main.atm` REPL
  checks (the test build takes stdin for a tty and enters interactive
  mode). The debug build is used for the task and expression files.

See `tools/README.md` for the conversion rules and the language semantics
the converted tests assume.

## 3. Expression-based language

See [expressions.md](expressions.md): how statement values are
implemented (value register, no hidden locals, no block closures), what
the converted Lua suite does and does not exercise, and the open points
(functions ending in a call return its first value; truncation to one
value; `match`/`where`/`catch`/pipes pending).

## 4. Errors with multiple values

Done: see [errors.md](errors.md) and [catch.md](catch.md) (`catch`,
`throw`, `defer`; `catch` is an inline block under a VM try region,
option (B)). Errors carry n values on the stack
(`throw(...)`, `lua_errorn`); `pcall`/`xpcall`/`coroutine.resume`/
`coroutine.close` return them, handlers receive and return n values,
`__close` gets value 1. Tests: `testes/throw.atm`. Next: `catch`/`throw`
syntax and `defer` in the parser on top of it.

## 5. `catch` as VM try regions (option B): done

`OP_TRY`/`OP_ENDTRY`/`OP_CATCH`/`OP_CATCHRES`, a per-thread region
stack recovered through `lua_resume`'s `precover` (yieldable threads)
or a nested protected call (non-yieldable). `catch` and `watching`
bodies are inline blocks: `break`/`until`/`while`/`escape`/`return`
cross them. Jumps out of task bodies (`spawn { }`, `par` branches,
`toggle on`) are throws of status `LUA_ERRJUMP` dispatched by a region
of the owning block to the loop, `do :X` or function they target
(checked at compile time). Tests: `testes/catch.atm`, `testes/jumps.atm`,
`ref/throw.atm`. See catch.md for the implementation notes.

## 6. Compound expressions with live temporaries (done)

`do`/`if`/`ifs`/`loop` used as expressions took their value register at
the level of the active locals, overwriting live temporaries
(`print(do { 1 })`, `f() + do { ... }`). Fixed: `compoundexp` in
`lparser.c` declares hidden `(temp)` locals covering the temporaries
while the statement is parsed (see expressions.md). Tests:
`testes/exprs.atm`.

## 7. Operators, pipes, match (done); where (pending)

See [operators.md](operators.md): `??`/`!?`, `=>=`/`=<=`/`===`/`=!=`,
`?>`/`!>` as C functions named by the operator in `_G`; `++` on tables
in the VM; pipes, `match` and lambda operator shortcuts in the parser.
Tests: `testes/ops.atm`. `where` deferred (see the plan: needs either a
scan-ahead or a code rollback; `do { val ...; e }` covers it).

## 8. Tasks

See [tasks.md](tasks.md): model from the reference, design (C runtime
`latmlib.c`, parser desugaring to calls, await pattern constructors as
non-identifier globals), three stages, all done: stage 1 (tables
instead of userdata, `OP_PINCHK`/`__pinchk` instead of pin-check calls,
sweep only in `atmos.gc`/`atmos.loop`, exit sentinel, SIGINT on the
running task), stage 2a (`par`, `par :any`, `par :all`, `watching`,
`loop on`, `<PAT>` combinators), stage 2b (clock numerals, timers and
ticks, `toggle` both forms, `atmos.next/stop`), stage 3 (`atmos.env`,
the loop driving the steps and quitting the envs, `atmos.start`,
`atmos.stop`, `__atmos`, the built-in clock env `atmos.env.clock`;
tests `testes/envs.atm`). Tests: `testes/ref/*.atm` generated by
`tools/tst2atm.py` from the reference `tst/*.lua` (copied in
`testes/ref/tst/`), run by `testes/ref/harness.atm`. Deferred: `++` on
pools, `escape` across tasks (a throw in the reference), `thread` (out
of scope), the reference's error traces, streams (`atmos.streams`, a
Lua module in the reference: `ref/streams.atm` cases disabled).

## 9. Tail calls

- Tail call optimization is bad for error messages / tracebacks
  (`(...tail calls...)`). Could it be disabled? Or made explicit
  (opt-in syntax)?
