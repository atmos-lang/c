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

- `XTask` userdata: coroutine (`lua_State*`, kept alive through the
  user value table), `up`, `dns` (table), `status` (none/aborted/
  toggled), `time`, `tra` (transparent), `pin`, `ret`, `toggle` gate.
  `Tasks` userdata: `up`, `dns`, `max`, `toggle`. `Task` prototype
  userdata: the function. A weak table maps coroutines to xtasks
  (`me`).
- `spawn`: create, link, `lua_resume`; the result handling is
  `task_result` (error: close subtree and re-raise with all values;
  aborted while running: close now; dead: store `ret`, emit the task to
  the owner's await, close subtree).
- `emit`: recursive traversal in C with `lua_pcall` per child; a child
  error wakes the owner with `lua_resumeerror`; the owner is resumed
  with `(nil, e)` when `time` allows. Clock ticks (`emit @:global 10s`)
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
