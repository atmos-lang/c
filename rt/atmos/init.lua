--
-- Atmos runtime: Lua layer on top of the C core ('atmos.core').
--
-- The C core implements the task tree, scheduling, events, timers,
-- toggling, errors and the value helpers (X). This module provides the
-- public API (globals) and the derived combinators (await patterns,
-- par/watching/loop_on, toggle blocks, threads).
--

local core = require "atmos.core"
require "atmos.util"

local X = core.x
package.loaded["atmos.x"] = X
_G.X = X   -- global bc of threads/lanes

local RT = core.rt
local S  -- streams (lazy)

local atmos = {
    loop   = core.loop,
    --
    start  = core.start,
    stop   = core.stop,
    --
    status = core.status,
    env    = core.env,
    gc     = core.gc,
    next   = core.next,
    --
    thread_modules = {}, -- requires to pass to threads
}

loop  = core.loop
start = core.start
stop  = core.stop

throw   = core.throw
catch   = core.catch
defer   = core.defer
tasks   = core.tasks
abort   = core.abort
task    = core.task
xtask   = core.xtask
spawn   = core.spawn
spawn_in = core.spawn_in
do_spawn = core.do_spawn
emit    = core.emit
emit_in = core.emit_in
escape  = RT.escape
_break_ = RT["break"]

-- clock duration constants (base unit = microseconds)
_us_  = 1
_ms_  = 1000 * _us_
_s_   = 1000 * _ms_
_min_ = 60 * _s_
_h_   = 60 * _min_
_day_ = 24 * _h_

-------------------------------------------------------------------------------
-- AWAIT : combinators on top of the primitive 'core.await'
-------------------------------------------------------------------------------

local function stream_is (v)
    if S == nil then
        local ok, mod = pcall(require, "atmos.streams")
        S = ok and mod or false
    end
    return S and S.is(v)
end

local await_

await_ = function (time, awt, ...)
    local tag = ((type(awt) == 'table') and awt.tag) or awt
    if tag=='or' or tag=='and' then
        local fs = {}
        for _,sub in ipairs(awt) do
            fs[#fs+1] = function () return await_(time, sub) end
        end
        local f = (tag=='or' and par_any) or par_all
        return f(table.unpack(fs, 1, #awt))
    elseif tag == 'not' then
        assertn(2, #awt==1, "invalid await : too many arguments")
        -- pass `time` into each re-await: an internal reject keeps the
        -- original birth time, so it does not shadow an in-flight outer emit
        while true do
            local ret = table.pack(par_any(function()
                await_(time, awt[1])
                return false
            end, function()
                return true, await_(time, true)
            end))
            if ret[1] then
                return table.unpack(ret, 2, ret.n)
            end
        end
    elseif tag=='until' or tag=='while' then
        if type(awt[1]) == 'function' then
            local f = awt[1]
            if tag == 'while' then
                f = function (e) return not awt[1](e) end
            end
            return core.await(time, f, true)
        else
            assertn(2, #awt==2 and type(awt[2])=='function',
                "invalid await : expected single function predicate"
            )
            -- until: accept when all predicates hold (last one decides result);
            -- while: accept when any predicate fails (returns the event).
            while true do
                local it = await_(time, awt[1])
                local res = awt[2](it)
                if tag=='until' and res then
                    return (res~=true and res) or it
                elseif tag=='while' and (not res) then
                    return it
                end
            end
        end
    elseif stream_is(awt) then
        return await_(time, spawn(task(function () return awt() end)))
    elseif tag == 'spawn' then
        -- spawn the prototype inside the awaiting task/branch, then await it
        assertn(2, X.is(awt[1],'task'), "invalid spawn : expected task prototype")
        return await_(time, spawn(awt[1], table.unpack(awt, 2, awt.n or #awt)))
    end
    return core.await(time, awt, ...)
end

function await (awt, ...)
    if X.is(awt, 'task') then
        -- await T(...) : wrap into a spawn carrier
        local t = table.pack(awt, ...)
        t.tag = 'spawn'
        return await_(core.time(), t)
    elseif type(awt) == 'function' then
        -- await f(...)
        assertn(2, false, "invalid spawn : expected task prototype")
    else
        -- await(...)
        return await_(core.time(), awt, ...)
    end
end

-------------------------------------------------------------------------------
-- TOGGLE
-------------------------------------------------------------------------------

function toggle (t, on, filter, ...)
    if type(t) == 'string' then
        --@ derived: spawn body; loop { await; toggle; await; toggle }
        local e = t
        local f = filter or on               -- body is the last arg
        local p = (filter ~= nil) and on or nil
        assertn(2, type(f)=='function', "invalid toggle : expected task prototype")
        do
            local t <close> = core.spawn_tra(f)
            local _ <close> = core.spawn_tra(function ()
                while true do
                    await_(core.time(), {tag=e, false})
                    toggle(t, false, p)   -- task primitive sets up the filter
                    await_(core.time(), {tag=e, true})
                    toggle(t, true)
                end
            end)
            return await_(core.time(), t)
        end
    end
    if filter ~= nil and on == false then
        -- gate task (off-tree): keeps the task responsive to the filter
        local gate = function ()
            while true do
                await_(core.time(), filter)
                t._.toggle.pass = core.time()
            end
        end
        return core.toggle(t, on, gate)
    end
    return core.toggle(t, on, filter)
end

-------------------------------------------------------------------------------
-- THREAD (via LuaLanes)
-------------------------------------------------------------------------------

local lanes -- lazy require in `thread`
local _gen_cache = setmetatable({}, { __mode = 'k' })

function thread (f)
    local me = core.me(true)
    assertn(2, type(f)=='function', "invalid thread : expected body function")
    assertn(2, me, "invalid thread : expected enclosing task")

    if not lanes then   -- lazy require
        -- requires lanes installed only if uses `thread`
        lanes = require("lanes").configure()
    end

    local gen = _gen_cache[f]
    if not gen then
        gen = lanes.gen("*", function (linda, mods)
            for _,mod in ipairs(mods) do
                require(mod)
            end
            linda:send("ok", { pcall(f) })
        end)
        _gen_cache[f] = assert(gen)
    end

    local linda = lanes.linda()
    local lane = assert(gen(linda, atmos.thread_modules))

    local _ <close> = defer(function ()
        while lane.status == 'pending' do
            -- busy wait: "Not started yet. Shouldn't stay very long in that state."
        end
        assert(lane:cancel('hard', 0, true, 1))
    end)

    while true do
        local key, t = linda:receive(0, "ok")
        if key then
            if t[1] then
                return table.unpack(t, 2)
            else
                error(t[2], 0)
            end
        else
            await_(core.time(), true)
        end
    end
end

-------------------------------------------------------------------------------
-- DERIVED COMPOUNDS : loop_on, par, par_any, par_all, watching
-------------------------------------------------------------------------------

--@ derived: loop { f(await(awt, payload...)) }
function loop_on (...)
    assertn(2, core.me(true), "invalid loop_on : expected enclosing task")
    local t = { ... }
    local blk = table.remove(t, #t)
    -- tag-specific catch: break exits the loop, but return (atm-func),
    -- abort, and any other throw still propagate past loop_on
    catch('atm-loop', function ()
        while true do
            blk(await_(core.time(), t[1], table.unpack(t, 2, #t)))
        end
    end)
end

local meta_par = {
    __close = function (ts)
        for _, t in ipairs(ts) do
            getmetatable(t).__close(t)
        end
    end
}

--@ derived: spawn each + await(false) + lifetime
function par (...)
    assertn(2, core.me(true), "invalid par : expected enclosing task")
    local fs = { ... }
    local ts <close> = setmetatable({}, meta_par)
    for i,f in ipairs(fs) do
        assertn(2, type(f) == 'function', "invalid par : expected task prototype")
        ts[i] = core.spawn_tra(select(i,...))
    end
    await_(core.time(), false)
end

--@ derived: throw-based race
function par_any (...)
    assertn(2, core.me(true), "invalid par_any : expected enclosing task")
    local fs = { ... }
    local trap = {}
    return (function (_, _, ...) return ... end)(catch(trap, function ()
        local ts <close> = setmetatable({}, meta_par)
        for i,f in ipairs(fs) do
            assertn(2, type(f) == 'function', "invalid par_any : expected task prototype")
            ts[i] = core.spawn_tra(function ()
                throw(trap, f())
            end)
        end
        await_(core.time(), false)
    end))
end

--@ derived: sequential await on each spawn
function par_all (...)
    assertn(2, core.me(true), "invalid par_all : expected enclosing task")
    local fs = { ... }
    local ts <close> = setmetatable({}, meta_par)
    for i,f in ipairs(fs) do
        assertn(2, type(f) == 'function', "invalid par_all : expected task prototype")
        ts[i] = core.spawn_tra(f)
    end
    local rets = {}
    for i,t in ipairs(ts) do
        rets[i] = await_(core.time(), t)
    end
    return table.unpack(rets, 1, #fs)
end

--@ derived: par_any { await(awt, payload...) } with { f() }
function watching (...)
    assertn(2, core.me(true), "invalid watching : expected enclosing task")
    local t = { ... }
    local f = table.remove(t, #t)
    assertn(2, type(f) == 'function', "invalid watching : expected task prototype")
    return par_any(
        function () return await_(core.time(), t[1], table.unpack(t, 2, #t)) end,
        f
    )
end

-------------------------------------------------------------------------------
-- RUNTIME TABLE for generated code
-------------------------------------------------------------------------------

RT.await    = await
RT.toggle   = toggle
RT.par      = par
RT.par_any  = par_any
RT.par_all  = par_all
RT.watching = watching
RT.thread   = thread

return atmos
