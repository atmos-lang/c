/*
** $Id: latmos.h $
** Atmos front end: AST, parser, and code generator
** See Copyright Notice in lua.h
*/

#ifndef latmos_h
#define latmos_h

#include "llex.h"
#include "lobject.h"
#include "lparser.h"


/*
** {======================================================
** Abstract Syntax Tree
** =======================================================
*/

/* node tags (mirror the tags used by the reference Lua implementation) */
typedef enum {
  N_NIL, N_BOOL, N_NUM, N_STR, N_TAG, N_NAT, N_CLK, N_DOTS,
  N_ACC,      /* identifier access; u.ts = name */
  N_RT,       /* runtime function reference; u.rt = RT_xxx */
  N_UNO,      /* unary operation; sub = token */
  N_BIN,      /* binary operation; sub = token */
  N_INDEX,    /* t@(idx) */
  N_TABLE,    /* [ k=v, ... ] */
  N_CALL,     /* f(es) */
  N_MET,      /* o::m (only as the 'f' of a call) */
  N_PROTO,    /* func/task/lua (pars) { blk } */
  N_PARENS,   /* (e) */
  N_ES,       /* (e1, e2, ...) */
  N_DCL,      /* val/var/pin ids = set */
  N_SET,      /* set dsts = src */
  N_DO,       /* do [:tag] { blk } */
  N_STMTS,    /* inline sequence of statements */
  N_BLOCK,    /* { es } */
  N_DEFER,    /* defer { blk } */
  N_IFS,      /* if/ifs/match */
  N_LOOP,     /* loop ids in itr { blk } */
  N_CATCH,    /* catch cnd { blk } */
  N_RETURN    /* return(es) */
} NTag;


/* runtime functions referenced by generated code (fields of _ATM) */
typedef enum {
  RT_FUNC, RT_TASK, RT_XTASK, RT_SPAWN, RT_SPAWN_IN, RT_DO_SPAWN,
  RT_EMIT, RT_EMIT_IN, RT_AWAIT, RT_TOGGLE,
  RT_PAR, RT_PAR_ANY, RT_PAR_ALL, RT_WATCHING, RT_THREAD, RT_TASKS,
  RT_CATCH, RT_THROW, RT_ESCAPE,
  RT_DO, RT_LOOP, RT_BREAK, RT_UNTIL, RT_WHILE, RT_RETURN,
  RT_PIN_CHK_SET, RT_TAG_DO, RT_VOID, RT_DEFER, RT_PUB,
  RT_ITER, RT_IS, RT_GTE, RT_EQ, RT_CAT, RT_XIN,
  RT_N  /* number of entries */
} RTId;

LUAI_DDEC(const char *const luaA_rtnames[];)


/* prototype kinds */
#define P_FUNC	0  /* func (...) { ... } : wrapped by atm_func */
#define P_TASK	1  /* task (...) { ... } : wrapped by task(atm_func) */
#define P_LUA	2  /* plain Lua closure (internal blocks) */


/* kinds of cases in an 'ifs' */
#define C_EXPR		0  /* cnd => body */
#define C_ELSE		1  /* else => body */
#define C_MATCH		2  /* (match) cmp => body : X.is(v, cmp) */
#define C_MATCHFUNC	3  /* (match) \{...} => body : f(v) */


typedef struct ANode ANode;

typedef struct NList {
  ANode **arr;
  int n;
  int size;
} NList;

typedef struct KV {
  ANode *k;
  ANode *v;
} KV;

typedef struct KVList {
  KV *arr;
  int n;
  int size;
} KVList;

typedef struct Case {
  ANode *cnd;  /* condition (NULL for 'else') */
  ANode *f;  /* body (a P_LUA/P_FUNC proto, called with the condition) */
  lu_byte kind;  /* C_xxx */
} Case;

typedef struct CaseList {
  Case *arr;
  int n;
  int size;
} CaseList;


struct ANode {
  lu_byte tag;  /* NTag */
  lu_byte flags;  /* N_TABLE: 1 = await pattern; N_STR: 1 = multi-line */
  int sub;  /* proto: P_xxx; dcl: TK_VAL/TK_VAR/TK_PIN; uno/bin: op */
  int line;  /* source line (for debug info and messages) */
  int line2;  /* ending line (for blocks) */
  TString *text;  /* token text for messages ("near 'xxx'") */
  union {
    lua_Integer i;  /* N_NUM (integer), N_CLK (integer) */
    lua_Number r;  /* N_NUM (float), N_CLK (float) */
    int b;  /* N_BOOL */
    TString *ts;  /* N_STR, N_TAG, N_NAT, N_ACC */
    int rt;  /* N_RT */
    struct { ANode *e; } uno;  /* N_UNO, N_PARENS */
    struct { ANode *e1, *e2; } bin;  /* N_BIN */
    struct { ANode *t, *idx; } index;  /* N_INDEX */
    struct { KVList es; TString *patname; int nitems; } table;  /* N_TABLE */
    struct { ANode *f; NList es; int pipeop; } call;  /* N_CALL */
    struct { ANode *o; TString *met; } met;  /* N_MET */
    struct { NList pars; ANode *blk; lu_byte dots; } proto;  /* N_PROTO */
    struct { NList es; } es;  /* N_ES, N_STMTS, N_BLOCK, N_RETURN */
    struct { NList ids; ANode *set; ANode *spw; } dcl;  /* N_DCL */
    struct { NList dsts; ANode *src; } set;  /* N_SET */
    struct { TString *esc; ANode *blk; } do_;  /* N_DO */
    struct { ANode *blk; } defer;  /* N_DEFER */
    struct { ANode *match; CaseList cases; } ifs;  /* N_IFS */
    struct { NList ids; ANode *itr; ANode *blk; } loop;  /* N_LOOP */
    struct { ANode *cnd; ANode *blk; } catch_;  /* N_CATCH */
  } u;
};

/* numeric node is a float? */
#define NFLT	2
#define isnflt(n)	((n)->flags & NFLT)

/* }====================================================== */


/* flags for 'luaA_parse' */
#define ATM_TEST	1  /* enable 'test' blocks */

LUAI_FUNC void *luaA_alloc (LexState *ls, size_t size);
LUAI_FUNC ANode *luaA_parse (LexState *ls, int flags);
LUAI_FUNC l_noret luaA_error (LexState *ls, int line, TString *near,
                              const char *msg);
LUAI_FUNC void luaA_codemain (LexState *ls, FuncState *fs, ANode *ast);
LUAI_FUNC LClosure *luaY_atmparser (lua_State *L, ZIO *z, Table *anchor,
                                    Mbuffer *buff, Dyndata *dyd,
                                    const char *name, int firstchar);

#endif
