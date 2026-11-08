# Atmos (C implementation)

Atmos reimplemented on top of the Lua 5.5 C sources: the Lua front end
(`llex.c`, `lparser.c`) is replaced by an Atmos one, the code generator,
VM, libraries and bytecode format stay Lua's.

## Layout

- `*.c`, `*.h`: the interpreter (Lua 5.5 core with the Atmos front end).
- `testes/*.atm`: the Lua 5.5 test suite converted to Atmos syntax
  (`cd testes && ../lua all.atm`, or one file: `../lua math.atm`).
- `tools/lua2atm.lua` + `tools/README.md`: Lua-to-Atmos converter and the
  conversion rules / assumed semantics.
- `.claude/plans/`: TODO.md and design notes (coroutines, dropped tests).
- Reference implementation (Lua): `atmos-lang/atmos` (grammar in
  `src/parser.lua`, `src/prim.lua`; manual in `doc/manual.md`).

## Workflow

- Work on branch `second`; never force-push.
- One commit per step (one test file, one feature); push after each commit
  and report the GitHub link of the commit
  (`https://github.com/atmos-lang/c/commit/<sha>`).
- Discussion first: analyze, explain and record decisions in the plan
  files; ask before implementing anything (never start changing code on
  a design question or a plan item without an explicit go-ahead).
- Once a step is approved, do not ask for permissions or confirmation on
  the details; decide and proceed, then report numbered summaries.
- Tests first: for an approved step, write the tests (failing) in
  `testes/`, commit and push them, report the link, and only then
  implement; the implementation commit makes them pass.
- Build with `make`; debug build with `T` library:
  `make MYCFLAGS='-DLUA_USER_H=\"ltests.h\"'` in a scratch copy.
- Never put model names in commits, code or docs.

## Language decisions (see tools/README.md for the full list)

- `val` is a read-only register local (no compile-time constant folding),
  `var` mutable, `pin` to-be-closed (`<close>`).
- Statements on one line need `;`; a call's `(` must be on the same line
  as the callee.
- Blocks, `if`, `do`, `loop` evaluate to their last expression / break
  value; function bodies return their last expression (one value, unless
  it is a parenthesized tuple).
- `loop i in (a, b[, s])` is a numeric loop; any other `in` expression
  goes through `loop.iter` (latmlib.c): two or more values are a Lua
  iterator triple; one value iterates a number (1..n), nil (1..), a
  table (array part, then other fields, as key/value), a function
  (generator until nil), `__pairs`/`__call`.
- Lambda branches (`if c => \{}`, `ifs`, `match`) are called with the
  condition value (the case result for a lambda case of `match`);
  `test { }` runs only when `atmos.test` is true.
- Error messages keep Lua's formats.
