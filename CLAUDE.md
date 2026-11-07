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
- Do not ask for permissions or confirmation; decide and proceed, then
  report numbered summaries.
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
  uses the Lua iterator protocol.
- Error messages keep Lua's formats.
