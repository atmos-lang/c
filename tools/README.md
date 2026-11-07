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
  `loop { ... until(c) }`; `for i = a, b` → `loop i in (a, b)`;
  `for k, v in pairs(t)` → `loop k, v in t`
- identifiers that are Atmos keywords get a `_` suffix (e.g. `val_`)
- `global` declarations become comments (no Atmos equivalent)
- `goto`/labels, bitwise operators, `for` with steps, `<close>` variables:
  manual conversion (goto and bitwise tests are dropped)

Other manual adjustments needed by the test suite:

- `...` cannot be used inside nested blocks (`if`, loops): copy it to
  locals first
- `string.dump` of Atmos functions is not supported: such tests are dropped
- numerals must start with a digit (`.5` → `0.5`); hexadecimal numerals
  followed by `+`/`-` need spaces (`0xE+1` → `0xE + 1`)
- `load(src)` compiles Atmos source; chunks evaluate to their last
  expression (`load("return 2 // 0")` → `load("2 // 0")`)
