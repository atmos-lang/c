/*
** $Id: lcorolib.c $
** Coroutine Library
** See Copyright Notice in lua.h
*/

#define lcorolib_c
#define LUA_LIB

#include "lprefix.h"


#include <stdlib.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "llimits.h"


static lua_State *getco (lua_State *L) {
  lua_State *co = lua_tothread(L, 1);
  luaL_argexpected(L, co, 1, "thread");
  return co;
}


/*
** Resumes a coroutine. Returns the number of results for non-error
** cases or -1 for errors.
*/
static int auxresume (lua_State *L, lua_State *co, int narg) {
  int status, nres;
  if (l_unlikely(!lua_checkstack(co, narg))) {
    lua_pushliteral(L, "too many arguments to resume");
    return -1;  /* error flag */
  }
  lua_xmove(L, co, narg);
  status = lua_resume(co, L, narg, &nres);
  if (l_likely(status == LUA_OK || status == LUA_YIELD)) {
    if (l_unlikely(!lua_checkstack(L, nres + 1))) {
      lua_pop(co, nres);  /* remove results anyway */
      lua_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */
    }
    lua_xmove(co, L, nres);  /* move yielded values */
    return nres;
  }
  else {  /* error: 'nres' error values */
    if (l_unlikely(!lua_checkstack(L, nres + 1))) {
      lua_pop(co, nres);  /* remove error values anyway */
      lua_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */
    }
    lua_xmove(co, L, nres);  /* move error values */
    return -nres;  /* error flag: number of error values */
  }
}


static int luaB_coresume (lua_State *L) {
  lua_State *co = getco(L);
  int r;
  r = auxresume(L, co, lua_gettop(L) - 1);
  if (l_unlikely(r < 0)) {  /* error with '-r' values? */
    lua_pushboolean(L, 0);
    lua_insert(L, r - 1);  /* 'false' before the error values */
    return 1 - r;  /* return false + error values */
  }
  else {
    lua_pushboolean(L, 1);
    lua_insert(L, -(r + 1));
    return r + 1;  /* return true + 'resume' returns */
  }
}


static int luaB_auxwrap (lua_State *L) {
  lua_State *co = lua_tothread(L, lua_upvalueindex(1));
  int r = auxresume(L, co, lua_gettop(L));
  if (l_unlikely(r < 0)) {  /* error with '-r' values? */
    int n = -r;  /* number of error values */
    int stat = lua_status(co);
    if (stat != LUA_OK && stat != LUA_YIELD) {  /* error in the coroutine? */
      stat = lua_closethread(co, L);  /* close its tbc variables */
      if (stat == LUA_OK)
        return 0;  /* nothing to return */
      n = lua_gettop(co);  /* error values left by 'lua_closethread' */
      luaL_checkstack(L, n, "too many results to resume");
      lua_xmove(co, L, n);  /* move error values to the caller */
    }
    if (stat != LUA_ERRMEM &&  /* not a memory error and ... */
        lua_type(L, -n) == LUA_TSTRING) {  /* ... error object is a string? */
      int idx = lua_gettop(L) - n + 1;  /* index of the error object */
      luaL_checkstack(L, 2, "too many results to resume");
      luaL_where(L, 1);  /* add extra info, if available */
      lua_pushvalue(L, idx);
      lua_concat(L, 2);
      lua_replace(L, idx);
    }
    return lua_errorn(L, n);  /* propagate error */
  }
  return r;
}


static int luaB_cocreate (lua_State *L) {
  lua_State *NL;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  NL = lua_newthread(L);
  lua_pushvalue(L, 1);  /* move function to top */
  lua_xmove(L, NL, 1);  /* move function from L to NL */
  return 1;
}


static int luaB_cowrap (lua_State *L) {
  luaB_cocreate(L);
  lua_pushcclosure(L, luaB_auxwrap, 1);
  return 1;
}


static int luaB_yield (lua_State *L) {
  return lua_yield(L, lua_gettop(L));
}


#define COS_RUN		0
#define COS_DEAD	1
#define COS_YIELD	2
#define COS_NORM	3


static const char *const statname[] =
  {"running", "dead", "suspended", "normal"};


static int auxstatus (lua_State *L, lua_State *co) {
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


static int luaB_costatus (lua_State *L) {
  lua_State *co = getco(L);
  lua_pushstring(L, statname[auxstatus(L, co)]);
  return 1;
}


static lua_State *getoptco (lua_State *L) {
  return (lua_isnone(L, 1) ? L : getco(L));
}


static int luaB_yieldable (lua_State *L) {
  lua_State *co = getoptco(L);
  lua_pushboolean(L, lua_isyieldable(co));
  return 1;
}


static int luaB_corunning (lua_State *L) {
  int ismain = lua_pushthread(L);
  lua_pushboolean(L, ismain);
  return 2;
}


static int luaB_close (lua_State *L) {
  lua_State *co = getoptco(L);
  int status = auxstatus(L, co);
  switch (status) {
    case COS_DEAD: case COS_YIELD: {
      status = lua_closethread(co, L);
      if (status == LUA_OK) {
        lua_pushboolean(L, 1);
        return 1;
      }
      else {
        int n = lua_gettop(co);  /* error values left by 'lua_closethread' */
        lua_pushboolean(L, 0);
        luaL_checkstack(L, n, "too many results to close");
        lua_xmove(co, L, n);  /* move error values */
        return n + 1;
      }
    }
    case COS_NORM:
      return luaL_error(L, "cannot close a %s coroutine", statname[status]);
    case COS_RUN:
      lua_geti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);  /* get main */
      if (lua_tothread(L, -1) == co)
        return luaL_error(L, "cannot close main thread");
      lua_closethread(co, L);  /* close itself */
      /* previous call does not return *//* FALLTHROUGH */
    default:
      lua_assert(0);
      return 0;
  }
}


static const luaL_Reg co_funcs[] = {
  {"create", luaB_cocreate},
  {"resume", luaB_coresume},
  {"running", luaB_corunning},
  {"status", luaB_costatus},
  {"wrap", luaB_cowrap},
  {"yield", luaB_yield},
  {"isyieldable", luaB_yieldable},
  {"close", luaB_close},
  {NULL, NULL}
};



LUAMOD_API int luaopen_coroutine (lua_State *L) {
  luaL_newlib(L, co_funcs);
  return 1;
}

