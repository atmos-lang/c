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
- `a ++ b` concatenates tables when both are tables (array parts
  appended, other fields merged, the right side winning); a table with
  a non-table operand is an error
- `??`/`!?`, `=>=`/`=<=`/`===`/`=!=` and `?>`/`!>` call the functions
  `_G["??"]`, `_G["=>="]`, `_G["==="]`, `_G["?>"]` (tag hierarchy,
  structural comparison, membership as in the reference)
- pipes: `x -> f(a)` is `f(x, a)`, `f(a) <- x` is `f(a, x)`; single
  pipes bind tighter than unary/binary operators, double pipes looser;
  `return`/`break`/`until`/`while`/`escape` accept `<-`/`<--` forms
- `match h { c => b ... else => b }` tests `h ?? c` (or calls a lambda
  case with `h`); `\+`, `\#`, `\===` are operator lambdas
- error messages, variable-kind names (global/local/upvalue/field/method/
  constant) and syntax-error formats are Lua's
- `coroutine.error(co, e1, ..., en)` resumes a suspended coroutine
  raising the values at its pending yield (a not-started coroutine dies
  without running); C API `lua_resumeerror`
- `catch CND { }` evaluates to `true, values` or `false, e1, ..., en`;
  the body is a closure (no `break`/`escape`/`return` across it, no
  `...`); `defer { }` is a closure in a hidden to-be-closed slot: a
  function as a to-be-closed value is called on close (see
  `.claude/plans/catch.md`, `testes/catch.atm`)
- errors carry several values: `throw(e1, ..., en)` raises them all (no
  position prefix), `pcall` returns `false, e1, ..., en`, `xpcall`
  handlers receive them all and their results replace them, `__close`
  receives `e1` only; `error(v, level)` is unchanged and
  `errorn(level, e1, ..., en)` is `error` with the level first and n
  values (`testes/throw.atm`)

## tst2atm.py

Extracts the test cases of the reference implementation
(`atmos-lang/atmos/tst/*.lua`: Atmos sources inside Lua strings with the
expected output) into an Atmos test file made of `T(name, src, exp)`
calls on the harness of `testes/tasks1.atm`:

```
python3 tools/tst2atm.py ../atmos-lang/atmos/tst/tasks.lua 101 3609
```

Expectations: a string (exact output), `TX` (trimmed), `P`/`TP` (Lua
pattern), `F` (substring), `E(pre, msg)`/`EP(pre, pattern)` (an error
with that message after the output `pre`; only the last line of the
reference's "==> ERROR:" trace is compared). Unrecognized shapes become
`;; TODO(tst2atm)` comments.

Flow: the reference `tst/` directory is copied as is to
`testes/ref/tst/` (`COMMIT.txt` records the reference commit); each
test file is generated to `testes/ref/<name>.atm` with `--commented`
(every case disabled) and then with `--enabled LIST` where `LIST` holds
the keys (`<file>:<line> <name>`, see `--list`) of the cases expected
to pass; `testes/ref/harness.atm` runs them. To enable new cases:
generate the file without options into a scratch copy, run it, remove
the `FAIL:` keys from the `--list` output, regenerate with `--enabled`.
