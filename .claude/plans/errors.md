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

Decision pending. Options: (a) keep one value and pack in a table in
`throw`; (b) extend the core to n values as sketched above, with value 1
as the Lua-visible error object.
