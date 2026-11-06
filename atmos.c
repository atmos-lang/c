/*
** $Id: atmos.c $
** Atmos stand-alone interpreter
** See Copyright Notice in lua.h
*/

#define atmos_c

#include "lprefix.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "llimits.h"


#define ATMOS_VERSION	"atmos 0.9 (C)"

#if !defined(ATMOS_PROGNAME)
#define ATMOS_PROGNAME		"atmos"
#endif

/* environment variable with the Lua path for the runtime files */
#if !defined(ATMOS_PATH_VAR)
#define ATMOS_PATH_VAR		"ATMOS_PATH"
#endif


static lua_State *globalL = NULL;

static const char *progname = ATMOS_PROGNAME;


#if defined(LUA_USE_POSIX)   /* { */

/*
** Use 'sigaction' when available.
*/
static void setsignal (int sig, void (*handler)(int)) {
  struct sigaction sa;
  sa.sa_handler = handler;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);  /* do not mask any signal */
  sigaction(sig, &sa, NULL);
}

#else           /* }{ */

#define setsignal            signal

#endif                               /* } */


/*
** Hook set by signal function to stop the interpreter.
*/
static void lstop (lua_State *L, lua_Debug *ar) {
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);  /* reset hook */
  luaL_error(L, "interrupted!");
}


/*
** Function to be called at a C signal. Because a C signal cannot
** just change a Lua state (as there is no proper synchronization),
** this function only sets a hook that, when called, will stop the
** interpreter.
*/
static void laction (int i) {
  int flag = LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT;
  setsignal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
  lua_sethook(globalL, lstop, flag, 1);
}


static void print_usage (void) {
  lua_writestringerror("%s",
  "Usage: atmos [-h] [-t] [-v] <input> [<args>] ...\n"
  "\n"
  "The Programming Language Atmos.\n"
  "\n"
  "Arguments:\n"
  "   input                 Input program.\n"
  "   args                  Program arguments.\n"
  "\n"
  "Options:\n"
  "   -h, --help            Show this help message and exit.\n"
  "   -t, --test            Enable test blocks.\n"
  "   -v, --version         Show version.\n"
  "\n"
  "For more information, please visit our website:\n"
  "\n"
  "    https://github.com/atmos-lang/atmos/\n"
  "\n");
}


/*
** Prints an error message, adding the program name in front of it
** (if present)
*/
static void l_message (const char *pname, const char *msg) {
  if (pname) lua_writestringerror("%s: ", pname);
  lua_writestringerror("%s\n", msg);
}


/*
** Check whether 'status' is not OK and, if so, prints the error
** message on the top of the stack.
*/
static int report (lua_State *L, int status) {
  if (status != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    if (msg == NULL)
      msg = "(error message not a string)";
    l_message(progname, msg);
    lua_pop(L, 1);  /* remove message */
  }
  return status;
}


/*
** Message handler used to run all chunks
*/
static int msghandler (lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL) {  /* is error object not a string? */
    if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
        lua_type(L, -1) == LUA_TSTRING)  /* that produces a string? */
      return 1;  /* that is the message */
    else
      msg = lua_pushfstring(L, "(error object is a %s value)",
                               luaL_typename(L, 1));
  }
  luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
  return 1;  /* return the traceback */
}


/*
** Interface to 'lua_pcall', which sets appropriate message function
** and C-signal handler. Used to run all chunks.
*/
static int docall (lua_State *L, int narg, int nres) {
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  lua_pushcfunction(L, msghandler);  /* push message handler */
  lua_insert(L, base);  /* put it under function and args */
  globalL = L;  /* to be available to 'laction' */
  setsignal(SIGINT, laction);  /* set C-signal handler */
  status = lua_pcall(L, narg, nres, base);
  setsignal(SIGINT, SIG_DFL); /* reset C-signal handler */
  lua_remove(L, base);  /* remove message handler */
  return status;
}


/*
** Create the 'arg' table, which stores all arguments from the
** command line ('argv'), aligned so that index 0 is the script name.
*/
static void createargtable (lua_State *L, char **argv, int argc, int script) {
  int i, narg;
  narg = argc - (script + 1);  /* number of positive indices */
  lua_createtable(L, narg, script + 1);
  for (i = 0; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - script);
  }
  lua_setglobal(L, "arg");
}


/*
** Push on the stack the contents of table 'arg' from 1 to #arg
*/
static int pushargs (lua_State *L) {
  int i, n;
  if (lua_getglobal(L, "arg") != LUA_TTABLE)
    luaL_error(L, "'arg' is not a table");
  n = (int)luaL_len(L, -1);
  luaL_checkstack(L, n + 3, "too many arguments to script");
  for (i = 1; i <= n; i++)
    lua_rawgeti(L, -i, i);
  lua_remove(L, -i);  /* remove table from the stack */
  return n;
}


/*
** Directory of the running executable (to find the runtime files)
*/
static void setpath (lua_State *L, const char *argv0) {
  const char *env = getenv(ATMOS_PATH_VAR);
  luaL_Buffer b;
  lua_getglobal(L, "package");
  luaL_buffinit(L, &b);
  if (env != NULL) {
    luaL_addstring(&b, env);
    luaL_addstring(&b, ";");
  }
  else {
    const char *slash = strrchr(argv0, '/');
    size_t dirlen = (slash != NULL) ? cast_sizet(slash - argv0 + 1) : 0;
    luaL_addlstring(&b, argv0, dirlen);
    luaL_addstring(&b, "rt/?.lua;");
    luaL_addlstring(&b, argv0, dirlen);
    luaL_addstring(&b, "rt/?/init.lua;");
  }
  lua_getfield(L, -2, "path");
  luaL_addvalue(&b);
  luaL_pushresult(&b);
  lua_setfield(L, -2, "path");
  lua_pop(L, 1);  /* package */
}


/*
** Main body: runs the program as the body of 'atmos.loop'.
*/
static int pmain (lua_State *L) {
  int argc = (int)lua_tointeger(L, 1);
  char **argv = (char **)lua_touserdata(L, 2);
  int i;
  int script = 0;
  int test = 0;
  int n, status;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage();
      lua_pushboolean(L, 1);
      return 1;
    }
    else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
      lua_writestring(ATMOS_VERSION, strlen(ATMOS_VERSION));
      lua_writeline();
      lua_pushboolean(L, 1);
      return 1;
    }
    else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--test") == 0) {
      test = 1;
    }
    else if (argv[i][0] == '-' && argv[i][1] != '\0' && script == 0) {
      lua_writestringerror("%s: unrecognized option\n", argv[i]);
      print_usage();
      return luaL_error(L, "invalid arguments");
    }
    else if (script == 0)
      script = i;
  }
  if (script == 0) {
    print_usage();
    return luaL_error(L, "missing input program");
  }
  luaL_checkversion(L);  /* check that interpreter has correct version */
  luaL_openlibs(L);  /* open standard libraries */
  setpath(L, argv[0]);
  luaL_requiref(L, LUA_ATMLIBNAME, luaopen_atmos_core, 0);
  lua_pop(L, 1);
  lua_pushboolean(L, test);
  lua_setfield(L, LUA_REGISTRYINDEX, "atmos.test");
  createargtable(L, argv, argc, script);  /* create table 'arg' */
  lua_gc(L, LUA_GCRESTART);  /* start GC... */
  lua_gc(L, LUA_GCGEN);  /* ...in generational mode */
  /* require "atmos" ; require "atmos.lang.rt" */
  lua_getglobal(L, "require");
  lua_pushstring(L, "atmos");
  lua_call(L, 1, 1);  /* atmos module */
  {
    const char *fname = argv[script];
    size_t len = strlen(fname);
    if (len >= 4 && strcmp(fname + len - 4, ".lua") == 0) {
      /* a Lua script: run it directly (used by the test suite) */
      lua_pop(L, 1);  /* remove atmos module */
      status = luaL_loadfilex(L, fname, "bt");
      if (status != LUA_OK)
        return lua_error(L);
      n = pushargs(L);  /* push arguments to script */
      lua_call(L, n, 0);
      lua_pushboolean(L, 1);
      return 1;
    }
  }
  /* atmos.loop(chunk, args...) */
  lua_getfield(L, -1, "loop");
  lua_remove(L, -2);  /* remove atmos module */
  status = luaL_loadfilex(L, argv[script], "a");
  if (status != LUA_OK)
    return lua_error(L);
  n = pushargs(L);  /* push arguments to script */
  lua_call(L, n + 1, 0);
  lua_pushboolean(L, 1);  /* signal no errors */
  return 1;
}


int main (int argc, char **argv) {
  int status, result;
  lua_State *L = luaL_newstate();  /* create state */
  if (argv[0] && argv[0][0]) progname = argv[0];
  if (L == NULL) {
    l_message(progname, "cannot create state: not enough memory");
    return EXIT_FAILURE;
  }
  lua_gc(L, LUA_GCSTOP);  /* stop GC while building state */
  lua_pushcfunction(L, &pmain);  /* to call 'pmain' in protected mode */
  lua_pushinteger(L, argc);  /* 1st argument */
  lua_pushlightuserdata(L, argv); /* 2nd argument */
  status = docall(L, 2, 1);  /* do the call */
  result = lua_toboolean(L, -1);  /* get result */
  report(L, status);
  lua_close(L);
  return (result && status == LUA_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
