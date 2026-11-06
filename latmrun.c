/*
** $Id: latmrun.c $
** Atmos runtime: tasks, events, timers, errors (port of lua-atmos)
** See Copyright Notice in lua.h
*/

#define latmrun_c
#define LUA_LIB

#include "lprefix.h"


#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "llimits.h"


/*
** The runtime keeps the data model of the reference implementation:
** tasks are tables with a metatable and a '_' subtable with internals.
**
**   task   : { _ = { dbg, f } }                            (prototype)
**   xtask  : { _ = { up, dns, pin, dbg, th, tra, status, time, ret },
**              pin, ret, pub }                             (instance)
**   tasks  : { _ = { up, dns, pin, dbg, max, [cache] }, pin }  (pool)
**
** The root pool TASKS owns all top-level tasks and pools.
*/

/* registry keys */
#define RK_STATE	"atmos.state"  /* userdata with RunState */
#define RK_TASKS	"atmos.TASKS"  /* root pool */
#define RK_GC		"atmos.GC"  /* set of parents with dead tasks */
#define RK_ENVS		"atmos.envs"  /* list of environments */
#define RK_RT		"ATMOS"  /* runtime table for generated code */

/* metatable names */
#define MT_TASK		"atmos.task"
#define MT_XTASK	"atmos.xtask"
#define MT_TASKS	"atmos.tasks"
#define MT_THROW	"atmos.throw"
#define MT_DEFER	"atmos.defer"
#define MT_HANDLE	"atmos.handle"  /* close-only handle of 'do_spawn' */


typedef struct RunState {
  lua_Integer time;  /* logical time: incremented by each emit */
  lua_Number clkmin;  /* nearest timer (lower bound) */
  lua_Number clklate;  /* residual of current clock piece */
} RunState;


static RunState *getrs (lua_State *L) {
  RunState *rs;
  lua_getfield(L, LUA_REGISTRYINDEX, RK_STATE);
  rs = cast(RunState *, lua_touserdata(L, -1));
  lua_pop(L, 1);
  return rs;
}


/*
** {======================================================
** Helpers
** =======================================================
*/

/* error with message prefixed by the position of the caller 'level' */
static int atmerror (lua_State *L, int level, const char *msg) {
  luaL_where(L, level);
  lua_pushstring(L, msg);
  lua_concat(L, 2);
  return lua_error(L);
}

#define argcheck(L,cond,level,msg)	{ if (!(cond)) atmerror(L, level, msg); }


/* does value at 'idx' have the metatable named 'mt'? */
static int hasmeta (lua_State *L, int idx, const char *mt) {
  int res;
  if (!lua_getmetatable(L, idx)) return 0;
  luaL_getmetatable(L, mt);
  res = lua_rawequal(L, -1, -2);
  lua_pop(L, 2);
  return res;
}

#define istask(L,i)	hasmeta(L, i, MT_TASK)
#define isxtask(L,i)	hasmeta(L, i, MT_XTASK)
#define istasks(L,i)	hasmeta(L, i, MT_TASKS)
#define isthrow(L,i)	hasmeta(L, i, MT_THROW)


/* is value at 'idx' the root pool? */
static int isroot (lua_State *L, int idx) {
  int res;
  idx = lua_absindex(L, idx);
  lua_getfield(L, LUA_REGISTRYINDEX, RK_TASKS);
  res = lua_rawequal(L, -1, idx);
  lua_pop(L, 1);
  return res;
}


static void pushroot (lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, RK_TASKS);
}


/* push t._ */
static void get_ (lua_State *L, int idx) {
  lua_getfield(L, idx, "_");
}


/* push t._.<field> */
static int getint (lua_State *L, int idx, const char *field) {
  int tt;
  get_(L, idx);
  tt = lua_getfield(L, -1, field);
  lua_remove(L, -2);
  return tt;
}


/* t._.<field> = value at top (pops it) */
static void setint (lua_State *L, int idx, const char *field) {
  idx = lua_absindex(L, idx);
  get_(L, idx);
  lua_insert(L, -2);
  lua_setfield(L, -2, field);
  lua_pop(L, 1);
}


/* is t._.<field> equal to string 's'? */
static int intstreq (lua_State *L, int idx, const char *field, const char *s) {
  int res;
  getint(L, idx, field);
  res = (lua_type(L, -1) == LUA_TSTRING && strcmp(lua_tostring(L, -1), s) == 0);
  lua_pop(L, 1);
  return res;
}


/* push the thread t._.th */
static lua_State *getthread (lua_State *L, int idx) {
  lua_State *co;
  getint(L, idx, "th");
  co = lua_tothread(L, -1);
  lua_pop(L, 1);
  return co;
}


/* coroutine status (as in lcorolib.c) */
#define COS_RUN		0
#define COS_DEAD	1
#define COS_YIELD	2
#define COS_NORM	3

static const char *const statname[] =
  {"running", "dead", "suspended", "normal"};


static int costatus (lua_State *L, lua_State *co) {
  if (L == co) return COS_RUN;
  else {
    switch (lua_status(co)) {
      case LUA_YIELD:
        return COS_YIELD;
      case LUA_OK: {
        lua_Debug ar;
        if (lua_getstack(co, 0, &ar))  /* does it have frames? */
          return COS_NORM;  /* it is running */
        else if (lua_gettop(co) == 0)
            return COS_DEAD;
        else
          return COS_YIELD;  /* initial state */
      }
      default:  /* some error occurred */
        return COS_DEAD;
    }
  }
}


/* status of the thread of task at 'idx' */
static int taskstatus (lua_State *L, int idx) {
  return costatus(L, getthread(L, idx));
}


/*
** Push a 'dbg' table {file=..., line=...} with the position of the
** function at the given stack level.
*/
static void pushdbg (lua_State *L, int level) {
  lua_Debug ar;
  lua_createtable(L, 0, 2);
  if (lua_getstack(L, level, &ar) && lua_getinfo(L, "Sl", &ar)) {
    lua_pushstring(L, ar.short_src);
    lua_setfield(L, -2, "file");
    lua_pushinteger(L, ar.currentline);
    lua_setfield(L, -2, "line");
  }
  else {
    lua_pushliteral(L, "?");
    lua_setfield(L, -2, "file");
    lua_pushinteger(L, -1);
    lua_setfield(L, -2, "line");
  }
}


/* length of table at idx (raw) */
static lua_Integer rawlen (lua_State *L, int idx) {
  return cast(lua_Integer, lua_rawlen(L, idx));
}

/* }====================================================== */


/*
** {======================================================
** X : value helpers (is, gte, eq, iter, xin, cat, tostring, copy)
** =======================================================
*/

static int x_gte (lua_State *L, int a, int b);


/* 'b' starts with 'a' followed by '.'? */
static int dotprefix (const char *a, size_t la, const char *b, size_t lb) {
  return (lb > la && memcmp(a, b, la) == 0 && b[la] == '.');
}


static int x_is (lua_State *L, int v, int x) {
  int tv, tx;
  v = lua_absindex(L, v);
  x = lua_absindex(L, x);
  tv = lua_type(L, v);
  tx = lua_type(L, x);
  if (lua_compare(L, v, x, LUA_OPEQ))
    return 1;
  else if (tx == LUA_TSTRING && strcmp(lua_typename(L, tv), lua_tostring(L, x)) == 0)
    return 1;
  else if (tx == LUA_TSTRING && strcmp(lua_tostring(L, x), "task") == 0 && istask(L, v))
    return 1;
  else if (tx == LUA_TSTRING && strcmp(lua_tostring(L, x), "xtask") == 0 && isxtask(L, v))
    return 1;
  else if (tx == LUA_TSTRING && strcmp(lua_tostring(L, x), "tasks") == 0 && istasks(L, v))
    return 1;
  else if (tv == LUA_TTABLE && tx == LUA_TSTRING) {
    int res = 0;
    if (lua_getfield(L, v, "tag") == LUA_TSTRING)
      res = x_gte(L, x, -1);
    lua_pop(L, 1);
    return res;
  }
  else if (tv == LUA_TSTRING && tx == LUA_TSTRING)
    return x_gte(L, x, v);
  else
    return 0;
}


static int sametypeandmeta (lua_State *L, int a, int b) {
  int ma, mb, res;
  if (lua_type(L, a) != lua_type(L, b)) return 0;
  ma = lua_getmetatable(L, a);
  mb = lua_getmetatable(L, b);
  if (ma && mb) res = lua_rawequal(L, -1, -2);
  else res = (ma == mb);
  lua_pop(L, ma + mb);
  return res;
}


static int x_gte (lua_State *L, int a, int b) {
  a = lua_absindex(L, a);
  b = lua_absindex(L, b);
  if (lua_type(L, a) != lua_type(L, b))
    return 0;
  else if (lua_compare(L, a, b, LUA_OPEQ))
    return 1;
  else if (!sametypeandmeta(L, a, b))
    return 0;
  else if (lua_type(L, a) == LUA_TSTRING) {
    size_t la, lb;
    const char *sa = lua_tolstring(L, a, &la);
    const char *sb = lua_tolstring(L, b, &lb);
    return dotprefix(sa, la, sb, lb);
  }
  else if (lua_type(L, a) == LUA_TTABLE) {
    if (istask(L, a) || isxtask(L, a) || istasks(L, a) ||
        istask(L, b) || isxtask(L, b) || istasks(L, b))
      return 0;
    luaL_checkstack(L, 4, "deep comparison");
    lua_pushnil(L);
    while (lua_next(L, a) != 0) {  /* for k, va in pairs(a) */
      int res;
      lua_pushvalue(L, -2);  /* key */
      lua_gettable(L, b);  /* b[k] */
      res = x_gte(L, -2, -1);
      lua_pop(L, 2);  /* b[k] and va */
      if (!res) {
        lua_pop(L, 1);  /* key */
        return 0;
      }
    }
    return 1;
  }
  else
    return 0;
}


static int x_is_f (lua_State *L) {
  lua_settop(L, 2);
  lua_pushboolean(L, x_is(L, 1, 2));
  return 1;
}


static int x_gte_f (lua_State *L) {
  lua_settop(L, 2);
  lua_pushboolean(L, x_gte(L, 1, 2));
  return 1;
}


static int x_eq_f (lua_State *L) {
  lua_settop(L, 2);
  lua_pushboolean(L, x_gte(L, 1, 2) && x_gte(L, 2, 1));
  return 1;
}


/* numeric iterator: fi(N, i) */
static int x_fi (lua_State *L) {
  lua_Integer n = luaL_checkinteger(L, 1);
  lua_Integer i = luaL_checkinteger(L, 2) + 1;
  if (i > n)
    lua_pushnil(L);
  else
    lua_pushinteger(L, i);
  return 1;
}


/*
** stateless table iterator: array part (1..#t) then non-array keys,
** skipping numeric keys already covered by the array part
*/
static int x_fx (lua_State *L) {
  lua_Integer n;
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 2);
  n = luaL_len(L, 1);
  if (lua_isnil(L, 2)) {
    if (n >= 1) {
      lua_pushinteger(L, 1);
      lua_geti(L, 1, 1);
      return 2;
    }
  }
  else if (lua_isinteger(L, 2)) {
    lua_Integer k = lua_tointeger(L, 2);
    if (k >= 1 && k < n) {
      lua_pushinteger(L, k + 1);
      lua_geti(L, 1, k + 1);
      return 2;
    }
    else if (k == n) {
      lua_pushnil(L);
      lua_replace(L, 2);
    }
  }
  lua_pushvalue(L, 2);  /* key for 'next' */
  for (;;) {
    if (lua_next(L, 1) == 0)
      return 0;
    /* skip numeric keys in the array part */
    if (lua_type(L, -2) == LUA_TNUMBER) {
      lua_Number k = lua_tonumber(L, -2);
      if (k > 0 && k <= cast_num(n)) {
        lua_pop(L, 1);  /* value: key stays for the next iteration */
        continue;
      }
    }
    return 2;
  }
}


/*
** generic-for iterator over 't'; yield arity depends on the source:
**   number / (n,m) / nil : one value (the index)
**   table / __pairs      : key, value
**   function / __call    : generator values, until nil
*/
static int x_iter (lua_State *L) {
  int t = lua_type(L, 1);
  if (lua_getmetatable(L, 1)) {
    if (lua_getfield(L, -1, "__pairs") != LUA_TNIL) {
      lua_pushvalue(L, 1);
      lua_call(L, 1, 3);
      return 3;
    }
    lua_pop(L, 1);
    if (lua_getfield(L, -1, "__call") != LUA_TNIL) {
      lua_pushvalue(L, 1);
      return 1;
    }
    lua_pop(L, 2);
  }
  if (t == LUA_TNONE || t == LUA_TNIL) {
    lua_pushcfunction(L, x_fi);
    lua_pushinteger(L, LUA_MAXINTEGER);
    lua_pushinteger(L, 0);
    return 3;
  }
  else if (t == LUA_TFUNCTION) {
    lua_pushvalue(L, 1);
    return 1;
  }
  else if (t == LUA_TNUMBER) {
    lua_pushcfunction(L, x_fi);
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
      lua_pushvalue(L, 2);  /* to */
      lua_pushinteger(L, luaL_checkinteger(L, 1) - 1);  /* from */
    }
    else {
      lua_pushvalue(L, 1);  /* to */
      lua_pushinteger(L, 0);  /* from */
    }
    return 3;
  }
  else if (t == LUA_TTABLE) {
    lua_pushcfunction(L, x_fx);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
  }
  else
    return luaL_error(L, "TODO - iter(t)");
}


/*
** Iteration helper: calls X.iter on value at 'idx' and leaves the
** iterator triple (f, s, ctl) at the top of the stack.
*/
static void iter_start (lua_State *L, int idx) {
  idx = lua_absindex(L, idx);
  lua_pushcfunction(L, x_iter);
  lua_pushvalue(L, idx);
  lua_call(L, 1, 3);
}


/*
** Iteration step: with the triple at 'base' (f, s, ctl), calls
** f(s, ctl) and leaves (a, b, c) on the stack; returns 0 at the end
** (nothing pushed). The control variable is updated to 'a'.
*/
static int iter_step (lua_State *L, int base) {
  lua_pushvalue(L, base);
  lua_pushvalue(L, base + 1);
  lua_pushvalue(L, base + 2);
  lua_call(L, 2, 3);
  if (lua_isnil(L, -3)) {
    lua_pop(L, 3);
    return 0;
  }
  lua_pushvalue(L, -3);
  lua_replace(L, base + 2);  /* update control */
  return 1;
}


static int x_xin (lua_State *L) {
  int base;
  lua_settop(L, 2);
  iter_start(L, 2);
  base = 3;
  while (iter_step(L, base)) {  /* a, b, c at 6, 7, 8 */
    int found = 0;
    if (!lua_isnil(L, 8))
      return luaL_error(L, "assertion failed!");
    if (lua_isnil(L, 7))
      found = x_gte(L, 1, 6);
    else if (lua_type(L, 6) == LUA_TNUMBER)
      found = x_gte(L, 1, 7);
    else
      found = x_gte(L, 1, 6) || x_gte(L, 1, 7);
    lua_pop(L, 3);
    if (found) {
      lua_pushboolean(L, 1);
      return 1;
    }
  }
  lua_pushboolean(L, 0);
  return 1;
}


static int concat2 (lua_State *L) {
  lua_concat(L, 2);
  return 1;
}


static int x_cat (lua_State *L) {
  lua_Integer n;
  int base;
  lua_settop(L, 2);
  /* try v1 .. v2 */
  lua_pushcfunction(L, concat2);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  if (lua_pcall(L, 2, 1, 0) == LUA_OK)
    return 1;
  lua_pop(L, 1);  /* error */
  lua_newtable(L);  /* ret at 3 */
  iter_start(L, 1);  /* triple at 4-6 */
  base = 4;
  while (iter_step(L, base)) {  /* a, b, c at 7, 8, 9 */
    if (!lua_isnil(L, 9)) return luaL_error(L, "assertion failed!");
    if (lua_isnil(L, 8)) {
      lua_pushvalue(L, 7);
      lua_rawseti(L, 3, rawlen(L, 3) + 1);
    }
    else {
      lua_pushvalue(L, 7);
      lua_pushvalue(L, 8);
      lua_settable(L, 3);
    }
    lua_pop(L, 3);
  }
  lua_settop(L, 3);
  n = 1;
  iter_start(L, 2);  /* triple at 4-6 */
  while (iter_step(L, base)) {
    if (!lua_isnil(L, 9)) return luaL_error(L, "assertion failed!");
    if (lua_isnil(L, 8)) {
      lua_pushvalue(L, 7);
      lua_rawseti(L, 3, rawlen(L, 3) + 1);
    }
    else if (lua_isinteger(L, 7) && lua_tointeger(L, 7) == n) {
      lua_pushvalue(L, 8);
      lua_rawseti(L, 3, rawlen(L, 3) + 1);
      n++;
    }
    else {
      lua_pushvalue(L, 7);
      lua_pushvalue(L, 8);
      lua_settable(L, 3);
    }
    lua_pop(L, 3);
  }
  lua_settop(L, 3);
  return 1;
}


/* sort function for keys: numbers before, by value; then by string */
static int keyless (lua_State *L) {
  int n1 = lua_isnumber(L, 1);
  int n2 = lua_isnumber(L, 2);
  if (n1 && n2)
    lua_pushboolean(L, lua_tonumber(L, 1) < lua_tonumber(L, 2));
  else {
    luaL_tolstring(L, 1, NULL);
    luaL_tolstring(L, 2, NULL);
    lua_pushboolean(L, lua_compare(L, -2, -1, LUA_OPLT));
  }
  return 1;
}


/*
** Push the string representation of the value at 'idx', rendering
** tables as atmos-lang constructors: :X [a, k=v]
*/
static void tostr (lua_State *L, int idx) {
  idx = lua_absindex(L, idx);
  if (lua_type(L, idx) != LUA_TTABLE)
    luaL_tolstring(L, idx, NULL);
  else {
    lua_Integer n, i, nxt = 1;
    int keys;
    luaL_Buffer b;
    luaL_checkstack(L, 10, "tostring");
    /* collect keys (except 'tag') */
    lua_newtable(L);
    keys = lua_gettop(L);
    n = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
      int tk = lua_type(L, -2);
      lua_pop(L, 1);
      if (tk != LUA_TNUMBER && tk != LUA_TSTRING)
        luaL_error(L, "assertion failed!");
      if (tk == LUA_TSTRING && strcmp(lua_tostring(L, -1), "tag") == 0)
        continue;
      lua_pushvalue(L, -1);
      lua_rawseti(L, keys, ++n);
    }
    /* sort keys */
    lua_getglobal(L, "table");
    lua_getfield(L, -1, "sort");
    lua_remove(L, -2);
    lua_pushvalue(L, keys);
    lua_pushcfunction(L, keyless);
    lua_call(L, 2, 0);
    luaL_buffinit(L, &b);
    /* tag prefix */
    if (lua_getfield(L, idx, "tag") != LUA_TNIL) {
      const char *ts = luaL_tolstring(L, -1, NULL);
      lua_pushfstring(L, ":%s ", ts);
      lua_copy(L, -1, -3);
      lua_pop(L, 2);
      luaL_addvalue(&b);
    }
    else
      lua_pop(L, 1);
    luaL_addchar(&b, '[');
    for (i = 1; i <= n; i++) {
      if (i > 1) luaL_addstring(&b, ", ");
      lua_rawgeti(L, keys, i);  /* key */
      if (lua_isinteger(L, -1) && lua_tointeger(L, -1) == nxt) {
        nxt++;
        lua_gettable(L, idx);  /* value */
        tostr(L, -1);
        lua_remove(L, -2);
        luaL_addvalue(&b);
      }
      else {
        const char *ks;
        lua_pushvalue(L, -1);
        lua_gettable(L, idx);  /* key, value */
        tostr(L, -1);  /* key, value, vs */
        ks = luaL_tolstring(L, -3, NULL);  /* key, value, vs, ks */
        lua_pushfstring(L, "%s=%s", ks, lua_tostring(L, -2));
        lua_copy(L, -1, -5);
        lua_pop(L, 4);
        luaL_addvalue(&b);
      }
    }
    luaL_addchar(&b, ']');
    luaL_pushresult(&b);
    lua_remove(L, keys);
  }
}


static int x_tostring (lua_State *L) {
  lua_settop(L, 1);
  tostr(L, 1);
  return 1;
}


static int x_print (lua_State *L) {
  int n = lua_gettop(L);
  int i;
  lua_getglobal(L, "print");
  for (i = 1; i <= n; i++) {
    lua_pushcfunction(L, x_tostring);
    lua_pushvalue(L, i);
    lua_call(L, 1, 1);
  }
  lua_call(L, n, 0);
  return 0;
}


static void x_copy_aux (lua_State *L, int idx) {
  idx = lua_absindex(L, idx);
  if (lua_type(L, idx) != LUA_TTABLE)
    lua_pushvalue(L, idx);
  else {
    luaL_checkstack(L, 4, "deep copy");
    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
      lua_pushvalue(L, -2);  /* key */
      x_copy_aux(L, -2);  /* value copy */
      lua_settable(L, -5);
      lua_pop(L, 1);
    }
  }
}


static int x_copy (lua_State *L) {
  lua_settop(L, 1);
  x_copy_aux(L, 1);
  return 1;
}


static const luaL_Reg xlib[] = {
  {"is", x_is_f},
  {"gte", x_gte_f},
  {"eq", x_eq_f},
  {"iter", x_iter},
  {"xin", x_xin},
  {"cat", x_cat},
  {"tostring", x_tostring},
  {"print", x_print},
  {"copy", x_copy},
  {NULL, NULL}
};

/* }====================================================== */


/*
** {======================================================
** Tasks: identity, closing, results
** =======================================================
*/

static void xtask_close (lua_State *L, int idx);
static void tasks_close (lua_State *L, int idx);
static void emit_to (lua_State *L, int stk, int toidx, int emtidx, int level);


/*
** Push the task identity of 't' (at idx): skips pools and (unless
** 'tra') transparent tasks; pushes nil for the root pool.
*/
static void me_aux (lua_State *L, int tra, int idx) {
  lua_pushvalue(L, idx);
  for (;;) {
    if (lua_isnil(L, -1) || isroot(L, -1)) {
      lua_pop(L, 1);
      lua_pushnil(L);
      return;
    }
    else if (istasks(L, -1)) {
      getint(L, -1, "up");
      lua_remove(L, -2);
    }
    else if (!tra) {
      int t;
      getint(L, -1, "tra");
      t = lua_toboolean(L, -1);
      lua_pop(L, 1);
      if (!t) return;
      getint(L, -1, "up");
      lua_remove(L, -2);
    }
    else
      return;
  }
}


/* push the running task (or nil) */
static void pushme (lua_State *L, int tra) {
  pushroot(L);
  getint(L, -1, "cache");
  lua_pushthread(L);
  lua_rawget(L, -2);  /* cache[th] */
  lua_remove(L, -2);  /* cache */
  lua_remove(L, -2);  /* root */
  if (!lua_isnil(L, -1)) {
    me_aux(L, tra, -1);
    lua_remove(L, -2);
  }
}


/* close a child (xtask or pool) */
static void child_close (lua_State *L, int idx) {
  if (isxtask(L, idx))
    xtask_close(L, idx);
  else
    tasks_close(L, idx);
}


static void close_dns (lua_State *L, int idx) {
  lua_Integer i, n;
  idx = lua_absindex(L, idx);
  getint(L, idx, "dns");
  n = rawlen(L, -1);
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    child_close(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}


/* close the off-tree toggle gate task, if any */
static void close_gate (lua_State *L, int idx) {
  idx = lua_absindex(L, idx);
  if (getint(L, idx, "toggle") == LUA_TTABLE) {
    if (lua_getfield(L, -1, "task") != LUA_TNIL)
      xtask_close(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}


static void xtask_close (lua_State *L, int idx) {
  lua_State *co;
  int st;
  idx = lua_absindex(L, idx);
  luaL_checkstack(L, 6, "closing task");
  close_dns(L, idx);
  close_gate(L, idx);
  co = getthread(L, idx);
  st = costatus(L, co);
  if (st == COS_YIELD || st == COS_DEAD) {
    /* dead: runs pending '<close>' of an errored coroutine */
    int status = lua_closethread(co, L);
    if (status != LUA_OK) {
      if (st == COS_DEAD)
        lua_pop(co, 1);  /* dead+error: raised by 'task_result' */
      else {
        lua_xmove(co, L, 1);  /* error object */
        lua_error(L);
      }
    }
  }
  else {
    lua_pushliteral(L, "aborted");  /* running: closed in 'task_result' */
    setint(L, idx, "status");
  }
  if (getint(L, idx, "up") != LUA_TNIL) {
    lua_getfield(L, LUA_REGISTRYINDEX, RK_GC);
    lua_insert(L, -2);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);  /* GC[up] = true */
    lua_pop(L, 1);
  }
  else
    lua_pop(L, 1);
}


static void tasks_close (lua_State *L, int idx) {
  idx = lua_absindex(L, idx);
  close_dns(L, idx);
  close_gate(L, idx);
}


/*
** Handles the result of resuming task at 'tidx': 'status' and 'nres'
** come from 'lua_resume' on its thread.
*/
static void task_result (lua_State *L, int tidx, int status, int nres) {
  lua_State *co = getthread(L, tidx);
  tidx = lua_absindex(L, tidx);
  if (status != LUA_OK && status != LUA_YIELD) {  /* error exit: no emit */
    lua_xmove(co, L, 1);  /* error object */
    xtask_close(L, tidx);  /* closes subtree, flags parent 'GC' */
    lua_error(L);
  }
  else if (intstreq(L, tidx, "status", "aborted")) {
    /* aborted while running: close now (could not before), no emit */
    lua_pop(co, nres);
    xtask_close(L, tidx);
  }
  else if (status == LUA_OK) {  /* normal end: emits termination to parent */
    if (nres > 0) {
      lua_xmove(co, L, 1);  /* first result */
      lua_pop(co, nres - 1);
    }
    else
      lua_pushnil(L);
    lua_setfield(L, tidx, "ret");
    getint(L, tidx, "up");  /* t._.up */
    if (istasks(L, -1) && !isroot(L, -1)) {
      getint(L, -1, "up");  /* await(ts) must reach parent */
      lua_remove(L, -2);
    }
    me_aux(L, 0, -1);
    lua_remove(L, -2);
    lua_pushvalue(L, tidx);
    emit_to(L, 0, -2, -1, 1);
    lua_pop(L, 2);
    xtask_close(L, tidx);
  }
  else {  /* yielded: suspended */
    lua_pop(co, nres);
  }
}


/*
** Suspends the running task if it was aborted while running.
** Returns true if the caller must yield.
*/
static int aborted (lua_State *L) {
  int res = 0;
  pushme(L, 1);
  if (!lua_isnil(L, -1))
    res = intstreq(L, -1, "status", "aborted");
  lua_pop(L, 1);
  return res;
}


static int bug_k (lua_State *L, int status, lua_KContext ctx) {
  (void)status; (void)ctx;
  return luaL_error(L, "bug found");
}


/* wait to be closed from outside */
#define yield_if_aborted(L) \
	{ if (aborted(L)) return lua_yieldk(L, 0, 0, bug_k); }


/*
** Sweeps dead tasks: call between reactions only.
*/
static void gc (lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, RK_GC);  /* GC */
  lua_pushnil(L);
  while (lua_next(L, -2) != 0) {
    lua_Integer i, n;
    lua_pop(L, 1);  /* value */
    /* GC[t] = nil (ok: clearing existing fields during traversal) */
    lua_pushvalue(L, -1);
    lua_pushnil(L);
    lua_rawset(L, -4);
    getint(L, -1, "dns");  /* t._.dns */
    n = rawlen(L, -1);
    for (i = n; i >= 1; i--) {
      int dead;
      lua_rawgeti(L, -1, i);
      dead = (isxtask(L, -1) && taskstatus(L, -1) == COS_DEAD);
      lua_pop(L, 1);
      if (dead) {  /* table.remove(dns, i) */
        lua_Integer j;
        for (j = i; j < n; j++) {
          lua_rawgeti(L, -1, j + 1);
          lua_rawseti(L, -2, j);
        }
        lua_pushnil(L);
        lua_rawseti(L, -2, n);
        n--;
      }
    }
    lua_pop(L, 1);  /* dns */
  }
  lua_pop(L, 1);  /* GC */
}


static int atm_gc (lua_State *L) {
  gc(L);
  return 0;
}

/* }====================================================== */


/*
** {======================================================
** Errors: throw objects, traces, catch
** =======================================================
*/

/*
** Push a trace (list of {msg, dbg}) of the running task and its
** ancestors.
*/
static void pushtrace (lua_State *L) {
  lua_Integer n = 0;
  lua_newtable(L);
  pushme(L, 1);
  while (!lua_isnil(L, -1) && !isroot(L, -1)) {
    lua_createtable(L, 0, 2);
    lua_pushstring(L, isxtask(L, -2) ? "task" : "tasks");
    lua_setfield(L, -2, "msg");
    getint(L, -2, "dbg");
    lua_setfield(L, -2, "dbg");
    lua_rawseti(L, -3, ++n);
    getint(L, -1, "up");
    lua_remove(L, -2);
  }
  lua_pop(L, 1);
}


/*
** Build a throw object from the values at positions 'first'..top, with
** 'dbg' (a table) as its position. Leaves the object on the stack.
*/
static void tothrow (lua_State *L, int dbg, int first, int last) {
  int i, n = last - first + 1;
  dbg = lua_absindex(L, dbg);
  lua_createtable(L, n, 1);
  for (i = 0; i < n; i++) {
    lua_pushvalue(L, first + i);
    lua_rawseti(L, -2, i + 1);
  }
  lua_createtable(L, 0, 3);  /* _ */
  lua_pushvalue(L, dbg);
  lua_setfield(L, -2, "dbg");
  pushtrace(L);
  lua_setfield(L, -2, "pre");
  lua_newtable(L);
  lua_setfield(L, -2, "pos");
  lua_setfield(L, -2, "_");
  luaL_setmetatable(L, MT_THROW);
}


/* throw(...): raises a throw object with the position of the caller */
static int atm_throw (lua_State *L) {
  int n = lua_gettop(L);
  pushdbg(L, 1);
  tothrow(L, -1, 1, n);
  return lua_error(L);
}


/* push "file:line (msg)" of trace entry at idx */
static void pushtraceentry (lua_State *L, int idx) {
  int base = lua_gettop(L);
  const char *file, *line, *msg;
  idx = lua_absindex(L, idx);
  lua_getfield(L, idx, "dbg");
  lua_getfield(L, -1, "file");
  file = luaL_tolstring(L, -1, NULL);
  lua_getfield(L, -3, "line");
  line = luaL_tolstring(L, -1, NULL);
  lua_getfield(L, idx, "msg");
  msg = luaL_tolstring(L, -1, NULL);
  lua_pushfstring(L, "%s:%s (%s)", file, line, msg);
  lua_copy(L, -1, base + 1);
  lua_settop(L, base + 1);
}


/* flatten a throw object (at idx) into a string (pushed) */
static void flatten (lua_State *L, int idx) {
  luaL_Buffer b;
  lua_Integer i, j, n;
  int pos, pre, dbg;
  idx = lua_absindex(L, idx);
  luaL_checkstack(L, 10, "flatten");
  lua_getfield(L, idx, "_");
  lua_getfield(L, -1, "pos");
  pos = lua_gettop(L);
  lua_getfield(L, -2, "pre");
  pre = lua_gettop(L);
  lua_getfield(L, -3, "dbg");
  dbg = lua_gettop(L);
  luaL_buffinit(L, &b);
  luaL_addstring(&b, "==> ERROR:\n");
  n = rawlen(L, pos);
  for (i = n; i >= 1; i--) {
    lua_Integer m;
    luaL_addstring(&b, " |  ");
    lua_rawgeti(L, pos, i);  /* t */
    m = rawlen(L, -1);
    lua_pop(L, 1);
    for (j = 1; j <= m; j++) {
      lua_rawgeti(L, pos, i);
      lua_rawgeti(L, -1, j);
      pushtraceentry(L, -1);
      lua_copy(L, -1, -3);
      lua_pop(L, 2);
      luaL_addvalue(&b);
      if (j < m) luaL_addstring(&b, " <- ");
    }
    luaL_addchar(&b, '\n');
  }
  luaL_addstring(&b, " v  ");
  {
    int base = lua_gettop(L);
    const char *file, *line;
    lua_getfield(L, dbg, "file");
    file = luaL_tolstring(L, -1, NULL);
    lua_getfield(L, dbg, "line");
    line = luaL_tolstring(L, -1, NULL);
    lua_pushfstring(L, "%s:%s (throw)", file, line);
    lua_copy(L, -1, base + 1);
    lua_settop(L, base + 1);
    luaL_addvalue(&b);
  }
  n = rawlen(L, pre);
  for (i = 1; i <= n; i++) {
    luaL_addstring(&b, " <- ");
    lua_rawgeti(L, pre, i);
    pushtraceentry(L, -1);
    lua_remove(L, -2);
    luaL_addvalue(&b);
  }
  luaL_addstring(&b, "\n==> ");
  n = rawlen(L, idx);
  for (i = 1; i <= n; i++) {
    if (i > 1) luaL_addstring(&b, ", ");
    lua_rawgeti(L, idx, i);
    luaL_tolstring(L, -1, NULL);
    lua_remove(L, -2);
    luaL_addvalue(&b);
  }
  luaL_addchar(&b, '\n');
  luaL_pushresult(&b);
  lua_remove(L, dbg);
  lua_remove(L, pre);
  lua_remove(L, pos);
  lua_remove(L, -2);  /* _ */
}


/*
** Converts the error at the top of the stack (from a protected call
** at the top-level 'loop' or an 'emit') into a throw object with the
** position info 'dbg' and the current trace; flattens it for 'loop'.
** The (new) error object replaces the old one at the top.
*/
static void xcall_error (lua_State *L, int dbg, const char *stk) {
  dbg = lua_absindex(L, dbg);
  if (lua_type(L, -1) == LUA_TSTRING) {
    const char *s = lua_tostring(L, -1);
    if (strncmp(s, "==> ERROR:", 10) == 0)
      return;  /* already flatten */
  }
  if (stk != NULL) {
    if (!isthrow(L, -1)) {
      /* err = { _={dbg={file,line}, pre=trace(), pos={}}, msg } */
      const char *file = "?", *line = "?";
      size_t fl = 1, ll = 1;
      const char *msg = NULL;
      if (lua_type(L, -1) == LUA_TSTRING) {
        /* parse '(.-):(%d-): (.*)' */
        const char *s = lua_tostring(L, -1);
        const char *p;
        for (p = strchr(s, ':'); p != NULL; p = strchr(p + 1, ':')) {
          const char *q = p + 1;
          while (*q >= '0' && *q <= '9') q++;
          if (q[0] == ':' && q[1] == ' ') {
            file = s; fl = cast_sizet(p - s);
            line = p + 1; ll = cast_sizet(q - (p + 1));
            msg = q + 2;
            break;
          }
        }
      }
      lua_createtable(L, 1, 1);
      if (msg != NULL) lua_pushstring(L, msg);
      else lua_pushvalue(L, -2);  /* original error */
      lua_rawseti(L, -2, 1);
      lua_createtable(L, 0, 3);  /* _ */
      lua_createtable(L, 0, 2);  /* dbg */
      lua_pushlstring(L, file, fl);
      lua_setfield(L, -2, "file");
      lua_pushlstring(L, line, ll);
      lua_setfield(L, -2, "line");
      lua_setfield(L, -2, "dbg");
      pushtrace(L);
      lua_setfield(L, -2, "pre");
      lua_newtable(L);
      lua_setfield(L, -2, "pos");
      lua_setfield(L, -2, "_");
      luaL_setmetatable(L, MT_THROW);
      lua_remove(L, -2);  /* original error */
    }
    /* err._.pos[#+1] = t ; table.insert(t, 1, {msg=stk, dbg=dbg}) */
    pushtrace(L);  /* t */
    lua_createtable(L, 0, 2);
    lua_pushstring(L, stk);
    lua_setfield(L, -2, "msg");
    lua_pushvalue(L, dbg);
    lua_setfield(L, -2, "dbg");
    {  /* insert at position 1 */
      lua_Integer n = rawlen(L, -2);
      lua_Integer i;
      for (i = n; i >= 1; i--) {
        lua_rawgeti(L, -2, i);
        lua_rawseti(L, -3, i + 1);
      }
      lua_rawseti(L, -2, 1);
    }
    lua_getfield(L, -2, "_");
    lua_getfield(L, -1, "pos");
    lua_pushvalue(L, -3);
    lua_rawseti(L, -2, rawlen(L, -2) + 1);
    lua_pop(L, 3);  /* pos, _, t */
  }
  if (stk != NULL && strcmp(stk, "loop") == 0) {
    flatten(L, -1);
    lua_remove(L, -2);
  }
}


/*
** catch(cnd..., blk): runs 'blk' in protected mode and matches thrown
** objects against 'cnd'.
*/
static int catch_k (lua_State *L, int status, lua_KContext ctx) {
  int ncnd = cast_int(ctx);
  if (status == LUA_OK || status == LUA_YIELD) {  /* no errors? */
    int n = lua_gettop(L) - ncnd;
    luaL_checkstack(L, 1, "catch");
    lua_pushboolean(L, 1);
    lua_insert(L, ncnd + 1);
    return n + 1;
  }
  else {  /* error object at top */
    int err = lua_gettop(L);
    if (!isthrow(L, err))
      return lua_error(L);
    if (ncnd >= 1 && lua_type(L, 1) == LUA_TBOOLEAN && !lua_toboolean(L, 1))
      return lua_error(L);  /* false: ignores any throw */
    else if (ncnd >= 1 && lua_type(L, 1) == LUA_TBOOLEAN) {
      /* true: catches any throw */
    }
    else if (ncnd >= 1 && lua_type(L, 1) == LUA_TFUNCTION) {
      /* function: matches if it returns true, replacing the values */
      lua_Integer n = rawlen(L, err), i;
      int base = lua_gettop(L);
      int nres;
      lua_pushvalue(L, 1);
      luaL_checkstack(L, cast_int(n) + 2, "catch");
      for (i = 1; i <= n; i++)
        lua_rawgeti(L, err, i);
      lua_call(L, cast_int(n), LUA_MULTRET);
      nres = lua_gettop(L) - base;
      if (nres >= 1 && lua_toboolean(L, base + 1)) {
        lua_pushboolean(L, 0);
        lua_replace(L, base + 1);
        return nres;
      }
      lua_settop(L, err);
      return lua_error(L);
    }
    else {  /* values: catches if err[i] ?? cnd[i] for all i */
      int i;
      for (i = 1; i <= ncnd; i++) {
        int res;
        lua_rawgeti(L, err, i);
        res = x_is(L, -1, i);
        lua_pop(L, 1);
        if (!res) return lua_error(L);
      }
    }
    {  /* return false, unpack(err) */
      lua_Integer n = rawlen(L, err), i;
      luaL_checkstack(L, cast_int(n) + 2, "catch");
      lua_pushboolean(L, 0);
      for (i = 1; i <= n; i++)
        lua_rawgeti(L, err, i);
      return cast_int(n) + 1;
    }
  }
}


static int atm_catch (lua_State *L) {
  int n = lua_gettop(L);
  if (n < 1 || !lua_isfunction(L, n))
    return atmerror(L, 1, "invalid catch : expected body function");
  return catch_k(L, lua_pcallk(L, 0, LUA_MULTRET, 0, n - 1, catch_k), n - 1);
}

/* }====================================================== */


/*
** {======================================================
** Environments, defer, time
** =======================================================
*/

static int atm_env (lua_State *L) {
  lua_Integer n;
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, RK_ENVS);
  n = rawlen(L, -1);
  lua_pushvalue(L, 1);
  lua_rawseti(L, -2, n + 1);
  if (n + 1 > 1) {
    int ok;
    lua_rawgeti(L, -1, 1);  /* first env */
    ok = (lua_getfield(L, -1, "mode") == LUA_TTABLE &&
          (lua_getfield(L, -1, "primary"), lua_toboolean(L, -1)));
    argcheck(L, ok, 1, "invalid env : first env must support primary mode");
    lua_pop(L, 1);  /* primary */
    lua_pushliteral(L, "primary");
    lua_setfield(L, -2, "current");
    lua_pop(L, 2);  /* mode, first */
    ok = (lua_getfield(L, 1, "mode") == LUA_TTABLE &&
          (lua_getfield(L, -1, "secondary"), lua_toboolean(L, -1)));
    argcheck(L, ok, 1, "invalid env : non-first envs must support secondary mode");
    lua_pop(L, 1);
    lua_pushliteral(L, "secondary");
    lua_setfield(L, -2, "current");
    lua_pop(L, 1);
  }
  return 0;
}


static int defer_k (lua_State *L, int status, lua_KContext ctx) {
  (void)L; (void)status; (void)ctx;
  return 0;
}


/* __close of defer objects: calls the deferred function */
static int defer_close (lua_State *L) {
  lua_getfield(L, 1, "f");
  lua_callk(L, 0, 0, 0, defer_k);
  return 0;
}


static int atm_defer (lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_settop(L, 1);
  lua_createtable(L, 0, 1);
  lua_insert(L, 1);
  lua_setfield(L, 1, "f");
  luaL_setmetatable(L, MT_DEFER);
  return 1;
}


static int atm_time (lua_State *L) {
  lua_pushinteger(L, getrs(L)->time);
  return 1;
}


/* us to nearest timer (lower bound), or nil if none (envs may sleep) */
static int atm_next (lua_State *L) {
  RunState *rs = getrs(L);
  if (rs->clkmin < HUGE_VAL)
    lua_pushnumber(L, rs->clkmin);
  else
    lua_pushnil(L);
  return 1;
}


static int atm_status (lua_State *L) {
  argcheck(L, isxtask(L, 1), 1, "invalid status : expected task");
  lua_pushstring(L, statname[taskstatus(L, 1)]);
  return 1;
}

/* }====================================================== */


/*
** {======================================================
** Task creation and spawning
** =======================================================
*/

/* push a new table '_' with common fields; 'up' at upidx (or 0) */
static void newinternals (lua_State *L, int upidx, int dbgidx) {
  lua_createtable(L, 0, 8);
  if (upidx != 0) {
    lua_pushvalue(L, upidx);
    lua_setfield(L, -2, "up");
  }
  lua_newtable(L);
  lua_setfield(L, -2, "dns");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "pin");
  if (dbgidx != 0) {
    lua_pushvalue(L, dbgidx);
    lua_setfield(L, -2, "dbg");
  }
}


/* tasks(max): creates a task pool */
static int atm_tasks (lua_State *L) {
  int hasmax = !lua_isnoneornil(L, 1);
  lua_settop(L, 1);
  if (hasmax)
    argcheck(L, lua_isnumber(L, 1), 1, "invalid tasks limit : expected number");
  pushme(L, 1);  /* up = me(true) or TASKS */
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    pushroot(L);
  }
  pushdbg(L, 1);  /* 3: dbg */
  lua_createtable(L, 0, 2);  /* 4: ts */
  newinternals(L, 2, 3);
  if (hasmax) {
    lua_pushnumber(L, lua_tonumber(L, 1));
    lua_setfield(L, -2, "max");
  }
  lua_setfield(L, 4, "_");
  luaL_setmetatable(L, MT_TASKS);
  /* up._.dns[#+1] = ts */
  getint(L, 2, "dns");
  lua_pushvalue(L, 4);
  lua_rawseti(L, -2, rawlen(L, -2) + 1);
  lua_pop(L, 1);
  lua_pushvalue(L, 4);
  return 1;
}


/* creates a task prototype from function at 'fidx' with 'dbg' */
static void newtask (lua_State *L, int fidx, int dbgidx, int level) {
  fidx = lua_absindex(L, fidx);
  dbgidx = lua_absindex(L, dbgidx);
  argcheck(L, lua_isfunction(L, fidx), level, "invalid task : expected function");
  lua_createtable(L, 0, 1);
  lua_createtable(L, 0, 2);
  lua_pushvalue(L, dbgidx);
  lua_setfield(L, -2, "dbg");
  lua_pushvalue(L, fidx);
  lua_setfield(L, -2, "f");
  lua_setfield(L, -2, "_");
  luaL_setmetatable(L, MT_TASK);
}


/* task(f): creates a task prototype */
static int atm_task (lua_State *L) {
  lua_settop(L, 1);
  pushdbg(L, 1);
  newtask(L, 1, 2, 1);
  return 1;
}


/*
** Creates a task instance (unstarted) from a prototype (or raw
** function, the transparent-combinator path) at 'Tidx'.
*/
static void newxtask (lua_State *L, int Tidx, int tra, int dbgidx, int level) {
  lua_State *co;
  Tidx = lua_absindex(L, Tidx);
  dbgidx = lua_absindex(L, dbgidx);
  if (istask(L, Tidx))
    getint(L, Tidx, "f");
  else if (tra)
    lua_pushvalue(L, Tidx);
  else
    lua_pushnil(L);
  argcheck(L, lua_isfunction(L, -1), level, "invalid xtask : expected task prototype");
  co = lua_newthread(L);  /* thread */
  lua_pushvalue(L, -2);  /* function */
  lua_xmove(L, co, 1);  /* move function to the new thread */
  lua_remove(L, -2);  /* function */
  lua_createtable(L, 0, 3);  /* t */
  newinternals(L, 0, dbgidx);
  lua_pushvalue(L, -3);  /* thread */
  lua_setfield(L, -2, "th");
  lua_pushboolean(L, tra);
  lua_setfield(L, -2, "tra");
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "time");
  lua_setfield(L, -2, "_");
  luaL_setmetatable(L, MT_XTASK);
  /* TASKS._.cache[th] = t */
  pushroot(L);
  getint(L, -1, "cache");
  lua_pushvalue(L, -4);  /* thread */
  lua_pushvalue(L, -4);  /* t */
  lua_rawset(L, -3);
  lua_pop(L, 2);
  lua_remove(L, -2);  /* thread */
}


/* xtask([T]): the running task, or a new instance of T */
static int atm_xtask (lua_State *L) {
  if (lua_isnoneornil(L, 1)) {
    pushme(L, 0);
    return 1;
  }
  lua_settop(L, 1);
  pushdbg(L, 1);
  newxtask(L, 1, 0, 2, 1);
  return 1;
}


/*
** Spawns the task at 'tidx' (a prototype, function, or instance) with
** arguments at 'firstarg'..top under 'upidx' (or the running task).
** Pushes the instance (or nil when the pool is full). Returns 1 if the
** caller must yield (aborted while spawning).
*/
static int spawn_aux (lua_State *L, int upidx, int tra, int tidx,
                      int dbgidx, int firstarg, int nargs, int level) {
  lua_State *co;
  int status, nres, i;
  upidx = lua_absindex(L, upidx);
  tidx = lua_absindex(L, tidx);
  dbgidx = lua_absindex(L, dbgidx);
  luaL_checkstack(L, nargs + 10, "spawn");
  if (lua_isfunction(L, tidx)) {
    /* raw function: only the transparent inline-body path */
    argcheck(L, tra, level, "invalid spawn : expected task prototype");
    newxtask(L, tidx, tra, dbgidx, level);
    lua_replace(L, tidx);
  }
  else if (istask(L, tidx)) {
    /* prototype: never transparent (transparency = no identity) */
    argcheck(L, !tra, level, "invalid spawn : transparent task prototype");
    newxtask(L, tidx, tra, dbgidx, level);
    lua_replace(L, tidx);
  }
  argcheck(L, isxtask(L, tidx), level, "invalid spawn : expected task prototype");
  getint(L, tidx, "tra");
  argcheck(L, lua_toboolean(L, -1) == (tra != 0), level,
           "invalid spawn : transparent modifier mismatch");
  lua_pop(L, 1);
  /* up = up or me(true) or TASKS */
  if (lua_isnil(L, upidx)) {
    pushme(L, 1);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      pushroot(L);
    }
    lua_replace(L, upidx);
  }
  if (istasks(L, upidx)) {
    lua_pushboolean(L, 1);
    lua_setfield(L, tidx, "pin");
  }
  if (getint(L, upidx, "max") != LUA_TNIL) {
    lua_Number max = lua_tonumber(L, -1);
    lua_Integer n, j;
    lua_pop(L, 1);
    getint(L, upidx, "dns");
    n = rawlen(L, -1);
    if (cast_num(n) >= max) {
      for (j = 1; j <= rawlen(L, -1); j++) {
        lua_rawgeti(L, -1, j);
        if (taskstatus(L, -1) == COS_DEAD) n--;
        lua_pop(L, 1);
      }
      if (cast_num(n) >= max) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 0;
      }
    }
    lua_pop(L, 1);
  }
  else
    lua_pop(L, 1);
  getint(L, tidx, "up");
  argcheck(L, lua_isnil(L, -1), level, "invalid spawn : unexpected active task");
  lua_pop(L, 1);
  /* up._.dns[#+1] = t ; t._.up = up */
  getint(L, upidx, "dns");
  lua_pushvalue(L, tidx);
  lua_rawseti(L, -2, rawlen(L, -2) + 1);
  lua_pop(L, 1);
  lua_pushvalue(L, upidx);
  setint(L, tidx, "up");
  /* resume */
  co = getthread(L, tidx);
  if (!lua_checkstack(co, nargs))
    return luaL_error(L, "too many arguments to resume");
  for (i = 0; i < nargs; i++)
    lua_pushvalue(L, firstarg + i);
  lua_xmove(L, co, nargs);
  status = lua_resume(co, L, nargs, &nres);
  task_result(L, tidx, status, nres);
  lua_pushvalue(L, tidx);
  return aborted(L);  /* maybe 't' aborted me */
}


static int spawn_k (lua_State *L, int status, lua_KContext ctx) {
  (void)status; (void)ctx;
  return luaL_error(L, "bug found");
}


/* spawn(t, ...) */
static int atm_spawn (lua_State *L) {
  int top = lua_gettop(L);
  luaL_checkstack(L, 4, "spawn");
  lua_pushnil(L);  /* up */
  pushdbg(L, 1);  /* dbg */
  if (spawn_aux(L, top + 1, 0, 1, top + 2, 2, top - 1, 1))
    return lua_yieldk(L, 0, 0, spawn_k);
  return 1;
}


/* spawn_in(up, t, ...) */
static int atm_spawn_in (lua_State *L) {
  int top = lua_gettop(L);
  luaL_checkstack(L, 4, "spawn");
  pushdbg(L, 1);  /* dbg */
  if (spawn_aux(L, 1, 0, 2, top + 1, 3, top - 2, 1))
    return lua_yieldk(L, 0, 0, spawn_k);
  return 1;
}


/* spawn_tra(f, ...): spawns a transparent task; returns the instance */
static int atm_spawn_tra (lua_State *L) {
  int top = lua_gettop(L);
  luaL_checkstack(L, 4, "spawn");
  lua_pushnil(L);  /* up */
  pushdbg(L, 1);  /* dbg */
  if (spawn_aux(L, top + 1, 1, 1, top + 2, 2, top - 1, 1))
    return lua_yieldk(L, 0, 0, spawn_k);
  return 1;
}


/* me([tra]): the running task (nil outside tasks) */
static int atm_me (lua_State *L) {
  pushme(L, lua_toboolean(L, 1));
  return 1;
}


/* __close of 'do_spawn' handles: closes the hidden task */
static int handle_close (lua_State *L) {
  lua_getiuservalue(L, 1, 1);
  xtask_close(L, -1);
  return 0;
}


/*
** do_spawn(f, ...): a transparent task has no identity to manipulate,
** so we return a close-only handle instead of the instance.
*/
static int atm_do_spawn (lua_State *L) {
  int top = lua_gettop(L);
  int ab;
  luaL_checkstack(L, 4, "spawn");
  lua_pushnil(L);  /* up */
  pushdbg(L, 1);  /* dbg */
  ab = spawn_aux(L, top + 1, 1, 1, top + 2, 2, top - 1, 1);
  lua_newuserdatauv(L, 0, 1);
  lua_insert(L, -2);
  lua_setiuservalue(L, -2, 1);
  luaL_setmetatable(L, MT_HANDLE);
  if (ab)
    return lua_yieldk(L, 0, 0, spawn_k);
  return 1;
}


/* abort(t) */
static int atm_abort (lua_State *L) {
  argcheck(L, isxtask(L, 1) || istasks(L, 1), 1, "invalid abort : expected task");
  lua_settop(L, 1);
  child_close(L, 1);
  yield_if_aborted(L);  /* self or ancestor */
  return 0;
}

/* }====================================================== */


/*
** {======================================================
** Await (primitive patterns)
** =======================================================
*/

/*
** Stack layout of 'await' while suspended:
**   1: time   2: awt   3: me   4: emt   5: clk (number or nil)
**   6: tag    7: mta (metatable of awt or nil)
*/
#define AW_TIME	1
#define AW_AWT	2
#define AW_ME	3
#define AW_EMT	4
#define AW_CLK	5
#define AW_TAG	6
#define AW_MTA	7

static int await_k (lua_State *L, int status, lua_KContext ctx);


/* 'ret ~= true and ret or emt' */
static int retor (lua_State *L, int retidx, int emtidx) {
  if (lua_type(L, retidx) == LUA_TBOOLEAN && lua_toboolean(L, retidx))
    lua_pushvalue(L, emtidx);
  else
    lua_pushvalue(L, retidx);
  return 1;
}


static int tageq (lua_State *L, const char *s) {
  return (lua_type(L, AW_TAG) == LUA_TSTRING &&
          strcmp(lua_tostring(L, AW_TAG), s) == 0);
}


/*
** Checks that can be made before suspending; returns the number of
** results if the await terminates, or -1 to suspend.
*/
static int await_pre (lua_State *L) {
  RunState *rs = getrs(L);
  lua_settop(L, AW_MTA);
  if (!lua_isnil(L, AW_MTA) && lua_getfield(L, AW_MTA, "__atmos") != LUA_TNIL) {
    lua_pushvalue(L, AW_AWT);
    lua_pushvalue(L, AW_EMT);
    lua_call(L, 2, 1);
    if (lua_toboolean(L, -1))
      return retor(L, -1, AW_EMT);
    lua_pop(L, 1);
  }
  else if (!lua_isnil(L, AW_MTA)) lua_pop(L, 1);
  if (isxtask(L, AW_AWT)) {
    if (taskstatus(L, AW_AWT) == COS_DEAD) {
      lua_getfield(L, AW_AWT, "ret");
      lua_pushvalue(L, AW_AWT);
      return 2;
    }
  }
  else if (!lua_isnil(L, AW_CLK)) {
    lua_Number clk = lua_tonumber(L, AW_CLK);
    if (clk <= 0) {
      lua_pushnumber(L, rs->clklate - clk);  /* on time: 'late' */
      return 1;
    }
    if (clk < rs->clkmin) rs->clkmin = clk;  /* report */
  }
  else if (lua_isfunction(L, AW_AWT)) {
    lua_pushvalue(L, AW_AWT);
    lua_pushvalue(L, AW_EMT);
    lua_call(L, 1, 1);
    if (lua_toboolean(L, -1))
      return retor(L, -1, AW_EMT);
    lua_pop(L, 1);
  }
  return -1;
}


static int await_loop (lua_State *L) {
  int n = await_pre(L);
  if (n >= 0) return n;
  return lua_yieldk(L, 0, 0, await_k);
}


/*
** Continuation after a wake-up: the stack has the state plus the
** values passed to resume: (err, emt).
*/
static int await_k (lua_State *L, int status, lua_KContext ctx) {
  RunState *rs = getrs(L);
  int mte;
  (void)status; (void)ctx;
  lua_settop(L, AW_MTA + 2);
  if (lua_toboolean(L, AW_MTA + 1)) {  /* error? */
    lua_settop(L, AW_MTA + 2);
    return lua_error(L);  /* error(emt, 0) */
  }
  lua_replace(L, AW_EMT);  /* emt */
  lua_settop(L, AW_MTA);
  mte = lua_getmetatable(L, AW_EMT);
  if (mte && lua_getfield(L, -1, "__atmos") != LUA_TNIL) {
    lua_pushvalue(L, AW_EMT);
    lua_pushvalue(L, AW_AWT);
    lua_call(L, 2, 1);
    if (lua_toboolean(L, -1))
      return retor(L, -1, AW_EMT);
    lua_settop(L, AW_MTA);
  }
  else {
    lua_settop(L, AW_MTA);
    if (lua_type(L, AW_AWT) == LUA_TBOOLEAN) {
      if (lua_toboolean(L, AW_AWT)) {  /* true: any event */
        lua_pushvalue(L, AW_EMT);
        return 1;
      }
      /* false: never awakes */
    }
    else if (!lua_isnil(L, AW_CLK) || tageq(L, "clock")) {
      if (lua_type(L, AW_EMT) == LUA_TNUMBER) {
        if (!lua_isnil(L, AW_CLK)) {
          /* elapsed time: a bare number advances the countdown */
          lua_pushnumber(L, lua_tonumber(L, AW_CLK) - lua_tonumber(L, AW_EMT));
          lua_replace(L, AW_CLK);
        }
        else {
          /* clock tick: wake on any bare-number emit, return the delta */
          lua_pushvalue(L, AW_EMT);
          lua_pushnumber(L, rs->clklate);
          return 2;
        }
      }
    }
    else if (tageq(L, "tasks")) {
      int ok;
      lua_getfield(L, AW_AWT, "tasks");  /* ts */
      ok = isxtask(L, AW_EMT);
      if (ok) {
        getint(L, AW_EMT, "up");
        ok = lua_rawequal(L, -1, -2);
        lua_pop(L, 1);
      }
      if (ok) {
        lua_getfield(L, AW_AWT, "mode");
        if (strcmp(lua_tostring(L, -1), "all") == 0) {
          /* requires pool poll to check at the very termination moment */
          lua_Integer i, n;
          getint(L, -2, "dns");
          n = rawlen(L, -1);
          for (i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            if (taskstatus(L, -1) != COS_DEAD) {
              ok = 0;
              lua_pop(L, 1);
              break;
            }
            lua_pop(L, 1);
          }
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
      if (ok) {
        lua_getfield(L, AW_EMT, "ret");
        lua_pushvalue(L, AW_EMT);
        lua_pushvalue(L, -3);  /* ts */
        return 3;
      }
      lua_pop(L, 1);
    }
    else if (lua_type(L, AW_AWT) == LUA_TTABLE) {
      if (!isxtask(L, AW_AWT) && x_gte(L, AW_AWT, AW_EMT)) {
        lua_pushvalue(L, AW_EMT);
        return 1;
      }
    }
    else {  /* string, number */
      if (x_is(L, AW_EMT, AW_TAG)) {
        lua_pushvalue(L, AW_EMT);
        return 1;
      }
    }
  }
  return await_loop(L);
}


/* await(time, awt): primitive await */
static int atm_await (lua_State *L) {
  RunState *rs = getrs(L);
  int n = lua_gettop(L);
  int allowf = (n == 3 && lua_toboolean(L, 3));  /* internal predicates */
  argcheck(L, (n == 2 || allowf) && !lua_isnil(L, 2), 1,
           "invalid await : invalid event pattern");
  lua_settop(L, 2);
  pushme(L, 1);  /* 3: me */
  argcheck(L, !lua_isnil(L, 3), 1, "invalid await : expected enclosing task");
  argcheck(L, !istasks(L, 2), 1, "invalid await : unexpected tasks pool");
  argcheck(L, allowf || !lua_isfunction(L, 2), 1,
           "invalid await : unexpected function");
  lua_pushnil(L);  /* 4: emt */
  if (lua_type(L, 2) == LUA_TNUMBER)
    lua_pushvalue(L, 2);  /* 5: clk */
  else
    lua_pushnil(L);
  if (lua_type(L, 2) == LUA_TTABLE) {  /* 6: tag */
    if (lua_getfield(L, 2, "tag") == LUA_TNIL) {
      lua_pop(L, 1);
      lua_pushvalue(L, 2);
    }
  }
  else
    lua_pushvalue(L, 2);
  if (tageq(L, "tasks")) {
    int ok;
    lua_getfield(L, 2, "mode");
    ok = (lua_type(L, -1) == LUA_TSTRING &&
          (strcmp(lua_tostring(L, -1), "any") == 0 ||
           strcmp(lua_tostring(L, -1), "all") == 0));
    argcheck(L, ok, 1, "invalid await : invalid mode");
    lua_pop(L, 1);
  }
  if (!lua_getmetatable(L, 2))  /* 7: mta */
    lua_pushnil(L);
  /* stamp await birth time: a task reacts only to broadcasts that begin
     after its current await is established (one wake per emit) */
  lua_pushvalue(L, 1);
  setint(L, 3, "time");
  (void)rs;
  return await_loop(L);
}

/* }====================================================== */


/*
** {======================================================
** Emit
** =======================================================
*/

/*
** Broadcasts the event at 'emtidx' to the task tree rooted at 'tidx'
** (recursive). Errors propagate to the awaiting tasks and up.
*/
static int emit_aux (lua_State *L, lua_Integer time, int tidx, int emtidx);


/* protected wrapper: emit_aux(time, t, emt) */
static int emit_pcall (lua_State *L) {
  lua_Integer time = lua_tointeger(L, 1);
  emit_aux(L, time, 2, 3);
  return 0;
}


static int emit_aux (lua_State *L, lua_Integer time, int tidx, int emtidx) {
  int ok = 1;
  lua_Integer i, n;
  tidx = lua_absindex(L, tidx);
  emtidx = lua_absindex(L, emtidx);
  luaL_checkstack(L, 12, "emit");
  if (intstreq(L, tidx, "status", "toggled")) {
    /* toggled off: drive the off-tree gate task first, then gate the
       subtree */
    int pass = 0;
    if (getint(L, tidx, "toggle") == LUA_TTABLE) {
      lua_getfield(L, -1, "task");
      emit_aux(L, time, -1, emtidx);
      lua_pop(L, 1);
      lua_getfield(L, -1, "pass");
      pass = (lua_isinteger(L, -1) && lua_tointeger(L, -1) == time);
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (!pass) return 1;
  }
  getint(L, tidx, "dns");
  n = rawlen(L, -1);
  for (i = 1; i <= n; i++) {
    lua_pushcfunction(L, emit_pcall);
    lua_pushinteger(L, time);
    lua_rawgeti(L, -3, cast_int(i));
    lua_pushvalue(L, emtidx);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
      ok = 0;  /* error object at top */
      break;
    }
  }
  if (ok) lua_pushnil(L);  /* err */
  lua_remove(L, -2);  /* dns */
  /* stack: err */
  if (isxtask(L, tidx)) {
    lua_State *co = getthread(L, tidx);
    if (!ok) {
      if (costatus(L, co) == COS_YIELD) {
        int status, nres;
        lua_pushliteral(L, "atm_error");
        lua_pushvalue(L, -2);
        lua_xmove(L, co, 2);
        status = lua_resume(co, L, 2, &nres);
        if (status == LUA_OK || status == LUA_YIELD) {
          task_result(L, tidx, status, nres);
          lua_pop(L, 1);  /* err */
          return 1;
        }
        else {  /* error: replace */
          lua_pop(L, 1);
          lua_xmove(co, L, 1);
        }
      }
      return lua_error(L);  /* TODO: error in defer? */
    }
    else {
      lua_Integer t;
      lua_pop(L, 1);  /* err */
      getint(L, tidx, "time");
      t = lua_tointeger(L, -1);
      lua_pop(L, 1);
      if (t < time && costatus(L, co) == COS_YIELD) {
        int status, nres;
        lua_pushnil(L);
        lua_pushvalue(L, emtidx);
        lua_xmove(L, co, 2);
        status = lua_resume(co, L, 2, &nres);
        task_result(L, tidx, status, nres);
      }
      return 1;
    }
  }
  else {
    if (!ok) return lua_error(L);
    lua_pop(L, 1);
    return 1;
  }
}


/*
** emit_(dbg, stk, tgt, emt): one broadcast with trace handling.
** Returns true if the caller must yield (aborted).
*/
static int emit_ (lua_State *L, int dbgidx, int stk, int tgtidx, int emtidx) {
  RunState *rs = getrs(L);
  dbgidx = lua_absindex(L, dbgidx);
  tgtidx = lua_absindex(L, tgtidx);
  emtidx = lua_absindex(L, emtidx);
  rs->time++;
  lua_pushcfunction(L, emit_pcall);
  lua_pushinteger(L, rs->time);
  lua_pushvalue(L, tgtidx);
  lua_pushvalue(L, emtidx);
  if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
    xcall_error(L, dbgidx, stk ? "emit" : NULL);
    lua_error(L);
  }
  return aborted(L);  /* maybe awakened tasks aborted me */
}


/*
** Resolve an emit target 'to' (at toidx) to a task/pool (pushed).
** 'level' is the error level for invalid targets.
*/
static void fto (lua_State *L, int toidx, int level) {
  lua_Integer n = -1;
  toidx = lua_absindex(L, toidx);
  if (lua_isnil(L, toidx)) n = 0;
  else if (lua_type(L, toidx) == LUA_TSTRING) {
    const char *s = lua_tostring(L, toidx);
    if (strcmp(s, "task") == 0) n = 0;
    else if (strcmp(s, "parent") == 0) n = 1;
    else if (strcmp(s, "global") == 0) {
      pushroot(L);
      return;
    }
    else atmerror(L, level, "invalid emit : invalid target");
  }
  else if (isxtask(L, toidx) || istasks(L, toidx)) {
    lua_pushvalue(L, toidx);
    return;
  }
  else if (lua_type(L, toidx) == LUA_TNUMBER)
    n = lua_tointeger(L, toidx);
  else
    atmerror(L, level, "invalid emit : invalid target");
  /* n levels up from me (identity-based) */
  pushme(L, 0);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    pushroot(L);
  }
  while (n > 0) {
    getint(L, -1, "up");
    lua_remove(L, -2);
    argcheck(L, !lua_isnil(L, -1), level, "invalid emit : invalid target");
    /* transparent tasks are invisible (pools count) */
    for (;;) {
      int tra;
      if (isroot(L, -1)) break;
      getint(L, -1, "tra");
      tra = lua_toboolean(L, -1);
      lua_pop(L, 1);
      if (!tra) break;
      getint(L, -1, "up");
      lua_remove(L, -2);
      argcheck(L, !lua_isnil(L, -1), level, "invalid emit : invalid target");
    }
    n--;
  }
}


/*
** Emits event at 'emtidx' to target at 'toidx' (nil = current task).
** Numeric events are clock ticks split in pieces, one per timer.
** Returns true if the caller must yield.
*/
static int emit_main (lua_State *L, int stk, int toidx, int emtidx, int level) {
  RunState *rs = getrs(L);
  int dbg, tgt;
  toidx = lua_absindex(L, toidx);
  emtidx = lua_absindex(L, emtidx);
  luaL_checkstack(L, 8, "emit");
  pushdbg(L, level);
  dbg = lua_gettop(L);
  fto(L, toidx, level);
  tgt = lua_gettop(L);
  if (lua_type(L, emtidx) != LUA_TNUMBER)
    return emit_(L, dbg, stk, tgt, emtidx);
  else {
    /* numeric tick: global only (targeted would keep a stale 'clkmin') */
    lua_Number emt = lua_tonumber(L, emtidx);
    lua_Number late = rs->clklate;  /* nested numeric emit */
    argcheck(L, isroot(L, tgt), level, "invalid emit : expected global");
    do {  /* split in pieces, each ends at the nearest timer */
      lua_Number d = (rs->clkmin < emt) ? rs->clkmin : emt;
      int ab;
      emt = emt - d;  /* residual after this piece */
      rs->clkmin = HUGE_VAL;  /* timers re-report */
      rs->clklate = emt;
      lua_pushnumber(L, d);
      ab = emit_(L, dbg, stk, tgt, -1);
      lua_pop(L, 1);
      if (ab) return 1;
    } while (emt > 0);
    rs->clklate = late;  /* outer piece's (0 at top) */
    return 0;
  }
}


/* emit to task at 'toidx' (C internal use; no yield) */
static void emit_to (lua_State *L, int stk, int toidx, int emtidx, int level) {
  int top = lua_gettop(L);
  emit_main(L, stk, toidx, emtidx, level);
  lua_settop(L, top);
}


/* emit(e) */
static int atm_emit (lua_State *L) {
  argcheck(L, lua_gettop(L) == 1, 1, "invalid emit : invalid event");
  lua_pushnil(L);  /* to */
  if (emit_main(L, 1, 2, 1, 1))
    return lua_yieldk(L, 0, 0, bug_k);
  return 0;
}


/* emit_in(to, e) */
static int atm_emit_in (lua_State *L) {
  lua_settop(L, 2);
  if (emit_main(L, 1, 1, 2, 1))
    return lua_yieldk(L, 0, 0, bug_k);
  return 0;
}

/* }====================================================== */


/*
** {======================================================
** Toggle (task form)
** =======================================================
*/

/*
** toggle(t, on [, gate]): 'gate' is a function (built by the Lua
** side) that keeps the task responsive to a filter while toggled off.
*/
static int atm_toggle (lua_State *L) {
  argcheck(L, isxtask(L, 1) || istasks(L, 1), 1, "invalid toggle : expected task");
  argcheck(L, lua_type(L, 2) == LUA_TBOOLEAN, 1, "invalid toggle : expected bool argument");
  lua_settop(L, 3);
  if (lua_toboolean(L, 2)) {
    argcheck(L, lua_isnil(L, 3), 1, "invalid toggle : unexpected argument");
    argcheck(L, intstreq(L, 1, "status", "toggled"), 1,
             "invalid toggle : expected toggled off task");
    lua_pushnil(L);
    setint(L, 1, "status");
    if (getint(L, 1, "toggle") == LUA_TTABLE) {
      lua_getfield(L, -1, "task");
      xtask_close(L, -1);  /* off-tree: just close it */
      lua_pop(L, 1);
      lua_pushnil(L);
      setint(L, 1, "toggle");
    }
    lua_pop(L, 1);
    /* frozen timers re-report: 'emit_' (skips global check) */
    pushdbg(L, 1);
    lua_pushinteger(L, 0);
    if (emit_(L, -2, 0, 1, -1))
      return lua_yieldk(L, 0, 0, bug_k);
  }
  else {
    argcheck(L, getint(L, 1, "status") == LUA_TNIL, 1,
             "invalid toggle : expected awaiting task");
    lua_pop(L, 1);
    lua_pushliteral(L, "toggled");
    setint(L, 1, "status");
    if (!lua_isnil(L, 3)) {
      /* hidden gate task (off-tree): emit drives it explicitly before the
         gate check; on a match it stamps t._.toggle.pass = TIME */
      lua_State *co;
      int status, nres;
      pushdbg(L, 1);  /* 4 */
      newxtask(L, 3, 1, 4, 1);  /* 5: gate */
      getint(L, 1, "up");
      setint(L, 5, "up");  /* gate._.up = t._.up */
      lua_createtable(L, 0, 2);
      lua_pushvalue(L, 5);
      lua_setfield(L, -2, "task");
      setint(L, 1, "toggle");  /* t._.toggle = { task = gate } */
      co = getthread(L, 5);
      status = lua_resume(co, L, 0, &nres);  /* start (run to first await) */
      task_result(L, 5, status, nres);
    }
  }
  return 0;
}

/* }====================================================== */


/*
** {======================================================
** Top level: loop, start, stop
** =======================================================
*/

/*
** Body of 'loop' (protected): spawns the body as a task and steps the
** environments until it terminates. Arguments: box, dbg, body, args...
** The task is stored in 'box.t'.
*/
static int loop_body (lua_State *L) {
  int top = lua_gettop(L);
  int tidx;
  lua_pushnil(L);  /* up */
  newtask(L, 3, 2, 1);  /* task(dbg, body) */
  spawn_aux(L, top + 1, 0, top + 2, 2, 4, top - 3, 1);  /* pushes t */
  tidx = lua_gettop(L);
  lua_pushvalue(L, tidx);
  lua_setfield(L, 1, "t");
  for (;;) {
    lua_Integer i, n;
    int quit = 0;
    if (taskstatus(L, tidx) == COS_DEAD)
      break;
    lua_getfield(L, LUA_REGISTRYINDEX, RK_ENVS);
    n = rawlen(L, -1);
    for (i = 1; i <= n; i++) {
      lua_rawgeti(L, -1, i);
      lua_getfield(L, -1, "step");
      lua_call(L, 0, 1);
      quit = lua_toboolean(L, -1);
      lua_pop(L, 2);
      if (quit) break;
    }
    lua_pop(L, 1);  /* envs */
    if (quit)
      break;  /* will call stop, will call gc */
    gc(L);
  }
  lua_getfield(L, tidx, "ret");
  return 1;
}


static int stop (lua_State *L);


/* loop(body, ...) */
static int atm_loop (lua_State *L) {
  int n = lua_gettop(L);
  int status, status2;
  argcheck(L, lua_isfunction(L, 1), 1, "invalid loop : expected body function");
  luaL_checkstack(L, 8, "loop");
  lua_pushcfunction(L, loop_body);
  lua_newtable(L);  /* box */
  pushdbg(L, 1);  /* dbg */
  lua_rotate(L, 1, 3);  /* loop_body, box, dbg, body, args... */
  lua_pushvalue(L, 2);  /* keep box */
  lua_insert(L, 1);  /* box, loop_body, box, dbg, body, args... */
  lua_pushvalue(L, 4);  /* keep dbg */
  lua_insert(L, 2);  /* box, dbg, loop_body, box, dbg, body, args... */
  status = lua_pcall(L, n + 2, 1, 0);
  /* stack: box, dbg, result-or-error */
  /* close the main task (as a '<close>' variable would) */
  if (lua_getfield(L, 1, "t") != LUA_TNIL) {
    lua_pushcfunction(L, handle_close);  /* reuse: closes uservalue */
    lua_newuserdatauv(L, 0, 1);
    lua_pushvalue(L, -3);
    lua_setiuservalue(L, -2, 1);
    status2 = lua_pcall(L, 1, 0, 0);
    if (status2 != LUA_OK) {  /* error while closing replaces */
      lua_replace(L, 3);
      status = status2;
    }
  }
  lua_settop(L, 3);
  lua_pushcfunction(L, stop);
  status2 = lua_pcall(L, 0, 0, 0);
  if (status2 != LUA_OK) {
    lua_replace(L, 3);
    status = status2;
  }
  lua_settop(L, 3);
  if (status != LUA_OK) {
    xcall_error(L, 2, "loop");
    return lua_error(L);
  }
  return 1;
}


/* start(body, ...) */
static int atm_start (lua_State *L) {
  int n = lua_gettop(L);
  int ok;
  argcheck(L, lua_isfunction(L, 1), 1, "invalid start : expected body function");
  lua_getfield(L, LUA_REGISTRYINDEX, RK_ENVS);
  ok = (rawlen(L, -1) == 1 &&
        (lua_rawgeti(L, -1, 1), lua_getfield(L, -1, "mode") == LUA_TNIL));
  argcheck(L, ok, 1, "invalid start : expected single-mode env only");
  lua_settop(L, n);
  lua_pushnil(L);  /* up */
  pushdbg(L, 1);  /* dbg */
  newtask(L, 1, n + 2, 1);  /* task */
  spawn_aux(L, n + 1, 0, n + 3, n + 2, 2, n - 1, 1);
  return 0;
}


static int stop (lua_State *L) {
  RunState *rs = getrs(L);
  lua_Integer i;
  pushroot(L);
  tasks_close(L, -1);
  gc(L);
  lua_newtable(L);
  setint(L, -2, "dns");  /* remove pools */
  lua_pop(L, 1);
  rs->clkmin = HUGE_VAL;
  rs->clklate = 0;
  lua_getfield(L, LUA_REGISTRYINDEX, RK_ENVS);
  for (i = rawlen(L, -1); i >= 1; i--) {
    lua_rawgeti(L, -1, i);
    if (lua_getfield(L, -1, "quit") != LUA_TNIL)
      lua_call(L, 0, 0);
    else
      lua_pop(L, 1);
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_rawseti(L, -2, i);
  }
  lua_pop(L, 1);
  return 0;
}


static int atm_stop (lua_State *L) {
  return stop(L);
}

/* }====================================================== */


/*
** {======================================================
** Helpers for generated code (the '_ATM' table)
** =======================================================
*/

/*
** Continuation of 'func' wrappers: catches 'atm-func' throws
** (returns) and returns their values.
*/
static int ctl_k (lua_State *L, int status, lua_KContext ctx) {
  const char *tag = cast(const char *, ctx);
  if (status == LUA_OK || status == LUA_YIELD)
    return lua_gettop(L);
  else {
    int err = lua_gettop(L);
    lua_Integer n, i;
    int ok;
    if (!isthrow(L, err))
      return lua_error(L);
    lua_rawgeti(L, err, 1);
    lua_pushstring(L, tag);
    ok = x_is(L, -2, -1);
    lua_pop(L, 2);
    if (!ok)
      return lua_error(L);
    n = rawlen(L, err);
    luaL_checkstack(L, cast_int(n), "return");
    for (i = 2; i <= n; i++)
      lua_rawgeti(L, err, i);
    return cast_int(n - 1);
  }
}


static const char ctl_func[] = "atm-func";
static const char ctl_loop[] = "atm-loop";
static const char ctl_do[] = "atm-do";


/* calls the wrapped function (upvalue 1) catching 'atm-func' */
static int func_call (lua_State *L) {
  int n = lua_gettop(L);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  return ctl_k(L, lua_pcallk(L, n, LUA_MULTRET, 0,
                             cast(lua_KContext, ctl_func), ctl_k),
               cast(lua_KContext, ctl_func));
}


/* RT.func(f): wraps a function so that 'return' can escape it */
static int rt_func (lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_settop(L, 1);
  lua_pushcclosure(L, func_call, 1);
  return 1;
}


/* RT.loop(blk): runs 'blk' catching 'atm-loop' (break) */
static int rt_loop (lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_settop(L, 1);
  return ctl_k(L, lua_pcallk(L, 0, LUA_MULTRET, 0,
                             cast(lua_KContext, ctl_loop), ctl_k),
               cast(lua_KContext, ctl_loop));
}


/* continuation of 'do': catches 'atm-do' throws with a matching tag */
static int do_k (lua_State *L, int status, lua_KContext ctx) {
  (void)ctx;
  if (status == LUA_OK || status == LUA_YIELD)
    return lua_gettop(L) - 1;  /* results above the tag */
  else {
    int err = lua_gettop(L);
    lua_Integer n, i;
    int ok;
    if (!isthrow(L, err))
      return lua_error(L);
    lua_rawgeti(L, err, 1);
    lua_pushstring(L, ctl_do);
    ok = x_is(L, -2, -1);
    lua_pop(L, 2);
    if (ok) {
      lua_rawgeti(L, err, 2);
      ok = x_is(L, -1, 1);  /* err[2] ?? tag */
      lua_pop(L, 1);
    }
    if (!ok)
      return lua_error(L);
    n = rawlen(L, err);
    luaL_checkstack(L, cast_int(n), "escape");
    if (n == 2) {  /* no values: the block evaluates to the tag */
      lua_rawgeti(L, err, 2);
      return 1;
    }
    for (i = 3; i <= n; i++)
      lua_rawgeti(L, err, i);
    return cast_int(n - 2);
  }
}


/* RT.do(tag, blk) */
static int rt_do (lua_State *L) {
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_settop(L, 2);
  return do_k(L, lua_pcallk(L, 0, LUA_MULTRET, 0, 0, do_k), 0);
}


/* throw(tag, ...) with the position of the caller */
static int throwtag (lua_State *L, const char *tag) {
  int n;
  lua_pushstring(L, tag);
  lua_insert(L, 1);
  n = lua_gettop(L);
  pushdbg(L, 1);
  tothrow(L, -1, 1, n);
  return lua_error(L);
}


static int rt_break (lua_State *L) {
  return throwtag(L, ctl_loop);
}


static int rt_return (lua_State *L) {
  return throwtag(L, ctl_func);
}


static int rt_escape (lua_State *L) {
  return throwtag(L, ctl_do);
}


/* RT.until(cnd, ...) : if cnd then break(... or cnd) */
static int rt_until (lua_State *L) {
  if (lua_toboolean(L, 1)) {
    if (lua_gettop(L) >= 2 && lua_toboolean(L, 2))
      lua_remove(L, 1);
    else
      lua_settop(L, 1);
    return throwtag(L, ctl_loop);
  }
  return 0;
}


/* RT.while(cnd, ...) : if not cnd then break(...) */
static int rt_while (lua_State *L) {
  if (!lua_toboolean(L, 1)) {
    lua_remove(L, 1);
    return throwtag(L, ctl_loop);
  }
  return 0;
}


/* t is the running task or one of its ancestors? */
static int is_up (lua_State *L, int tidx) {
  tidx = lua_absindex(L, tidx);
  pushme(L, 0);
  while (!lua_isnil(L, -1)) {
    if (lua_rawequal(L, -1, tidx)) {
      lua_pop(L, 1);
      return 1;
    }
    getint(L, -1, "up");
    lua_remove(L, -2);
  }
  lua_pop(L, 1);
  return 0;
}


/* RT.pin_chk_set(chk, pin, ...) */
static int rt_pin_chk_set (lua_State *L) {
  int n = lua_gettop(L);
  int chk = lua_toboolean(L, 1);
  int pin = lua_toboolean(L, 2);
  if (n >= 3 && (isxtask(L, 3) || istasks(L, 3))) {
    if (pin) {
      if (chk) {
        lua_getfield(L, 3, "pin");
        argcheck(L, !lua_toboolean(L, -1), 1,
                 "invalid assignment : unexpected pinned value");
        lua_pop(L, 1);
        /* pin is ownership + abort-on-close: taking an ancestor
           (or "me") would abort upward on termination */
        argcheck(L, !is_up(L, 3), 1,
                 "invalid assignment : unexpected parent task");
      }
      lua_pushboolean(L, 1);
      lua_setfield(L, 3, "pin");
    }
    else if (chk) {
      /* aliasing "me" or an ancestor is always valid:
         it necessarily outlives the current scope */
      lua_getfield(L, 3, "pin");
      argcheck(L, lua_toboolean(L, -1) || is_up(L, 3), 1,
               "invalid assignment : expected pinned value");
      lua_pop(L, 1);
    }
  }
  return n - 2;
}


/* RT.tag_do(tag, t) */
static int rt_tag_do (lua_State *L) {
  argcheck(L, lua_type(L, 2) == LUA_TTABLE, 1,
           "invalid tag operation : expected table");
  lua_settop(L, 2);
  lua_pushvalue(L, 1);
  lua_setfield(L, 2, "tag");
  return 1;
}


static int rt_void (lua_State *L) {
  (void)L;
  return 0;
}


/* RT.pub(): the enclosing task (holder of 'pub') */
static int rt_pub (lua_State *L) {
  pushme(L, 0);
  if (lua_isnil(L, -1)) {
    lua_pushliteral(L, "invalid pub : expected enclosing task");
    return lua_error(L);
  }
  return 1;
}

/* }====================================================== */


/*
** {======================================================
** Metatables and module
** =======================================================
*/

static int task_tostring (lua_State *L) {
  lua_pushfstring(L, "task: %p", lua_topointer(L, 1));
  return 1;
}


static int xtask_tostring (lua_State *L) {
  lua_pushfstring(L, "xtask: %p", lua_topointer(L, 1));
  return 1;
}


static int tasks_tostring (lua_State *L) {
  lua_pushfstring(L, "tasks: %p", lua_topointer(L, 1));
  return 1;
}


static int xtask_close_f (lua_State *L) {
  xtask_close(L, 1);
  return 0;
}


static int tasks_close_f (lua_State *L) {
  tasks_close(L, 1);
  return 0;
}


static int tasks_len (lua_State *L) {
  getint(L, 1, "dns");
  lua_pushinteger(L, rawlen(L, -1));
  return 1;
}


static int tasks_pairs (lua_State *L) {
  lua_getglobal(L, "ipairs");
  getint(L, 1, "dns");
  lua_call(L, 1, 3);
  return 3;
}


static const luaL_Reg atmlib[] = {
  {"loop", atm_loop},
  {"start", atm_start},
  {"stop", atm_stop},
  {"status", atm_status},
  {"env", atm_env},
  {"gc", atm_gc},
  {"next", atm_next},
  {"time", atm_time},
  {"tasks", atm_tasks},
  {"task", atm_task},
  {"xtask", atm_xtask},
  {"spawn", atm_spawn},
  {"spawn_in", atm_spawn_in},
  {"spawn_tra", atm_spawn_tra},
  {"do_spawn", atm_do_spawn},
  {"me", atm_me},
  {"abort", atm_abort},
  {"await", atm_await},
  {"emit", atm_emit},
  {"emit_in", atm_emit_in},
  {"toggle", atm_toggle},
  {"throw", atm_throw},
  {"catch", atm_catch},
  {"defer", atm_defer},
  {NULL, NULL}
};


/* functions for generated code, besides the shared ones above */
static const luaL_Reg rtlib[] = {
  {"func", rt_func},
  {"loop", rt_loop},
  {"do", rt_do},
  {"break", rt_break},
  {"until", rt_until},
  {"while", rt_while},
  {"return", rt_return},
  {"escape", rt_escape},
  {"pin_chk_set", rt_pin_chk_set},
  {"tag_do", rt_tag_do},
  {"void", rt_void},
  {"pub", rt_pub},
  {NULL, NULL}
};


static void newmeta (lua_State *L, const char *name, const luaL_Reg *l) {
  if (luaL_newmetatable(L, name))
    luaL_setfuncs(L, l, 0);
  lua_pop(L, 1);
}


/*
** Opens the runtime: fills table at the top of the stack with the core
** API, and returns the X and RT tables (pushed in this order).
*/
LUAMOD_API void luaA_openrun (lua_State *L) {
  static const luaL_Reg mt_task[] = {{"__tostring", task_tostring}, {NULL, NULL}};
  static const luaL_Reg mt_xtask[] = {{"__tostring", xtask_tostring},
    {"__close", xtask_close_f}, {NULL, NULL}};
  static const luaL_Reg mt_tasks[] = {{"__tostring", tasks_tostring},
    {"__close", tasks_close_f}, {"__len", tasks_len}, {"__pairs", tasks_pairs},
    {NULL, NULL}};
  static const luaL_Reg mt_defer[] = {{"__close", defer_close}, {NULL, NULL}};
  static const luaL_Reg mt_handle[] = {{"__close", handle_close}, {NULL, NULL}};
  static const luaL_Reg mt_throw[] = {{NULL, NULL}};
  int core = lua_gettop(L);
  const char *const rtshared[] = {"task", "xtask", "spawn", "spawn_in",
    "do_spawn", "emit", "emit_in", "tasks", "catch", "throw", "defer", NULL};
  int i;
  newmeta(L, MT_TASK, mt_task);
  newmeta(L, MT_XTASK, mt_xtask);
  newmeta(L, MT_TASKS, mt_tasks);
  newmeta(L, MT_DEFER, mt_defer);
  newmeta(L, MT_HANDLE, mt_handle);
  newmeta(L, MT_THROW, mt_throw);
  /* state */
  {
    RunState *rs = cast(RunState *, lua_newuserdatauv(L, sizeof(RunState), 0));
    rs->time = 1;
    rs->clkmin = HUGE_VAL;
    rs->clklate = 0;
    lua_setfield(L, LUA_REGISTRYINDEX, RK_STATE);
  }
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, RK_GC);
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, RK_ENVS);
  /* root pool */
  lua_createtable(L, 0, 1);
  newinternals(L, 0, 0);
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "pin");
  lua_newtable(L);  /* cache: weak keys */
  lua_createtable(L, 0, 1);
  lua_pushliteral(L, "k");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, "cache");
  lua_setfield(L, -2, "_");
  luaL_setmetatable(L, MT_TASKS);
  lua_setfield(L, LUA_REGISTRYINDEX, RK_TASKS);
  /* core functions */
  luaL_setfuncs(L, atmlib, 0);
  /* X */
  luaL_newlib(L, xlib);
  lua_pushvalue(L, -1);
  lua_setfield(L, core, "x");
  /* RT: registry table for generated code */
  if (lua_getfield(L, LUA_REGISTRYINDEX, RK_RT) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, RK_RT);
  }
  luaL_setfuncs(L, rtlib, 0);
  for (i = 0; rtshared[i] != NULL; i++) {
    lua_getfield(L, core, rtshared[i]);
    lua_setfield(L, -2, rtshared[i]);
  }
  {
    static const char *const xnames[] = {"iter", "is", "gte", "eq", "cat", "xin", NULL};
    for (i = 0; xnames[i] != NULL; i++) {
      lua_getfield(L, -2, xnames[i]);
      lua_setfield(L, -2, xnames[i]);
    }
  }
}

/* }====================================================== */
