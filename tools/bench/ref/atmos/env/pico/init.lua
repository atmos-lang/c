-- Benchmark replacement of 'atmos.env.pico' (lua-atmos/env-pico v0.4):
-- same steps (clock tick, events, clear/draw/present), but a fixed
-- simulated tick per frame, no frame pacing, scripted input, a fixed
-- random seed, and measurements printed at the end.
--   BENCH_GAME   rocks | birds | snkrx (input script)
--   BENCH_FRAMES number of frames (default 3000)
--   BENCH_DRAW   0: no clear/draw/present (game logic only)

local atmos = require "atmos"
local pico  = require "pico"

local GAME   = os.getenv("BENCH_GAME") or "none"
local FRAMES = tonumber(os.getenv("BENCH_FRAMES")) or 3000
local DRAW   = os.getenv("BENCH_DRAW") ~= "0"
local DT     = 33
local SHOT   = os.getenv("BENCH_SHOT")  -- screenshots prefix
local EVERY  = tonumber(os.getenv("BENCH_EVERY")) or 500

local M = {
    fps = 30,
    now = 0,
    mode = { primary=true, secondary=true },
}

-- the games call 'math.randomseed()': make it deterministic
local seed = math.randomseed
math.randomseed = function () seed(42) end
seed(42)  -- (games that never call it)

pico.init(true)
pico.set.expert(true, true)     -- no frame pacing (poll)

-- input script: own generator (does not disturb 'math.random')
local rnd = 12345
local function rand (n)
    rnd = ((rnd * 1103515245) + 12345) % 2147483648
    return (rnd // 65536) % n
end

local function key (name, dn)
    local tag = "key.up"
    if dn then tag = "key.dn" end
    emit({ tag=tag, key=name, ctrl=false, shift=false, alt=false })
end

-- a virtual mouse: events and 'pico.get.mouse' follow the script (real
-- pointer warps reach SDL asynchronously, through the X server)
local mx, my, mleft = 0.5, 0.5, false
pico.get.mouse = function (mode)
    return { '%', x=mx, y=my, left=mleft, right=false, middle=false }
end

local function motion (x, y)
    mx = x
    my = y
    emit({ '%', tag="mouse.motion", x=x, y=y, anchor={ x=0.5, y=0.5 },
           left=mleft, right=false, middle=false })
end

local function click (x, y, dn)
    motion(x, y)
    mleft = dn
    local tag = "mouse.button.up"
    if dn then tag = "mouse.button.dn" end
    emit({ '%', tag=tag, x=x, y=y, anchor={ x=0.5, y=0.5 },
           left=dn, right=false, middle=false })
end

local LKEYS = { "A", "D", "W", "S" }
local RKEYS = { "Left", "Right", "Up", "Down" }
local lkey, rkey = nil, nil

local function input (f)
    if GAME == "rocks" then
        if (f % 90) == 5 then key("Return", true) end
        if (f % 90) == 6 then key("Return", false) end
        if (f % 6) == 0 then
            lkey = LKEYS[rand(4) + 1]
            key(lkey, true)
            rkey = RKEYS[rand(4) + 1]
            key(rkey, true)
        end
        if (f % 6) == 4 then
            key(lkey, false)
            key(rkey, false)
        end
        if (f % 10) == 0 then key("Left Shift", true) end
        if (f % 10) == 1 then key("Left Shift", false) end
        if (f % 10) == 5 then key("Right Shift", true) end
        if (f % 10) == 6 then key("Right Shift", false) end
    elseif GAME == "birds" then
        if (f % 15) == 0 then
            click(rand(1000) / 1000, rand(1000) / 1000, true)
        end
        if (f % 15) == 1 then
            click(rand(1000) / 1000, rand(1000) / 1000, false)
        end
        if (f % 600) == 300 then key("P", true) end
        if (f % 600) == 301 then key("P", false) end
        if (f % 600) == 360 then key("P", true) end
        if (f % 600) == 361 then key("P", false) end
    elseif GAME == "snkrx" then
        local t = f / 40
        if (f % 100) == 10 then click(0.08, 0.5, true) end    -- arena run
        if (f % 100) == 11 then click(0.08, 0.5, false) end
        if (f % 100) == 40 then click(0.88, 0.9, true) end    -- Go!
        if (f % 100) == 41 then click(0.88, 0.9, false) end
        if (f % 100) == 20 then click(0.16, 0.3, true) end    -- shop cards
        if (f % 100) == 21 then click(0.16, 0.3, false) end
        if (f % 100) == 24 then click(0.32, 0.3, true) end
        if (f % 100) == 25 then click(0.32, 0.3, false) end
        if (f % 100) == 28 then click(0.48, 0.3, true) end
        if (f % 100) == 29 then click(0.48, 0.3, false) end
        if (f % 100) == 70 then click(0.5, 0.5, true) end      -- continue
        if (f % 100) == 71 then click(0.5, 0.5, false) end
        if ((f % 100) < 10) or ((f % 100) > 75) then
            motion(0.5 + (0.3 * math.cos(t)), 0.5 + (0.3 * math.sin(t)))
        end
    end
end

local frame = 0
local t0, c0 = nil, nil
local memmax, memsum = 0, 0

local function status (k)
    local f = io.open("/proc/self/status")
    local s = f:read("a")
    f:close()
    return tonumber(string.match(s, k .. ":%s*(%d+)"))
end

local function report ()
    local wall = (pico.get.now() - t0) / 1000
    local cpu = os.clock() - c0
    print(string.format(
        "BENCH game=%s frames=%d draw=%s wall=%.3f cpu=%.3f fps=%.1f cpufps=%.1f heapmax=%.0fK heapavg=%.0fK rss=%dK hwm=%dK",
        GAME, frame, tostring(DRAW), wall, cpu, frame / wall, frame / cpu,
        memmax, memsum / frame, status("VmRSS"), status("VmHWM")))
end

function M.quit ()
    pico.init(false)
end

function M.step ()
    if t0 == nil then
        t0 = pico.get.now()
        c0 = os.clock()
    end
    frame = frame + 1
    M.now = M.now + DT
    emit(DT * 1000)
    input(frame)
    while true do
        local e = pico.input.event()
        if e == nil then break end
        emit(e)
        if e.tag == 'quit' then
            report()
            return true
        end
    end
    if DRAW then
        pico.output.clear()
        emit('draw')
        if SHOT and ((frame % EVERY) == 0) then
            pico.output.screenshot(nil, string.format("%s-%05d.png", SHOT, frame))
        end
        pico.output.present()
    end
    local m = collectgarbage("count")
    if m > memmax then memmax = m end
    memsum = memsum + m
    if frame >= FRAMES then
        report()
        return true
    end
end

atmos.env(M)

return M
