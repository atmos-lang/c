/*
** Atmos library: the 'atmos' table, the 'X' helpers (X.tostring,
** X.print, X.copy), and runtime support called by compiled code
** ('loop.iter' for 'loop .. in' iteration).
** See Copyright Notice in lua.h
*/

#define latmlib_c
#define LUA_LIB

#include "lprefix.h"

#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"


/*
** {======================================================================
** X.tostring: tables as '[v1, v2, k=v]' with a ':Tag ' prefix for a
** string 'tag' field (which is not listed); keys sorted (numbers first,
** in order, then the other keys by their string form).
** =======================================================================
*/

/* pushes the string for the value at 'idx' */
static void tostr (lua_State *L, int idx, int depth);


static int sortkeys (lua_State *L) {  /* comparison for table.sort */
  int n1 = lua_isnumber(L, 1), n2 = lua_isnumber(L, 2);
  if (n1 && n2)
    lua_pushboolean(L, lua_tonumber(L, 1) < lua_tonumber(L, 2));
  else if (n1 != n2)
    lua_pushboolean(L, n1);  /* numbers first */
  else {
    luaL_tolstring(L, 1, NULL);
    luaL_tolstring(L, 2, NULL);
    lua_pushboolean(L, lua_compare(L, -2, -1, LUA_OPLT));
  }
  return 1;
}


/*
** Pushes the string for the table at 'idx'. The buffer is used with a
** balanced stack: the list of keys is created before it, and every
** value pushed while adding is popped by the add itself.
*/
static void tostrtable (lua_State *L, int idx, int depth) {
  luaL_Buffer b;
  int keys, n, i;
  lua_Integer seq = 1;  /* next expected sequence index */
  if (depth > 64)
    luaL_error(L, "X.tostring: table too deep");
  luaL_checkstack(L, 6, "X.tostring");
  lua_newtable(L);  /* list of keys */
  keys = lua_gettop(L);
  n = 0;
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    lua_pop(L, 1);  /* value */
    if (!(lua_type(L, -1) == LUA_TSTRING &&
          strcmp(lua_tostring(L, -1), "tag") == 0)) {
      lua_pushvalue(L, -1);
      lua_rawseti(L, keys, ++n);
    }
  }
  lua_getglobal(L, "table");
  lua_getfield(L, -1, "sort");
  lua_remove(L, -2);
  lua_pushvalue(L, keys);
  lua_pushcfunction(L, sortkeys);
  lua_call(L, 2, 0);
  luaL_buffinit(L, &b);
  if (lua_getfield(L, idx, "tag") == LUA_TSTRING) {  /* ':Tag ' prefix */
    lua_pop(L, 1);
    luaL_addchar(&b, ':');
    lua_getfield(L, idx, "tag");
    luaL_addvalue(&b);
    luaL_addchar(&b, ' ');
  }
  else
    lua_pop(L, 1);
  luaL_addchar(&b, '[');
  for (i = 1; i <= n; i++) {
    int isseq;
    if (i > 1) luaL_addstring(&b, ", ");
    lua_rawgeti(L, keys, i);  /* key */
    isseq = (lua_isinteger(L, -1) && lua_tointeger(L, -1) == seq);
    if (isseq) {
      seq++;  /* sequence item: no key */
      lua_pop(L, 1);
    }
    else {  /* 'key=' */
      luaL_tolstring(L, -1, NULL);
      lua_remove(L, -2);  /* keep only the string (balanced buffer) */
      luaL_addvalue(&b);
      luaL_addchar(&b, '=');
    }
    lua_rawgeti(L, keys, i);  /* key again */
    lua_gettable(L, idx);  /* value (replaces the key) */
    tostr(L, lua_gettop(L), depth + 1);  /* its string */
    lua_remove(L, -2);  /* remove the value: only the string on top */
    luaL_addvalue(&b);
  }
  luaL_addchar(&b, ']');
  luaL_pushresult(&b);
  lua_remove(L, keys);
}


static void tostr (lua_State *L, int idx, int depth) {
  idx = lua_absindex(L, idx);
  if (lua_type(L, idx) == LUA_TTABLE)
    tostrtable(L, idx, depth);
  else
    luaL_tolstring(L, idx, NULL);
}


static int X_tostring (lua_State *L) {
  luaL_checkany(L, 1);
  lua_settop(L, 1);
  tostr(L, 1, 0);
  return 1;
}


static int X_print (lua_State *L) {
  int n = lua_gettop(L);
  int i;
  luaL_checkstack(L, n + 2, "too many arguments");
  lua_getglobal(L, "print");
  for (i = 1; i <= n; i++) {
    lua_pushcfunction(L, X_tostring);
    lua_pushvalue(L, i);
    lua_call(L, 1, 1);
  }
  lua_call(L, n, 0);
  return 0;
}


static void copy (lua_State *L, int depth) {  /* copies the top value */
  if (lua_type(L, -1) != LUA_TTABLE)
    return;  /* value itself */
  if (depth > 64)
    luaL_error(L, "X.copy: table too deep");
  luaL_checkstack(L, 4, "X.copy");
  lua_newtable(L);  /* src, dst */
  lua_pushnil(L);
  while (lua_next(L, -3)) {  /* src, dst, k, v */
    copy(L, depth + 1);  /* src, dst, k, v' */
    lua_pushvalue(L, -2);  /* src, dst, k, v', k */
    lua_insert(L, -2);  /* src, dst, k, k, v' */
    lua_settable(L, -4);  /* dst[k] = v' */
  }
  lua_remove(L, -2);  /* remove source */
}


static int X_copy (lua_State *L) {
  luaL_checkany(L, 1);
  lua_settop(L, 1);
  copy(L, 0);
  return 1;
}

/* }====================================================================== */


/*
** {======================================================================
** 'loop k, v in e': iteration ('_ENV["loop.iter"]'), called with all
** the values of the loop expression. Two or more values are a Lua
** iterator (f, s, control[, closing]) and pass through. One value:
** a number n iterates 1..n; nil iterates 1..maxinteger; a table its
** array part (1..#t) and then its other fields; a function (or a
** value with '__call') is a generator, called until it returns nil; a
** table with '__pairs' uses it.
** =======================================================================
*/

static int numnext (lua_State *L) {  /* (limit, i) -> i + 1 */
  lua_Integer limit = lua_tointeger(L, 1);
  lua_Integer i = lua_tointeger(L, 2);
  if (i >= limit)
    return 0;
  lua_pushinteger(L, i + 1);
  return 1;
}


static int tabnext (lua_State *L) {  /* (t, k) -> next k, v */
  lua_Integer n = luaL_len(L, 1);
  lua_settop(L, 2);
  if (lua_isnil(L, 2)) {  /* first step */
    if (n >= 1) {
      lua_pushinteger(L, 1);
      lua_geti(L, 1, 1);
      return 2;
    }
  }
  else if (lua_isinteger(L, 2)) {
    lua_Integer k = lua_tointeger(L, 2);
    if (k >= 1 && k < n) {  /* inside the array part */
      lua_pushinteger(L, k + 1);
      lua_geti(L, 1, k + 1);
      return 2;
    }
    else if (k == n)  /* end of the array part: start the other keys */
      lua_pushnil(L);
    else
      lua_pushvalue(L, 2);
  }
  else
    lua_pushvalue(L, 2);
  for (;;) {  /* other fields, skipping the array part */
    if (!lua_next(L, 1))
      return 0;
    if (!(lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1 &&
          lua_tointeger(L, -2) <= n))
      return 2;
    lua_pop(L, 1);
  }
}


static int loop_iter (lua_State *L) {
  int n = lua_gettop(L);
  if (n >= 2)
    return n;  /* Lua iterator protocol */
  switch (lua_type(L, 1)) {
    case LUA_TNUMBER: case LUA_TNIL: {
      lua_Integer limit = lua_isnil(L, 1) ? LUA_MAXINTEGER
                                          : luaL_checkinteger(L, 1);
      lua_pushcfunction(L, numnext);
      lua_pushinteger(L, limit);
      lua_pushinteger(L, 0);
      return 3;
    }
    case LUA_TFUNCTION: {
      return 1;  /* generator: called with (nil, nil) */
    }
    case LUA_TTABLE: {
      if (luaL_getmetafield(L, 1, "__pairs") != LUA_TNIL) {
        lua_pushvalue(L, 1);
        lua_call(L, 1, 3);
        return 3;
      }
      if (luaL_getmetafield(L, 1, "__call") != LUA_TNIL) {
        lua_pop(L, 1);
        return 1;  /* generator */
      }
      lua_pushcfunction(L, tabnext);
      lua_pushvalue(L, 1);
      lua_pushnil(L);
      return 3;
    }
    default: {
      if (luaL_getmetafield(L, 1, "__call") != LUA_TNIL) {
        lua_pop(L, 1);
        return 1;
      }
      return luaL_error(L, "attempt to iterate a %s value", luaL_typename(L, 1));
    }
  }
}

/* }====================================================================== */


static const luaL_Reg X_funcs[] = {
  {"tostring", X_tostring},
  {"print", X_print},
  {"copy", X_copy},
  {NULL, NULL}
};





/*
** {======================================================================
** Tasks. See .claude/plans/tasks.md and the reference runtime
** (lua-atmos/atmos/atmos/run.lua), which this follows closely.
**
** - 'task' prototype: userdata with the body function as user value.
** - 'xtask' instance: userdata (XTask) whose user value is a table with
**   fields 'th' (its coroutine), 'up' (owner), 'dns' (children, in
**   spawn order), 'pub', 'ret', 'gate'.
** - 'tasks' pool: userdata (Pool) with user value table 'up', 'dns'.
** - the root is a hidden pool (registry ATM_ROOT); a weak table
**   (registry ATM_CACHE) maps coroutines to their xtasks.
** =======================================================================
*/

#define XTASK	"xtask"
#define POOL	"tasks"
#define PROTO	"task"

#define ATM_CACHE	"atmos.cache"
#define ATM_ROOT	"atmos.root"
#define ATM_EXIT	"atmos.exit"
#define ATM_PATT	"atmos.patt"
#define ATM_GC		"atmos.gc"

#define ST_OK		0
#define ST_ABORTED	1
#define ST_TOGGLED	2

typedef struct XTask {
  lua_State *th;  /* its coroutine (anchored in the user value table) */
  lua_Integer time;  /* time stamp of its current await */
  unsigned char tra;  /* transparent task */
  unsigned char pin;  /* owned by a block or a pool */
  unsigned char status;  /* ST_* */
} XTask;

typedef struct Pool {
  lua_Integer max;  /* maximum number of tasks (-1: unbounded) */
  unsigned char pin;
  unsigned char isroot;
} Pool;

/* broadcast time stamp and nesting of emits (for the sweep of dead tasks) */
static lua_Integer TIME = 1;
static int DEPTH = 0;

/* the thread of the task being run ('lua_resume' in progress), if any */
static lua_State *RUNNING = NULL;

LUALIB_API lua_State *atmos_running (void) {
  return RUNNING;
}

/* 'lua_resume' of a task's thread, tracking the running one */
static int resumetask (lua_State *co, lua_State *from, int nargs, int *nres) {
  lua_State *prev = RUNNING;
  int status;
  RUNNING = co;
  status = lua_resume(co, from, nargs, nres);
  RUNNING = prev;
  return status;
}


/*
** Tasks, pools and prototypes are tables (so that 'type' says "table",
** as in the reference) with a metatable that names them; the C struct
** is a userdata in field '_', whose user value table holds the
** internal references ('th', 'up', 'dns', 'gate'); 'pub' and 'ret'
** are plain fields of the table; a prototype has its function in '_'.
*/
static int hasmeta (lua_State *L, int idx, const char *name) {
  int res = 0;
  if (lua_type(L, idx) == LUA_TTABLE && lua_getmetatable(L, idx)) {
    luaL_getmetatable(L, name);
    res = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
  }
  return res;
}

#define isxtask(L,i)	hasmeta(L, i, XTASK)
#define ispool(L,i)	hasmeta(L, i, POOL)
#define isproto(L,i)	hasmeta(L, i, PROTO)

static void *structof (lua_State *L, int idx) {  /* the '_' userdata */
  void *p;
  lua_getfield(L, idx, "_");
  p = lua_touserdata(L, -1);
  lua_pop(L, 1);
  return p;
}

#define xtaskof(L,i)	((XTask *)structof(L, i))
#define poolof(L,i)	((Pool *)structof(L, i))
#define isrootpool(L,i)	(ispool(L, i) && poolof(L, i)->isroot)


/* user value table of a task/pool (in its '_' userdata) */
static void getuv (lua_State *L, int idx) {
  lua_getfield(L, idx, "_");
  lua_getiuservalue(L, -1, 1);
  lua_remove(L, -2);
}


/* new table with metatable 'name' and a '_' userdata of 'size' bytes
   whose user value table holds 'dns'; pushes the table, returns the
   userdata */
static void *newnamed (lua_State *L, const char *name, size_t size) {
  void *p;
  lua_newtable(L);
  luaL_setmetatable(L, name);
  p = lua_newuserdatauv(L, size, 1);
  lua_newtable(L);  /* user value table */
  lua_newtable(L);
  lua_setfield(L, -2, "dns");
  lua_setiuservalue(L, -2, 1);
  lua_setfield(L, -2, "_");
  return p;
}


static int uvget (lua_State *L, int idx, const char *k) {
  idx = lua_absindex(L, idx);
  getuv(L, idx);
  lua_getfield(L, -1, k);
  lua_remove(L, -2);
  return lua_type(L, -1);
}


static void uvset (lua_State *L, int idx, const char *k) {  /* value on top */
  idx = lua_absindex(L, idx);
  getuv(L, idx);
  lua_insert(L, -2);
  lua_setfield(L, -2, k);
  lua_pop(L, 1);
}


/* coroutine status of the xtask at 'idx' (as 'coroutine.status') */
#define CO_RUNNING	0
#define CO_DEAD		1
#define CO_SUSPENDED	2
#define CO_NORMAL	3

static const char *const costatname[] =
  {"running", "dead", "suspended", "normal"};

static int costatus (lua_State *L, int idx) {
  XTask *t = xtaskof(L, idx);
  lua_State *co = t->th;
  if (L == co) return CO_RUNNING;
  switch (lua_status(co)) {
    case LUA_YIELD: return CO_SUSPENDED;
    case LUA_OK: {
      lua_Debug ar;
      if (lua_getstack(co, 0, &ar)) return CO_NORMAL;
      else if (lua_gettop(co) == 0) return CO_DEAD;
      else return CO_SUSPENDED;  /* initial state */
    }
    default: return CO_DEAD;  /* some error occurred */
  }
}


/*
** Pushes the running xtask, or nil. With 'tra' false, transparent
** tasks are skipped (the enclosing non-transparent task is given);
** pools and the root are never a 'me'.
*/
static void pushme (lua_State *L, int tra) {
  lua_getfield(L, LUA_REGISTRYINDEX, ATM_CACHE);
  lua_pushthread(L);
  lua_rawget(L, -2);
  lua_remove(L, -2);
  for (;;) {
    if (lua_isnil(L, -1))
      return;
    else if (ispool(L, -1)) {
      if (isrootpool(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return;
      }
    }
    else if (tra || !(xtaskof(L, -1))->tra)
      return;
    uvget(L, -1, "up");  /* go up */
    lua_remove(L, -2);
  }
}


/* 'up' chain from the value at 'idx' (same rule as 'pushme'); replaces it */
static void toowner (lua_State *L, int tra) {
  for (;;) {
    if (lua_isnil(L, -1))
      return;
    else if (ispool(L, -1)) {
      if (isrootpool(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return;
      }
    }
    else if (tra || !(xtaskof(L, -1))->tra)
      return;
    uvget(L, -1, "up");
    lua_remove(L, -2);
  }
}


/* is the task at 'idx' the running task or one of its ancestors? */
static int isancestor (lua_State *L, int idx) {
  idx = lua_absindex(L, idx);
  pushme(L, 1);
  while (!lua_isnil(L, -1)) {
    if (lua_rawequal(L, -1, idx)) {
      lua_pop(L, 1);
      return 1;
    }
    uvget(L, -1, "up");
    lua_remove(L, -2);
  }
  lua_pop(L, 1);
  return 0;
}


/* mark the owner of 'idx' for the sweep of dead tasks */
static void markgc (lua_State *L, int idx) {
  if (uvget(L, idx, "up") != LUA_TNIL) {
    lua_getfield(L, LUA_REGISTRYINDEX, ATM_GC);
    lua_insert(L, -2);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
  }
  lua_pop(L, 1);
}


/* remove dead tasks from the children of the marked owners */
static void sweep (lua_State *L) {
  if (DEPTH > 0) return;  /* not while an emit traverses the tree */
  lua_getfield(L, LUA_REGISTRYINDEX, ATM_GC);
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    lua_Integer n, i, j = 0;
    lua_pop(L, 1);  /* boolean */
    uvget(L, -1, "dns");  /* gc, owner, dns */
    n = (lua_Integer)lua_rawlen(L, -1);
    for (i = 1; i <= n; i++) {
      lua_rawgeti(L, -1, i);
      if (isxtask(L, -1) && costatus(L, -1) == CO_DEAD)
        lua_pop(L, 1);  /* drop it */
      else
        lua_rawseti(L, -2, ++j);  /* keep it (compacting) */
    }
    for (i = j + 1; i <= n; i++) {
      lua_pushnil(L);
      lua_rawseti(L, -2, i);
    }
    lua_pop(L, 1);  /* dns */
    lua_pushvalue(L, -1);
    lua_pushnil(L);
    lua_rawset(L, -4);  /* gc[owner] = nil (safe: existing key) */
  }
  lua_pop(L, 1);
}


/* link task/pool at 'tidx' under 'upidx' */
static void link (lua_State *L, int upidx, int tidx) {
  upidx = lua_absindex(L, upidx);
  tidx = lua_absindex(L, tidx);
  lua_pushvalue(L, upidx);
  uvset(L, tidx, "up");
  uvget(L, upidx, "dns");
  lua_pushvalue(L, tidx);
  lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
  lua_pop(L, 1);
}


/*
** Creates an xtask for the function at 'fidx' and pushes it.
*/
static XTask *newxtask (lua_State *L, int fidx, int tra) {
  XTask *t;
  lua_State *th;
  fidx = lua_absindex(L, fidx);
  t = (XTask *)newnamed(L, XTASK, sizeof(XTask));  /* the xtask table */
  th = lua_newthread(L);
  lua_pushvalue(L, fidx);
  lua_xmove(L, th, 1);  /* body function into the thread */
  t->th = th;
  t->time = 0;
  t->tra = (unsigned char)tra;
  t->pin = 0;
  t->status = ST_OK;
  lua_pushvalue(L, -1);
  uvset(L, -3, "th");  /* anchor the thread */
  lua_getfield(L, LUA_REGISTRYINDEX, ATM_CACHE);  /* cache[th] = xtask */
  lua_pushvalue(L, -2);
  lua_pushvalue(L, -4);
  lua_rawset(L, -3);
  lua_pop(L, 2);  /* cache and thread */
  return t;
}


static void closeany (lua_State *L, int idx);

/*
** Aborts the task at 'idx' (its '__close'): children first (in order),
** then the toggle gate, then the coroutine; a running task is marked
** and closed when it regains control ('task_result').
*/
static void closextask (lua_State *L, int idx) {
  XTask *t;
  lua_Integer n, i;
  int st;
  idx = lua_absindex(L, idx);
  t = xtaskof(L, idx);
  uvget(L, idx, "dns");
  n = (lua_Integer)lua_rawlen(L, -1);
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    closeany(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  if (uvget(L, idx, "gate") != LUA_TNIL)
    closextask(L, -1);
  lua_pop(L, 1);
  st = costatus(L, idx);
  if (st == CO_SUSPENDED || st == CO_DEAD) {
    int status = lua_closethread(t->th, L);
    if (status != LUA_OK && st != CO_DEAD) {  /* error in a defer? */
      int nerr = lua_gettop(t->th);
      luaL_checkstack(L, nerr, "closing task");
      lua_xmove(t->th, L, nerr);
      lua_errorn(L, nerr);
    }
    lua_settop(t->th, 0);
  }
  else
    t->status = ST_ABORTED;  /* running: closed in 'task_result' */
  markgc(L, idx);
}


static void closepool (lua_State *L, int idx) {
  lua_Integer n, i;
  idx = lua_absindex(L, idx);
  uvget(L, idx, "dns");
  n = (lua_Integer)lua_rawlen(L, -1);
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    closeany(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  markgc(L, idx);
}


static void closeany (lua_State *L, int idx) {
  if (isxtask(L, idx)) closextask(L, idx);
  else if (ispool(L, idx)) closepool(L, idx);
}


static void emitto (lua_State *L, int tgtidx, int eidx);

/*
** Handles the result of resuming the task at 'tidx' ('status' and
** 'nres' from 'lua_resume'): an error aborts its subtree and is
** re-raised; a task aborted while running is closed now; a finished
** task stores its result, is emitted to its owner (so 'await t'
** awakes) and its subtree is closed.
*/
static void taskresult (lua_State *L, int tidx, int status, int nres) {
  XTask *t;
  tidx = lua_absindex(L, tidx);
  t = xtaskof(L, tidx);
  if (status != LUA_OK && status != LUA_YIELD) {  /* error? */
    luaL_checkstack(L, nres, "task error");
    lua_xmove(t->th, L, nres);  /* error values */
    closextask(L, tidx);  /* abort its subtree */
    lua_errorn(L, nres);  /* propagate */
  }
  else if (t->status == ST_ABORTED) {  /* aborted while running? */
    lua_settop(t->th, 0);
    closextask(L, tidx);
  }
  else if (status == LUA_OK) {  /* finished */
    if (nres >= 1) {
      lua_xmove(t->th, L, 1);  /* first result */
      lua_settop(t->th, 0);
    }
    else
      lua_pushnil(L);
    lua_setfield(L, tidx, "ret");
    uvget(L, tidx, "up");  /* owner... */
    toowner(L, 0);  /* ...as a non-transparent task (or nil) */
    if (lua_isnil(L, -1)) {  /* under the root: target is the running task */
      lua_pop(L, 1);
      pushme(L, 0);
      if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
      }
    }
    emitto(L, lua_gettop(L), tidx);  /* terminated task as the event */
    lua_pop(L, 1);
    closextask(L, tidx);
  }
  else  /* yielded: discard the yielded values (keep its frames) */
    lua_pop(t->th, nres);
}


/* 'yield_if_aborted': the running task was aborted (by itself or an
   ancestor): wait to be closed from outside */
static int neverresumed (lua_State *L, int status, lua_KContext ctx) {
  (void)status; (void)ctx;
  return luaL_error(L, "aborted task resumed");
}

static int isaborted (lua_State *L) {
  int res = 0;
  pushme(L, 1);
  if (isxtask(L, -1) && (xtaskof(L, -1))->status == ST_ABORTED)
    res = 1;
  lua_pop(L, 1);
  return res;
}

#define yieldifaborted(L,nret) \
  { if (isaborted(L)) return lua_yieldk(L, 0, 0, neverresumed); return (nret); }


/*
** Protected emit to one child: args (time, child, event).
*/
static void emittree (lua_State *L, lua_Integer time, int tidx, int eidx);

static int emitchild (lua_State *L) {
  emittree(L, lua_tointeger(L, 1), 2, 3);
  return 0;
}


/*
** Broadcast of the event at 'eidx' to the subtree of the task/pool at
** 'tidx': children first, in order, then the task itself if it is
** awaiting since before this emit. An error in a child is raised at
** the awaiting point of its owner ('lua_resumeerror'), or propagated.
*/
static void emittree (lua_State *L, lua_Integer time, int tidx, int eidx) {
  int isx;
  int base;
  int err = 0;
  lua_Integer n, i;
  tidx = lua_absindex(L, tidx);
  eidx = lua_absindex(L, eidx);
  isx = isxtask(L, tidx);
  if (isx && (xtaskof(L, tidx))->status == ST_TOGGLED)
    return;  /* toggled off (filters: stage 2) */
  base = lua_gettop(L);
  uvget(L, tidx, "dns");
  n = (lua_Integer)lua_rawlen(L, -1);
  DEPTH++;
  for (i = 1; i <= n && !err; i++) {
    int top = lua_gettop(L);
    lua_pushcfunction(L, emitchild);
    lua_pushinteger(L, time);
    lua_rawgeti(L, -4 + 1, i);  /* dns[i] (dns is below the function and time) */
    lua_pushvalue(L, eidx);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK)
      err = lua_gettop(L) - top;  /* number of error values (on the stack) */
  }
  DEPTH--;
  if (isx) {
    XTask *t = xtaskof(L, tidx);
    if (err) {
      if (costatus(L, tidx) == CO_SUSPENDED) {  /* raise at its await */
        int nres;
        int status;
        luaL_checkstack(t->th, err, "task error");
        lua_xmove(L, t->th, err);
        lua_remove(L, base + 1);  /* dns */
        status = lua_resumeerror(t->th, L, err, &nres);
        taskresult(L, tidx, status, nres);  /* re-raises if not handled */
        return;
      }
      else {
        lua_remove(L, base + 1);  /* dns */
        lua_errorn(L, err);
      }
    }
    else if (t->time < time && costatus(L, tidx) == CO_SUSPENDED) {
      int nres, status;
      lua_pushvalue(L, eidx);
      lua_xmove(L, t->th, 1);  /* the event */
      status = resumetask(t->th, L, 1, &nres);
      taskresult(L, tidx, status, nres);
    }
  }
  else if (err) {
    lua_remove(L, base + 1);
    lua_errorn(L, err);
  }
  lua_settop(L, base);
}


/* a new broadcast of the event at 'eidx' to the subtree of 'tgtidx' */
static void emitto (lua_State *L, int tgtidx, int eidx) {
  TIME++;
  emittree(L, TIME, tgtidx, eidx);
}


/*
** Spawns the task at 'tidx' (a prototype, an unstarted instance, or a
** raw function for a transparent task) under 'upidx' (0: the running
** task, or the root), with 'nargs' arguments on the top of the stack.
** Pushes the instance (or nil when the pool is full) and returns 1.
*/
static int spawnaux (lua_State *L, int upidx, int tra, int tidx, int nargs) {
  XTask *t;
  int status, nres;
  tidx = lua_absindex(L, tidx);
  if (upidx != 0) upidx = lua_absindex(L, upidx);
  if (lua_type(L, tidx) == LUA_TFUNCTION) {  /* raw function: transparent */
    if (!tra) luaL_error(L, "invalid spawn : expected task prototype");
    t = newxtask(L, tidx, 1);
  }
  else if (isproto(L, tidx)) {  /* prototype: never transparent */
    if (tra) luaL_error(L, "invalid spawn : transparent task prototype");
    lua_getfield(L, tidx, "_");  /* its function */
    t = newxtask(L, -1, 0);
    lua_remove(L, -2);
  }
  else if (isxtask(L, tidx)) {  /* unstarted instance */
    t = xtaskof(L, tidx);
    if (t->tra != tra)
      luaL_error(L, "invalid spawn : transparent modifier mismatch");
    if (uvget(L, tidx, "up") != LUA_TNIL)
      luaL_error(L, "invalid spawn : unexpected active task");
    lua_pop(L, 1);
    lua_pushvalue(L, tidx);
  }
  else
    return luaL_error(L, "invalid spawn : expected task prototype");
  /* stack: ..., args..., xtask */
  if (upidx != 0)
    lua_pushvalue(L, upidx);
  else {
    pushme(L, 1);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      lua_getfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
    }
  }
  /* stack: ..., args..., xtask, up */
  if (ispool(L, -1)) {
    Pool *p = poolof(L, -1);
    t->pin = 1;
    if (p->max >= 0) {  /* bounded pool: count live tasks */
      lua_Integer n, i, live = 0;
      uvget(L, -1, "dns");
      n = (lua_Integer)lua_rawlen(L, -1);
      for (i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        if (!(isxtask(L, -1) && costatus(L, -1) == CO_DEAD)) live++;
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
      if (live >= p->max) {  /* full */
        lua_pop(L, 2 + nargs);
        lua_pushnil(L);
        return 1;
      }
    }
  }
  link(L, -1, -2);
  lua_pop(L, 1);  /* up */
  /* stack: ..., args..., xtask */
  lua_insert(L, -(nargs + 1));  /* xtask below the arguments */
  luaL_checkstack(t->th, nargs, "spawn arguments");
  lua_xmove(L, t->th, nargs);
  status = resumetask(t->th, L, nargs, &nres);
  taskresult(L, -1, status, nres);
  yieldifaborted(L, 1);
}


static int atm_spawn (lua_State *L) {  /* spawn(T, ...) */
  luaL_checkany(L, 1);
  return spawnaux(L, 0, 0, 1, lua_gettop(L) - 1);
}


static int atm_spawn_in (lua_State *L) {  /* spawn_in(up, T, ...) */
  if (!(isxtask(L, 1) || ispool(L, 1)))
    luaL_argexpected(L, 0, 1, "task or tasks");
  luaL_checkany(L, 2);
  return spawnaux(L, 1, 0, 2, lua_gettop(L) - 2);
}


static int atm_do_spawn (lua_State *L) {  /* do_spawn(f, ...): transparent */
  luaL_checktype(L, 1, LUA_TFUNCTION);
  return spawnaux(L, 0, 1, 1, lua_gettop(L) - 1);
}


static int atm_task (lua_State *L) {  /* task(f): prototype */
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_settop(L, 1);
  lua_newtable(L);
  luaL_setmetatable(L, PROTO);
  lua_pushvalue(L, 1);
  lua_setfield(L, -2, "_");
  return 1;
}


static int atm_xtask (lua_State *L) {  /* xtask(): me; xtask(T): instance */
  if (lua_isnoneornil(L, 1)) {
    pushme(L, 0);
    return 1;
  }
  if (!isproto(L, 1))
    luaL_argexpected(L, 0, 1, "task prototype");
  lua_getfield(L, 1, "_");
  newxtask(L, -1, 0);
  return 1;
}


static int atm_tasks (lua_State *L) {  /* tasks([max]) */
  Pool *p;
  lua_Integer max = -1;
  if (!lua_isnoneornil(L, 1)) {
    if (!lua_isnumber(L, 1))
      luaL_error(L, "invalid tasks limit : expected number");
    max = luaL_checkinteger(L, 1);
  }
  lua_settop(L, 0);
  pushme(L, 1);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
  }
  p = (Pool *)newnamed(L, POOL, sizeof(Pool));
  p->max = max;
  p->pin = 0;
  p->isroot = 0;
  link(L, 1, 2);
  return 1;
}


static int atm_abort (lua_State *L) {  /* abort(t) */
  if (!(isxtask(L, 1) || ispool(L, 1)))
    luaL_error(L, "invalid abort : expected task");
  closeany(L, 1);
  yieldifaborted(L, 0);
}


/*
** The state is being closed ('os.exit(code, true)', 'lua_close'):
** close every task under 'idx', children first, the running ones
** included (their frames are never resumed), so that pending 'defer's
** and pins run, as for the to-be-closed variables of a main chunk.
*/
static void exitclose (lua_State *L, int idx) {
  lua_Integer n, i;
  idx = lua_absindex(L, idx);
  uvget(L, idx, "dns");
  n = (lua_Integer)lua_rawlen(L, -1);
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    exitclose(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  if (isxtask(L, idx)) {
    XTask *t = xtaskof(L, idx);
    if (uvget(L, idx, "gate") != LUA_TNIL)
      exitclose(L, -1);
    lua_pop(L, 1);
    if (costatus(L, idx) != CO_DEAD) {
      int status = lua_closethread(t->th, L);
      if (status != LUA_OK) {  /* error in a defer? */
        int nerr = lua_gettop(t->th);
        luaL_checkstack(L, nerr, "closing task");
        lua_xmove(t->th, L, nerr);
        lua_errorn(L, nerr);
      }
    }
  }
}

static int exit_gc (lua_State *L) {  /* finalizer of the exit sentinel */
  lua_getfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
  exitclose(L, -1);
  return 0;
}


static int atm_gc (lua_State *L) {  /* atmos.gc(): sweep dead tasks */
  sweep(L);
  return 0;
}


static int atm_status (lua_State *L) {  /* atmos.status(t) */
  if (!isxtask(L, 1))
    luaL_argexpected(L, 0, 1, "task");
  lua_pushstring(L, costatname[costatus(L, 1)]);
  return 1;
}


/*
** Target of an emit: nil/'task' (the running task), 'parent', 'global'
** (the root), a task or pool, or a number of levels up (transparent
** tasks are skipped). Pushes the target.
*/
static void pushtarget (lua_State *L, int idx) {
  lua_Integer n = 0;
  if (idx != 0) idx = lua_absindex(L, idx);
  if (idx != 0 && (isxtask(L, idx) || ispool(L, idx))) {
    lua_pushvalue(L, idx);
    return;
  }
  if (idx == 0 || lua_isnil(L, idx)) n = 0;
  else if (lua_type(L, idx) == LUA_TSTRING) {
    const char *s = lua_tostring(L, idx);
    if (strcmp(s, "task") == 0) n = 0;
    else if (strcmp(s, "parent") == 0) n = 1;
    else if (strcmp(s, "global") == 0) {
      lua_getfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
      return;
    }
    else luaL_error(L, "invalid emit : invalid target");
  }
  else if (lua_isinteger(L, idx)) n = lua_tointeger(L, idx);
  else luaL_error(L, "invalid emit : invalid target");
  pushme(L, 0);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
  }
  while (n > 0) {
    uvget(L, -1, "up");
    lua_remove(L, -2);
    if (lua_isnil(L, -1)) luaL_error(L, "invalid emit : invalid target");
    while (!isrootpool(L, -1) && isxtask(L, -1) &&
           (xtaskof(L, -1))->tra) {
      uvget(L, -1, "up");
      lua_remove(L, -2);
      if (lua_isnil(L, -1)) luaL_error(L, "invalid emit : invalid target");
    }
    n--;
  }
}


static int emitaux (lua_State *L, int tgtidx, int eidx) {
  pushtarget(L, tgtidx);
  emitto(L, lua_gettop(L), eidx);
  lua_pop(L, 1);
  yieldifaborted(L, 0);
}


static int atm_emit (lua_State *L) {  /* emit(e) */
  if (lua_gettop(L) != 1)
    luaL_error(L, "invalid emit : invalid event");
  return emitaux(L, 0, 1);
}


static int atm_emit_in (lua_State *L) {  /* emit_in(to, e) */
  if (lua_gettop(L) != 2)
    luaL_error(L, "invalid emit : invalid event");
  return emitaux(L, 1, 2);
}


/*
** {------------------------------------------------------------------
** await
** -------------------------------------------------------------------
*/

/* await patterns built by the compiler: tables with metatable ATM_PATT,
   field 'kind' ("any", "all", "until", "while") and [1] */
static int newpatt (lua_State *L, const char *kind) {
  lua_settop(L, 1);
  lua_newtable(L);
  lua_pushstring(L, kind);
  lua_setfield(L, -2, "kind");
  lua_pushvalue(L, 1);
  lua_rawseti(L, -2, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, ATM_PATT);
  lua_setmetatable(L, -2);
  return 1;
}

static int atm_await_any (lua_State *L) {
  if (!ispool(L, 1)) luaL_error(L, "invalid await : expected tasks pool");
  return newpatt(L, "any");
}

static int atm_await_all (lua_State *L) {
  if (!ispool(L, 1)) luaL_error(L, "invalid await : expected tasks pool");
  return newpatt(L, "all");
}

static int atm_await_until (lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  return newpatt(L, "until");
}

static int atm_await_while (lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  return newpatt(L, "while");
}


static const char *pattkind (lua_State *L, int idx) {
  const char *kind = NULL;
  if (lua_type(L, idx) == LUA_TTABLE && lua_getmetatable(L, idx)) {
    lua_getfield(L, LUA_REGISTRYINDEX, ATM_PATT);
    if (lua_rawequal(L, -1, -2)) {
      lua_getfield(L, idx, "kind");
      kind = lua_tostring(L, -1);
      lua_pop(L, 1);
    }
    lua_pop(L, 2);
  }
  return kind;
}


/* result of a predicate: 'res' (true: the event) */
static int predresult (lua_State *L, int residx, int eidx) {
  if (lua_type(L, residx) == LUA_TBOOLEAN && lua_toboolean(L, residx))
    lua_pushvalue(L, eidx);
  else
    lua_pushvalue(L, residx);
  return 1;
}


static int await_k (lua_State *L, int status, lua_KContext ctx);

/*
** Checks the pattern (index 1) against the event (index 3, or none
** when 'hasevent' is false, right after the await starts). Returns the
** number of results, or -1 to keep waiting.
*/
static int awaitmatch (lua_State *L, int hasevent) {
  const char *kind = pattkind(L, 1);
  lua_settop(L, hasevent ? 3 : 2);
  if (kind != NULL) {
    int arg;
    lua_rawgeti(L, 1, 1);  /* its argument */
    arg = lua_gettop(L);
    if (strcmp(kind, "any") == 0 || strcmp(kind, "all") == 0) {
      if (!hasevent || !isxtask(L, 3)) return -1;
      uvget(L, 3, "up");
      if (!lua_rawequal(L, -1, arg)) return -1;  /* not from this pool */
      lua_pop(L, 1);
      if (kind[1] == 'l') {  /* all: every task of the pool dead? */
        lua_Integer n, i;
        uvget(L, arg, "dns");
        n = (lua_Integer)lua_rawlen(L, -1);
        for (i = 1; i <= n; i++) {
          lua_rawgeti(L, -1, i);
          if (isxtask(L, -1) && costatus(L, -1) != CO_DEAD) return -1;
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
      lua_getfield(L, 3, "ret");
      lua_pushvalue(L, 3);
      lua_pushvalue(L, arg);
      return 3;  /* ret, task, pool */
    }
    else {  /* until/while: predicate on the event */
      int res;
      lua_pushvalue(L, arg);
      if (hasevent) lua_pushvalue(L, 3); else lua_pushnil(L);
      lua_call(L, 1, 1);
      res = lua_toboolean(L, -1);
      if (kind[0] == 'u') {  /* until */
        if (!res) return -1;
        return predresult(L, lua_gettop(L), 3);
      }
      else {  /* while */
        if (res) return -1;
        lua_pushvalue(L, 3);
        return 1;
      }
    }
  }
  if (isxtask(L, 1)) {  /* termination of a task */
    if (costatus(L, 1) != CO_DEAD) return -1;
    lua_getfield(L, 1, "ret");
    lua_pushvalue(L, 1);
    return 2;
  }
  if (!hasevent) return -1;
  switch (lua_type(L, 1)) {
    case LUA_TBOOLEAN: {
      if (!lua_toboolean(L, 1)) return -1;  /* false: never */
      lua_pushvalue(L, 3);  /* true: any event */
      return 1;
    }
    case LUA_TTABLE: {  /* pattern =>= event */
      if (!luaB_gte(L, 1, 3, 0)) return -1;
      lua_pushvalue(L, 3);
      return 1;
    }
    default: {  /* event ?? pattern */
      if (!luaB_tagmatch(L, 3, 1)) return -1;
      lua_pushvalue(L, 3);
      return 1;
    }
  }
}


static int await_k (lua_State *L, int status, lua_KContext ctx) {
  int n;
  (void)status; (void)ctx;
  /* stack: pattern, me, event */
  n = awaitmatch(L, 1);
  if (n >= 0) return n;
  lua_settop(L, 2);
  return lua_yieldk(L, 0, 0, await_k);
}


static int atm_await (lua_State *L) {  /* await(p) */
  XTask *t;
  int n;
  luaL_checkany(L, 1);
  if (lua_gettop(L) != 1) luaL_error(L, "invalid await : invalid event pattern");
  if (ispool(L, 1)) luaL_error(L, "invalid await : unexpected tasks pool");
  if (lua_type(L, 1) == LUA_TFUNCTION)
    luaL_error(L, "invalid await : unexpected function");
  pushme(L, 1);
  if (!isxtask(L, 2)) luaL_error(L, "invalid await : expected enclosing task");
  t = xtaskof(L, 2);
  t->time = TIME;  /* reacts only to broadcasts after this point */
  n = awaitmatch(L, 0);
  if (n >= 0) return n;
  lua_settop(L, 2);
  return lua_yieldk(L, 0, 0, await_k);
}

/* }------------------------------------------------------------------ */


/*
** '__pinchk' metamethod of tasks and pools, called by OP_PINCHK on the
** value of a declaration or assignment (the reference's
** 'atm_pin_chk_set'): mode 1 ('pin x = v') marks a fresh task/pool as
** owned; mode 0 ('val'/'var'/'set') requires an owned task or an
** ancestor; mode 2 (statement 'spawn', hidden pin) marks it as owned.
*/
static int pinerror (lua_State *L, const char *msg) {
  luaL_where(L, 1);  /* position of the assignment */
  lua_pushstring(L, msg);
  lua_concat(L, 2);
  return lua_error(L);
}

static int pinchk_tm (lua_State *L) {
  unsigned char *pin = isxtask(L, 1) ? &(xtaskof(L, 1))->pin
                                     : &(poolof(L, 1))->pin;
  switch (lua_tointeger(L, 2)) {
    case 0: {  /* check */
      if (!*pin && !isancestor(L, 1))
        pinerror(L, "invalid assignment : expected pinned value");
      break;
    }
    case 1: {  /* set */
      if (*pin) pinerror(L, "invalid assignment : unexpected pinned value");
      if (isancestor(L, 1))
        pinerror(L, "invalid assignment : unexpected parent task");
      *pin = 1;
      break;
    }
    default: *pin = 1;  /* own */
  }
  return 0;
}


/*
** atmos.loop(body, ...): runs 'body' as a task under the root. With no
** environments, nothing can awake a suspended body, so the loop ends
** when the body returns control; its subtree is then closed.
*/
static int atm_loop (lua_State *L) {
  int nargs = lua_gettop(L) - 1;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_pushcfunction(L, atm_task);  /* body as a prototype */
  lua_pushvalue(L, 1);
  lua_call(L, 1, 1);
  lua_replace(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
  lua_insert(L, 1);  /* root, body, args... */
  spawnaux(L, 1, 0, 2, nargs);  /* the body is the running task at the top */
  /* stack: root, body, xtask */
  closextask(L, -1);
  sweep(L);
  lua_getfield(L, -1, "ret");
  return 1;
}


/* metamethods */

static int xtask_close (lua_State *L) {
  closextask(L, 1);
  return 0;
}

static int pool_close (lua_State *L) {
  closepool(L, 1);
  return 0;
}

static int pool_len (lua_State *L) {
  uvget(L, 1, "dns");
  lua_pushinteger(L, (lua_Integer)lua_rawlen(L, -1));
  return 1;
}

static int poolnext (lua_State *L) {  /* (dns, i) -> i+1, dns[i+1] */
  lua_Integer i = luaL_checkinteger(L, 2) + 1;
  lua_pushinteger(L, i);
  return (lua_rawgeti(L, 1, i) == LUA_TNIL) ? 1 : 2;
}

static int pool_pairs (lua_State *L) {
  lua_pushcfunction(L, poolnext);
  uvget(L, 1, "dns");
  lua_pushinteger(L, 0);
  return 3;
}

static int tostr_named (lua_State *L) {
  luaL_getmetafield(L, 1, "__name");
  lua_pushfstring(L, "%s: %p", lua_tostring(L, -1), lua_topointer(L, 1));
  return 1;
}


static const luaL_Reg xtask_meta[] = {
  {"__close", xtask_close}, {"__tostring", tostr_named},
  {"__pinchk", pinchk_tm}, {NULL, NULL}
};
static const luaL_Reg pool_meta[] = {
  {"__close", pool_close}, {"__len", pool_len}, {"__pairs", pool_pairs},
  {"__tostring", tostr_named}, {"__pinchk", pinchk_tm}, {NULL, NULL}
};
static const luaL_Reg proto_meta[] = {
  {"__tostring", tostr_named}, {NULL, NULL}
};

static const luaL_Reg task_funcs[] = {
  {"task", atm_task}, {"xtask", atm_xtask}, {"tasks", atm_tasks},
  {"spawn", atm_spawn}, {"spawn_in", atm_spawn_in}, {"do_spawn", atm_do_spawn},
  {"await", atm_await}, {"emit", atm_emit}, {"emit_in", atm_emit_in},
  {"abort", atm_abort},
  {"await.any", atm_await_any}, {"await.all", atm_await_all},
  {"await.until", atm_await_until}, {"await.while", atm_await_while},
  {NULL, NULL}
};


static void opentasks (lua_State *L) {  /* 'atmos' table on top */
  Pool *root;
  const luaL_Reg *f;
  luaL_newmetatable(L, XTASK); luaL_setfuncs(L, xtask_meta, 0); lua_pop(L, 1);
  luaL_newmetatable(L, POOL); luaL_setfuncs(L, pool_meta, 0); lua_pop(L, 1);
  luaL_newmetatable(L, PROTO); luaL_setfuncs(L, proto_meta, 0); lua_pop(L, 1);
  lua_newtable(L);  /* await patterns metatable */
  lua_setfield(L, LUA_REGISTRYINDEX, ATM_PATT);
  lua_newtable(L);  /* cache: weak keys */
  lua_newtable(L);
  lua_pushliteral(L, "k");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);
  lua_setfield(L, LUA_REGISTRYINDEX, ATM_CACHE);
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, ATM_GC);
  root = (Pool *)newnamed(L, POOL, sizeof(Pool));
  root->max = -1; root->pin = 1; root->isroot = 1;
  lua_setfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
  lua_newuserdatauv(L, 0, 0);  /* exit sentinel: finalized at 'lua_close' */
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, exit_gc);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_setfield(L, LUA_REGISTRYINDEX, ATM_EXIT);
  for (f = task_funcs; f->name != NULL; f++) {
    lua_pushcfunction(L, f->func);
    lua_setglobal(L, f->name);
  }
  lua_pushcfunction(L, atm_loop);
  lua_setfield(L, -2, "loop");
  lua_pushcfunction(L, atm_status);
  lua_setfield(L, -2, "status");
  lua_pushcfunction(L, atm_gc);
  lua_setfield(L, -2, "gc");
}

/* }====================================================================== */


/*
** The library is the 'atmos' table (runtime configuration and status);
** it also sets the global 'X' (helpers; not registered as a module, so
** 'require "X"' still loads a file) and the runtime support used by
** compiled code.
*/
LUAMOD_API int luaopen_atmos (lua_State *L) {
  lua_newtable(L);  /* 'atmos' */
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "test");
  luaL_newlib(L, X_funcs);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "X");  /* also 'atmos.X' */
  lua_setglobal(L, "X");
  lua_pushcfunction(L, loop_iter);
  lua_setglobal(L, "loop.iter");
  opentasks(L);
  return 1;
}
