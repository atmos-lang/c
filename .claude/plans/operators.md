# Operators, pipes, match, where

Reference: `doc/manual.md` "Operations" (equivalence, length,
concatenation, membership, structural comparison), "Pipes",
"Precedence and Associativity", "Match", "Where", "Ambiguities";
runtime `lua-atmos/atmos/atmos/x.lua` (`is`, `gte`, `eq`, `iter`,
`xin`, `cat`); tests `tst/exec.lua` ("CALL / METHOD / PIPE", "WHERE",
match cases), `tst/expr.lua` (operator lambdas).

Status: implemented except `where` (`lparser.c`: `opcall`, pipes,
`matchstat`, `oplambda`, keyword pipe forms `kwopen`; `lbaselib.c`:
`_G["??"]`, `_G["=>="]`, `_G["==="]`, `_G["?>"]`; `ltm.c`: `++` on
tables). `testes/ops.atm` passes. Limitation: a parenthesized tuple
piped with `-->` keeps all its values in statement and declaration
contexts (`(10,20) --> f(30)` is `f(10,20,30)`); inside a call's
argument list a tuple is a single value, so there it pipes one value.

Status before (this repo): all these tokens existed in the lexer
(`TK_IS`, `TK_ISNOT`, `TK_XIN`, `TK_XNOTIN`, `TK_SEQ`, `TK_SNE`,
`TK_SGE`, `TK_SLE`, `TK_LPIPE1/2`, `TK_RPIPE1/2`, `TK_WHERE`,
`TK_MATCH`) and the parser rejects them with "operator/statement not
implemented". `++` is Lua's `..` only (tables are an error, although
tools/README.md already lists table concatenation as assumed). Lambda
operator shortcuts (`\+`) are rejected ("'{' expected").

## Semantics (from the reference)

- `a ?? b` ("is"): `a == b`, or `type(a) == b`, or `b` is a dot-prefix
  of the string `a`, or of `a.tag` (raw field) when `a` is a table
  (`X` matches `X.Y`; later also `:task`/`:xtask`/`:tasks`). `!?` is
  its negation. This is exactly `tagmatch` in `lbaselib.c`, used by
  `catch` today.
- `a =>= b` ("sup"), in order: different Lua types → false; different
  metatables → false; `a == b` → true; strings: `a` dot-prefix of `b`;
  tables: every `k=va` in `a` satisfies `va =>= b[k]` (recursive);
  otherwise false. `a =<= b` is `b =>= a`. `a === b` is both; `=!=`
  negates it.
- `a ?> b` ("in"): iterate `b` as a `loop` would (array part `1..#b`
  in order, then the other keys with `next`; `__pairs` if present;
  functions as generators); true if `a =>= v`, or `a =>= k` for a
  non-numeric key. `!>` negates it. (The reference uses `gte`, not
  `==`.)
- `a ++ b`: Lua `..` for strings/numbers/`__concat`; otherwise a new
  table: iterate `a` (array values appended, keyed values set), then
  `b` (array values appended, keyed values set, `b` wins on
  duplicates).
- Pipes: `x -> f` is `f(x)`, `x -> f(a)` is `f(x, a)` (prepended);
  `f <- x` is `f(x)`, `f(a) <- x` is `f(a, x)` (appended). Single
  pipes bind tighter than unary and binary operators (level 3, "inner
  pipe"); double pipes `-->`/`<--` bind looser than binary operators
  (level 6, "outer pipe"): `10+1 --> f` is `f(10+1)`, `f <-- 10+10` is
  `f(20)`, `f <-- 10 -> g` is `f(g(10))`. Multiple values flow through:
  `(10,20) --> f(30,40)` is `f(10,20,30,40)`. Keyword targets:
  `return <- v`, `throw <- v`, `escape <- :X`, `until <- c`,
  `print <- spawn T()`. Ambiguity: `x<-y` is the pipe, never `x < -y`.
- `match h { c1 => b1 ... else => bn }`: `h` evaluated once; each case
  `c` tests `h ?? c`, or calls `c` with `h` when `c` is a lambda;
  branches are blocks, `=>` expressions, or lambdas (called with `h`).
  Value: the branch value; no match and no `else`: nil.
- Lambda operator shortcuts: `\+` is `\(a,b){ a + b }`, `\#`/`\!`/`\-`
  unary (`\-` is binary per "Ambiguities"), `\===`, `\++`; pipes and
  non-operators are "lambda error : invalid operator".
- `e where { decls }`: the declarations are evaluated before `e`, as
  immutable locals visible only in `e` and the clause.

## Design

### Runtime helpers

`??`, `?>`, `=>=` need code at run time. Options: new opcodes, or C
functions called like `catch`. Chosen: C functions in `lbaselib.c`
registered under the operator's own name in the global table
(`_G["??"]`, `_G["?>"]`, `_G["=>="]`, `_G["==="]`), reached by the
parser as `_ENV["??"]` (`VINDEXUP` with the string constant). An
operator string is not an identifier, so Atmos code cannot shadow it
by accident; it stays reachable for Lua-side use (`_G["??"](a, b)`).
`!?`, `!>`, `=!=`, `=<=` compile to the same calls plus `not` or
swapped operands. Opcodes can replace this later without touching the
tests.

`tagmatch` already implements `??` (used by `catch`); `=>=` and `?>`
are new C code (`gte` recursive over tables with a depth limit through
`luaL_checkstack`, `xin` iterating the array part then `next`,
honouring `__pairs`).

### `++` on tables

Kept as `OP_CONCAT`. In `luaV_concat`, when `..` has no `__concat`
metamethod and both operands are tables, build the result in C
(`luaT_tryconcatTM` path), following the reference's `cat`. Strings
and numbers keep Lua's fast path; a table with a non-table operand
stays an error.

### Pipes (parser only)

`binloop` gets the four pipe operators at their precedence levels: the
single pipes are parsed in `suffixedexp` (after suffixes, before unary
operators apply), the double pipes after the binary operators in
`expr`. The right side of `->` (left side of `<-`) is a suffixed
expression; when it is a call, its arguments are regenerated with the
piped value prepended or appended. To keep one pass, the pipe target
call is parsed by `funcargs` into a register layout: for `x -> f(a)`
the parser evaluates `x` first, then `f`, then moves `x` after `f` and
parses the arguments (`OP_MOVE`s); for `f(a) <- x` the call is parsed
with its arguments and `x` is appended before `OP_CALL` is emitted
(`funcargs` returns the pending call; the parser checks the previous
instruction is the `OP_CALL` it just emitted and extends `B`). Keyword
targets (`return`, `throw`, `escape`, `until`, `break`) are handled in
their statement parsers: `kw <- e` is `kw(e)`.

### `match` (parser only)

Like `ifsstat`: `h` into a register, each case compiles to
`_ENV["??"](h, c)` or `c(h)` for a lambda case (`\` token), branches
reuse `branch`; lambda branches are called with `h`.

### Lambda operator shortcuts (parser only)

In `lambda`, after `\`, a binary or unary operator token builds the
closure directly: binary ones get two parameters `a, b`, unary `#`/`!`
one; `-` is binary; pipes/others are an error ("lambda error: invalid
operator").

### `where` (separate step)

The clause comes textually after the expression it feeds, but its
declarations must be compiled first; a one-pass compiler cannot emit
the expression before knowing its locals. Needs token recording in the
lexer: when an expression statement starts, tokens are recorded until
its end; if a `where` follows, the clause is parsed and compiled first,
then the recorded tokens are replayed for the expression. Planned as
its own step after the others (lexer buffer, nesting of `where` inside
`where`, line numbers of replayed tokens).

## Tests

`testes/ops.atm`: `??`/`!?` (values, types, tag hierarchy, tagged
tables, nil), `=>=`/`=<=`/`===`/`=!=` (scalars, strings, nested tables,
metatables, functions), `?>`/`!>` (vectors, keyed tables, mixed, strings
as values, generators), `++` on tables (vectors, keyed, mixed, nested,
with `__concat`, errors), pipes (single/double, prepend/append,
multiple values, keyword targets, precedence with unary/binary, the
`x<-y` ambiguity), `match` (reference cases), lambda operators. `where`
tests go with its own step.
