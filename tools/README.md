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
- a block (`do`, `loop`, `if`, `ifs`, `match`) yields all the values of
  its exit: a parenthesized tuple as its last expression, `break(a, b)`,
  `escape(:X, a, b)`, `until(c, a, b)`, `=> (a, b)` branches; exits with
  fewer values are padded with nils to the longest (at most 15); `val a,
  b = do :X { escape(:X, 1, 2) }`, `f(loop { break(1, 2) })` and a
  function body ending in such a block get all of them, a non-final
  argument position one (as a call)
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
- `loop k, v in e`: a numeric tuple `(a, b[, s])` counts; otherwise the
  values of `e` go to `loop.iter`: 2+ values are a Lua iterator, one
  value iterates a number (1..n), nil (1..), a table (array part then
  other fields, key and value), a function (generator until nil),
  `__pairs`/`__call` (manual, "Loop")
- lambda branches of `if`/`ifs`/`match` are called with the condition
  value (`else` with `true`; a lambda case of `match` passes its result);
  `test { }` runs when `atmos.test` is true; `X.print`, `X.tostring`
  (tables as `[1, 2, k=v]`, `:Tag [..]`), `X.copy`
- `coroutine.error(co, e1, ..., en)` resumes a suspended coroutine
  raising the values at its pending yield (a not-started coroutine dies
  without running); C API `lua_resumeerror`
- `catch CND { }` evaluates to `true, values` or `false, e1, ..., en`;
  the body is an inline block under a VM try region, so
  `break`/`until`/`while`/`escape`/`return` and `...` work inside it;
  `defer { }` is a closure in a hidden to-be-closed slot: a function as
  a to-be-closed value is called on close (see `.claude/plans/catch.md`,
  `testes/catch.atm`)
- a `break`/`until`/`while`, `escape` or `return` inside a task body
  (`spawn { }`, a `par` branch, a `toggle on` body) with no target in
  the body reaches the owner task's loop, `do :X` block or function
  (as the reference), checked at compile time; `watching P { }` has an
  inline body (jumps work); `escape :X [..]` is accepted without
  parentheses (`testes/jumps.atm`)
- errors carry several values: `throw(e1, ..., en)` raises them all (no
  position prefix), `pcall` returns `false, e1, ..., en`, `xpcall`
  handlers receive them all and their results replace them, `__close`
  receives `e1` only; `error(v, level)` is unchanged and
  `errorn(level, e1, ..., en)` is `error` with the level first and n
  values (`testes/throw.atm`)
- tasks follow the reference (`.claude/plans/tasks.md`): `task`, `spawn`,
  `await`, `emit`, `pub`, `tasks`, `abort`, `defer`; `spawn` as a
  statement is owned by its block unless it is the block's last
  statement (then it is the block's value); `val`/`var`/`set` of a task
  or pool require a pinned value or an ancestor, `pin` takes ownership
  (`OP_PINCHK` and the `__pinchk` metamethod; other values are not
  affected); a script runs as the body of a root task (`atmos.loop`), so
  `coroutine.running()` at the top level is not the main thread, a raw
  `coroutine.yield()` there suspends the program, and `os.exit(code,
  true)` closes all tasks (their `defer`s and pins run); `atmos.gc()`
  removes dead tasks from pools
- `par { } with { }` (`par :any`, `par :all`) run their blocks as
  transparent tasks; `loop [ids] on P { }` is a loop whose turn
  starts with `await(P)` (inline: `break` works); `await <PAT>` with
  `||`, `&&`, `!`, `until (c)`, `while (c)`, nested `<P>`, `T(args)`
  (spawns), `(E)` values; a bare variable in pattern position is a
  compile error (write `await(x)`)
- clock durations (`1s`, `10ms`, `1min2s3ms`) are numbers in
  microseconds; `await(n)` is a timer, `emit @:global n` a tick split at
  the nearest timer (`late` returned), `await(:clock)` any tick; `toggle
  t(false) [with P]` / `toggle t(true)` / `toggle on :T [with P] { }` as
  the reference; `atmos.next()`, `atmos.stop()` (aborts the innermost
  `atmos.loop` body, quits the environments)
- environments as the reference (`atmos.env{step=, quit=, mode=}`;
  `atmos.loop` drives the steps and quits the envs on exit;
  `atmos.start` for host-driven envs; `__atmos` metamethod on patterns
  and events; `require "atmos.env.clock"` is built in, emitting the
  elapsed processor time); a loop body is spawned under the running
  task when there is one (nested loops), else under the root

## tst2atm.py

Extracts the test cases of the reference implementation
(`atmos-lang/atmos/tst/*.lua`: Atmos sources inside Lua strings with the
expected output) into an Atmos test file made of `R.T(key, src, exp)`
calls on the harness `testes/ref/harness.atm`:

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
