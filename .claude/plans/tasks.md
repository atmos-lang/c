# Tasks

Reference: manual sections "Structured Deterministic Concurrency",
"Event Signaling Mechanisms", "Tasks", "Pub", "Task Operations" (abort,
await, emit, toggle, par, watching), "Loop" (`loop on`); runtime
`lua-atmos/atmos/atmos/run.lua` (950 lines) plus `init.lua` (the global
API), `atmos/lang/run.lua` (language helpers: `atm_pin_chk_set`,
`atm_tag_do`, `X.tostring`), `atmos/x.lua` (`X.print`, `X.copy`);
parser `src/prim.lua` (spawn/emit/await/toggle/task/par/watching/loop
on) and `src/await.lua` (await patterns); tests `tst/exec.lua` (TASKS,
AWAIT / TASK, AWAIT / CLOCK, TOGGLE), `tst/tasks.lua` (232 cases),
`tst/await.lua`, `tst/toggle.lua`, `tst/timers.lua`.

## Model (from the reference)

- A **task prototype** (`task (..) {}`) is a function blessed as
  spawnable; an **xtask** is a running instance (a coroutine plus
  bookkeeping); a **tasks** pool groups instances (optional max size;
  a full pool makes `spawn` return `nil`).
- Instances form a tree: each xtask/pool has `up` (owner) and `dns`
  (children, in spawn order). The root is a hidden pool `TASKS`. The
  running task is `me` (`xtask()`); transparent tasks (`spawn {}`,
  `par`/`watching` branches) are skipped by `me(false)` so `pub` and
  `emit` targets resolve to the enclosing non-transparent task.
- `spawn T(...)` creates the instance, links it under `me` (or the
  given pool), and **resumes it immediately** up to its first `await`
  (run to completion). Its result is in `t.ret`; `t.pub` is a public
  field writable from inside (`pub`) and outside.
- **Structured abort**: a spawn at statement level and a pool are held
  by a to-be-closed slot of the enclosing block (`pin`), whose `__close`
  aborts the task and its subtree (children first, in reverse order,
  running `defer`s). `val t = spawn ...` is an error ("expected pinned
  value"); `pin x = t` of an already pinned or ancestor task is an error
  ("unexpected pinned value" / "unexpected parent task"). A task
  attached to a pool is aborted with the pool.
- **emit(e)**: broadcast with a time stamp `TIME+1`; traversal of the
  target's tree in source order: for each child, recurse (children
  before their owner), then the owner itself if it is suspended and its
  await was established before this emit (`t.time < time`, one wake
  per emit). Targets: `me` (default), `@t`, `@ts`, `@:parent`,
  `@:global` (the root), `@n` levels up. Errors raised by an awakened
  task propagate to its owner's await point as an error (the reference
  resumes the owner with an `'atm_error'` marker; here
  `coroutine.error`), and up to the emitter.
- **await(P)**: stamps `me.time`, yields, and on each wake checks the
  event against the pattern; value patterns (`true`, `false`, tag/
  value with `??`, table with `=>=`), task termination (`await t` gives
  `t.ret, t`; `await T(...)` spawns then awaits), pools (`:any ts`,
  `:all ts` give `v, t, ts`), clock durations (`1s`; numeric emits
  advance timers, `late` returned), `until`/`while` predicates
  (lazy), combinators `&&`/`||`/`!` (implemented with `par_all`/
  `par_any`), `__atmos` metamethod for user patterns. Awaiting a dead
  task returns at once.
- **abort(t)**: closes the task/pool subtree now; if `t` is `me` or an
  ancestor, the current task yields and waits to be closed from above
  (`yield_if_aborted`, also after `emit`/`spawn`, see coroutine.md b).
- **toggle**: `toggle t(false)` freezes a subtree (emits skip it, except
  for `with` filter patterns driven by a hidden gate task);
  `toggle on :T with P { }` runs a block as a transparent task toggled
  by `emit(:T [bool])`.
- **par / par :any / par :all / watching**: transparent tasks per
  branch held in a to-be-closed set; `par` awaits forever, `:any`
  rejoins on the first termination (throw of a private tag caught by
  the composition), `:all` awaits each; `watching P { }` is
  `par :any { await P } with { }`. `loop on P { }` is
  `loop { val .. = await(P); .. }`.
- Program: the main chunk runs as the body of a root task inside
  `loop(body)`, which also drives environments (clock/IO plugins) until
  the body dies. Dead tasks are swept from `dns` between reactions.
- Termination of a task emits the task itself to its owner's await
  point (`await t`, `:any/:all ts` match on it).

## Design

### Where the runtime lives

Option C: a C library (`latmlib.c`, like `lcorolib.c`) implementing the
tree, spawn/emit/await/abort/toggle/par in C, with tasks as userdata
(`__name` "xtask"/"tasks"/"task" so `??` sees the types through
`tagmatch`; `t.pub`/`t.ret` through `__index`/`__newindex` on a user
value table; `__close` aborting; `__len`/`__pairs` on pools). Globals
mirror the reference API: `task`, `xtask`, `tasks`, `spawn`,
`spawn_in`, `do_spawn`, `await`, `emit`, `emit_in`, `abort`, `toggle`,
`par`, `par_any`, `par_all`, `watching`, `loop_on`, plus `X`
(`X.print`, `X.tostring`, `X.copy`) and `atmos` (`atmos.status`,
`atmos.loop`, `atmos.next`, environments later).
Option L: port `run.lua` to a Lua/Atmos prelude loaded at start.
Recommendation: C. It is the point of this implementation, it uses the
core work already done (n-value errors, `coroutine.error`), and the
stackless coroutines of coroutine.md (c) plug in underneath it.

### Parser (desugaring to calls, as the reference does)

- `task (..) { }` → `task(func (..) { })`; `task T (..) { }` → `set T =
  task(..)`; `val task T` → `val T = task(..)`; a bare `task` not
  followed by `(`/NAME on the same line → `xtask()` (the running task).
- `pub` → `xtask().pub` (readable and assignable; error "invalid pub :
  expected enclosing task" outside tasks).
- `spawn T(args)` → `spawn(T, args)`; `spawn @ts T(args)` →
  `spawn_in(ts, T, args)`; `spawn { }` → `do_spawn(\{ })`; as a
  statement, the result goes to a hidden to-be-closed slot (as `defer`
  does) so the block owns it; `pin t = spawn { }` → `spawn(task(\{ }))`.
  `pin`/`val`/`var`/`set` of a spawn or pool go through a check (the
  reference's `atm_pin_chk_set`): pin marks ownership, others require
  an already pinned value or an ancestor.
- `tasks(n)` plain call (pools must be pinned).
- `emit(e)` / `emit :X [..]` → `emit(e)`; `emit @t (e)` →
  `emit_in(t, e)`.
- `await P` (bare), `await(E)` (value), `await<PAT>` (combinators):
  compiled to `await(pattern)` with pattern constructors reachable as
  non-identifier globals (like the operators): `_ENV["await.spawn"](T,
  args..)`, `["await.any"](ts)`, `["await.all"](ts)`,
  `["await.until"](p)`, `["await.while"](p)`, `["await.and"](..)`,
  `["await.or"](..)`, `["await.not"](p)`; predicates `(c)` become
  `\it{ c }`. A bare call in pattern position spawns.
- `abort(t)` is a plain function. `toggle t(b) [with P..]` →
  `toggle(t, b, P..)`; `toggle on :T [with P..] { }` → `toggle(:T, P..,
  \{ })`.
- `par { } with { }` → `par(\{ }, \{ })`; `par :any`/`:all` →
  `par_any`/`par_all`; `watching P { }` → `watching(P, \{ })`;
  `loop on P { }` → `loop { val it = await(P); .. }` (names before
  `on` bind the event: `loop e on P`).
- Blocks become closures here (as in the reference); `break`/`return`
  inside them follow the `catch` rule (compile error) until option B
  of catch.md exists. `await` inside them works (yieldable `pcall`).

### Runtime (C)

- Tasks and pools are tables (field `_`: a `Node` userdata, one
  struct for both: coroutine `lua_State*`, `status` (none/aborted/
  toggled), `time`, `pass`, `tra` (transparent), `pin`, `max`, and the
  await filter `akind`/`pkind`/`wkind`, `clk`, `atask`); the node's
  user values (slots, no string keys) hold the coroutine, `up`, `dns`
  (table of children), the toggle gate and the awaited pattern. `ret`
  and `pub` are fields of the table. A prototype is a table with its
  function in `_`. A weak table maps coroutines to xtasks (`me`).
- `spawn`: create, link, `lua_resume`; the result handling is
  `task_result` (error: close subtree and re-raise with all values;
  aborted while running: close now; dead: store `ret`, emit the task to
  the owner's await, close subtree).
- `emit`: recursive traversal in C returning a status (no protected
  call per child); a task is resumed (under `lua_pcall`) only when its
  `time` allows and its await filter says the event may match: tags,
  `true`/`false`, timers (countdown in the node), `:clock`, task
  termination, `:any`/`:all` and table patterns are decided in C;
  `__atmos` patterns/events, predicates and raw yields always resume
  (see coroutine.md, "Wasted resumes"). A child error is raised at the
  owner's await with `lua_resumethrow`, or returned up. Clock ticks (`emit @:global 10s`)
  split in pieces at the nearest timer (`CLK.min`/`late`).
- `await`: C function with a continuation (`lua_yieldk`) matching on
  each wake; task termination via the emitted task; pools by owner;
  timers by countdown; `__atmos` metamethod; errors injected by
  `coroutine.error` surface at the yield.
- Dead tasks are unlinked between reactions (a sweep set, as `M.gc`),
  never while an emit traverses `dns`.
- `loop(body)`: spawn the body as the root task, run environment steps
  (none yet) until it is dead; `lua.c` runs the main chunk through it;
  `dofile`/`load` chunks run inline in the current task.
- Error traces ("==> ERROR:" bidimensional traces) are not in scope;
  errors keep Lua's messages and the n-value convention.

### Stages

1. Core: `task`/`xtask`/`tasks`, `spawn` (all forms, pin checks),
   `pub`, `await` of values, tags, tables, `true`/`false`, tasks
   (`await t`, `await T()`), pools (`:any`, `:all`), `until`/`while`
   predicates; `emit` with targets; `abort`; structured abort through
   blocks; errors across tasks; `defer` order; `loop(body)` driver;
   `X.print`/`X.tostring`/`X.copy`; `atmos.status`.
2. `par`, `par :any`, `par :all`, `watching`, `loop on`, `&&`/`||`/`!`
   patterns, toggles, clock timers (`await 1s`, numeric emits, `late`).
3. Environments (`atmos.env`, `atmos.next`, a clock env), `thread`
   (out of scope unless wanted), stack traces.

### Tests

The reference cases are Atmos sources inside Lua strings with the
expected output; a converter (`tools/tst2atm.py`) extracts them into
`testes/tasks*.atm` as `T(name, src, expected)` calls on a small
harness written in Atmos: it loads `src`, runs it as a task tree inside
a block (so leftovers are aborted), captures `print`, and compares; an
expected "==> ERROR:" block compares only the message line. Stage 1
takes the TASKS, AWAIT / TASK sections of `exec.lua` and the EMIT /
SCOPE, PIN, TERMINATION, PUB, NESTED, ABORTION, THROW / CATCH, RETURN,
TASKS sections of `tasks.lua`; stage 2 the rest.

## Status: stage 1 implemented

Runtime `latmlib.c`, parser desugaring in `lparser.c` ("Tasks" section),
driver `lua.c`. Deviations from the design above, and decisions taken
while implementing:

- Tasks, xtasks and pools are tables, not userdata (`type(t)` is
  `"table"`, as in the reference): the struct lives in field `_` (a
  userdata), the links (`th`, `up`, `dns`, `gate`) in a user value
  table, `pub`/`ret` are plain fields. The metatables are named
  `"task"`/`"xtask"`/`"tasks"` so `??` matches them through `__name`.
- Pin checks are an opcode, not calls. The reference wraps every
  declaration and assignment in `atm_pin_chk_set(...)`; the first
  version here called global functions `pin.chk`/`pin.set`/`pin.own`,
  which added an `_ENV` upvalue and a C call to every assignment
  (`debug.getupvalue`, hook traces and `db.atm` saw it). Now `OP_PINCHK
  A B` runs the `__pinchk` metamethod of the value in `R[A]` when it has
  one (only tasks and pools do; B is 0 check, 1 set, 2 own); other
  values cost a type test. The parser skips it for constants,
  closures, constructors and arithmetic. Only single-value
  declarations/assignments are checked (`val a, b = f()` is not; the
  reference checks the first value).
- A statement `spawn` that is the last statement of its block is the
  block's value (not pinned: `spawn T()` as the last statement of a
  `do` block or a function flows out); otherwise the block owns it
  through a hidden to-be-closed local `(spawn)`. `spawn @pool` is never
  pinned (the pool owns it).
- Dead tasks are swept only by `atmos.gc()` and by `atmos.loop` when the
  body ends (the reference sweeps between reactions); a dead task stays
  in its pool until then, but does not count for the pool limit.
  `tasks(0)` is a full pool, `tasks()` is unbounded.
- `atmos.loop(body, ...)` returns when the body dies or when it is
  suspended with nothing to drive it (no environments yet, stage 3): a
  raw `coroutine.yield()` or `await(false)` at the top level ends the
  program silently. `lua.c` runs scripts and `-e` strings through
  `atmos.loop`; REPL lines run in the main thread. Consequences in the
  Lua suite: `coroutine.running()` at the top level is the task's
  coroutine (not the main thread, yieldable), `coroutine.close(main)`
  would end the program, `debug.getinfo` from an error handler does not
  see the chunk's frames (another thread); `coroutine.atm`, `errors.atm`
  and `main.atm` were adapted (TODO.md item 2).
- SIGINT: the interpreter hooks the thread of the running task
  (`atmos_running()` in `lualib.h`, tracked around each resume), not the
  main thread.
- `os.exit(code, true)` closes the whole task tree: a sentinel in the
  registry with a `__gc` finalizer (run by `lua_close`) closes every
  task, the running ones included, so pending `defer`s and pins run as
  the to-be-closed variables of a Lua main chunk would.
- Errors raised by a task propagate to the resumer with all their values
  (`taskresult`); an error in a `defer` while closing is raised with its
  values; `__pinchk` errors carry the position of the assignment.
- Test harness `testes/ref/harness.atm` runs each case through
  `pcall(atmos.loop, f)`, as the reference's `atm_test`; the enabled sets
  are regenerated from a run of all cases (scratch `refgen.sh`:
  `tst2atm.py --list`, run, `--enabled` with the passing keys).

Reference cases passing (enabled): exec 173/217, tasks 187/217, x 21/23,
await 3/18, throw 2/8; toggle, timers, streams, thread 0 (stage 2/3).
The disabled ones need: `par`/`watching`/`loop on`/`every`, `toggle`,
clock patterns (`1s`), `await` combinators `&&`/`||`/`!`, `where`,
backtick natives, the reference's error-trace format and compile-error
format, `return` inside `catch` (catch.md option B), `++` on pools
(needs `__pairs` in table concatenation), `xtask(f)` message text.

## Stage 2a: compositions and patterns (design)

Decisions (discussed before implementing):

- `loop [ids] on P { body }` is inline: `loop { val ids = await(P);
  body }`. No closure, so `break` (with a value), `until`/`while` and
  `return` behave as in any loop. (The reference wraps it in a closure
  and a `catch 'atm-loop'` only because its coder works with closures.)
- `par { } with { }` → `par(\{ }, \{ })`, `par :any` → `par_any`,
  `par :all` → `par_all`: each block is a closure spawned as a
  transparent task, as `spawn { }` is; a branch is a task body, so a
  closure is unavoidable (a coroutine needs a function). `par` awaits
  forever (the branches live until the enclosing block ends); `par_any`
  returns the values of the first branch to terminate and aborts the
  others; `par_all` returns the value of each branch, in order, when
  all have terminated. Branches are owned by the composition (a hidden
  pin), so leaving it aborts them.
- `watching P { body }` → `watching(P, \{ })` = `par_any(\{ await(P)
  }, body)`: the body is a transparent task too (as the reference);
  the value is the body's value or the awaited event.
- (Superseded by catch.md option B, implemented: `watching` has an
  inline body under a try region with a hidden watcher task; jumps out
  of `par` branches, `spawn { }` and `toggle on` bodies reach their
  targets in the owner through a region of the owning block.)
- Patterns `await <PAT>` (also after `watching`, `loop on`): leaves are
  tags/values, `(E)` value expressions, `:any ts`/`:all ts`, `until (c)`/
  `while (c)`, nested `<P>`, and a bare call `T(args)` which spawns
  (`["await.spawn"]`); `!p` prefix; `p && p`, `p || p`, `p until (c)`,
  `p while (c)` at one level, same operator only (mixing `||` and `&&`
  without parentheses is a compile error). Compiled to constructor
  calls `_ENV["await.or"](..)`, `["await.and"](..)`, `["await.not"](p)`,
  `["await.until"](p, pred)`, `["await.while"](p, pred)`. Semantics as
  the reference: `or` is `par_any` of the sub-awaits (the first to
  match wins, sub-tasks of the losers are aborted), `and` is `par_all`,
  `not p` re-awaits until an event that is not `p`; `until`/`while`
  re-await while the predicate fails/holds, the predicate seeing the
  event (`it`). `__atmos` metamethod for user patterns.
- The reference's EVERY section (`every N` cases) is `loop on`.

Tests (enabled, failing before the implementation): exec `loop 13`,
`tasks 13` (×2), `tasks 14`; tasks.lua `abort 5`, `tasks 25`, EVERY
(except `every-where`, `every-clock`), PAR/WATCHING (all but `par :any
6`); await.lua all but `op_clock_or` (clock, 2b) and `op_mixed_err`
(compile-error format). Still out: `escape TCO`, `par :any 6` and
`catch 8b` (`escape` as a throw across tasks), `await 1` (error-trace
format).

### Status: implemented

- Parser (`lparser.c`): `pattexp`/`pattbin`/`pattunary`/`pattprim`
  compile the three tiers; a bare variable in pattern position is a
  compile error "invalid await : unexpected expression" (as the
  reference: a value must be written `(x)`); `loop [ids] on P` is an
  infinite loop whose turn starts with `await(P)` in a fresh scope;
  `parexp` and `watchingexp` build the calls with `closurebody`
  (kinds `CB_PAR`/`CB_WATCHING`; `return` inside gives "cannot use
  'return' inside 'par'").
- Runtime (`latmlib.c`): patterns are tables with `kind` and `n`
  (`spawn`, `or`, `and`, `not`, `until2`/`while2` besides the stage-1
  kinds). `await` of a `spawn` pattern spawns under the running task and
  awaits the instance; `or`/`and` call `par_any`/`par_all` over C
  closures `awaitbranch` (a transparent task body doing `await(p)`);
  `not` loops a `par_any` of `await(p)` (returning a marker) against
  `await(true)`; `until2`/`while2` loop the sub-await and apply the
  predicate. `par`/`par_any`/`par_all` spawn their branches as
  transparent tasks into a table with a `__close` that aborts them,
  declared to-be-closed in the C frame (`lua_toclose`, kept across the
  yields), and wait with `lua_yieldk` continuations: `par_any` returns
  the `ret` of the first dead branch (a branch that terminates while
  being spawned ends the composition before the next ones start, as in
  the reference), `par_all` waits for each branch and returns their
  `ret`s. `watching(P, f)` is `par_any(awaitbranch(P), f)`.
- Not done: `__atmos` user patterns (no test yet); `escape` across a
  task boundary (needs the throw scheme or option B).

## Stage 2b: toggles and clock (design)

From the reference (`run.lua`: `M.toggle`, `emit` gating, `M.emit`
numeric pieces, `M.await` clocks; `prim.lua` toggle forms):

- **Clock literals.** The lexer already produces `TK_CLK` (`1s`,
  `10ms`, `1h30min`, `1min2s3ms`: integer microseconds, a float when a
  component has a fraction); the parser gets the token as a numeral, so
  `1s ?? :number` is true and `await(10ms)` awaits a number.
- **Timers.** `await(n)` with a number is a countdown: at the start, if
  `n <= 0` it returns at once with `late - n`; otherwise it reports the
  timer (`CLK.min = min(CLK.min, n)`) and waits. A numeric event `d`
  decrements the countdown (the pattern slot keeps the remainder); when
  it reaches zero or less the await returns `late - remainder`, where
  `late` is how much of the current tick remained after this piece.
  Non-numeric events are ignored. `await(:clock)` wakes on any numeric
  event and returns `d, late`.
- **Ticks.** `emit(n)` with a number must target the root (`emit @:global
  10ms`; otherwise "invalid emit : expected global"). It is split in
  pieces, each ending at the nearest reported timer: `d = min(CLK.min,
  n)`, `n -= d`, `CLK.min = huge` (timers re-report when they wake),
  `CLK.late = n`, broadcast `d`; repeat until `n <= 0` (`emit(0)` is one
  piece). `CLK.late` is saved and restored around the loop (nested
  ticks). `atmos.next()` is `CLK.min` or nil; `atmos.stop()` closes the
  root pool, sweeps and resets the clock.
- **Toggle off.** `toggle t(false) [with P {, P}]`: `t` a task or pool
  with status OK (else "invalid toggle : expected awaiting task");
  status becomes TOGGLED. An emit reaching a toggled node first drives
  its gate (if any) and then skips the node's subtree unless the gate
  stamped `pass = time` for this emit. The gate is a hidden transparent
  task off the tree (not in any `dns`, `up` = the node's owner) whose
  body loops `await(P); pass = TIME`; several filters are awaited as
  `or`. The gate is stored in the node's `gate` link (already closed by
  `closextask`; `closepool` closes it too).
- **Toggle on.** `toggle t(true)`: status must be TOGGLED ("invalid
  toggle : expected toggled off task"; an extra argument is "invalid
  toggle : unexpected argument"); status OK, the gate is closed, then a
  targeted `emit` of `0` to `t` (bypassing the global check) wakes
  `await(true)`s and makes frozen timers re-report.
- **Block form.** `toggle on :T [with P {, P}] { body }` →
  `toggle(:T, P..., \{ body })`: the body runs as a transparent task
  `t`; a hidden controller task loops `await(:T [false]); toggle(t,
  false, P...); await(:T [true]); toggle(t, true)`; the value is the
  body's `ret` (the composition is a `par_any` of body and controller,
  so leaving it aborts both). `return` inside the body is rejected as in
  `par`.
- **Statement form.** `toggle t(false)`: `toggle` followed by a
  suffixed expression that must end in a call (else "expected call
  syntax"): `toggle f(args) [with P, ...]` → `toggle(f, args..., P...)`.
- Structs: `status` and `pass` in both `XTask` and `Pool`.

Tests enabled (failing): exec CLOCK `await 2/3/4`, TOGGLE `toggle 3`..`9`,
`toggle filter`, `toggle filter block` (`toggle 1/2` are error-trace
formats); tasks `every-clock`; await `op_clock_or`; x `is 3`;
timers.lua all 9; toggle.lua EXEC (`toggle filter combinator`, `toggle
pool target`; the PARSE section checks the reference's AST and is not
convertible).

### Status: implemented

- Lexer: a clock duration is a numeral (`TK_INT`/`TK_FLT` in
  microseconds; `TK_CLK` is no longer produced). Parser: `toggleexp`
  (both forms; the block is a closure of kind `CB_TOGGLE`).
- Runtime: `CLKMIN`/`CLKLATE` statics; `awaitmatch` handles a numeric
  pattern as a countdown kept in the pattern slot and the tag `clock`;
  `emitaux` splits numeric emits to the root in pieces; `atmos.next()`;
  `atmos.stop()` aborts the body of the innermost `atmos.loop` (the
  ancestor directly under the root) rather than the whole root pool, so
  that nested loops, as the test harness, survive. Toggle: `status` and
  `pass` in `XTask` and `Pool`; `toggleoff` builds the gate as a C
  closure task (`gateloop`/`gate_k`) off the tree; `emittree` drives the
  gate of a toggled node and lets the emit through when the gate
  stamped `pass` during that drive (`pass` is cleared before driving
  and tested for non-zero afterwards, not compared with the emit time:
  here a branch termination inside the gate's `or` filter broadcasts a
  new time stamp, while the reference's `par_any` ends by a throw);
  `toggle on` is `atm_toggle_on`: body and controller tasks in a
  to-be-closed set, waited with `parany_k`.
- Several `with` filters are awaited as an `or` (the reference awaits
  the first only).
- Results: exec 189, tasks 210, await 17, x 22, timers 9, toggle 2, all
  passing.

## Stage 3: environments (design)

Reference: `run.lua` (`M.env`, `M.loop` step loop, `M.start`,
`M.stop`), `atmos/env/README.md`, `atmos/env/clock/init.lua`, tests
`tst/envs.lua` (Lua API; rewritten in Atmos as `testes/envs.atm`),
`tst/guide.lua`.

- `atmos.env(e)`: registers a table with `step` (polls once, emits;
  returning true quits the loop), optional `quit` (teardown) and
  optional `mode = {primary=, secondary=}`. Registration order: a
  second env requires the first to support primary mode and itself
  secondary mode ("invalid env : first env must support primary mode",
  "invalid env : non-first envs must support secondary mode");
  `mode.current` is set to 'primary'/'secondary' then. An invalid
  registration leaves nothing registered.
- `atmos.loop(body, ...)`: spawns the body (under the running task if
  any, else the root: nested loops, as the test harness, then see the
  emits of a step issued from the enclosing task), then while the body
  is alive calls each env's `step` in order, quitting if one returns
  true, and sweeps dead tasks after each round; with no env and a
  suspended body it returns (the reference spins). On exit, normal or
  by error, it closes the body and calls `quit` on each env in reverse
  order, unregistering them; the body's `ret` is the value.
- `atmos.start(body, ...)`: for host-driven environments: requires a
  single env without `mode` ("invalid start : expected single-mode env
  only"), spawns the body and returns.
- `atmos.stop()`: aborts the innermost loop body (or everything, outside
  a loop), quits and unregisters the envs, resets the clock.
- `__atmos` metamethod: a pattern with it is matched by
  `__atmos(pattern, event)` (event nil at the start of the await); an
  event with it is matched by `__atmos(event, pattern)`; a true result
  returns the event, another truthy result is the value, false ends the
  matching for that event (no default matching).
- Clock env: `require "atmos.env.clock"` (a C module in
  `package.preload`): `step` emits the elapsed microseconds of
  `os.clock()` to the root; module fields `now`, `env`.
- Loop bodies carry a flag so `atmos.stop()` finds the innermost one.

### Status: implemented

- `latmlib.c`: registry table `atmos.envs`; `atm_env` validates before
  registering (so a failed registration leaves nothing); `atm_loop`
  spawns the body under the running task (or the root), flagged
  `isloop`, drives the steps while it lives, sweeps after each round,
  and tears down (close the body, `quit` the envs in reverse order,
  unregister) through a to-be-closed table of the C frame, so also on
  errors; with no env and a suspended body it returns. `atm_start`,
  `atm_stop` (innermost loop body by the `isloop` flag, else the whole
  root; then quits the envs and resets the clock). `__atmos` checked
  first in `awaitmatch` for the pattern, then for the event (false
  ends the matching). Clock env: `package.preload["atmos.env.clock"]`
  (`clock()` of the C library, as the reference's `os.clock`), emitting
  to the root through `emit_in`.
- Note: a to-be-closed slot cannot be moved on the stack
  (`lua_insert`/`lua_rotate` after `lua_toclose` breaks it); `atm_loop`
  arranges the stack first.
- Tests: `testes/envs.atm` (hand-written after the reference's
  `tst/envs.lua`, which uses the Lua API): quit (single env, on error,
  two envs in reverse order, a step returning true), the mode errors,
  primary + secondary, `__atmos` (event true/false, pattern true/false),
  `start`/`stop` (inside a nested loop: `stop` aborts the innermost
  loop body, which otherwise is the test itself), the clock env.
  `testes/ref/tst/guide.atm` (real-time clock, 5 seconds) is not run.

Note: exec `loop 7` (`loop k,v in [x=1,y=2]`) accepts two output orders
in the reference (`assert(out == a or out == b)`); the converter keeps
the first only and string hashing is randomized per run, so the case is
flaky and stays disabled.

## Pools and `++`

`xs ++ ys` with pools: `concatinto` (ltm.c) iterates a table through
its `__pairs` metamethod when it has one (pools keep their tasks in a
hidden table and expose them through `__pairs`), appending the values
in order; the result is a plain table of tasks (`tasks 24`).
