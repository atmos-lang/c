# TODO

## 1. Changes to coroutines

- a. `coroutine.error(co)`: raise an error inside a suspended coroutine
  (resume it with an error instead of values).
- b. Close a running (normal) coroutine.
  (Lua 5.5 already allows `coroutine.close()` with no argument to close the
  running coroutine itself; closing a *normal* coroutine, one that resumed
  another, is still an error: "cannot close a normal coroutine".)
- c. Bounded stack for coroutines? Pass `N` in `coroutine.create`?
  (What is `N` exactly: number of stack slots? C calls? Lua call depth?)
  For tasks, `N` could be inferred.

## 2. Tail calls

- Tail call optimization is bad for error messages / tracebacks
  (`(...tail calls...)`). Could it be disabled? Or made explicit
  (opt-in syntax)?
