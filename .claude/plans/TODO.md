# TODO

## 1. Changes to coroutines

See [coroutine.md](coroutine.md) for the full items and discussion.

- a. `coroutine.error(co)`: raise an error inside a suspended coroutine.
- b. Close a running (normal) coroutine.
- c. Bounded stacks; "stackless" coroutines and tasks (yield only at the
  prototype level, shallow frame kept while suspended).

## 2. Tail calls

- Tail call optimization is bad for error messages / tracebacks
  (`(...tail calls...)`). Could it be disabled? Or made explicit
  (opt-in syntax)?
