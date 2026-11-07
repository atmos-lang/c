# tools

## lua2atm.lua

Converts Lua source to Atmos syntax (v0.8):

```
lua tools/lua2atm.lua testes/math.lua > testes/math.atm
```

The mechanical constructs are converted automatically; the rest is
flagged with `TODO(lua2atm)` comments (or `TODO_BITOP(...)` for bitwise
operators) and converted by hand.

Conversion rules:

- `local x <const>` → `val x`; `local x` → `val x`, or `var x` when `x` is
  assigned later; `local function f` → `val func f` (or `var f` + `set f`
  when recursive)
- `x = e` → `set x = e`; `return e` → `return(e)`; `break` → `break()`
- `..` → `++`; `~=` → `!=`; `and`/`or`/`not` → `&&`/`||`/`!`; `^` → `**`
- mixed binary operators are parenthesized (Atmos requires it)
- `{...}` → `[...]`; `t[k]` → `t@(k)`; `o:m()` → `o::m()`
- `if/elseif/else` → `if {} else {}` or `ifs { c => {} else => {} }`
- `while c do` → `loop { while(c) ... }`; `repeat ... until c` →
  `loop { ... until(c) }`; `for i = a, b [, s]` → `loop i in (a, b [, s])`;
  `for k, v in e` → `loop k, v in e` (Lua iterator protocol)
- `local x <close>` → `pin x`
- identifiers that are Atmos keywords get a `_` suffix (e.g. `val_`); table
  fields with keyword names are written `@("name")=`
- `global function f` → `func f`; `global x = e` → `set x = e`; other
  `global` declarations become comments
- `goto`/labels, bitwise operators, named varargs (`...t`): manual
  conversion (goto and bitwise tests are dropped)

Other manual adjustments needed by the test suite:

- numerals must start with a digit (`.5` → `0.5`); hexadecimal numerals
  followed by `+`/`-` need spaces (`0xE+1` → `0xE + 1`)
- `load(src)`, `loadfile` and `dofile` compile Atmos source; chunks and
  function bodies evaluate to their last expression
  (`load("return 2 // 0")` → `load("2 // 0")`); a callback whose last
  statement is a call that returns values must end with an explicit `nil`
  when the extra values matter (e.g. `gsub` replacement functions)
- named varargs (`...t`) become `val t = table.pack(...)`
- `local a, b <close>, c = ...` has no equivalent: `<close>` variables are
  declared separately with `pin`
- `goto` out of nested loops becomes `do :tag { ... escape(:tag) }`

Semantics assumed for the Atmos implementation (the tests rely on them):

- `func NAME (...) {...}` assigns to the visible variable `NAME` (global if
  none is in scope), like Lua's `function NAME`
- a parenthesized single expression truncates to one value (`(f())`)
- two statements on one line need `;` between them; newlines otherwise are
  whitespace
- `"""..."""` and `'''...'''` are raw multi-line strings (no escapes),
  closed by the first run of three or more matching quotes (the extra
  quotes belong to the content); a newline right after the opening
  delimiter is skipped (Lua long-string semantics)
- an `if` without `else` evaluates to `nil` (one value); a function whose
  body ends in a call returns that call's first value, so an iterator or
  callback that must return nothing has to end with an explicit `nil`
- `pin x = v` is a to-be-closed variable (Lua `<close>`); `nil`/`false`
  are allowed; `escape`/`break` close pending variables
- `return(...)` of a call is a proper tail call
- `a ++ b` concatenates tables when both are tables; a table with a
  non-table operand is an error
- error messages, variable-kind names (global/local/upvalue/field/method/
  constant) and syntax-error formats are Lua's
- errors carry several values: `throw(e1, ..., en)` raises them all (no
  position prefix), `pcall` returns `false, e1, ..., en`, `xpcall`
  handlers receive them all and their results replace them, `__close`
  receives `e1` only; `error(v, level)` is unchanged and
  `errorn(level, e1, ..., en)` is `error` with the level first and n
  values (`testes/throw.atm`)
