#!../lua
--
-- lua2atm.lua: converts Lua source to Atmos syntax (v0.8).
--
-- Usage: lua lua2atm.lua input.lua > output.atm
--
-- Mechanical constructs are converted; constructs without an Atmos
-- counterpart (goto/labels, bitwise operators, for-loops with steps,
-- <close> variables, 'global' declarations) are flagged with ";; TODO"
-- comments for manual conversion.
--

-------------------------------------------------------------------------------
-- LEXER
-------------------------------------------------------------------------------

local KEYWORDS = {}
for w in ([[and break do else elseif end false for function goto if in
    local nil not or repeat return then true until while global]]):gmatch("%a+") do
  KEYWORDS[w] = true
end

local SYMBOLS = {
  "...", "..", "==", "~=", "<=", ">=", "<<", ">>", "//", "::",
  "+", "-", "*", "/", "%", "^", "#", "&", "~", "|", "<", ">", "=",
  "(", ")", "{", "}", "[", "]", ";", ":", ",", ".",
}

local function lex (src)
  local toks = {}
  local comments = {}   -- { line=, text=, multi= }
  local i = 1
  local line = 1
  local n = #src
  local function add (tag, val, l)
    toks[#toks+1] = { tag=tag, val=val, line=l }
  end
  while i <= n do
    local c = src:sub(i,i)
    if c == '\n' then
      line = line + 1; i = i + 1
    elseif c:match("%s") then
      i = i + 1
    elseif src:sub(i,i+1) == "--" then
      local l = line
      local lvl = src:match("^%[(=*)%[", i+2)
      if lvl then
        local close = "]" .. lvl .. "]"
        local j = src:find(close, i+2, true)
        assert(j, "unfinished long comment")
        local text = src:sub(i+2+#lvl+2, j-1)
        comments[#comments+1] = { line=l, text=text, multi=true }
        for _ in text:gmatch("\n") do line = line + 1 end
        i = j + #close
      else
        local j = src:find("\n", i, true) or (n+1)
        comments[#comments+1] = { line=l, text=src:sub(i+2, j-1) }
        i = j
      end
    elseif c:match("[%a_]") then
      local w = src:match("^[%w_]+", i)
      if KEYWORDS[w] then add(w, w, line) else add("name", w, line) end
      i = i + #w
    elseif c:match("%d") or (c == '.' and src:sub(i+1,i+1):match("%d")) then
      local num
      if src:match("^0[xX]", i) then
        num = src:match("^0[xX][%x%.]*[pP][%+%-]?%d+", i) or src:match("^0[xX][%x%.]*", i)
      else
        num = src:match("^[%d%.]*[eE][%+%-]?%d+", i) or src:match("^[%d%.]+", i)
      end
      add("number", num, line)
      i = i + #num
    elseif c == '"' or c == "'" then
      local j = i + 1
      local l = line
      while true do
        local d = src:sub(j,j)
        assert(d ~= "" and d ~= "\n", "unfinished string at line " .. line)
        if d == "\\" then
          local n2 = src:sub(j+1,j+1)
          if n2 == "z" then  -- skip following whitespace (with newlines)
            j = j + 2
            while src:sub(j,j):match("%s") do
              if src:sub(j,j) == "\n" then line = line + 1 end
              j = j + 1
            end
          else
            if n2 == "\n" then line = line + 1 end
            j = j + 2
          end
        elseif d == c then break
        else j = j + 1 end
      end
      add("string", src:sub(i, j), l)
      i = j + 1
    elseif c == '[' and src:match("^%[=*%[", i) then
      local lvl = src:match("^%[(=*)%[", i)
      local close = "]" .. lvl .. "]"
      local j = src:find(close, i, true)
      assert(j, "unfinished long string")
      local text = src:sub(i+#lvl+2, j-1)
      add("lstring", text, line)
      for _ in text:gmatch("\n") do line = line + 1 end
      i = j + #close
    else
      local found
      for _, s in ipairs(SYMBOLS) do
        if src:sub(i, i+#s-1) == s then found = s; break end
      end
      assert(found, "invalid character '" .. c .. "' at line " .. line)
      add(found, found, line)
      i = i + #found
    end
  end
  add("eof", "<eof>", line)
  return toks, comments
end

-------------------------------------------------------------------------------
-- PARSER
-------------------------------------------------------------------------------

local P = {}
P.__index = P

local function parser (toks)
  return setmetatable({ toks=toks, i=1 }, P)
end

function P:peek (k) return self.toks[self.i + (k or 0)] end
function P:tag (k) return self:peek(k).tag end
function P:next () local t = self.toks[self.i]; self.i = self.i + 1; return t end
function P:check (tag) return self:tag() == tag end
function P:accept (tag) if self:check(tag) then return self:next() end end
function P:expect (tag)
  local t = self:next()
  if t.tag ~= tag then
    error(("line %d: expected '%s', got '%s'"):format(t.line, tag, t.val))
  end
  return t
end
function P:line () return self:peek().line end

local function blockfollow (p)
  local t = p:tag()
  return t == "else" or t == "elseif" or t == "end" or t == "eof" or t == "until"
end

function P:block ()
  local stats = {}
  while not blockfollow(self) do
    if self:check("return") then
      stats[#stats+1] = self:retstat()
      self:accept(";")
      break
    end
    local s = self:statement()
    if s then stats[#stats+1] = s end
  end
  return stats
end

function P:retstat ()
  local line = self:expect("return").line
  local es = {}
  if not blockfollow(self) and not self:check(";") then
    es = self:explist()
  end
  return { tag="return", es=es, line=line, lastline=self:peek(-1).line }
end

function P:names ()
  local names = { self:expect("name").val }
  while self:accept(",") do names[#names+1] = self:expect("name").val end
  return names
end

function P:attrib ()
  if self:accept("<") then
    local a = self:expect("name").val
    self:expect(">")
    return a
  end
end

function P:statement ()
  local line = self:line()
  local t = self:tag()
  if t == ";" then self:next(); return nil
  elseif t == "if" then
    self:next()
    local cases = {}
    local cnd = self:expr(); self:expect("then")
    cases[#cases+1] = { cnd=cnd, body=self:block(), line=line }
    while self:check("elseif") do
      local l = self:next().line
      local c = self:expr(); self:expect("then")
      cases[#cases+1] = { cnd=c, body=self:block(), line=l }
    end
    local els
    if self:check("else") then
      local l = self:next().line
      els = { body=self:block(), line=l }
    end
    local lastline = self:expect("end").line
    return { tag="if", cases=cases, els=els, line=line, lastline=lastline }
  elseif t == "while" then
    self:next()
    local cnd = self:expr(); self:expect("do")
    local body = self:block()
    local lastline = self:expect("end").line
    return { tag="while", cnd=cnd, body=body, line=line, lastline=lastline }
  elseif t == "do" then
    self:next()
    local body = self:block()
    local lastline = self:expect("end").line
    return { tag="do", body=body, line=line, lastline=lastline }
  elseif t == "for" then
    self:next()
    local n1 = self:expect("name").val
    if self:accept("=") then
      local e1 = self:expr(); self:expect(",")
      local e2 = self:expr()
      local e3
      if self:accept(",") then e3 = self:expr() end
      self:expect("do")
      local body = self:block()
      local lastline = self:expect("end").line
      return { tag="fornum", var=n1, e1=e1, e2=e2, e3=e3, body=body,
               line=line, lastline=lastline }
    else
      local names = { n1 }
      while self:accept(",") do names[#names+1] = self:expect("name").val end
      self:expect("in")
      local es = self:explist()
      self:expect("do")
      local body = self:block()
      local lastline = self:expect("end").line
      return { tag="forin", names=names, es=es, body=body,
               line=line, lastline=lastline }
    end
  elseif t == "repeat" then
    self:next()
    local body = self:block()
    self:expect("until")
    local cnd = self:expr()
    return { tag="repeat", body=body, cnd=cnd, line=line,
             lastline=self:peek(-1).line }
  elseif t == "function" then
    self:next()
    local name = { self:expect("name").val }
    local method = false
    while self:check(".") or self:check(":") do
      if self:next().tag == ":" then method = true end
      name[#name+1] = self:expect("name").val
      if method then break end
    end
    local f = self:funcbody(line, method)
    return { tag="funcstat", name=name, method=method, func=f, line=line,
             lastline=f.lastline }
  elseif t == "local" then
    self:next()
    if self:accept("function") then
      local name = self:expect("name").val
      local f = self:funcbody(line, false)
      return { tag="localfunc", name=name, func=f, line=line, lastline=f.lastline }
    else
      local attr = self:attrib()
      local names, attrs = {}, {}
      repeat
        names[#names+1] = self:expect("name").val
        attrs[#attrs+1] = self:attrib() or attr
      until not self:accept(",")
      local es
      if self:accept("=") then es = self:explist() end
      return { tag="local", names=names, attrs=attrs, es=es, line=line,
               lastline=self:peek(-1).line }
    end
  elseif t == "global" then
    self:next()
    if self:accept("function") then  -- global function: as a funcstat
      local name = { self:expect("name").val }
      local f = self:funcbody(line, false)
      return { tag="funcstat", name=name, method=false, func=f, line=line,
               lastline=f.lastline }
    end
    local attr = self:attrib()
    if self:check("*") then  -- 'global *'
      self:next()
      return { tag="global", text="*", line=line, lastline=line }
    end
    local names = {}
    repeat
      names[#names+1] = self:expect("name").val
      self:attrib()
    until not self:accept(",")
    local es
    if self:accept("=") then es = self:explist() end
    local text = (attr and ("<" .. attr .. "> ") or "") .. table.concat(names, ", ")
    return { tag="global", text=text, names=names, es=es, line=line,
             lastline=self:peek(-1).line }
  elseif t == "return" then
    return self:retstat()
  elseif t == "break" then
    self:next()
    return { tag="break", line=line, lastline=line }
  elseif t == "goto" then
    self:next()
    local name = self:expect("name").val
    return { tag="goto", name=name, line=line, lastline=line }
  elseif t == "::" then
    self:next()
    local name = self:expect("name").val
    self:expect("::")
    return { tag="label", name=name, line=line, lastline=line }
  else
    local e = self:suffixedexp()
    if self:check("=") or self:check(",") then
      local vars = { e }
      while self:accept(",") do vars[#vars+1] = self:suffixedexp() end
      self:expect("=")
      local es = self:explist()
      return { tag="set", vars=vars, es=es, line=line, lastline=self:peek(-1).line }
    else
      assert(e.tag == "call" or e.tag == "method",
             "line " .. line .. ": syntax error (expression statement)")
      return { tag="callstat", call=e, line=line, lastline=self:peek(-1).line }
    end
  end
end

function P:funcbody (line, method)
  self:expect("(")
  local params, dots, dotsname = {}, false, nil
  if not self:check(")") then
    repeat
      if self:accept("...") then
        dots = true
        if self:check("name") then dotsname = self:next().val end  -- '...t'
        break
      end
      params[#params+1] = self:expect("name").val
    until not self:accept(",")
  end
  self:expect(")")
  local body = self:block()
  local lastline = self:expect("end").line
  return { tag="function", params=params, dots=dots, dotsname=dotsname,
           body=body, method=method, line=line, lastline=lastline }
end

function P:explist ()
  local es = { self:expr() }
  while self:accept(",") do es[#es+1] = self:expr() end
  return es
end

function P:primaryexp ()
  local t = self:peek()
  if t.tag == "name" then
    self:next()
    return { tag="name", name=t.val, line=t.line }
  elseif t.tag == "(" then
    self:next()
    local e = self:expr()
    self:expect(")")
    return { tag="paren", e=e, line=t.line }
  else
    error(("line %d: unexpected symbol '%s'"):format(t.line, t.val))
  end
end

function P:suffixedexp ()
  local e = self:primaryexp()
  while true do
    local t = self:tag()
    if t == "." then
      self:next()
      local k = self:next()  -- name or keyword
      e = { tag="field", obj=e, name=k.val, line=k.line }
    elseif t == "[" then
      self:next()
      local k = self:expr()
      self:expect("]")
      e = { tag="index", obj=e, key=k, line=e.line }
    elseif t == ":" then
      self:next()
      local name = self:expect("name").val
      local args = self:callargs()
      e = { tag="method", obj=e, name=name, args=args, line=e.line }
    elseif t == "(" or t == "string" or t == "lstring" or t == "{" then
      local args = self:callargs()
      e = { tag="call", f=e, args=args, line=e.line }
    else
      return e
    end
  end
end

function P:callargs ()
  local t = self:peek()
  if t.tag == "string" or t.tag == "lstring" then
    self:next()
    return { { tag=t.tag, val=t.val, line=t.line } }
  elseif t.tag == "{" then
    return { self:tableconstructor() }
  else
    self:expect("(")
    local args = {}
    if not self:check(")") then args = self:explist() end
    self:expect(")")
    return args
  end
end

function P:tableconstructor ()
  local line = self:expect("{").line
  local items = {}
  while not self:check("}") do
    if self:check("[") then
      self:next()
      local k = self:expr(); self:expect("]"); self:expect("=")
      items[#items+1] = { key=k, val=self:expr() }
    elseif self:check("name") and self:tag(1) == "=" then
      local k = self:next().val; self:next()
      items[#items+1] = { name=k, val=self:expr() }
    else
      items[#items+1] = { val=self:expr() }
    end
    if not (self:accept(",") or self:accept(";")) then break end
  end
  local lastline = self:expect("}").line
  return { tag="table", items=items, line=line, lastline=lastline }
end

local BINPRI = {
  ["or"]={1,1}, ["and"]={2,2},
  ["<"]={3,3}, [">"]={3,3}, ["<="]={3,3}, [">="]={3,3}, ["~="]={3,3}, ["=="]={3,3},
  ["|"]={4,4}, ["~"]={5,5}, ["&"]={6,6}, ["<<"]={7,7}, [">>"]={7,7},
  [".."]={9,8}, ["+"]={10,10}, ["-"]={10,10},
  ["*"]={11,11}, ["/"]={11,11}, ["//"]={11,11}, ["%"]={11,11},
  ["^"]={14,13},
}
local UNARY_PRI = 12
local UNOPS = { ["not"]=true, ["-"]=true, ["#"]=true, ["~"]=true }

function P:simpleexp ()
  local t = self:peek()
  if t.tag == "number" or t.tag == "string" or t.tag == "lstring" then
    self:next(); return { tag=t.tag, val=t.val, line=t.line }
  elseif t.tag == "nil" or t.tag == "true" or t.tag == "false" then
    self:next(); return { tag=t.tag, line=t.line }
  elseif t.tag == "..." then
    self:next(); return { tag="dots", line=t.line }
  elseif t.tag == "{" then
    return self:tableconstructor()
  elseif t.tag == "function" then
    self:next()
    return self:funcbody(t.line, false)
  else
    return self:suffixedexp()
  end
end

function P:subexpr (limit)
  local e
  local t = self:peek()
  if UNOPS[t.tag] then
    self:next()
    local operand = self:subexpr(UNARY_PRI)
    e = { tag="unop", op=t.tag, e=operand, line=t.line }
  else
    e = self:simpleexp()
  end
  while true do
    local op = self:tag()
    local pri = BINPRI[op]
    if not pri or pri[1] <= limit then break end
    local line = self:next().line
    local e2 = self:subexpr(pri[2])
    e = { tag="binop", op=op, e1=e, e2=e2, line=line }
  end
  return e
end

function P:expr () return self:subexpr(0) end

function P:chunk ()
  local stats = self:block()
  self:expect("eof")
  return stats
end

-------------------------------------------------------------------------------
-- PRINTER (Atmos)
-------------------------------------------------------------------------------

local out = {}         -- output lines
local curindent = ""   -- indentation of the statement being printed
local comments         -- from lexer
local ci = 1           -- next comment index
local curline = 0      -- last source line emitted

local function emit (s) out[#out+1] = s end

local function todo (s) return ";; TODO(lua2atm): " .. s end

local function commentline (c, indent)
  if c.multi then
    return indent .. ";;;" .. c.text:gsub("\n", "\n" .. indent) .. ";;;"
  else
    return indent .. ";;" .. c.text
  end
end

-- emit comments that appear before source line 'line'
local function flushcomments (line, indent)
  while comments[ci] and comments[ci].line < line do
    local c = comments[ci]
    if c.line > curline + 1 then emit("") end
    emit(commentline(c, indent))
    curline = c.line
    ci = ci + 1
  end
end

-- trailing comment on 'line' (same line as the statement end)
local function trailing (line)
  local c = comments[ci]
  if c and c.line == line and not c.multi then
    ci = ci + 1
    return "  ;;" .. c.text
  end
  return ""
end

local OPMAP = {
  ["~="]="!=", [".."]="++", ["and"]="&&", ["or"]="||", ["^"]="**",
}
local BITOPS = { ["|"]=true, ["&"]=true, ["~"]=true, ["<<"]=true, [">>"]=true }

local pexpr
local pblock  -- forward

-- Atmos keywords that are valid identifiers in Lua: rename with a suffix
local ATMKEYS = {}
for w in ([[await catch defer emit func ifs loop match on par pin set spawn
    task tasks test thread toggle val var watching where with]]):gmatch("%a+") do
  ATMKEYS[w] = true
end

local function pname (s)
  if ATMKEYS[s] then return s .. "_" end
  return s
end

local function pstring (e)
  if e.tag == "lstring" then
    local s = e.val:gsub("^\n", "")
    -- multi-line literal: delimiters cannot touch the same quote char
    for _, q in ipairs{'"', "'"} do
      if s:sub(1,1) ~= q and s:sub(-1) ~= q and not s:find(q:rep(3), 1, true) then
        -- a newline right after the opening delimiter is skipped
        if s:sub(1,1) == "\n" then s = "\n" .. s end
        return q:rep(3) .. s .. q:rep(3)
      end
    end
    -- fallback: single-line string with escapes
    s = s:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\n", "\\n")
    return '"' .. s .. '"'
  else
    return e.val
  end
end

local function pnumber (v)
  -- Atmos numbers must start with a digit
  if v:sub(1,1) == "." then v = "0" .. v end
  return v
end

local function pargs (args)
  local t = {}
  for i, a in ipairs(args) do t[i] = pexpr(a) end
  return "(" .. table.concat(t, ", ") .. ")"
end

local function ptable (e)
  local t = {}
  for i, it in ipairs(e.items) do
    if it.key then
      t[i] = "@(" .. pexpr(it.key) .. ")=" .. pexpr(it.val)
    elseif it.name then
      if ATMKEYS[it.name] then  -- keyword as field name
        t[i] = '@("' .. it.name .. '")=' .. pexpr(it.val)
      else
        t[i] = it.name .. "=" .. pexpr(it.val)
      end
    else
      t[i] = pexpr(it.val)
    end
  end
  return "[" .. table.concat(t, ", ") .. "]"
end

local pfunction  -- forward

-- does binary child 'c' of binary node with operator 'op' need parens?
local function needparens (op, c, right)
  if c.tag == "binop" then
    if c.op ~= op then return true end
    if op == "^" or op == ".." then return right end  -- right assoc in Lua
    return right  -- a - (b - c)
  end
  return false
end

local function pbinop (e)
  if BITOPS[e.op] then  -- no Atmos equivalent: flag for manual conversion
    return "TODO_BITOP(" .. pexpr(e.e1) .. " " .. e.op .. " " .. pexpr(e.e2) .. ")"
  end
  local op = OPMAP[e.op] or e.op
  local a = pexpr(e.e1)
  local b = pexpr(e.e2)
  if needparens(e.op, e.e1, false) then a = "(" .. a .. ")" end
  if needparens(e.op, e.e2, true) then b = "(" .. b .. ")" end
  return a .. " " .. op .. " " .. b
end

local function punop (e)
  if e.op == "~" then  -- no Atmos equivalent: flag for manual conversion
    return "TODO_BITOP(~" .. pexpr(e.e) .. ")"
  end
  local op = (e.op == "not") and "!" or e.op
  local a = pexpr(e.e)
  if e.e.tag == "binop" then a = "(" .. a .. ")" end
  if e.e.tag == "unop" and e.op == "-" and e.e.op == "-" then a = " " .. a end
  return op .. a
end

local function pprefix (e)
  -- prefix for calls/indexing: keep as is
  return pexpr(e)
end

pexpr = function (e)
  local t = e.tag
  if t == "number" then return pnumber(e.val)
  elseif t == "string" or t == "lstring" then return pstring(e)
  elseif t == "nil" then return "nil"
  elseif t == "true" then return "true"
  elseif t == "false" then return "false"
  elseif t == "dots" then return "..."
  elseif t == "name" then return pname(e.name)
  elseif t == "paren" then return "(" .. pexpr(e.e) .. ")"
  elseif t == "field" then return pprefix(e.obj) .. "." .. e.name
  elseif t == "index" then
    local k = e.key
    if k.tag == "string" then
      local s = k.val:sub(2, -2)
      if s:match("^[%a_][%w_]*$") then return pprefix(e.obj) .. "." .. s end
    end
    return pprefix(e.obj) .. "@(" .. pexpr(k) .. ")"
  elseif t == "call" then return pprefix(e.f) .. pargs(e.args)
  elseif t == "method" then return pprefix(e.obj) .. "::" .. e.name .. pargs(e.args)
  elseif t == "table" then return ptable(e)
  elseif t == "function" then
    -- function expression: print its body inline (multi-line)
    local head, f = pfunction(e, nil)
    local saved = out
    local ind = curindent
    out = {}
    pblock(f.body, ind .. "  ")
    flushcomments(f.lastline, ind .. "  ")
    local body = table.concat(out, "\n")
    out = saved
    curindent = ind
    curline = f.lastline
    if body == "" then return head .. "}" end
    return head .. "\n" .. body .. "\n" .. ind .. "}"
  elseif t == "binop" then return pbinop(e)
  elseif t == "unop" then return punop(e)
  else error("unknown expression: " .. tostring(t)) end
end

-- function value: func (params) { ... }
pfunction = function (f, header)
  local params = {}
  for i, p in ipairs(f.params) do params[i] = pname(p) end
  if f.method then table.insert(params, 1, "self") end
  if f.dots then params[#params+1] = "..." end
  local head = (header or "func") .. " (" .. table.concat(params, ", ") .. ") {"
  if f.dotsname then
    head = head .. "  " .. todo("named vararg '..." .. f.dotsname .. "'")
  end
  return head, f
end

-- does the block reference name 'n' (approximation: any occurrence)?
local function references (node, n)
  if type(node) ~= "table" then return false end
  if node.tag == "name" and node.name == n then return true end
  for k, v in pairs(node) do
    if k ~= "tag" and type(v) == "table" and references(v, n) then return true end
  end
  return false
end

-- is local 'n' assigned anywhere in the following statements?
local function assigned (stats, from, n)
  for i = from, #stats do
    local s = stats[i]
    local function scan (node)
      if type(node) ~= "table" then return false end
      if node.tag == "set" then
        for _, v in ipairs(node.vars) do
          if v.tag == "name" and v.name == n then return true end
        end
      end
      if node.tag == "funcstat" and #node.name == 1 and node.name[1] == n then
        return true
      end
      for k, v in pairs(node) do
        if k ~= "tag" and type(v) == "table" and scan(v) then return true end
      end
      return false
    end
    if scan(s) then return true end
  end
  return false
end

local function pexplist (es)
  if #es == 1 then return pexpr(es[1]) end
  local t = {}
  for i, e in ipairs(es) do t[i] = pexpr(e) end
  return "(" .. table.concat(t, ", ") .. ")"
end

-- emit a statement list with indentation
local function pstat (s, indent, stats, idx)
  curindent = indent
  flushcomments(s.line, indent)
  if s.line > curline + 1 and curline > 0 then emit("") end
  curline = s.line
  local t = s.tag
  local function line1 (str)  -- single-line statement
    emit(indent .. str .. trailing(s.lastline))
    curline = s.lastline
  end
  local function open (str)
    emit(indent .. str .. trailing(s.line))
    curline = s.line
  end
  local function close (str)
    flushcomments(s.lastline, indent .. "  ")
    emit(indent .. (str or "}") .. trailing(s.lastline))
    curline = s.lastline
  end
  if t == "callstat" then
    line1(pexpr(s.call))
  elseif t == "set" then
    local vars = {}
    for i, v in ipairs(s.vars) do vars[i] = pexpr(v) end
    line1("set " .. table.concat(vars, ", ") .. " = " .. pexplist(s.es))
  elseif t == "local" then
    local kind = "val"
    for i, n in ipairs(s.names) do
      if s.attrs[i] == "close" then
        kind = "pin"  -- to-be-closed variable
      elseif s.attrs[i] ~= "const" and assigned(stats, idx + 1, n) then
        kind = "var"
      end
    end
    local names = {}
    for i, n in ipairs(s.names) do names[i] = pname(n) end
    local str = kind .. " " .. table.concat(names, ", ")
    if s.es then
      if #s.es == 1 and s.es[1].tag == "function" and #s.names == 1 then
        local head, f
        if kind == "var" then
          -- reassigned later: 'var f' + 'set f = func'
          emit(indent .. "var " .. pname(s.names[1]))
          head, f = pfunction(s.es[1], "set " .. pname(s.names[1]) .. " = func")
        else
          head, f = pfunction(s.es[1], kind .. " func " .. pname(s.names[1]))
        end
        open(head)
        pblock(f.body, indent .. "  ")
        close()
        return
      end
      str = str .. " = " .. pexplist(s.es)
    end
    line1(str)
  elseif t == "localfunc" then
    local f = s.func
    if references(f.body, s.name) or assigned(stats, idx + 1, s.name) then
      -- recursive (needs a visible local) or reassigned later
      emit(indent .. "var " .. pname(s.name))
      local head = pfunction(f, "set " .. pname(s.name) .. " = func")
      open(head)
    else
      local head = pfunction(f, "val func " .. pname(s.name))
      open(head)
    end
    pblock(f.body, indent .. "  ")
    close()
  elseif t == "funcstat" then
    local parts = {}
    for i, n in ipairs(s.name) do parts[i] = (i == 1) and pname(n) or n end
    local name = table.concat(parts, ".", 1, s.method and #parts - 1 or #parts)
    if s.method then name = name .. "::" .. parts[#parts] end
    local f = s.func
    local params = {}
    for i, p in ipairs(f.params) do params[i] = pname(p) end
    if f.dots then params[#params+1] = "..." end
    open("func " .. name .. " (" .. table.concat(params, ", ") .. ") {")
    pblock(f.body, indent .. "  ")
    close()
  elseif t == "return" then
    line1("return" .. pargs(s.es))
  elseif t == "break" then
    line1("break()")
  elseif t == "do" then
    open("do {")
    pblock(s.body, indent .. "  ")
    close()
  elseif t == "while" then
    open("loop {")
    emit(indent .. "  while(" .. pexpr(s.cnd) .. ")")
    pblock(s.body, indent .. "  ")
    close()
  elseif t == "repeat" then
    open("loop {")
    pblock(s.body, indent .. "  ")
    flushcomments(s.lastline, indent .. "  ")
    emit(indent .. "  until(" .. pexpr(s.cnd) .. ")")
    close()
  elseif t == "if" then
    if #s.cases == 1 then
      open("if " .. pexpr(s.cases[1].cnd) .. " {")
      pblock(s.cases[1].body, indent .. "  ")
      if s.els then
        flushcomments(s.els.line, indent .. "  ")
        emit(indent .. "} else {" .. trailing(s.els.line))
        curline = s.els.line
        pblock(s.els.body, indent .. "  ")
      end
      close()
    else
      open("ifs {")
      for i, c in ipairs(s.cases) do
        flushcomments(c.line, indent .. "  ")
        emit(indent .. "  " .. pexpr(c.cnd) .. " => {" .. trailing(c.line))
        curline = c.line
        pblock(c.body, indent .. "    ")
        emit(indent .. "  }")
      end
      if s.els then
        flushcomments(s.els.line, indent .. "  ")
        emit(indent .. "  else => {" .. trailing(s.els.line))
        curline = s.els.line
        pblock(s.els.body, indent .. "    ")
        emit(indent .. "  }")
      end
      close()
    end
  elseif t == "fornum" then
    -- numeric loop: loop i in (a, b [, step]) (same semantics as Lua's for)
    local range = pexpr(s.e1) .. ", " .. pexpr(s.e2)
    if s.e3 then range = range .. ", " .. pexpr(s.e3) end
    open("loop " .. pname(s.var) .. " in (" .. range .. ") {")
    pblock(s.body, indent .. "  ")
    close()
  elseif t == "forin" then
    -- iterator expressions (pairs(t), ipairs(t), f, s, c) are kept as
    -- is: 'loop ... in' uses the Lua iterator protocol
    local itr = pexplist(s.es)
    local names = {}
    for i, n in ipairs(s.names) do names[i] = pname(n) end
    open("loop " .. table.concat(names, ", ") .. " in " .. itr .. " {")
    pblock(s.body, indent .. "  ")
    close()
  elseif t == "global" then
    if s.es then  -- global declaration with initialization: an assignment
      local names = {}
      for i, n in ipairs(s.names) do names[i] = pname(n) end
      line1("set " .. table.concat(names, ", ") .. " = " .. pexplist(s.es) ..
            "  ;; (global " .. s.text .. ")")
    else
      line1(";; (global " .. s.text .. ")")
    end
  elseif t == "goto" then
    line1(todo("goto " .. s.name))
  elseif t == "label" then
    line1(todo("label ::" .. s.name .. "::"))
  else
    error("unknown statement: " .. tostring(t))
  end
end

pblock = function (stats, indent)
  for i, s in ipairs(stats) do
    pstat(s, indent, stats, i)
  end
end

-------------------------------------------------------------------------------
-- MAIN
-------------------------------------------------------------------------------

local fname = arg[1]
local f = assert(io.open(fname))
local src = f:read("a")
f:close()
src = src:gsub("^#[^\n]*\n", "\n")  -- skip shebang line

local toks, cmts = lex(src)
comments = cmts
local ast = parser(toks):chunk()
pblock(ast, "")
flushcomments(math.huge, "")
io.write(table.concat(out, "\n"), "\n")
