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
  return 1;
}
