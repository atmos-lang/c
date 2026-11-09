# Coroutines

Planned changes to the coroutine library and to the thread implementation
(`lstate.c`, `ldo.c`, `lcorolib.c`), motivated by Atmos tasks.

## a. `coroutine.error(co, err)`

Resume a suspended coroutine `co` with an error instead of values: the
pending `coroutine.yield()` inside `co` raises `err`. Pending
to-be-closed variables and `catch` blocks of the task see the error as
if it were raised at the yield point.

Today the only ways to wake a suspended coroutine are `resume` (with
values) and `close` (kills it, running `__close` handlers with no error
object from the caller's point of view).

## b. Close a running (normal) coroutine (not needed)

Lua 5.5 already allows `coroutine.close()` with no argument: the running
coroutine closes itself. Closing a *normal* coroutine (one that resumed
another one and is waiting for it) is still an error:
"cannot close a normal coroutine", because its `lua_resume` C frame is
live below the running coroutine.

Decision: not done. The only Atmos case that goes against the resume
chain is `abort(t)` of an ancestor (directly or through an `emit`);
throws, `escape`/`break`/`return` across `spawn`/`par`/`watching`
blocks all travel upward as errors returned by `resume` and need
nothing new. For the ancestor case the task layer uses the reference's
scheme: after an `emit`/`abort`, a task checks whether it or an ancestor
was aborted and yields, so control climbs back up until the aborted
task is suspended and its resumer closes it (`yield_if_aborted`,
`task_result`, `'aborted'` status in `run.lua`). A core "close when
control returns" (mark the normal coroutine, run `lua_closethread(from,
from)` at the end of `lua_resume`) would remove that cooperation, but
it is a convenience, not a requirement; revisit with the task layer if
the scheme proves too costly.

## c. Bounded stacks and "stackless" coroutines

### The idea

Two kinds of coroutines:

1. **stackful**: as today, a private Lua stack that grows on demand
   (`luaD_growstack`), yields allowed from any nesting level of Lua
   calls (and from yieldable C calls).
2. **stackless**: new. Not really stackless, see below.

`coroutine.create` receives the prototype (function or task) plus two
optional arguments:

1. a boolean: stackful or not;
2. a size: the stack size.

A stackless coroutine can only yield **at the prototype level**: from the
body of the function given to `create`, never from a nested call. Atmos
tasks are always stackless. The compiler computes the stack size a
prototype needs from its parameters and locals (this is `Proto.maxstacksize`,
plus the vararg area for vararg functions). This size is the *shallow size*
of the prototype, and task prototypes carry it around.

The trick: a task body still calls other functions, and although they
cannot yield they need stack space (nested calls, temporaries, C calls).
So a stackless coroutine, while *running*, uses a full working stack; while
*suspended* only the shallow frame is kept. On resume the shallow frame is
restored onto a full stack; on yield only the shallow frame is saved.

### Does it make sense?

Yes. Restated in terms of the implementation:

- A suspended stackful coroutine keeps a whole `lua_State`: the stack array
  (at least `BASIC_STACK_SIZE` = 40 slots, usually more after it grew),
  the `CallInfo` list, and so on. Thousands of suspended tasks cost
  thousands of stacks.
- With the "yield only at prototype level" restriction, a suspended
  coroutine has exactly one Lua frame: the body's frame. Its live state is
  `maxstacksize` registers plus the extra arguments of a vararg body.
  Everything below or above that frame is dead at the yield point.
- Therefore the suspended representation can be a buffer of exactly the
  shallow size. The full working stack is only needed between resume and
  yield, and it can be shared (one scratch stack per `lua_State` / per
  scheduler, or simply the resumer's own stack above its top).
- "Task protos carry their shallow size" is already true in Lua:
  `Proto.maxstacksize` is stored in every prototype (and in bytecode
  dumps). The vararg part is dynamic (number of extra arguments), so the
  saved size is `maxstacksize + nextraargs`, known at resume/yield time.

So the design is coherent. The restriction and the trick are the two halves:
the restriction makes the suspended state small and well-defined, the trick
keeps calls from the body working without giving each coroutine a big stack.

### Points to settle

1. **Enforcing the restriction.** `lua_yield` (and `luaD_throw`'s
   yield path) must check that the yielding frame is the base frame of the
   coroutine: `L->ci` is the first Lua `CallInfo` of the thread. A yield
   from a nested call raises "attempt to yield across a non-yieldable call"
   (or a specific message). Note that this also forbids yields inside
   `pcall`, metamethods, `table.sort` callbacks, etc., which are nested
   calls. For tasks it means that `await` must be compiled *inline* in the
   task body: `catch`, `defer`, `do :tag {}`, loops, etc. cannot be
   closures (the C implementation already plans blocks as inline code,
   unlike the Lua reference which turns blocks into closures).
2. **What `size` means.** Number of stack slots (`TValue`s). Options:
   - for stackless: the shallow size, but it can be inferred from the
     prototype (`maxstacksize`), so the explicit argument is only useful as
     an override or can be dropped;
   - for stackful: an upper bound (`LUAI_MAXSTACK` per coroutine) to catch
     runaway recursion early with "stack overflow", and/or the initial
     allocation to avoid regrowth. A bound in slots is simpler than a
     bound in call depth; `nci` (number of `CallInfo`s) could be bounded
     too.
3. **Saving and restoring the frame.** Two possible implementations:
   - *Swap*: the suspended coroutine owns only a small buffer. On resume,
     the frame is copied onto a working stack (the scheduler's thread
     stack, above its top, or a dedicated scratch thread); on yield it is
     copied back. Open upvalues that point into the frame (closures
     created by the body that capture its locals) must be relocated, as
     `correctstack` already does when a stack is reallocated; the saved
     buffer must be visited by the GC.
   - *Shrink*: keep a private `lua_State` per coroutine but size its stack
     to the shallow size while suspended (`luaD_reallocstack` down on
     yield, up on resume). Simpler, reuses existing code, but one
     reallocation per switch and the `lua_State` header itself (about 200
     bytes plus a `CallInfo`) is still paid per task.
   The swap variant gives the smallest suspended footprint; the shrink
   variant is a smaller change. Measure before choosing.
4. **Vararg bodies.** In Lua 5.5 a vararg function keeps its extra
   arguments below `func` (`ci->u.l.nextraargs`). The shallow frame must
   include them (or tasks forbid varargs).
5. **C functions.** A stackless body may call C functions that call back
   into Lua (`pcall`, `string.gsub`); fine as long as they do not yield.
   `lua_callk`/`lua_pcallk` continuations are irrelevant for them.
6. **`coroutine.wrap`, `close`, `status`, `isyieldable`.** Same API;
   `isyieldable` is true only in the base frame of a stackless coroutine.
7. **Scheduler.** Atmos tasks can skip `lua_State` creation entirely in the
   swap variant: a task is a closure plus a saved frame buffer. Emitting an
   event into thousands of waiting tasks then costs one frame copy per
   awakened task instead of one full coroutine switch.

### Measurements and options (proposal, to decide)

Measured with the current implementation (`testes` build): a task
suspended at its body level costs 1534 bytes; suspended 20 calls deep,
3846; one `emit` waking 20000 waiting tasks takes 11 ms (0.55 us per
task). The 1534 bytes are: `lua_State` 256, its stack 45 slots x 16 =
720 (`BASIC_STACK_SIZE`, allocated at creation and never shrunk), two
`CallInfo`s (the body frame and the C frame of `await`) 144, and about
400 for the xtask table, its link table and the struct userdata. Each
call level at the suspension point adds a `CallInfo` (72) plus stack.

Options:

A. Keep stackful coroutines (status quo). No language restriction;
   `await` works in helper functions called by a task, in callbacks,
   anywhere a yield works. About 1.5 KB per idle task.

B. Shrink the stack of suspended tasks. Keep one `lua_State` per task
   and give back the unused part of its stack. Measured with a
   prototype (scratch copy, 20000 tasks each in `loop { await(:X) }`,
   50 emits; "busy" also allocates a table per wake so that GC cycles
   run during the emits):

   | variant                                   | bytes/task | us/wake idle (1k / 20k tasks) | us/wake busy (1k / 20k) |
   |-------------------------------------------|-----------:|--------------:|--------------:|
   | none (status quo)                         | 1562-1582  | 0.53 / 0.79   | 0.73 / 1.03   |
   | B1 safe: shrink on yield to Lua's in-use  | 1322-1342  | 0.54 / 0.88   | 0.70 / 1.00   |
   | B2 tight: shrink on yield to top, regrow on resume | 1018-1038 | 0.73 / 1.06 | 1.10 / 1.81 |
   | B3 lazy: tight shrink only in GC, regrow on resume | 1018-1038 | 0.56 / 1.03 | 0.77 / 1.34 |

   (Clean builds with compile-time switches; a first round used
   `getenv` in the resume path and overstated all times.)

   Lua's "in use" includes the 20 slots (`LUA_MINSTACK`) reserved for
   the C frame of `await`, so B1 only saves 15 slots (240 bytes); after
   the first suspension the stack already has that size and no further
   reallocation happens (no measurable cost). B2 drops the reserve but
   pays a reallocation on every yield and every resume: +40-75% per
   wake when tasks are busy. B3 drops it only when a GC cycle traverses a
   suspended task (`luaD_shrinkstack`, already called by the GC for
   every thread), so a task pays at most one regrow per GC cycle and
   only if it was shrunk: idle tasks get the small stack; busy tasks
   pay +5% (1k tasks) to +30% (20k tasks). B3 needs care: the stack of a
   suspended task can then be smaller than its C frame's `ci->top`, so
   it must be regrown before anything is pushed onto it (resume, and
   `lua_xmove`/pushes by the task runtime), and `stackinuse` must not
   assert on it.

   The remaining ~1 KB is: `lua_State` 256, stack ~6+5 slots 176, two
   `CallInfo`s 144, and ~450 of task tables (xtask table, link table,
   struct userdata); the tables are the next target (one userdata with
   user values instead of tables), independent of A/B/C.

C. Stackless (the idea above, swap variant). A suspended task is a
   closure plus a saved copy of its body frame (`maxstacksize` slots,
   about 200-300 bytes), no `lua_State`; waking copies the frame onto a
   working stack. About 5x less memory than today and a faster wake-up,
   but a language rule: `await` (and `emit` paths that may suspend)
   only in the task body's own frame and in its inline blocks (`do`,
   `loop`, `if`, `catch`, `watching`, `loop on`: all inline now), never
   in a function called by the task, in a `defer`, a `pcall` callback
   or a metamethod. Helper functions that await become impossible; the
   reference allows them (plain Lua coroutines). Also a large change:
   a new coroutine representation beside `lua_State`, GC traversal of
   saved frames, relocation of open upvalues on each swap, and
   `coroutine.*` on tasks losing meaning.

Games case (thousands of tasks, all woken every tick): wake cost
dominates, not memory. Every stack is full size during each tick
anyway, so shrinking buys nothing between ticks and B1/B2/B3 only add
cost. Measured per wake (clean build): task wake 0.53 us (1000 tasks)
/ 0.79 us (20000); raw `coroutine.resume`/`yield` from a Lua loop
0.14-0.16 us; a plain Lua call from the same loop 0.03 us. So the
coroutine switch itself is ~0.12 us, and the task runtime is the
remaining ~0.4-0.6 us (75-80% of a wake). gprof of the emit benchmark:
~12 `lua_getfield` (string-keyed lookups in the xtask/link/struct
tables, `luaH_getstr` alone ~24% of the time), 3 `luaL_getmetafield`
(`__atmos`) and ~18 `lua_type` per wake.

Stackless (C) for this case. What it can save: memory, ~1.1 KB per
task (after the struct refactor below) down to ~0.3-0.5 KB (closure,
saved body frame, task struct); per wake, at most the switch, ~0.12
us. What it costs: (1) the rule "await only in the task body and its
inline blocks"; the reference tests never break it (instrumented run
of `testes/ref`: 567 awaits, all from the body frame or from the C
body of a transparent task), but lua-atmos' env-socket does
(`xaccept`, `xconnect`, `xrecv` are plain functions that await; they
would become tasks, `await(xrecv(tcp))`); (2) the largest change so
far: resuming a Lua frame from a saved copy on a shared stack,
relocating open upvalues, the tbc list and the try regions on each
swap, GC traversal of saved frames, per-task error recovery without a
per-task `lua_State`, debug info/tracebacks of suspended tasks. A
middle ground without the rule is stack copying (save all of the
task's frames, any depth, to a block reused across switches): memory
like B2 (~1 KB, `lua_State` kept), switch a bit cheaper than B2 (two
memcpy + pointer correction, no malloc), i.e. still dearer than today.

Wasted resumes. Today (as in the reference) `emit` resumes every
suspended task of the subtree and the match runs inside the task
(`awaitmatch` in the `await` loop), which yields again on a miss. In a
game most tasks await something other than the current event, so most
resumes are wasted. Measured (clean build, 1000 / 20000 tasks): an
emit that matches no task costs 0.37 / 0.58 us per task (70% of a
matching wake, 0.55 / 0.89); a clock tick that expires no timer costs
0.31 / 0.51 us per task. Deciding the match in C before resuming
removes these: keep the awaited pattern in the task struct (pattern
value in a user value slot, kind and clock countdown in C fields) and
let `emittree` test it:
- tag/value patterns: equality / tag hierarchy, decided in C;
- clock: the countdown is decremented in C (and `CLKMIN` reported);
  resume only on expiry, with `late`;
- task termination: resume only on the awaited task's event;
- `true` (any event): always resume;
- `__atmos` and Lua predicates (`until`/`while` with a function): user
  code that must run in the task's context; keep resuming (they are
  the minority);
- `or`/`and`/`not`: leaves are transparent tasks with simple patterns,
  filtered the same way.
A filtered-out task then costs a C test (tens of ns) instead of a
resume. Also: `emittree` runs every child under a `lua_pcall` (a
`setjmp` per child, matched or not); resumes are already protected
(`lua_resume` returns the status), so the per-child pcall can go too.

Recommendation (for games): keep stackful coroutines (A, no B) and cut
the runtime overhead of a wake first: keep the task tree and the hot
fields (status, pass, time, up/dns links, awaited pattern) in C
structs (the task userdata) instead of string-keyed tables, and cache
the `__atmos` lookup; on top of it, decide simple matches in C so
that only tasks that proceed are resumed, and drop the per-child
pcall. Expected: wake ~0.2-0.3 us, a non-matching task ~0.05 us or
less, ~400 bytes less per task. Every option needs this anyway. Then measure again: stackless
is worth it only if memory per task becomes the limit (10000 tasks:
~11 MB stackful vs ~4 MB stackless) or the remaining ~0.12 us switch
matters, and the rule is accepted.

### Status

(c): stackless coroutines are not implemented (decision pending, see
above). Implemented instead, as recommended for games: the task
runtime keeps its data in one C struct per task/pool with user-value
slots (no string-keyed user value table), the broadcast decides the
simple matches in C and resumes only the tasks that may proceed, and
the per-child `lua_pcall` is gone (tests `testes/emit.atm`). Measured
(clean build; before -> after):

| per task                         | 1000 tasks      | 20000 tasks     |
|----------------------------------|-----------------|-----------------|
| matching wake                    | 0.52 -> 0.33 us | 0.88 -> 0.59 us |
| event that matches nothing       | 0.37 -> 0.07 us | 0.59 -> 0.08 us |
| tick that expires no timer       | 0.31 -> 0.03 us | 0.51 -> 0.06 us |
| memory of an idle task           | 1562 -> 1514 B  | 1582 -> 1534 B  |

The memory gain is small: the task table, the node userdata (with its
user values) and the empty `dns` table remain; creating `dns` lazily
or merging the tables is the next step if memory matters. A matching
wake still costs ~4 string-keyed lookups (the node from the task
table twice, the coroutine->xtask cache, `__atmos` of the pattern).

Item (a) is implemented
(`lua_resumeerror` in `ldo.c`, `coroutine.error` in `lcorolib.c`;
tests `testes/coerror.atm`); (b) is not needed (see above). Note: as
in Lua, the to-be-closed variables of a coroutine that dies by an
uncaught error stay pending until `coroutine.close(co)`; the task
layer must close dead tasks (the reference does).

## Design for (a)

### a. `coroutine.error(co, e1, ..., en)`

Resumes `co` (which must be suspended by a yield) so that the pending
`coroutine.yield` raises the error values instead of returning. In
`ldo.c`, `resume` gets a flag: when resuming from a yield in error
mode, instead of finishing the yield call (`luaD_poscall` / the
continuation), it raises `luaG_errormsgn(L, n)` with the n values
already moved onto the coroutine's stack. Raising at that point is
exactly an error inside the yield call: the enclosing yieldable
`pcall`/`catch` frames recover it through `precover`, `__close`
handlers and `defer`s run with the error, a message handler installed
by an enclosing `xpcall` is called, and an uncaught error kills the
coroutine and comes back to the caller as `false, e1, ..., en`.
C API: `lua_resumeerror(L, from, nerr, &nresults)` beside `lua_resume`.
Library: `coroutine.error(co, ...)` returns what `resume` returns.
A coroutine that is not suspended by a yield (not started, running,
normal, dead) is an error: "cannot raise error in a <status> coroutine".
A yield inside a hook (`CIST_HOOKYIELD`) raises in the Lua frame at
the hook point.

Tests: `testes/coerror.atm`.

