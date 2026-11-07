# TODO

## 1. Changes to coroutines

See [coroutine.md](coroutine.md) for the full items and discussion.

- a. `coroutine.error(co)`: raise an error inside a suspended coroutine.
- b. Close a running (normal) coroutine.
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

See `tools/README.md` for the conversion rules and the language semantics
the converted tests assume.

## 3. Expression-based language

See [expressions.md](expressions.md): how statement values are
implemented (value register, no hidden locals, no block closures), what
the converted Lua suite does and does not exercise, and the open points
(functions ending in a call return its first value; truncation to one
value; `match`/`where`/`catch`/pipes pending).

## 4. Errors with multiple values

See [errors.md](errors.md): Lua passes the error object on the stack
already, but hard-wires the count to one (`luaD_seterrorobj`, message
handlers, `__close`, `lua_error`/`lua_pcall`/`lua_resume`, `error(v,
level)`). Extending to n values is local to the core; `throw`/`catch`
could then carry tag and payload without allocating.

## 5. Tail calls

- Tail call optimization is bad for error messages / tracebacks
  (`(...tail calls...)`). Could it be disabled? Or made explicit
  (opt-in syntax)?
