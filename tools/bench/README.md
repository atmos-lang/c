# Game benchmarks: reference vs this implementation

Three Atmos games, run by the reference (Lua) implementation and by this
one, with the same input and the same simulated time, measuring frames
per second and memory.

- Rocks: `atmos-lang/pico-rocks` (`main.atm`, 83ce521)
- Birds: `atmos-lang/pico-birds` (`birds-11.atm`, the last tutorial step, d72bca5)
- SNKRX: `kboltiz/snkrx`, `atmos/` (the Atmos port, 18c593b)

Reference: `atmos-lang/atmos` a06998a + `lua-atmos/atmos` aa0a99a (main,
the version this implementation follows; the released 0.8 rocks have no
tick splitting and play Rocks differently), Ubuntu's Lua 5.4.6.
Both use pico-sdl v0.7 (`fsantanna/pico-sdl`, built against each
interpreter's headers).

## How

`setup.sh` builds everything under `$W` (default `~/atmos-bench`);
`bench.sh <ref|ours> <game> <frames> <draw 1|0> [shots]` runs one game
and prints a `BENCH` line.

- `ref/atmos/env/pico/init.lua` replaces `atmos.env.pico` (the reference
  loads it as is; this implementation gets it converted by
  `tools/lua2atm.lua`). It keeps the env's steps (a clock tick, the
  events, clear/`draw`/present) but: a fixed 33 ms tick per frame (same
  game time in both), no frame pacing, a fixed random seed, scripted
  input (keys for Rocks, clicks and pause for Birds, menu/cards/Go
  clicks and a circling mouse for SNKRX), a virtual mouse (events and
  `pico.get.mouse` follow the script: real pointer warps reach SDL
  asynchronously through X, which made SNKRX non-deterministic), and
  measurements at the end.
- Headless: `xvfb-run`, `SDL_RENDER_DRIVER=software`, dummy audio.
- Modes: `draw 1` (full frames, rendering included) and `draw 0` (no
  clear/draw/present: the tick and the input only, i.e. the language
  runtime).
- Memory: the Lua heap (`collectgarbage "count"` sampled every frame:
  peak and average) and the process peak RSS (`VmHWM`).
- `prepare.py` copies the games and rewrites what this implementation
  does not accept yet (see below); both implementations run the
  rewritten copies.

## Same behavior

Screenshots every 10 frames (`BENCH_SHOT`, `BENCH_EVERY`; `compare.py`):
Rocks (1500 frames) and Birds (1500 frames) are pixel-identical in both
implementations. SNKRX (3000 frames) is identical up to frame 357; from
there the clock pieces differ: the reference re-reports a timer's
remaining time whenever any event resumes its task, so a termination
event in the middle of a 3 ms piece reports a timer that the same piece
then expires, and the next piece is split at that stale value
(3+3+27 ms instead of 3+30 ms). This implementation does not resume
timers on other events (the match is decided in C). With that report
added (an experiment, not committed) SNKRX is pixel-identical for the
3000 frames too. See `.claude/plans/coroutine.md`.

## Results

Intel Xeon @ 2.10GHz (4 cores), median of 3 runs of 5000 frames.

Frames per second:

| game  | mode      | reference | this    | ratio |
|-------|-----------|----------:|--------:|------:|
| Rocks | full      |     135.4 |   142.1 | 1.05x |
| Rocks | logic     |      3826 |   14368 | 3.8x  |
| Birds | full      |     343.1 |   364.8 | 1.06x |
| Birds | logic     |      9042 |   47170 | 5.2x  |
| SNKRX | full      |     205.1 |   292.7 | 1.43x |
| SNKRX | logic     |      1330 |   11628 | 8.7x  |

Memory (full mode):

| game  | heap peak ref / this | heap avg ref / this | peak RSS ref / this |
|-------|---------------------:|--------------------:|--------------------:|
| Rocks | 1699 / 399 KB        | 1089 / 249 KB       | 145.9 / 145.6 MiB   |
| Birds | 1244 / 261 KB        | 879 / 173 KB        | 126.5 / 126.0 MiB   |
| SNKRX | 3377 / 865 KB        | 2026 / 587 KB       | 131.0 / 129.5 MiB   |

Notes: full frames are dominated by the software renderer (a few ms per
frame), so the language shows mostly in the logic mode (per frame:
Rocks 0.26 vs 0.07 ms, Birds 0.11 vs 0.02 ms, SNKRX 0.75 vs 0.09 ms).
The heap of the reference also holds its compiler's output (Lua source
and closures of the compiled program); the RSS is dominated by SDL, X
and the textures (~125 MiB in both).

## Not accepted yet by this implementation (rewritten by prepare.py)

- `e where { x = .. }` (Rocks, Birds, SNKRX): bindings hoisted to
  `val`s before the statement, with fresh names.
- statements after `=>` in `match`/`ifs`: `=> set x = v`, `=> break()`
  (Rocks, SNKRX): braced, `=> { set x = v }`.
- bare `break` (Birds) and `return [ .. ]` (SNKRX): `break()`,
  `return([ .. ])`.
- `until (c, v)` / `while (c, v)` with a value (SNKRX):
  `if c { break(v) }`.
- `spawn v->f(..)` (SNKRX): `spawn f(v, ..)`.
- `math.pow` (Rocks): Ubuntu's Lua 5.4 has the 5.3 compat functions; the
  benchmark env defines it for this implementation.
- SNKRX's shop lists characters in hash-table order (`loop name, _ in
  CHARACTERS`), which differs between Lua 5.4 and 5.5: sorted.

Bugs found by the games and fixed (with tests): `watching` a pattern that
already holds (a terminated task) let the watcher's throw escape;
`set t.f@+ = v` clobbered a temporary register; `until`/`while` on the
line after a bare `await P` were taken as predicates of the pattern.
