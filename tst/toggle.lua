require "test"

-- These tests encode the POST-FIX behavior of toggle-filter patterns: the
-- `with` filter must route through `parser_await`, so `&&`/`||`/`!`
-- combinators and `:any`/`:all` pools lower to `{tag=...}` tables (like
-- `await` / `loop on` / `watching`), NOT Lua boolean operators.
-- They FAIL until the fix lands (TDD).

-- (parse-shape tests of the reference implementation removed: no AST printer)

do
    -- a single-predicate `until`/`while` stops at the comma, so a
    -- trailing pattern joins the enclosing filter list (no swallowing)
    local src = "toggle t(false) with :a until (c), :b"
    print("Testing...", src)
    init()
    lexer_init("anon", src)
    lexer_next()
    local e = parser()
    assert(check('<eof>'))
end

print '--- TOGGLE FILTER : EXEC ---'

do
    -- combinator filter passes multiple events while toggled off
    local src = [[
        val T = task () {
            spawn (task () { loop on :Draw  { print(:draw)  } }) ()
            spawn (task () { loop on :Click { print(:click) } }) ()
            loop on :Tick { print(:tick) }
        }
        pin t = spawn T()
        emit(:Tick)                            ;; on  -> tick
        toggle t (false) with <:Draw || :Click>  ;; off, both pass
        emit(:Draw)                            ;; passes -> draw
        emit(:Click)                           ;; passes -> click
        emit(:Tick)                            ;; gated
        toggle t (true)
        emit(:Tick)                            ;; on  -> tick
    ]]
    print("Testing...", "toggle filter combinator")
    local out = atm_test(src)
    assertx(out, "tick\ndraw\nclick\ntick\n")

    -- toggle a whole tasks pool: gate/un-gate all members
    local src = [[
        val T = task (v) {
            loop {
                await(true)
                print(v)
            }
        }
        pin ts = tasks()
        spawn @ts T(1)
        spawn @ts T(2)
        emit(:X)
        toggle ts(false)
        print("---")
        emit(:X)
        emit(:X)
        print("---")
        toggle ts(true)                         ;; emit(0, ts): awakes all
        emit(:X)
    ]]
    print("Testing...", "toggle pool target")
    local out = atm_test(src)
    assertx(out, "1\n2\n---\n---\n1\n2\n1\n2\n")
end
