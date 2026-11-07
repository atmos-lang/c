# Errors with multiple values

Question: Lua errors carry a single value (`error(v)`, `pcall` returns
`false, v`, `__close(obj, err)`, message handlers take one argument).
Is there a reason? Could errors carry several values, relying on the
stack like call results do?

## How Lua does it (C sources, `ldo.c`, `ldebug.c`, `lbaselib.c`)

- The error object is *already* passed on the stack: at throw time it is
  the top value of the throwing thread (`L->top - 1`). `luaD_throw` does
  not copy it; it longjmps to the innermost `luaD_rawrunprotected`.
- After the jump, `luaD_seterrorobj(L, status, oldtop)` moves `L->top-1`
  to `oldtop` (the base of the protected call) and sets
  `L->top = oldtop + 1`. `lua_pcall` then returns status and the caller
  finds one value at `oldtop`. The same function serves `lua_pcall`,
  yieldable `pcall` continuations (`finishpcallk`), `lua_resume`
  (`coroutine.resume` returns `false, err`) and `lua_closethread`.
- Special cases replace the object: `LUA_ERRMEM` uses a preregistered
  message, `LUA_ERRERR` "error in error handling", a `nil` error object
  becomes "<no error object>" (`luaG_errormsg`).
- Message handlers (`xpcall`, `lua_pcall` with a handler): `luaG_errormsg`
  calls the handler with exactly one argument and keeps one result
  (`luaD_callnoyield(L, top - 2, 1)`).
- `__close` handlers of to-be-closed variables receive `(obj, err)`.
- Coroutines re-throw the single top value into the main thread when no
  handler exists (`luaD_throw`), and `lua_resume` keeps one error value.
- `error(v, level)`: the second argument is the *level*, so the standard
  function cannot take several values anyway; `lua_error`/`luaL_error`
  (C API) take one.

So there is no deep reason: everything travels on the stack already;
the count `1` is simply hard-wired in `luaD_seterrorobj`, in the message
handler call, in `__close`, in `lua_error`/`lua_pcall`/`lua_resume` and
in `error`.

## What "n values on the stack" would take

- A count travelling with the error (e.g. `L->nerr`, set by the throwing
  side next to the status); `luaD_seterrorobj` moves `n` values to
  `oldtop` and sets `L->top = oldtop + n`.
- New API entries (`lua_errorn(L, n)`; `lua_pcall` already returns
  "whatever is on the stack", so callers would just see n values after a
  non-OK status) and a base function `throw(...)` distinct from
  `error(v, level)`.
- Message handlers would receive n arguments and return n (or be limited
  to the first value); `__close` would receive `(obj, e1, ..., en)` or
  only `e1`; `coroutine.resume` would return `false, e1, ..., en`.
- Memory errors and "error in error handling" keep one value.
- Stack space: the error values must fit in the handler's frame; Lua
  reserves `EXTRA_STACK` for the single object, n values need a check.

Cost: moderate and local to `ldo.c`/`lapi.c`/`lbaselib.c`, but every C
library and host that reads one error value keeps working only if
value 1 stays the "main" object.

## Atmos

- `throw(:X, v1, v2)`: the reference packs everything into one table
  (`{tag, ...}`) and `catch` unpacks it. With n stack values, `throw`
  would allocate nothing: tag and payload travel on the stack like call
  results, and `catch :X { }` evaluates to the extra values. Same shape
  as `escape(:tag, v...)` and `break(v...)`, which already use the stack.
- `catch` must still match on the first value (the tag or tagged table),
  so value 1 is the natural "error object" for Lua interoperability
  (message handlers, `__close`, `pcall` from Lua code).
- `coroutine.error(co, e...)` (see coroutine.md item a) would use the same
  convention.

## Decision

(b): extend the core to n values, with value 1 as the Lua-visible error
object. (Alternative (a), packing a table in `throw`, was discarded.)
Implemented (tests in `testes/throw.atm`); the notes below describe the
implementation. Open points were settled as: handlers get all n values
in and n out; `__close` gets value 1 only; `throw` is a base function
(no longer a reserved word).

## Design notes (from reading the code paths)

### Where the count lives

The sketch above puts the count in a per-thread field (`L->nerr`), set
at throw time and read by `luaD_seterrorobj`. Reading the code shows a
window where that is fragile: between the throw and the final
`luaD_seterrorobj`, Lua runs the `__close` methods of the pending
to-be-closed variables (`luaD_closeprotected` in `luaD_pcall` and
`luaE_resetthread`; `luaF_close` in `finishpcallk` for the yieldable
`pcall` path, which re-runs `finishpcallk` after every yield/error inside
a `__close`). If a `__close` method catches an error of its own
(`pcall` inside `__close`), a per-thread field is overwritten and the
outer error gets the wrong count.

Options:

1. Keep `L->nerr` but save/restore it around every protected call, as
   Lua already does for `errfunc`: a local in `luaD_pcall`, and for the
   yieldable path a few bits of `CallInfo.callstatus` next to
   `CIST_RECST` (which already preserves the error *status* across the
   re-runs of `finishpcallk`). Contained; mirrors existing mechanisms.
   Preferred.
2. Keep the count on the stack as a marker value above the error values
   (self-describing, no thread state). Every reader of "the error object
   is `top - 1`" changes, including dead coroutines closed later by
   `coroutine.close`, whose stack must then keep the marker. Discarded.

Also needed: `luaD_throw` re-throwing into the main thread copies n
values (not one); `luaE_resetthread`/`lua_closethread` leave n values
(the assertion `top == stack + 2` becomes `+ 1 + n`); each
`prepcallclosemth` moves the n values down to `level + 1` and sets
`top = level + 1 + n`.

### Conventions (value 1 is the Lua-visible error object)

- `pcall(f, ...)` → `false, e1, ..., en`; `coroutine.resume` and
  `coroutine.close` likewise (`auxresume`/`luaB_close` move n values
  instead of one).
- Message handlers (`xpcall`, `lua_pcall` with handler) receive all n
  values and whatever they return (`LUA_MULTRET`) becomes the new error
  values; a handler returning nothing gives `"<no error object>"` (as a
  `nil` object does today). Caveat: `xpcall(f, debug.traceback)` reads a
  second error value as the traceback *level*; code that throws several
  values should use its own handler (`lua.c`'s `msghandler` only looks
  at value 1).
- `__close` methods keep receiving `(obj, e1)` only: passing n values
  there would need a stack check where Lua now relies on the fixed
  `EXTRA_STACK` reserve, and nothing in Atmos needs it.
- Memory errors and "error in error handling" (`LUA_ERRMEM`,
  `LUA_ERRERR`) keep one value.
- C API: `lua_errorn(L, n)` raises the top n values; `lua_error` is the
  n = 1 case. `lua_pcall`/`lua_resume` already return "what is on the
  stack". Stack check: the n values must fit in the catching frame
  (`luaD_seterrorobj` moves down, so only the handler call needs
  `luaD_checkstack` for n + 2 slots).
- Base library: new `throw(...)` raising all its arguments (no level, no
  position prefix); `error(v, level)` unchanged.

### Files

`lstate.h` (`nerr` field, status bits), `ldo.c`, `ldebug.c`
(`luaG_errormsg`), `lfunc.c` (`prepcallclosemth`), `lstate.c`
(`luaE_resetthread`, `lua_closethread`), `lapi.c`/`lua.h`
(`lua_errorn`), `lbaselib.c` (`throw`), `lcorolib.c`. The converted test
suite should pass unchanged: every existing error carries one value.

### Implementation notes

- `lua_State.nerr`: number of values of the error being propagated.
  Set by `luaD_thrown(L, status, n)` (`luaD_throw` is the n = 1 case),
  read by `luaD_seterrorobj`, which moves the n top values to `oldtop`.
  `luaD_pcall` saves and restores it like `errfunc`; `precover` stores
  it in `CallInfo.u.c.nerr` and `finishpcallk` restores it before
  closing tbc variables (yieldable `pcall` in coroutines).
- `luaG_errormsgn(L, n)`: moves the values up one slot, calls the
  handler with `LUA_MULTRET`, and the results are the new values (zero
  results or a nil first value become `"<no error object>"`). The base
  pointer is saved as an offset across the call (the stack can move).
- `lua_resume` leaves the n values in place and reports `n` in
  `nresults`; it also pushes a copy of them (in protected mode, the
  stack may grow) because the host moves the first copy out and
  `lua_closethread` must still find the values for `coroutine.close`.
  `resume_error` sets `nresults = 1`.
- `luaE_resetthread` keeps the n values at `stack + 1` and raises the
  base frame top if needed; `resetthread_aux` and `luaD_throwbaselevel`
  re-throw with the current count (not 1). Unhandled errors in a thread
  without handler copy the n values to the main thread.
- API: `lua_errorn(L, n)`; `lua_error` = `lua_errorn(L, 1)`. Base
  `throw(...)` and `errorn(level, ...)` (`error` with the level first,
  position prefix on value 1 when it is a string); `pcall`'s continuation reserves a slot before pushing
  (`lua_pcallk` raises the frame top only to the results);
  `coroutine.resume/wrap/close` move n values with stack checks.
- Invariant for a thread dead by error: `L->nerr` is the number of
  error values at the top of its stack (and at `stack + 1` after
  `lua_closethread`).
- `lua.c`'s message handler returns one value (the traceback of value
  1), so uncaught errors at the top level report value 1 only.
- Tested with the debug build (`ltests.h`): it reallocates the stack on
  every call and checks the frame tops, which caught a stale pointer and
  two missing stack checks.
