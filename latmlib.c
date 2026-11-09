/*
** Atmos library: the 'atmos' table, the 'X' helpers (X.tostring,
** X.print, X.copy), and runtime support called by compiled code
** ('loop.iter' for 'loop .. in' iteration).
** See Copyright Notice in lua.h
*/

#define latmlib_c
#define LUA_LIB

#include "lprefix.h"

#include <math.h>
#include <time.h>
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
#define ATM_ENVS	"atmos.envs"
#define LOOPCLOSE	"atmos.loopclose"

#define ST_OK		0
#define ST_ABORTED	1
#define ST_TOGGLED	2

/* kinds of nodes */
#define NK_XTASK	1
#define NK_POOL		2

/*
** What a suspended task is awaiting, as far as a broadcast can decide
** without resuming it (see coroutine.md, "Wasted resumes"): 'akind' is
** the filter of the current suspension (AW_RESUME: resume on every
** broadcast, the default for any yield other than 'await'); 'pkind'
** is the kind of the pattern of the 'await' in progress; 'wkind' is
** what the broadcast found when it decided the match by itself (the
** await returns without checking again), or AW_RESUME.
*/
#define AW_RESUME	0  /* undecided: resume (user code decides) */
#define AW_NEVER	1  /* internal waits that never proceed by an event */
#define AW_FALSE	2  /* await(false) */
#define AW_TRUE		3  /* await(true): any event */
#define AW_TAG		4  /* a tag or plain value: 'event ?? pattern' */
#define AW_TABLE	5  /* a table pattern: only tables can match */
#define AW_TASK		6  /* termination of a task */
#define AW_POOL		7  /* :any / :all of a pool: task events only */
#define AW_CLOCK	8  /* a timer: countdown in 'clk' */
#define AW_CLOCKANY	9  /* await(:clock): any tick */

/*
** Tasks and pools share one struct (the '_' userdata); its user values
** hold the references (UV_*).
*/
typedef struct Node {
  lua_State *th;  /* its coroutine (anchored in UV_TH) */
  struct Node *atask;  /* AW_TASK: the awaited task (anchored in UV_PATT) */
  lua_Integer time;  /* time stamp of its current await */
  lua_Integer pass;  /* time of the last emit let through by its gate */
  lua_Integer max;  /* pool: maximum number of tasks (-1: unbounded) */
  lua_Number clk;  /* AW_CLOCK: time left */
  unsigned char kind;  /* NK_* */
  unsigned char tra;  /* transparent task */
  unsigned char pin;  /* owned by a block or a pool */
  unsigned char status;  /* ST_* */
  unsigned char isloop;  /* body of an 'atmos.loop' */
  unsigned char isroot;  /* the root pool */
  unsigned char pkind, akind, wkind;  /* AW_* */
} Node;

typedef Node XTask;
typedef Node Pool;

#define UV_TH		1  /* the coroutine */
#define UV_UP		2  /* the owner (table) */
#define UV_DNS		3  /* the children (sequence of tables) */
#define UV_GATE		4  /* the gate task of a toggled-off node */
#define UV_PATT		5  /* AW_TAG/AW_TASK: the awaited pattern */
#define NUV		5

static int LOOPFLAG = 0;  /* the next task created is a loop body */

/* the clock: nearest reported timer and the remainder of the current
   tick after the piece being broadcast (see tasks.md, stage 2b) */
static lua_Number CLKMIN = HUGE_VAL;
static lua_Number CLKLATE = 0;

/* pushes 'x' as an integer when it has an integral value */
static void pushnum (lua_State *L, lua_Number x) {
  if (x == floor(x) && x >= -9.2e18 && x <= 9.2e18)
    lua_pushinteger(L, (lua_Integer)x);
  else
    lua_pushnumber(L, x);
}

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
** is a userdata in field '_', whose user values hold the internal
** references (UV_*); 'pub' and 'ret' are plain fields of the table; a
** prototype has its function in '_'.
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


/* pushes the '_' userdata of the task/pool at 'idx'; returns its struct */
static Node *pushnode (lua_State *L, int idx) {
  lua_getfield(L, idx, "_");
  return (Node *)lua_touserdata(L, -1);
}


/* new table with metatable 'name' and a zeroed '_' node of kind 'kind'
   with an empty 'dns'; pushes the table, returns the node */
static Node *newnamed (lua_State *L, const char *name, int kind) {
  Node *p;
  lua_newtable(L);
  luaL_setmetatable(L, name);
  p = (Node *)lua_newuserdatauv(L, sizeof(Node), NUV);
  memset(p, 0, sizeof(Node));
  p->kind = (unsigned char)kind;
  lua_newtable(L);
  lua_setiuservalue(L, -2, UV_DNS);
  lua_setfield(L, -2, "_");
  return p;
}


/* pushes the user value 'k' (UV_*) of the task/pool at 'idx' */
static int uvget (lua_State *L, int idx, int k) {
  int t;
  idx = lua_absindex(L, idx);
  lua_getfield(L, idx, "_");
  t = lua_getiuservalue(L, -1, k);
  lua_remove(L, -2);
  return t;
}


static void uvset (lua_State *L, int idx, int k) {  /* value on top */
  idx = lua_absindex(L, idx);
  lua_getfield(L, idx, "_");
  lua_insert(L, -2);
  lua_setiuservalue(L, -2, k);
  lua_pop(L, 1);
}


/* coroutine status of the xtask at 'idx' (as 'coroutine.status') */
#define CO_RUNNING	0
#define CO_DEAD		1
#define CO_SUSPENDED	2
#define CO_NORMAL	3

static const char *const costatname[] =
  {"running", "dead", "suspended", "normal"};

static int nodestatus (lua_State *L, Node *t) {
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

#define costatus(L,idx)	nodestatus(L, xtaskof(L, idx))


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
  if (tra) return;  /* the thread's own xtask (or nil) */
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
    uvget(L, -1, UV_UP);  /* go up */
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
    uvget(L, -1, UV_UP);
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
    uvget(L, -1, UV_UP);
    lua_remove(L, -2);
  }
  lua_pop(L, 1);
  return 0;
}


/* mark the owner of 'idx' for the sweep of dead tasks */
static void markgc (lua_State *L, int idx) {
  if (uvget(L, idx, UV_UP) != LUA_TNIL) {
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
    uvget(L, -1, UV_DNS);  /* gc, owner, dns */
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
  uvset(L, tidx, UV_UP);
  uvget(L, upidx, UV_DNS);
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
  t = newnamed(L, XTASK, NK_XTASK);  /* the xtask table */
  th = lua_newthread(L);
  lua_pushvalue(L, fidx);
  lua_xmove(L, th, 1);  /* body function into the thread */
  t->th = th;
  t->isloop = (unsigned char)LOOPFLAG;
  LOOPFLAG = 0;
  t->tra = (unsigned char)tra;
  t->status = ST_OK;
  t->pkind = t->akind = t->wkind = AW_RESUME;
  lua_pushvalue(L, -1);
  uvset(L, -3, UV_TH);  /* anchor the thread */
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
  uvget(L, idx, UV_DNS);
  n = (lua_Integer)lua_rawlen(L, -1);
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    closeany(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  if (uvget(L, idx, UV_GATE) != LUA_TNIL)
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
  uvget(L, idx, UV_DNS);
  n = (lua_Integer)lua_rawlen(L, -1);
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    closeany(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  if (uvget(L, idx, UV_GATE) != LUA_TNIL)
    closextask(L, -1);
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
    lua_errorstatus(L, status, nres);  /* propagate (jumps keep their status) */
  }
  else if (t->status == ST_ABORTED) {  /* aborted while running? */
    lua_settop(t->th, 0);
    closextask(L, tidx);
  }
  else if (status == LUA_OK) {  /* finished */
    if (nres >= 1) {
      lua_xmove(t->th, L, nres);  /* the results */
      lua_pop(L, nres - 1);  /* keep the first */
    }
    else
      lua_pushnil(L);
    lua_setfield(L, tidx, "ret");
    uvget(L, tidx, UV_UP);  /* owner... */
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
** A broadcast in progress: its time stamp and what the matches need to
** know about the event (computed once).
*/
typedef struct Emit {
  lua_Integer time;
  int eidx;  /* the event (absolute index) */
  int evtype;  /* its type */
  int evatmos;  /* it has '__atmos': it decides for every await */
  int evtask;  /* it is a task (a termination) */
  lua_Number evnum;  /* a tick: its value */
} Emit;


/*
** Does the suspended task 't' (its node userdata at 'ud') proceed with
** the event? Decides in C the awaits that need no user code (see
** 'classify'); sets 'wkind' when the await may return without checking
** again. Timers count down here (and report the nearest one).
*/
static int wakes (lua_State *L, Node *t, int ud, Emit *em) {
  switch (t->akind) {
    case AW_RESUME: return 1;
    case AW_NEVER: return 0;
    default: break;
  }
  if (em->evatmos) return 1;  /* the event decides (in the task) */
  switch (t->akind) {
    case AW_FALSE: return 0;
    case AW_TRUE: break;
    case AW_TAG: {
      int m;
      lua_getiuservalue(L, ud, UV_PATT);
      m = luaB_tagmatch(L, em->eidx, lua_gettop(L));
      lua_pop(L, 1);
      if (!m) return 0;
      break;
    }
    case AW_TABLE: return (em->evtype == LUA_TTABLE);
    case AW_TASK: return (nodestatus(L, t->atask) == CO_DEAD);
    case AW_POOL: return em->evtask;
    case AW_CLOCK: {
      if (em->evtype != LUA_TNUMBER) return 0;
      t->clk -= em->evnum;
      if (t->clk > 0) {  /* not yet: report it */
        if (t->clk < CLKMIN) CLKMIN = t->clk;
        return 0;
      }
      break;
    }
    case AW_CLOCKANY: {
      if (em->evtype != LUA_TNUMBER) return 0;
      break;
    }
  }
  t->wkind = t->akind;  /* decided: the await returns at once */
  return 1;
}


/*
** Resumes a task (protected by the caller): (task, node, mode, values...);
** mode LUA_OK resumes it with the values (the event), otherwise raises
** the values at its await with that status. Then 'taskresult'.
*/
static int wakef (lua_State *L) {
  Node *t = (Node *)lua_touserdata(L, 2);  /* (the node of the task at 1) */
  int mode = (int)lua_tointeger(L, 3);
  int n = lua_gettop(L) - 3;  /* values */
  int status, nres;
  t->akind = AW_RESUME;  /* running: no filter */
  if (mode != LUA_OK) t->wkind = AW_RESUME;
  luaL_checkstack(t->th, n, "task wake");
  lua_xmove(L, t->th, n);
  if (mode == LUA_OK)
    status = resumetask(t->th, L, n, &nres);
  else
    status = lua_resumethrow(t->th, L, mode, n, &nres);
  if (status == LUA_YIELD && t->status != ST_ABORTED)
    lua_pop(t->th, nres);  /* suspended again (as in 'taskresult') */
  else
    taskresult(L, 1, status, nres);
  return 0;
}


/* removes the values between 'base' and the 'n' values on the top */
static void keeptop (lua_State *L, int base, int n) {
  int k = lua_gettop(L) - n - base;  /* values to remove */
  if (k > 0) {
    lua_rotate(L, base + 1, -k);
    lua_pop(L, k);
  }
}


/*
** Broadcast of the event to the subtree of the task/pool at 'tidx':
** children first, in order, then the task itself if it is awaiting
** since before this emit and the event matches. Returns LUA_OK, or an
** error status with '*nerr' error values on the top (in place of
** nothing): an error in a child is raised at the awaiting point of its
** owner ('lua_resumethrow'), or returned to the caller.
*/
static int emittree (lua_State *L, Emit *em, int tidx, int *nerr) {
  Node *t;
  int base, ud, dns;
  int st = LUA_OK;
  lua_Integer n, i;
  tidx = lua_absindex(L, tidx);
  luaL_checkstack(L, 8, "broadcast");  /* node, dns, child, match, wake */
  base = lua_gettop(L);
  t = pushnode(L, tidx);
  ud = base + 1;
  if (t->status == ST_TOGGLED) {  /* toggled off: only what its gate lets through */
    int pass = 0;
    if (lua_getiuservalue(L, ud, UV_GATE) != LUA_TNIL) {
      t->pass = 0;
      st = emittree(L, em, -1, nerr);  /* the gate reacts first... */
      if (st != LUA_OK) {
        keeptop(L, base, *nerr);
        return st;
      }
      pass = (t->pass != 0);  /* ...and stamps if the filter matched */
    }
    if (!pass) {
      lua_settop(L, base);
      return LUA_OK;
    }
    lua_settop(L, ud);
  }
  lua_getiuservalue(L, ud, UV_DNS);
  dns = base + 2;
  n = (lua_Integer)lua_rawlen(L, dns);
  DEPTH++;
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, dns, i);
    st = emittree(L, em, -1, nerr);
    if (st != LUA_OK) break;  /* error values above the child */
    lua_pop(L, 1);
  }
  DEPTH--;
  if (st != LUA_OK) {
    if (t->kind == NK_XTASK && nodestatus(L, t) == CO_SUSPENDED) {
      int k = *nerr;  /* raise at its await */
      int top;
      keeptop(L, base, k);
      lua_pushcfunction(L, wakef);
      lua_pushvalue(L, tidx);
      lua_pushlightuserdata(L, t);
      lua_pushinteger(L, st);
      lua_rotate(L, base + 1, 4);  /* below the error values */
      st = lua_pcall(L, k + 3, 0, 0);  /* re-raises if not handled */
      top = lua_gettop(L);
      *nerr = top - base;
      return st;
    }
    keeptop(L, base, *nerr);  /* propagate */
    return st;
  }
  if (t->kind == NK_XTASK && t->time < em->time &&
      nodestatus(L, t) == CO_SUSPENDED && wakes(L, t, ud, em)) {
    lua_settop(L, base);
    lua_pushcfunction(L, wakef);
    lua_pushvalue(L, tidx);
    lua_pushlightuserdata(L, t);
    lua_pushinteger(L, LUA_OK);
    lua_pushvalue(L, em->eidx);
    st = lua_pcall(L, 4, 0, 0);
    *nerr = lua_gettop(L) - base;
    return st;
  }
  lua_settop(L, base);
  return LUA_OK;
}


/* a new broadcast of the event at 'eidx' to the subtree of 'tgtidx' */
static void emitto (lua_State *L, int tgtidx, int eidx) {
  Emit em;
  int st, nerr = 0;
  tgtidx = lua_absindex(L, tgtidx);
  em.eidx = lua_absindex(L, eidx);
  em.time = ++TIME;
  em.evtype = lua_type(L, em.eidx);
  em.evatmos = (luaL_getmetafield(L, em.eidx, "__atmos") != LUA_TNIL);
  if (em.evatmos) lua_pop(L, 1);
  em.evtask = isxtask(L, em.eidx);
  em.evnum = (em.evtype == LUA_TNUMBER) ? lua_tonumber(L, em.eidx) : 0;
  st = emittree(L, &em, tgtidx, &nerr);
  if (st != LUA_OK)
    lua_errorstatus(L, st, nerr);
}


/*
** Spawns the task at 'tidx' (a prototype, an unstarted instance, or a
** raw function for a transparent task) under 'upidx' (0: the running
** task, or the root), with 'nargs' arguments on the top of the stack.
** Pushes the instance (or nil when the pool is full) and returns 1.
*/
static int spawnaux (lua_State *L, int upidx, int tra, int tidx, int nargs);

static void spawncore (lua_State *L, int upidx, int tra, int tidx, int nargs) {
  XTask *t = NULL;
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
    if (uvget(L, tidx, UV_UP) != LUA_TNIL)
      luaL_error(L, "invalid spawn : unexpected active task");
    lua_pop(L, 1);
    lua_pushvalue(L, tidx);
  }
  else
    luaL_error(L, "invalid spawn : expected task prototype");
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
      uvget(L, -1, UV_DNS);
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
        return;
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
}

static int spawnaux (lua_State *L, int upidx, int tra, int tidx, int nargs) {
  spawncore(L, upidx, tra, tidx, nargs);
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
  p = newnamed(L, POOL, NK_POOL);
  p->max = max;
  p->status = ST_OK;
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
  uvget(L, idx, UV_DNS);
  n = (lua_Integer)lua_rawlen(L, -1);
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    exitclose(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  if (isxtask(L, idx)) {
    XTask *t = xtaskof(L, idx);
    if (uvget(L, idx, UV_GATE) != LUA_TNIL)
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

static int atm_next (lua_State *L) {  /* atmos.next(): nearest timer */
  if (CLKMIN == HUGE_VAL) lua_pushnil(L);
  else pushnum(L, CLKMIN);
  return 1;
}

/*
** atmos.stop(): aborts the program, that is, the body of the innermost
** 'atmos.loop' (the ancestor of the running task directly under the
** root) and resets the clock. (The reference closes the whole root
** pool; here nested loops, as the test harness, must survive.)
*/
static void quitenvs (lua_State *L);

static int atm_stop (lua_State *L) {
  lua_settop(L, 0);
  pushme(L, 1);
  while (isxtask(L, -1) && !(xtaskof(L, -1))->isloop) {  /* up to the body */
    uvget(L, -1, UV_UP);
    lua_remove(L, -2);
  }
  if (isxtask(L, -1))
    closextask(L, -1);
  else {  /* not inside a loop: everything */
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
    closepool(L, -1);
  }
  lua_pop(L, 1);
  quitenvs(L);
  sweep(L);
  CLKMIN = HUGE_VAL;
  CLKLATE = 0;
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
    uvget(L, -1, UV_UP);
    lua_remove(L, -2);
    if (lua_isnil(L, -1)) luaL_error(L, "invalid emit : invalid target");
    while (!isrootpool(L, -1) && isxtask(L, -1) &&
           (xtaskof(L, -1))->tra) {
      uvget(L, -1, UV_UP);
      lua_remove(L, -2);
      if (lua_isnil(L, -1)) luaL_error(L, "invalid emit : invalid target");
    }
    n--;
  }
}


/*
** A numeric emit is a clock tick, to the root only; it is split in
** pieces, each ending at the nearest reported timer (timers re-report
** when they wake); 'CLKLATE' is what remains of the tick after the
** piece being broadcast (nested ticks restore it).
*/
static int emitaux (lua_State *L, int tgtidx, int eidx) {
  int tgt;
  eidx = lua_absindex(L, eidx);
  pushtarget(L, tgtidx);
  tgt = lua_gettop(L);
  if (lua_type(L, eidx) == LUA_TNUMBER) {
    lua_Number n = lua_tonumber(L, eidx);
    lua_Number late = CLKLATE;
    if (!isrootpool(L, tgt)) luaL_error(L, "invalid emit : expected global");
    do {
      lua_Number d = (CLKMIN < n) ? CLKMIN : n;
      n -= d;
      CLKMIN = HUGE_VAL;
      CLKLATE = n;
      pushnum(L, d);
      emitto(L, tgt, lua_gettop(L));
      lua_pop(L, 1);
    } while (n > 0);
    CLKLATE = late;
  }
  else
    emitto(L, tgt, eidx);
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

/*
** await patterns built by the compiler: tables with metatable ATM_PATT,
** field 'kind' and the arguments in [1..n] (field 'n'):
** "any"/"all" (pool), "until"/"while" (predicate), "until2"/"while2"
** (pattern, predicate), "spawn" (prototype, args...), "or"/"and"
** (patterns), "not" (pattern).
*/
static int newpatt (lua_State *L, const char *kind) {
  int n = lua_gettop(L);
  int i;
  lua_createtable(L, n, 2);
  lua_pushstring(L, kind);
  lua_setfield(L, -2, "kind");
  lua_pushinteger(L, n);
  lua_setfield(L, -2, "n");
  for (i = 1; i <= n; i++) {
    lua_pushvalue(L, i);
    lua_rawseti(L, -2, i);
  }
  lua_getfield(L, LUA_REGISTRYINDEX, ATM_PATT);
  lua_setmetatable(L, -2);
  return 1;
}

static int atm_await_spawn (lua_State *L) {  /* T(args) in a pattern */
  luaL_checkany(L, 1);
  return newpatt(L, "spawn");
}

static int atm_await_or (lua_State *L) {
  return newpatt(L, "or");
}

static int atm_await_and (lua_State *L) {
  return newpatt(L, "and");
}

static int atm_await_not (lua_State *L) {
  luaL_checkany(L, 1);
  lua_settop(L, 1);
  return newpatt(L, "not");
}

static int atm_await_any (lua_State *L) {
  if (!ispool(L, 1)) luaL_error(L, "invalid await : expected tasks pool");
  return newpatt(L, "any");
}

static int atm_await_all (lua_State *L) {
  if (!ispool(L, 1)) luaL_error(L, "invalid await : expected tasks pool");
  return newpatt(L, "all");
}

static int atm_await_until (lua_State *L) {  /* (pred) | (p, pred) */
  if (lua_gettop(L) >= 2) {
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_settop(L, 2);
    return newpatt(L, "until2");
  }
  luaL_checktype(L, 1, LUA_TFUNCTION);
  return newpatt(L, "until");
}

static int atm_await_while (lua_State *L) {
  if (lua_gettop(L) >= 2) {
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_settop(L, 2);
    return newpatt(L, "while2");
  }
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
/* result of an '__atmos' metamethod call (on the top): -1 to keep
   waiting, else the number of results (true: the event itself) */
static int atmosresult (lua_State *L, int hasevent) {
  if (!lua_toboolean(L, -1)) return -1;
  if (lua_type(L, -1) == LUA_TBOOLEAN) {
    lua_pop(L, 1);
    if (!hasevent) return -1;
    lua_pushvalue(L, 3);
  }
  return 1;
}

static int awaitmatch (lua_State *L, int hasevent) {
  const char *kind = pattkind(L, 1);
  lua_settop(L, hasevent ? 3 : 2);
  if (luaL_getmetafield(L, 1, "__atmos") != LUA_TNIL) {  /* user pattern */
    lua_pushvalue(L, 1);
    if (hasevent) lua_pushvalue(L, 3); else lua_pushnil(L);
    lua_call(L, 2, 1);
    return atmosresult(L, hasevent);
  }
  if (hasevent && luaL_getmetafield(L, 3, "__atmos") != LUA_TNIL) {
    lua_pushvalue(L, 3);  /* user event: decides by itself */
    lua_pushvalue(L, 1);
    lua_call(L, 2, 1);
    return atmosresult(L, 1);
  }
  if (kind != NULL) {
    int arg;
    lua_rawgeti(L, 1, 1);  /* its argument */
    arg = lua_gettop(L);
    if (strcmp(kind, "any") == 0 || strcmp(kind, "all") == 0) {
      if (!hasevent || !isxtask(L, 3)) return -1;
      uvget(L, 3, UV_UP);
      if (!lua_rawequal(L, -1, arg)) return -1;  /* not from this pool */
      lua_pop(L, 1);
      if (kind[1] == 'l') {  /* all: every task of the pool dead? */
        lua_Integer n, i;
        uvget(L, arg, UV_DNS);
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
  if (lua_type(L, 1) == LUA_TNUMBER) {  /* timer: a countdown */
    lua_Number clk;
    if (hasevent) {
      if (lua_type(L, 3) != LUA_TNUMBER) return -1;
      pushnum(L, lua_tonumber(L, 1) - lua_tonumber(L, 3));
      lua_replace(L, 1);  /* the remainder */
    }
    clk = lua_tonumber(L, 1);
    if (clk <= 0) {  /* expired: how late */
      pushnum(L, CLKLATE - clk);
      return 1;
    }
    if (clk < CLKMIN) CLKMIN = clk;  /* report the timer */
    return -1;
  }
  if (lua_type(L, 1) == LUA_TSTRING &&
      strcmp(lua_tostring(L, 1), "clock") == 0) {  /* any tick */
    if (!hasevent || lua_type(L, 3) != LUA_TNUMBER) return -1;
    lua_pushvalue(L, 3);
    pushnum(L, CLKLATE);
    return 2;  /* elapsed, late */
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


/*
** The kind of the pattern at index 1 for the broadcasts (AW_*), for
** the task 't' (its node userdata at 'ud') starting an 'await'.
** User code (an '__atmos' metamethod, a predicate, a '__eq' of a
** userdata) can only run in the task: AW_RESUME.
*/
static int classify (lua_State *L, Node *t, int ud) {
  int k;
  const char *kind;
  if (luaL_getmetafield(L, 1, "__atmos") != LUA_TNIL) {
    lua_pop(L, 1);
    k = AW_RESUME;
  }
  else if ((kind = pattkind(L, 1)) != NULL)
    k = (strcmp(kind, "any") == 0 || strcmp(kind, "all") == 0)
        ? AW_POOL : AW_RESUME;
  else if (isxtask(L, 1)) {
    k = AW_TASK;
    t->atask = xtaskof(L, 1);
  }
  else switch (lua_type(L, 1)) {
    case LUA_TNUMBER: {
      k = AW_CLOCK;
      t->clk = lua_tonumber(L, 1);
      break;
    }
    case LUA_TSTRING: {
      k = (strcmp(lua_tostring(L, 1), "clock") == 0) ? AW_CLOCKANY : AW_TAG;
      break;
    }
    case LUA_TBOOLEAN: k = lua_toboolean(L, 1) ? AW_TRUE : AW_FALSE; break;
    case LUA_TTABLE: k = AW_TABLE; break;
    case LUA_TNIL: case LUA_TLIGHTUSERDATA: case LUA_TTHREAD:
      k = AW_TAG; break;
    default: k = AW_RESUME; break;  /* userdata ('__eq') */
  }
  t->pkind = (unsigned char)k;
  t->wkind = AW_RESUME;
  if (k == AW_TAG || k == AW_TASK) {  /* the pattern, for the broadcasts */
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, ud, UV_PATT);
  }
  return k;
}

/* suspends the 'await' of 't' (stack: pattern, me) */
static int awaityield (lua_State *L, Node *t) {
  t->akind = t->pkind;
  return lua_yieldk(L, 0, 0, await_k);
}

/* the 'await' of 't' returns 'n' values */
static int awaitdone (lua_State *L, Node *t, int n) {
  (void)L;
  t->pkind = t->akind = t->wkind = AW_RESUME;
  return n;
}

static int await_k (lua_State *L, int status, lua_KContext ctx) {
  Node *t = xtaskof(L, 2);
  int n;
  (void)status; (void)ctx;
  /* stack: pattern, me, event */
  switch (t->wkind) {  /* decided by the broadcast? */
    case AW_TRUE: case AW_TAG:
      lua_pushvalue(L, 3);
      return awaitdone(L, t, 1);
    case AW_CLOCK:  /* expired: how late */
      pushnum(L, CLKLATE - t->clk);
      return awaitdone(L, t, 1);
    case AW_CLOCKANY:  /* elapsed, late */
      lua_pushvalue(L, 3);
      pushnum(L, CLKLATE);
      return awaitdone(L, t, 2);
    default: break;
  }
  n = awaitmatch(L, 1);
  if (n >= 0) return awaitdone(L, t, n);
  lua_settop(L, 2);
  return awaityield(L, t);
}


static int atm_await (lua_State *L);

/*
** Compound patterns, derived as in the reference: 'or' is 'par_any' of
** the sub-awaits, 'and' is 'par_all'; 'not p' re-awaits while the
** event matches 'p' (a race between 'p' and any event); 'p until (c)'
** re-awaits 'p' while the predicate fails, 'p while (c)' while it
** holds; 'T(args)' spawns the task and awaits its termination.
*/

/* body of a transparent task: 'await(upvalue 1)' */
static int awaitbranch (lua_State *L) {
  lua_settop(L, 0);
  lua_pushvalue(L, lua_upvalueindex(1));
  return atm_await(L);
}

static char NOTMARK;  /* result of the 'p' branch of 'not p' */

static int notmark_k (lua_State *L, int status, lua_KContext ctx) {
  (void)status; (void)ctx;
  lua_pushlightuserdata(L, &NOTMARK);
  return 1;
}

static int notbranch (lua_State *L) {  /* 'await(p)' then the mark */
  lua_settop(L, 0);
  lua_pushcfunction(L, atm_await);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_callk(L, 1, 0, 0, notmark_k);
  return notmark_k(L, LUA_OK, 0);
}

static int atm_par_any (lua_State *L);
static int atm_par_all (lua_State *L);

static int results_k (lua_State *L, int status, lua_KContext ctx) {
  (void)status;
  return lua_gettop(L) - (int)ctx;  /* results above 'ctx' */
}

/* await(p) for p = or(...) / and(...): par_any / par_all of branches */
static int awaitcomb (lua_State *L, int isor) {
  lua_Integer n, i;
  int base = lua_gettop(L);
  lua_getfield(L, 1, "n");
  n = lua_tointeger(L, -1);
  lua_pop(L, 1);
  lua_pushcfunction(L, isor ? atm_par_any : atm_par_all);
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, 1, i);
    lua_pushcclosure(L, awaitbranch, 1);
  }
  lua_callk(L, (int)n, LUA_MULTRET, base, results_k);
  return results_k(L, LUA_OK, base);
}

static int not_k (lua_State *L, int status, lua_KContext ctx);

static int notloop (lua_State *L) {  /* stack: p (and me) */
  lua_settop(L, 2);
  lua_pushcfunction(L, atm_par_any);
  lua_rawgeti(L, 1, 1);
  lua_pushcclosure(L, notbranch, 1);  /* 'p' happened: reject */
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, awaitbranch, 1);  /* any other event: accept */
  lua_callk(L, 2, 1, 0, not_k);
  return not_k(L, LUA_OK, 0);
}

static int not_k (lua_State *L, int status, lua_KContext ctx) {
  (void)status; (void)ctx;
  if (lua_touserdata(L, -1) == &NOTMARK)
    return notloop(L);
  return 1;
}

static int pred_k (lua_State *L, int status, lua_KContext ctx);

static int predloop (lua_State *L) {  /* stack: p (and me) */
  lua_settop(L, 2);
  lua_pushcfunction(L, atm_await);
  lua_rawgeti(L, 1, 1);
  lua_callk(L, 1, LUA_MULTRET, 0, pred_k);
  return pred_k(L, LUA_OK, 0);
}

static int pred_k (lua_State *L, int status, lua_KContext ctx) {
  int nres = lua_gettop(L) - 2;  /* results of the sub-await */
  int res;
  (void)status; (void)ctx;
  lua_rawgeti(L, 1, 2);  /* predicate */
  if (nres >= 1) lua_pushvalue(L, 3); else lua_pushnil(L);
  lua_call(L, 1, 1);
  res = lua_toboolean(L, -1);
  if (pattkind(L, 1)[0] == 'u') {  /* until */
    if (!res) return predloop(L);
    if (lua_type(L, -1) != LUA_TBOOLEAN) return 1;  /* the predicate's value */
    lua_pop(L, 1);
    return nres;  /* the event */
  }
  else {  /* while */
    if (res) return predloop(L);
    lua_pop(L, 1);
    return nres;
  }
}

static int atm_await (lua_State *L) {  /* await(p) */
  XTask *t;
  int n;
  const char *kind;
  luaL_checkany(L, 1);
  if (lua_gettop(L) != 1) luaL_error(L, "invalid await : invalid event pattern");
  if (ispool(L, 1)) luaL_error(L, "invalid await : unexpected tasks pool");
  if (lua_type(L, 1) == LUA_TFUNCTION)
    luaL_error(L, "invalid await : unexpected function");
  pushme(L, 1);  /* the thread's xtask */
  if (lua_isnil(L, 2)) luaL_error(L, "invalid await : expected enclosing task");
  kind = pattkind(L, 1);
  if (kind != NULL) {
    if (strcmp(kind, "spawn") == 0) {  /* spawn the task, await it */
      lua_Integer nargs, i;
      lua_getfield(L, 1, "n");
      nargs = lua_tointeger(L, -1) - 1;
      lua_pop(L, 1);
      for (i = 1; i <= nargs + 1; i++)
        lua_rawgeti(L, 1, i);
      spawncore(L, 0, 0, 3, (int)nargs);
      lua_replace(L, 1);
      if (isaborted(L)) return lua_yieldk(L, 0, 0, neverresumed);
    }
    else if (strcmp(kind, "or") == 0) return awaitcomb(L, 1);
    else if (strcmp(kind, "and") == 0) return awaitcomb(L, 0);
    else if (strcmp(kind, "not") == 0) return notloop(L);
    else if (strcmp(kind, "until2") == 0 || strcmp(kind, "while2") == 0)
      return predloop(L);
  }
  lua_settop(L, 2);
  t = pushnode(L, 2);
  t->time = TIME;  /* reacts only to broadcasts after this point */
  switch (classify(L, t, 3)) {
    case AW_TAG: case AW_TRUE: case AW_FALSE: case AW_TABLE:
    case AW_CLOCKANY: case AW_POOL:
      break;  /* no event yet: cannot match */
    default: {  /* user patterns, predicates, tasks, timers: check now */
      lua_settop(L, 2);
      n = awaitmatch(L, 0);
      if (n >= 0) return awaitdone(L, t, n);
    }
  }
  lua_settop(L, 2);
  return awaityield(L, t);
}

/* }------------------------------------------------------------------ */


/*
** {------------------------------------------------------------------
** par / par_any / par_all / watching: compositions of transparent
** tasks (closures), owned by a to-be-closed set of the C frame, so
** that leaving the composition (normally, by an error or by an abort)
** aborts the branches still alive (see tasks.md, stage 2a).
** -------------------------------------------------------------------
*/

#define PARSET	"atmos.parset"

static int parset_close (lua_State *L) {
  lua_Integer n = (lua_Integer)lua_rawlen(L, 1);
  lua_Integer i;
  for (i = n; i >= 1; i--) {
    lua_rawgeti(L, 1, i);
    closeany(L, -1);
    lua_pop(L, 1);
  }
  return 0;
}

/* checks the enclosing task and the branches (functions at 1..n);
   pushes the to-be-closed set; returns n */
static int parbegin (lua_State *L, const char *name) {
  int n = lua_gettop(L);
  int i;
  pushme(L, 1);
  if (!isxtask(L, -1))
    luaL_error(L, "invalid %s : expected enclosing task", name);
  lua_pop(L, 1);
  for (i = 1; i <= n; i++) {
    if (lua_type(L, i) != LUA_TFUNCTION)
      luaL_error(L, "invalid %s : expected task prototype", name);
  }
  lua_createtable(L, n, 0);
  luaL_setmetatable(L, PARSET);
  lua_toclose(L, -1);
  return n;
}

/* spawns branch 'i' (function at index 'i') into the set at 'setidx' */
static void parspawn (lua_State *L, int setidx, int i) {
  lua_pushvalue(L, i);
  spawncore(L, 0, 1, lua_gettop(L), 0);
  lua_rawseti(L, setidx, i);
}

static int branchdead (lua_State *L, int setidx, int i) {
  int dead;
  lua_rawgeti(L, setidx, i);
  dead = (costatus(L, -1) == CO_DEAD);
  lua_pop(L, 1);
  return dead;
}

/* 'me' reacts to later broadcasts; returns it */
static Node *stamp (lua_State *L) {
  Node *t;
  pushme(L, 1);
  t = xtaskof(L, -1);
  t->time = TIME;
  lua_pop(L, 1);
  return t;
}

/* par: the branches run until the enclosing block ends (no event makes
   it proceed: only a jump or an abort ends it) */
static int par_k (lua_State *L, int status, lua_KContext ctx) {
  (void)status;
  lua_settop(L, (int)ctx);
  stamp(L)->akind = AW_NEVER;
  return lua_yieldk(L, 0, ctx, par_k);
}

static int atm_par (lua_State *L) {
  int n = parbegin(L, "par");
  int setidx = n + 1;
  int i;
  for (i = 1; i <= n; i++) {
    parspawn(L, setidx, i);
    if (isaborted(L)) return lua_yieldk(L, 0, 0, neverresumed);
  }
  return par_k(L, LUA_OK, setidx);
}

/* par_any: the value of the first branch to terminate */
static int parany_k (lua_State *L, int status, lua_KContext ctx) {
  int setidx = (int)ctx;
  lua_Integer n = (lua_Integer)lua_rawlen(L, setidx);
  lua_Integer i;
  (void)status;
  lua_settop(L, setidx);
  for (i = 1; i <= n; i++) {
    if (branchdead(L, setidx, (int)i)) {
      lua_rawgeti(L, setidx, i);
      lua_getfield(L, -1, "ret");
      return 1;
    }
  }
  stamp(L);
  return lua_yieldk(L, 0, ctx, parany_k);
}

static int atm_par_any (lua_State *L) {
  int n = parbegin(L, "par_any");
  int setidx = n + 1;
  int i;
  for (i = 1; i <= n; i++) {
    parspawn(L, setidx, i);
    if (isaborted(L)) return lua_yieldk(L, 0, 0, neverresumed);
    if (branchdead(L, setidx, i)) break;  /* done: the others are not run */
  }
  return parany_k(L, LUA_OK, setidx);
}

/* par_all: the values of all branches, when all have terminated */
static int parall_k (lua_State *L, int status, lua_KContext ctx) {
  int setidx = (int)ctx;
  lua_Integer n = (lua_Integer)lua_rawlen(L, setidx);
  lua_Integer i;
  (void)status;
  lua_settop(L, setidx);
  for (i = 1; i <= n; i++) {
    if (!branchdead(L, setidx, (int)i)) {
      stamp(L);
      return lua_yieldk(L, 0, ctx, parall_k);
    }
  }
  luaL_checkstack(L, (int)n, "par_all results");
  for (i = 1; i <= n; i++) {
    lua_rawgeti(L, setidx, i);
    lua_getfield(L, -1, "ret");
    lua_remove(L, -2);
  }
  return (int)n;
}

static int atm_par_all (lua_State *L) {
  int n = parbegin(L, "par_all");
  int setidx = n + 1;
  int i;
  for (i = 1; i <= n; i++) {
    parspawn(L, setidx, i);
    if (isaborted(L)) return lua_yieldk(L, 0, 0, neverresumed);
  }
  return parall_k(L, LUA_OK, setidx);
}

/*
** The watcher of an inline 'watching P { body }' (see catch.md): a
** transparent task that awaits P and then throws itself with the
** results, caught by the region around the body in the owner.
*/
static int watcher_k (lua_State *L, int status, lua_KContext ctx) {
  int n = lua_gettop(L);
  (void)status; (void)ctx;
  pushme(L, 1);
  lua_insert(L, 1);
  return lua_errorn(L, n + 1);
}

static int watcherbody (lua_State *L) {
  lua_settop(L, 0);
  lua_pushcfunction(L, atm_await);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_callk(L, 1, LUA_MULTRET, 0, watcher_k);
  return watcher_k(L, LUA_OK, 0);
}

static int atm_watching_spawn (lua_State *L) {  /* watching.spawn(P) */
  luaL_checkany(L, 1);
  lua_settop(L, 1);
  lua_pushvalue(L, 1);
  lua_pushcclosure(L, watcherbody, 1);  /* at 2 */
  return spawnaux(L, 0, 1, 2, 0);
}


/* atm.jump(kind, ...): a jump out of a task body, as a throw of status
   LUA_ERRJUMP, caught by the owner's region (see catch.md) */
static int atm_jump (lua_State *L) {
  return lua_errorstatus(L, LUA_ERRJUMP, lua_gettop(L));
}


/* watching(P, f): par_any of 'await(P)' and 'f' (library form) */
static int atm_watching (lua_State *L) {
  luaL_checkany(L, 1);
  if (lua_gettop(L) != 2 || lua_type(L, 2) != LUA_TFUNCTION)
    luaL_error(L, "invalid watching : expected task prototype");
  lua_pushvalue(L, 1);
  lua_pushcclosure(L, awaitbranch, 1);
  lua_replace(L, 1);
  return atm_par_any(L);
}

/* }------------------------------------------------------------------ */

/* }------------------------------------------------------------------ */


/*
** {------------------------------------------------------------------
** toggle (see tasks.md, stage 2b)
** -------------------------------------------------------------------
*/

static unsigned char *statusof (lua_State *L, int idx) {
  return isxtask(L, idx) ? &(xtaskof(L, idx))->status
                         : &(poolof(L, idx))->status;
}

static lua_Integer *passof (lua_State *L, int idx) {
  return isxtask(L, idx) ? &(xtaskof(L, idx))->pass
                         : &(poolof(L, idx))->pass;
}

static int atm_toggle (lua_State *L);

/* body of a gate: loop { await(filter); node.pass = TIME } */
static int gateloop (lua_State *L);

static int gate_k (lua_State *L, int status, lua_KContext ctx) {
  (void)status; (void)ctx;
  *passof(L, lua_upvalueindex(2)) = TIME;  /* this emit goes through */
  return gateloop(L);
}

static int gateloop (lua_State *L) {
  lua_settop(L, 0);
  lua_pushcfunction(L, atm_await);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_callk(L, 1, 0, 0, gate_k);
  return gate_k(L, LUA_OK, 0);
}

/* the filters at 'first'..top as one pattern (an 'or' of several) */
static void filterpatt (lua_State *L, int first) {
  int n = lua_gettop(L) - first + 1;
  if (n > 1) {
    lua_pushcfunction(L, atm_await_or);
    lua_insert(L, first);
    lua_call(L, n, 1);
  }
}

/* toggles off the node at 'idx' with the filters at 'first'..top */
static void toggleoff (lua_State *L, int idx, int first) {
  unsigned char *st = statusof(L, idx);
  if (*st != ST_OK) luaL_error(L, "invalid toggle : expected awaiting task");
  *st = ST_TOGGLED;
  if (lua_gettop(L) >= first) {  /* filters: a gate task, off the tree */
    XTask *g;
    int status, nres;
    filterpatt(L, first);
    lua_pushvalue(L, idx);
    lua_pushcclosure(L, gateloop, 2);
    g = newxtask(L, -1, 1);
    lua_remove(L, -2);  /* the function */
    uvget(L, idx, UV_UP);
    uvset(L, -2, UV_UP);
    lua_pushvalue(L, -1);
    uvset(L, idx, UV_GATE);
    status = resumetask(g->th, L, 0, &nres);  /* run to its first await */
    taskresult(L, -1, status, nres);
    lua_pop(L, 1);
  }
}

static int atm_toggle_on (lua_State *L);

static int atm_toggle (lua_State *L) {  /* toggle(t, bool, filters...) */
  unsigned char *st;
  if (lua_type(L, 1) == LUA_TSTRING)
    return atm_toggle_on(L);
  if (!(isxtask(L, 1) || ispool(L, 1)))
    luaL_error(L, "invalid toggle : expected task");
  if (lua_type(L, 2) != LUA_TBOOLEAN)
    luaL_error(L, "invalid toggle : expected bool argument");
  st = statusof(L, 1);
  if (lua_toboolean(L, 2)) {  /* on */
    if (lua_gettop(L) > 2) luaL_error(L, "invalid toggle : unexpected argument");
    if (*st != ST_TOGGLED)
      luaL_error(L, "invalid toggle : expected toggled off task");
    *st = ST_OK;
    if (uvget(L, 1, UV_GATE) != LUA_TNIL) {
      closextask(L, -1);
      lua_pushnil(L);
      uvset(L, 1, UV_GATE);
    }
    lua_pop(L, 1);
    lua_pushinteger(L, 0);  /* a zero tick: timers re-report, 'true' wakes */
    emitto(L, 1, lua_gettop(L));
    lua_pop(L, 1);
    yieldifaborted(L, 0);
  }
  else {
    toggleoff(L, 1, 3);
    return 0;
  }
}

/*
** toggle on :T [filters] { body }: the body runs as a transparent task
** controlled by a hidden task that toggles it off on ':T [false]' and
** on on ':T [true]'; the value is the body's.
*/
static int ctl_k (lua_State *L, int status, lua_KContext ctx);

static int ctlawait (lua_State *L, int on) {  /* await(:T [on]) */
  lua_settop(L, 0);
  lua_pushcfunction(L, atm_await);
  lua_createtable(L, 1, 1);
  lua_pushvalue(L, lua_upvalueindex(2));
  lua_setfield(L, -2, "tag");
  lua_pushboolean(L, on);
  lua_rawseti(L, -2, 1);
  lua_callk(L, 1, 0, on, ctl_k);
  return ctl_k(L, LUA_OK, on);
}

static int ctl_k (lua_State *L, int status, lua_KContext ctx) {
  (void)status;
  if (ctx == 0) {  /* ':T [false]': toggle off, with the filter */
    int nargs = 2;
    lua_settop(L, 0);
    lua_pushcfunction(L, atm_toggle);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushboolean(L, 0);
    if (!lua_isnil(L, lua_upvalueindex(3))) {
      lua_pushvalue(L, lua_upvalueindex(3));
      nargs = 3;
    }
    lua_call(L, nargs, 0);
    return ctlawait(L, 1);
  }
  else if (ctx == 1) {  /* ':T [true]': toggle on (may yield) */
    lua_settop(L, 0);
    lua_pushcfunction(L, atm_toggle);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushboolean(L, 1);
    lua_callk(L, 2, 0, 2, ctl_k);
    return ctl_k(L, LUA_OK, 2);
  }
  else
    return ctlawait(L, 0);
}

static int ctlbody (lua_State *L) {  /* upvalues: body task, tag, filter */
  return ctlawait(L, 0);
}

static int atm_toggle_on (lua_State *L) {  /* toggle(:T, filters..., body) */
  int body = lua_gettop(L);
  int setidx;
  if (body < 2 || lua_type(L, body) != LUA_TFUNCTION)
    luaL_error(L, "invalid toggle : expected task prototype");
  pushme(L, 1);
  if (!isxtask(L, -1)) luaL_error(L, "invalid toggle : expected enclosing task");
  lua_pop(L, 1);
  if (body > 2) {  /* filters at 2..body-1 */
    lua_rotate(L, 2, 1);  /* body below the filters */
    filterpatt(L, 3);  /* stack: tag, body, filter */
    lua_rotate(L, 2, 1);  /* stack: tag, filter, body */
  }
  else {
    lua_pushnil(L);
    lua_insert(L, 2);  /* stack: tag, nil, body */
  }
  /* stack: tag, filter-or-nil, body */
  lua_createtable(L, 2, 0);
  luaL_setmetatable(L, PARSET);
  lua_toclose(L, -1);
  setidx = 4;
  lua_pushvalue(L, 3);
  spawncore(L, 0, 1, lua_gettop(L), 0);  /* the body */
  lua_rawseti(L, setidx, 1);
  if (isaborted(L)) return lua_yieldk(L, 0, 0, neverresumed);
  if (!branchdead(L, setidx, 1)) {
    lua_rawgeti(L, setidx, 1);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, ctlbody, 3);
    spawncore(L, 0, 1, lua_gettop(L), 0);  /* the controller */
    lua_rawseti(L, setidx, 2);
    if (isaborted(L)) return lua_yieldk(L, 0, 0, neverresumed);
  }
  return parany_k(L, LUA_OK, setidx);
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
/*
** {------------------------------------------------------------------
** Environments (see tasks.md, stage 3): tables with 'step' (polls
** once and emits; true quits the loop), 'quit' and 'mode', registered
** by 'atmos.env' and driven by 'atmos.loop'.
** -------------------------------------------------------------------
*/

static int atm_env (lua_State *L) {  /* atmos.env(e) */
  lua_Integer n;
  if (lua_type(L, 1) != LUA_TTABLE) luaL_error(L, "invalid env : expected table");
  lua_settop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, ATM_ENVS);
  n = (lua_Integer)lua_rawlen(L, -1);
  if (n >= 1) {  /* multiple environments: the first is the primary */
    lua_rawgeti(L, 2, 1);
    lua_getfield(L, -1, "mode");
    if (lua_type(L, -1) != LUA_TTABLE ||
        (lua_getfield(L, -1, "primary"), !lua_toboolean(L, -1)))
      luaL_error(L, "invalid env : first env must support primary mode");
    lua_pop(L, 1);  /* primary flag; the first mode table stays */
    lua_getfield(L, 1, "mode");
    if (lua_type(L, -1) != LUA_TTABLE ||
        (lua_getfield(L, -1, "secondary"), !lua_toboolean(L, -1)))
      luaL_error(L, "invalid env : non-first envs must support secondary mode");
    lua_pop(L, 1);
    lua_pushliteral(L, "secondary");
    lua_setfield(L, -2, "current");
    lua_pop(L, 1);
    lua_pushliteral(L, "primary");
    lua_setfield(L, -2, "current");
    lua_pop(L, 2);  /* first mode table and the first env */
  }
  lua_pushvalue(L, 1);
  lua_rawseti(L, 2, n + 1);
  return 0;
}

/* calls 'quit' on each environment, in reverse order, and unregisters all */
static void quitenvs (lua_State *L) {
  lua_Integer n, i;
  lua_getfield(L, LUA_REGISTRYINDEX, ATM_ENVS);
  n = (lua_Integer)lua_rawlen(L, -1);
  for (i = n; i >= 1; i--) {
    lua_rawgeti(L, -1, i);
    lua_pushnil(L);
    lua_rawseti(L, -3, i);  /* unregister before calling (errors) */
    if (lua_getfield(L, -1, "quit") != LUA_TNIL)
      lua_call(L, 0, 0);
    else
      lua_pop(L, 1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}

/* the running task or the root, as the owner of a loop/start body */
static void pushowner (lua_State *L) {
  pushme(L, 1);
  if (!isxtask(L, -1)) {
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
  }
}

/* teardown of 'atmos.loop' (to-be-closed, so also on errors): closes
   the body and quits the environments */
static int loopclose_tm (lua_State *L) {
  if (lua_rawgeti(L, 1, 1) != LUA_TNIL)
    closextask(L, -1);
  lua_pop(L, 1);
  quitenvs(L);
  sweep(L);
  return 0;
}

/*
** atmos.loop(body, ...): runs 'body' as a task under the running task
** (or the root) and drives the environments until it dies (or a step
** returns true); with no environment, it returns when the body returns
** control (nothing could awake it). Its value is the body's.
*/
static int atm_loop (lua_State *L) {
  int nargs = lua_gettop(L) - 1;
  int tidx, closer;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_pushcfunction(L, atm_task);  /* body as a prototype */
  lua_pushvalue(L, 1);
  lua_call(L, 1, 1);
  lua_replace(L, 1);
  lua_createtable(L, 1, 0);  /* teardown */
  luaL_setmetatable(L, LOOPCLOSE);
  pushowner(L);
  lua_rotate(L, 2, 2);  /* body, closer, owner, args... */
  closer = 2;
  lua_toclose(L, closer);  /* (a to-be-closed slot cannot be moved) */
  LOOPFLAG = 1;
  spawncore(L, 3, 0, 1, nargs);  /* runs to its first await */
  /* stack: body, closer, owner, xtask */
  tidx = lua_gettop(L);
  lua_pushvalue(L, tidx);
  lua_rawseti(L, closer, 1);
  while (costatus(L, tidx) != CO_DEAD) {
    lua_Integer n, i;
    int quit = 0;
    lua_getfield(L, LUA_REGISTRYINDEX, ATM_ENVS);
    n = (lua_Integer)lua_rawlen(L, -1);
    if (n == 0) {
      lua_pop(L, 1);
      break;  /* nothing can awake the body */
    }
    for (i = 1; i <= n && !quit; i++) {
      lua_rawgeti(L, -1, i);
      lua_getfield(L, -1, "step");
      lua_call(L, 0, 1);
      quit = lua_toboolean(L, -1);
      lua_pop(L, 2);
    }
    lua_pop(L, 1);
    sweep(L);
    if (quit) break;
  }
  lua_getfield(L, tidx, "ret");
  return 1;
}

/* atmos.start(body, ...): host-driven environment: spawns the body */
static int atm_start (lua_State *L) {
  int nargs = lua_gettop(L) - 1;
  int ok;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_getfield(L, LUA_REGISTRYINDEX, ATM_ENVS);
  ok = (lua_rawlen(L, -1) == 1);
  if (ok) {
    lua_rawgeti(L, -1, 1);
    lua_getfield(L, -1, "mode");
    ok = lua_isnil(L, -1);
    lua_pop(L, 2);
  }
  lua_pop(L, 1);
  if (!ok) luaL_error(L, "invalid start : expected single-mode env only");
  lua_pushcfunction(L, atm_task);
  lua_pushvalue(L, 1);
  lua_call(L, 1, 1);
  lua_replace(L, 1);
  pushowner(L);
  lua_insert(L, 1);  /* owner, body, args... */
  LOOPFLAG = 1;
  spawncore(L, 1, 0, 2, nargs);
  return 0;
}

/* the clock environment ('require "atmos.env.clock"'): 'step' emits
   the elapsed processor time in microseconds to the root */
static clock_t CLOCKOLD;

static int clockstep (lua_State *L) {
  clock_t now = clock();
  if (now > CLOCKOLD) {
    lua_Number dt = (lua_Number)(now - CLOCKOLD) * 1e6 / CLOCKS_PER_SEC;
    lua_Number us = (lua_Number)now * 1e6 / CLOCKS_PER_SEC;
    CLOCKOLD = now;
    pushnum(L, us);
    lua_setfield(L, lua_upvalueindex(1), "now");
    lua_pushcfunction(L, atm_emit_in);
    lua_getfield(L, LUA_REGISTRYINDEX, ATM_ROOT);
    pushnum(L, floor(dt));
    lua_call(L, 2, 0);
  }
  return 0;
}

static int openclock (lua_State *L) {  /* the module */
  lua_newtable(L);  /* M */
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "now");
  lua_createtable(L, 0, 1);  /* M.env */
  lua_pushvalue(L, -2);
  lua_pushcclosure(L, clockstep, 1);
  lua_setfield(L, -2, "step");
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "env");
  lua_pushcfunction(L, atm_env);
  lua_insert(L, -2);
  lua_call(L, 1, 0);  /* atmos.env(M.env) */
  CLOCKOLD = clock();
  return 1;
}

/* }------------------------------------------------------------------ */


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
  uvget(L, 1, UV_DNS);
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
  uvget(L, 1, UV_DNS);
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
  {"await.spawn", atm_await_spawn}, {"await.or", atm_await_or},
  {"await.and", atm_await_and}, {"await.not", atm_await_not},
  {"par", atm_par}, {"par_any", atm_par_any}, {"par_all", atm_par_all},
  {"watching", atm_watching}, {"toggle", atm_toggle},
  {"watching.spawn", atm_watching_spawn}, {"atm.jump", atm_jump},
  {NULL, NULL}
};


static void opentasks (lua_State *L) {  /* 'atmos' table on top */
  Pool *root;
  const luaL_Reg *f;
  luaL_newmetatable(L, XTASK); luaL_setfuncs(L, xtask_meta, 0); lua_pop(L, 1);
  luaL_newmetatable(L, POOL); luaL_setfuncs(L, pool_meta, 0); lua_pop(L, 1);
  luaL_newmetatable(L, PROTO); luaL_setfuncs(L, proto_meta, 0); lua_pop(L, 1);
  luaL_newmetatable(L, PARSET);
  lua_pushcfunction(L, parset_close);
  lua_setfield(L, -2, "__close");
  lua_pop(L, 1);
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
  root = newnamed(L, POOL, NK_POOL);
  root->max = -1; root->pin = 1; root->isroot = 1;
  root->status = ST_OK;
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
  lua_pushcfunction(L, atm_next);
  lua_setfield(L, -2, "next");
  lua_pushcfunction(L, atm_stop);
  lua_setfield(L, -2, "stop");
  lua_pushcfunction(L, atm_env);
  lua_setfield(L, -2, "env");
  lua_pushcfunction(L, atm_start);
  lua_setfield(L, -2, "start");
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, ATM_ENVS);
  luaL_newmetatable(L, LOOPCLOSE);
  lua_pushcfunction(L, loopclose_tm);
  lua_setfield(L, -2, "__close");
  lua_pop(L, 1);
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  lua_pushcfunction(L, openclock);
  lua_setfield(L, -2, "atmos.env.clock");
  lua_pop(L, 1);
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
