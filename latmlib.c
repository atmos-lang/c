/*
** $Id: latmlib.c $
** Atmos core library: loading Atmos chunks
** See Copyright Notice in lua.h
*/

#define latmlib_c
#define LUA_LIB

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "llimits.h"


/* registry key for the runtime table (upvalue '(atmos)' of chunks) */
#define ATMOS_RT	"ATMOS"

/* registry key for the 'test' flag */
#define ATMOS_TEST	"atmos.test"


/*
** atmos.core.load (src [, chunkname])
** Compiles an Atmos source string; returns the function, or nil plus
** an error message.
*/
static int atm_load (lua_State *L) {
  size_t l;
  const char *s = luaL_checklstring(L, 1, &l);
  const char *name = luaL_optstring(L, 2, "=(load)");
  int status = luaL_loadbufferx(L, s, l, name, "a");
  if (status == LUA_OK)
    return 1;
  else {
    luaL_pushfail(L);
    lua_insert(L, -2);
    return 2;  /* return fail plus error message */
  }
}


/*
** atmos.core.loadfile (filename)
*/
static int atm_loadfile (lua_State *L) {
  const char *fname = luaL_optstring(L, 1, NULL);
  int status = luaL_loadfilex(L, fname, "a");
  if (status == LUA_OK)
    return 1;
  else {
    luaL_pushfail(L);
    lua_insert(L, -2);
    return 2;  /* return fail plus error message */
  }
}


/*
** atmos.core.dofile (filename, ...)
*/
static int atm_dofile (lua_State *L) {
  const char *fname = luaL_optstring(L, 1, NULL);
  int n = lua_gettop(L) - 1;
  if (luaL_loadfilex(L, fname, "a") != LUA_OK)
    return lua_error(L);
  lua_insert(L, 2);  /* function below arguments */
  lua_call(L, n, LUA_MULTRET);
  return lua_gettop(L) - 1;
}


/*
** atmos.core.test ([on]) : get/set the flag enabling 'test' blocks
*/
static int atm_test (lua_State *L) {
  if (lua_gettop(L) >= 1) {
    lua_pushboolean(L, lua_toboolean(L, 1));
    lua_setfield(L, LUA_REGISTRYINDEX, ATMOS_TEST);
  }
  lua_getfield(L, LUA_REGISTRYINDEX, ATMOS_TEST);
  return 1;
}


/*
** Loader for '.atm' modules found by the searcher.
*/
static int atm_loader (lua_State *L) {
  const char *filename = luaL_checkstring(L, 2);
  if (luaL_loadfilex(L, filename, "a") != LUA_OK)
    return luaL_error(L, "error loading module '%s' from file '%s':\n\t%s",
                      lua_tostring(L, 1), filename, lua_tostring(L, -1));
  lua_pushvalue(L, 1);  /* module name */
  lua_pushvalue(L, 2);  /* file name */
  lua_call(L, 2, 1);
  return 1;
}


/*
** Searcher for '.atm' modules: uses 'package.path' with '?.lua'
** replaced by '?.atm'.
*/
static int atm_searcher (lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  const char *path;
  lua_getglobal(L, "package");
  if (!lua_istable(L, -1))
    return luaL_error(L, "'package' must be a table");
  lua_getfield(L, -1, "path");
  path = lua_tostring(L, -1);
  if (path == NULL)
    return luaL_error(L, "'package.path' must be a string");
  path = luaL_gsub(L, path, ".lua", ".atm");
  lua_getfield(L, -3, "searchpath");
  lua_pushstring(L, name);
  lua_pushstring(L, path);
  lua_call(L, 2, 2);  /* package.searchpath(name, path) */
  if (lua_isnil(L, -2)) {  /* not found? */
    return 1;  /* return error message (second result) */
  }
  else {
    lua_pop(L, 1);  /* remove nil */
    lua_pushcfunction(L, atm_loader);
    lua_insert(L, -2);  /* loader, filename */
    return 2;
  }
}


static const luaL_Reg atmlib[] = {
  {"load", atm_load},
  {"loadfile", atm_loadfile},
  {"dofile", atm_dofile},
  {"test", atm_test},
  {"searcher", atm_searcher},
  {"rt", NULL},
  {NULL, NULL}
};


/*
** Install the '.atm' searcher in 'package.searchers'
*/
static void install_searcher (lua_State *L) {
  lua_getglobal(L, "package");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "searchers");
    if (lua_istable(L, -1)) {
      lua_Integer n = luaL_len(L, -1);
      lua_pushcfunction(L, atm_searcher);
      lua_rawseti(L, -2, n + 1);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
}


LUAMOD_API int luaopen_atmos_core (lua_State *L) {
  luaL_newlib(L, atmlib);
  /* runtime table (shared with generated code through the registry) */
  if (lua_getfield(L, LUA_REGISTRYINDEX, ATMOS_RT) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, ATMOS_RT);
  }
  lua_setfield(L, -2, "rt");
  install_searcher(L);
  return 1;
}
