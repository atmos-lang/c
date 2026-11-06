/*
** $Id: llex.h $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#ifndef llex_h
#define llex_h

#include <limits.h>

#include "lobject.h"
#include "lzio.h"


/*
** Single-char tokens (terminal symbols) are represented by their own
** numeric code. Other tokens start at the following value.
*/
#define FIRST_RESERVED	(UCHAR_MAX + 1)


#if !defined(LUA_ENV)
#define LUA_ENV		"_ENV"
#endif


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER RESERVED"
*/
enum RESERVED {
  /* terminal symbols denoted by reserved words (Lua) */
  TK_AND = FIRST_RESERVED, TK_BREAK,
  TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
  TK_GLOBAL, TK_GOTO, TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR,
  TK_REPEAT, TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
  /* terminal symbols denoted by reserved words (Atmos only) */
  TK_AWAIT, TK_CATCH, TK_DEFER, TK_EMIT, TK_FUNC, TK_IFS, TK_LOOP,
  TK_MATCH, TK_ON, TK_PAR, TK_PIN, TK_SET, TK_SPAWN, TK_TASK, TK_TASKS,
  TK_TEST, TK_THREAD, TK_TOGGLE, TK_VAL, TK_VAR, TK_WATCHING, TK_WHERE,
  TK_WITH,
  /* other terminal symbols (Lua) */
  TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
  TK_SHL, TK_SHR,
  TK_DBCOLON,
  /* other terminal symbols (Atmos only) */
  TK_POW, TK_CAT, TK_IS, TK_ISNOT, TK_XIN, TK_XNOTIN,
  TK_DEQ, TK_DNE, TK_SUP, TK_SUB, TK_ARROW,
  TK_LPIPE1, TK_LPIPE2, TK_RPIPE1, TK_RPIPE2,
  TK_LOR, TK_LAND, TK_LNOT, TK_LNE,
  TK_EOS,
  TK_FLT, TK_INT, TK_NAME, TK_STRING,
  /* literals (Atmos only) */
  TK_TAG, TK_CLK, TK_NAT
};

/* Lua reserved words are the first NUM_LUA_RESERVED entries */
#define NUM_LUA_RESERVED	(cast_int(TK_WHILE-FIRST_RESERVED + 1))

/* number of reserved words (Lua + Atmos) */
#define NUM_RESERVED	(cast_int(TK_WITH-FIRST_RESERVED + 1))


typedef union {
  lua_Number r;
  lua_Integer i;
  TString *ts;
} SemInfo;  /* semantics information */


typedef struct Token {
  int token;
  SemInfo seminfo;
  int line;  /* line where the token ends */
  int sep;  /* separator count (newlines/';') at the token (Atmos) */
  TString *text;  /* raw text of literal tokens (Atmos; for messages) */
} Token;


/* number of characters that the Atmos lexer may push back */
#define LUAX_MAXPBACK	4


/* state of the scanner plus state of the parser when shared by all
   functions */
typedef struct LexState {
  int current;  /* current character (charint) */
  int linenumber;  /* input line counter */
  int lastline;  /* line of last token 'consumed' */
  Token t;  /* current token */
  Token lookahead;  /* look ahead token */
  struct FuncState *fs;  /* current function (parser) */
  struct lua_State *L;
  ZIO *z;  /* input stream */
  Mbuffer *buff;  /* buffer for tokens */
  Table *h;  /* to avoid collection/reuse strings */
  struct Dyndata *dyd;  /* dynamic structures used by the parser */
  TString *source;  /* current source name */
  TString *envn;  /* environment variable name */
  TString *brkn;  /* "break" name (used as a label) */
  TString *glbn;  /* "global" name (when not a reserved word) */
  /* Atmos extensions */
  lu_byte atmos;  /* true when lexing/parsing Atmos (not Lua) */
  int sep;  /* separator counter: incremented by newlines and ';' */
  int tokline;  /* line where the current token starts */
  int toksep;  /* separator count where the current token starts */
  int npback;  /* number of pushed-back characters */
  int pback[LUAX_MAXPBACK];  /* pushed-back characters */
  Token prev;  /* previous token (Atmos parser needs 'sep' of it) */
  TString *atmn;  /* name of the hidden runtime upvalue */
  void *ast;  /* Atmos AST state (parser arena) */
} LexState;


LUAI_FUNC void luaX_init (lua_State *L);
LUAI_FUNC void luaX_setinput (lua_State *L, LexState *ls, ZIO *z,
                              TString *source, int firstchar);
LUAI_FUNC TString *luaX_newstring (LexState *ls, const char *str, size_t l);
LUAI_FUNC void luaX_next (LexState *ls);
LUAI_FUNC int luaX_lookahead (LexState *ls);
LUAI_FUNC l_noret luaX_syntaxerror (LexState *ls, const char *s);
LUAI_FUNC const char *luaX_token2str (LexState *ls, int token);

/* Atmos lexer (latmlex.c) */
LUAI_FUNC int luaX_atmlex (LexState *ls, Token *t);
LUAI_FUNC void luaX_save (LexState *ls, int c);
LUAI_FUNC l_noret luaX_lexerror (LexState *ls, const char *msg, int token);
LUAI_FUNC l_noret luaX_atmerror (LexState *ls, int line, const char *near,
                                 const char *msg);
LUAI_FUNC const char *luaX_atmtoktext (LexState *ls, const Token *t);
LUAI_FUNC int luaX_isatmword (int token);


#endif
