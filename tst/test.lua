--
-- Test harness for the C implementation of Atmos.
-- Mirrors the helpers of the reference implementation (atmos.lang.exec)
-- on top of 'atmos.core'.
--

local core = require "atmos.core"
atmos = require "atmos"
X = require "atmos.x"

function assertfx (cur, exp)
    return assert(string.find(cur,exp), cur)
end

function assertx (cur, exp)
    return assert(cur == exp, cur)
end

function trim (s)
    return (s:gsub("^%s*",""):gsub("\n%s*","\n"):gsub("%s*$",""))
end

function join (sep, t)
    local ret = ""
    for i,v in ipairs(t) do
        if i > 1 then
            ret = ret .. sep
        end
        ret = ret .. v
    end
    return ret
end

-- compiles an Atmos source string; returns the function or (nil, err)
function atm_loadstring (src, file)
    return core.load(src, '='..(file or 'anon.atm'))
end

-- runs the source under atmos.loop capturing 'print' output; returns the
-- output (followed by the error message, if any)
function atm_test (src, tst)
    local out = ""
    local PRINT = print
    print = (tst and print) or (function (...)
        local t = {}
        for i=1, select('#',...) do
            t[#t+1] = tostring(select(i,...))
        end
        out = out .. join('\t', t) .. '\n'
    end)
    core.test(atmos.test or false)  -- 'test' blocks enabled?
    local f, err = atm_loadstring(src, "anon.atm")
    if not f then
        print = PRINT
        return err
    end
    local ok, err = pcall(atmos.loop, f)
    print = PRINT
    if ok then
        return out
    else
        return out .. err
    end
end

-- Parser-level helpers of the reference implementation: here they only
-- compile the source, so that tests expecting syntax errors still work.
local SRC, FILE, NEXT
function init () end
function lexer_init (file, src) FILE, SRC, NEXT = file, src, 0 end
function lexer_next ()
    -- the first call reads the first token; further calls skip tokens
    NEXT = NEXT + 1
    if NEXT > 1 then
        SRC = SRC:gsub("^%s*%S+%s*", "", 1)
    end
end
function parser_main ()
    local f, err = core.load(SRC, '='..FILE)
    if not f then error(err, 0) end
    return { tag='ok' }
end
parser = parser_main
function check () return true end
function tosource () return nil end

function warnx (cur, exp)
    return warn(cur == exp, exp)
end

function warn (ok, msg)
    if not ok then
        msg = "WARNING: "..(msg or "<warning message>")
        io.stderr:write(msg..'\n')
    end
end
