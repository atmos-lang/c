/*
** $Id: latmparser.c $
** Atmos Parser: builds an AST from the token stream
** See Copyright Notice in lua.h
*/

#define latmparser_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "latmos.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"


const char *const luaA_rtnames[] = {
  "func", "task", "xtask", "spawn", "spawn_in", "do_spawn",
  "emit", "emit_in", "await", "toggle",
  "par", "par_any", "par_all", "watching", "thread", "tasks",
  "catch", "throw", "escape",
  "do", "loop", "break", "until", "while", "return",
  "pin_chk_set", "tag_do", "void", "defer", "pub",
  "iter", "is", "gte", "eq", "cat", "xin"
};


/*
** {======================================================
** Arena: AST nodes live in userdata blocks anchored in the scanner
** table, so they are collected with it (also in case of errors).
** =======================================================
*/

#define ARENA_BLOCK	(16 * 1024)

typedef struct AtmState {
  char *cur;  /* current allocation point */
  size_t left;  /* bytes left in current block */
  int test;  /* 'test' blocks enabled? */
} AtmState;


static AtmState *getstate (LexState *ls) {
  return cast(AtmState *, ls->ast);
}


void *luaA_alloc (LexState *ls, size_t size) {
  AtmState *as = getstate(ls);
  void *p;
  size = (size + 7u) & ~cast_sizet(7);  /* align to 8 bytes */
  if (as->left < size) {
    size_t bsize = (size > ARENA_BLOCK) ? size : ARENA_BLOCK;
    Udata *u = luaS_newudata(ls->L, bsize, 0);
    luaD_anchorobj(ls->L, ls->h, obj2gco(u));  /* anchor block */
    as->cur = cast_charp(getudatamem(u));
    as->left = bsize;
  }
  p = as->cur;
  as->cur += size;
  as->left -= size;
  return p;
}

#define newnode(ls)	cast(ANode *, luaA_alloc(ls, sizeof(ANode)))


static void nlist_add (LexState *ls, NList *l, ANode *n) {
  if (l->n >= l->size) {
    int ns = (l->size == 0) ? 4 : l->size * 2;
    ANode **na = cast(ANode **, luaA_alloc(ls, cast_sizet(ns) * sizeof(ANode *)));
    if (l->n > 0) memcpy(na, l->arr, cast_sizet(l->n) * sizeof(ANode *));
    l->arr = na;
    l->size = ns;
  }
  l->arr[l->n++] = n;
}


static void nlist_insert (LexState *ls, NList *l, int pos, ANode *n) {
  int i;
  nlist_add(ls, l, n);  /* grow */
  for (i = l->n - 1; i > pos; i--)
    l->arr[i] = l->arr[i - 1];
  l->arr[pos] = n;
}


static void kvlist_add (LexState *ls, KVList *l, ANode *k, ANode *v) {
  if (l->n >= l->size) {
    int ns = (l->size == 0) ? 4 : l->size * 2;
    KV *na = cast(KV *, luaA_alloc(ls, cast_sizet(ns) * sizeof(KV)));
    if (l->n > 0) memcpy(na, l->arr, cast_sizet(l->n) * sizeof(KV));
    l->arr = na;
    l->size = ns;
  }
  l->arr[l->n].k = k;
  l->arr[l->n].v = v;
  l->n++;
}


static void caselist_add (LexState *ls, CaseList *l, ANode *cnd, ANode *f,
                          int kind) {
  if (l->n >= l->size) {
    int ns = (l->size == 0) ? 4 : l->size * 2;
    Case *na = cast(Case *, luaA_alloc(ls, cast_sizet(ns) * sizeof(Case)));
    if (l->n > 0) memcpy(na, l->arr, cast_sizet(l->n) * sizeof(Case));
    l->arr = na;
    l->size = ns;
  }
  l->arr[l->n].cnd = cnd;
  l->arr[l->n].f = f;
  l->arr[l->n].kind = cast_byte(kind);
  l->n++;
}

/* }====================================================== */


/*
** {======================================================
** Token helpers (TK0 = previous token, TK1 = current token)
** =======================================================
*/

#define TK0(ls)	(&(ls)->prev)
#define TK1(ls)	(&(ls)->t)

#define samesep(ls)	(TK0(ls)->sep == TK1(ls)->sep)


l_noret luaA_error (LexState *ls, int line, TString *near,
                    const char *msg) {
  const char *n = (near != NULL)
                  ? luaO_pushfstring(ls->L, "'%s'", getstr(near))
                  : NULL;
  luaX_atmerror(ls, line, n, msg);
}


/* error at token 't' */
static l_noret err (LexState *ls, const Token *t, const char *msg) {
  luaX_atmerror(ls, t->line, luaX_atmtoktext(ls, t), msg);
}


/* text of token 't' as a string (for 'near' in nodes) */
static TString *toktext (LexState *ls, const Token *t) {
  if (t->text != NULL) return t->text;
  switch (t->token) {
    case TK_NAME: case TK_STRING: case TK_TAG: case TK_NAT:
      return t->seminfo.ts;
    case TK_EOS:
      return luaX_newstring(ls, "<eof>", 5);
    default: {
      if (t->token < FIRST_RESERVED) {
        char c = cast_char(t->token);
        return luaX_newstring(ls, &c, 1);
      }
      else {
        const char *s = luaX_token2str(ls, t->token);  /* "'xxx'" */
        size_t l = strlen(s);
        return luaX_newstring(ls, s + 1, l - 2);
      }
    }
  }
}


static int check (LexState *ls, int token) {
  return (TK1(ls)->token == token);
}


/* current token is a name with the given string? */
static int checkname (LexState *ls, const char *name) {
  return (TK1(ls)->token == TK_NAME &&
          strcmp(getstr(TK1(ls)->seminfo.ts), name) == 0);
}


/* current token is a tag with the given string (without ':')? */
static int check_tag (LexState *ls, const char *name) {
  return (TK1(ls)->token == TK_TAG &&
          strcmp(getstr(TK1(ls)->seminfo.ts), name) == 0);
}


static const char *tokdesc (LexState *ls, int token) {
  switch (token) {
    case TK_NAME: return "<id>";
    case TK_STRING: return "<str>";
    case TK_TAG: return "<tag>";
    case TK_INT: case TK_FLT: return "<num>";
    case TK_NAT: return "<nat>";
    case TK_CLK: return "<clk>";
    case TK_EOS: return "'<eof>'";
    default: return luaX_token2str(ls, token);
  }
}


static void check_err (LexState *ls, int token) {
  if (!check(ls, token))
    err(ls, TK1(ls), luaO_pushfstring(ls->L, "expected %s", tokdesc(ls, token)));
}


static int accept (LexState *ls, int token) {
  if (check(ls, token)) {
    luaX_next(ls);
    return 1;
  }
  return 0;
}




static void accept_err (LexState *ls, int token) {
  check_err(ls, token);
  luaX_next(ls);
}


/* field name: keyword or identifier; returns its string */
static TString *accept_field_err (LexState *ls) {
  int tk = TK1(ls)->token;
  if (tk == TK_NAME) {
    TString *ts = TK1(ls)->seminfo.ts;
    luaX_next(ls);
    return ts;
  }
  else if (tk >= FIRST_RESERVED && tk <= TK_WITH) {  /* keyword */
    TString *ts = toktext(ls, TK1(ls));
    luaX_next(ls);
    return ts;
  }
  else {
    check_err(ls, TK_NAME);
    return NULL;  /* to avoid warnings */
  }
}


static int is_binop (int tk) {
  switch (tk) {
    case TK_EQ: case TK_LNE: case TK_DEQ: case TK_DNE: case TK_SUP: case TK_SUB:
    case TK_IS: case TK_ISNOT:
    case '+': case '-': case '*': case TK_POW: case '/': case TK_IDIV: case '%':
    case '>': case '<': case TK_GE: case TK_LE:
    case TK_LOR: case TK_LAND:
    case TK_CAT:
    case TK_XIN: case TK_XNOTIN:
      return 1;
    default:
      return 0;
  }
}


static int is_unop (int tk) {
  return (tk == '-' || tk == '#' || tk == TK_LNOT);
}


static int is_optoken (int tk) {
  return (is_binop(tk) || is_unop(tk) || tk == '=' || tk == TK_ARROW ||
          tk == TK_LPIPE1 || tk == TK_LPIPE2 || tk == TK_RPIPE1 ||
          tk == TK_RPIPE2);
}

/* }====================================================== */


/*
** {======================================================
** ANode constructors
** =======================================================
*/

static ANode *mknode (LexState *ls, int tag, const Token *t) {
  ANode *n = newnode(ls);
  memset(n, 0, sizeof(ANode));
  n->tag = cast_byte(tag);
  if (t != NULL) {
    n->line = t->line;
    n->text = toktext(ls, t);
  }
  else {
    n->line = ls->lastline;
  }
  return n;
}


static ANode *mkrt (LexState *ls, int rt, int line) {
  ANode *n = mknode(ls, N_RT, NULL);
  n->u.rt = rt;
  n->line = line;
  n->text = luaX_newstring(ls, luaA_rtnames[rt], strlen(luaA_rtnames[rt]));
  return n;
}


static ANode *mkacc (LexState *ls, TString *name, int line) {
  ANode *n = mknode(ls, N_ACC, NULL);
  n->u.ts = name;
  n->text = name;
  n->line = line;
  return n;
}


static ANode *mkstr (LexState *ls, TString *s, int line) {
  ANode *n = mknode(ls, N_STR, NULL);
  n->u.ts = s;
  n->text = s;
  n->line = line;
  return n;
}


static ANode *mktag (LexState *ls, TString *s, int line) {
  ANode *n = mknode(ls, N_TAG, NULL);
  n->u.ts = s;
  n->text = s;
  n->line = line;
  return n;
}


static ANode *mkint (LexState *ls, lua_Integer i, int line) {
  ANode *n = mknode(ls, N_NUM, NULL);
  n->u.i = i;
  n->line = line;
  return n;
}



static ANode *mkcall (LexState *ls, ANode *f, int line) {
  ANode *n = mknode(ls, N_CALL, NULL);
  n->u.call.f = f;
  n->line = line;
  n->text = f->text;
  return n;
}


static ANode *mkcall1 (LexState *ls, ANode *f, ANode *a1, int line) {
  ANode *n = mkcall(ls, f, line);
  nlist_add(ls, &n->u.call.es, a1);
  return n;
}


static ANode *mkblock (LexState *ls, int line) {
  ANode *n = mknode(ls, N_BLOCK, NULL);
  n->line = line;
  return n;
}


static ANode *mkproto (LexState *ls, int sub, ANode *blk, int line) {
  ANode *n = mknode(ls, N_PROTO, NULL);
  n->sub = sub;
  n->u.proto.blk = blk;
  n->line = line;
  return n;
}


/* proto with no parameters and a block of statements */
static ANode *mklua (LexState *ls, ANode *blk) {
  return mkproto(ls, P_LUA, blk, blk->line);
}


static ANode *mkbin (LexState *ls, int op, ANode *e1, ANode *e2, int line) {
  ANode *n = mknode(ls, N_BIN, NULL);
  n->sub = op;
  n->u.bin.e1 = e1;
  n->u.bin.e2 = e2;
  n->line = line;
  n->text = e1->text;
  return n;
}


static ANode *mkuno (LexState *ls, int op, ANode *e, int line) {
  ANode *n = mknode(ls, N_UNO, NULL);
  n->sub = op;
  n->u.uno.e = e;
  n->line = line;
  n->text = e->text;
  return n;
}


static ANode *mkindex (LexState *ls, ANode *t, ANode *idx, int line) {
  ANode *n = mknode(ls, N_INDEX, NULL);
  n->u.index.t = t;
  n->u.index.idx = idx;
  n->line = line;
  n->text = t->text;
  return n;
}


static ANode *mkdcl (LexState *ls, int kind, int line) {
  ANode *n = mknode(ls, N_DCL, NULL);
  n->sub = kind;
  n->line = line;
  return n;
}

/* }====================================================== */


/*
** {======================================================
** Grammar
** =======================================================
*/

static ANode *parse_expr (LexState *ls);
static ANode *parse_1_prim (LexState *ls);
static ANode *parse_2_suf (LexState *ls, ANode *pre);
static ANode *parse_3_met (LexState *ls, ANode *pre);
static ANode *parse_4_pre (LexState *ls, ANode *pre);
static ANode *parse_5_bin (LexState *ls, ANode *pre);
static ANode *parse_6_pip (LexState *ls, ANode *pre);
static ANode *parse_7_out (LexState *ls, ANode *pre);
static ANode *parse_block (LexState *ls);
static ANode *parse_await (LexState *ls);
static ANode *parse_lambda (LexState *ls);


#define enterlevel(ls)	luaE_incCstack(ls->L)
#define leavelevel(ls)	((ls)->L->nCcalls--)


/*
** Parse a comma-separated list of expressions until token 'clo'
** (which is not consumed).
*/
static void parse_exprlist (LexState *ls, NList *l, int clo) {
  if (check(ls, clo)) return;
  nlist_add(ls, l, parse_expr(ls));
  while (!check(ls, clo)) {
    if (!accept(ls, ','))
      return;
    if (check(ls, clo)) return;
    nlist_add(ls, l, parse_expr(ls));
  }
}


/*
** Parse a non-empty list of identifiers separated by commas.
*/
static void parse_ids (LexState *ls, NList *l) {
  const Token *tk0 = TK0(ls);
  int line = tk0->line;
  TString *text = toktext(ls, tk0);
  do {
    check_err(ls, TK_NAME);
    nlist_add(ls, l, mkacc(ls, TK1(ls)->seminfo.ts, TK1(ls)->line));
    luaX_next(ls);
  } while (accept(ls, ','));
  if (l->n == 0)
    luaA_error(ls, line, text, "unexpected empty list");
}


/*
** Parse function parameters: [ID {',' ID}] ['...']. Returns vararg flag.
*/
static int parse_dots_pars (LexState *ls, NList *pars) {
  if (accept(ls, TK_DOTS))
    return 1;
  if (check(ls, ')'))
    return 0;
  check_err(ls, TK_NAME);
  nlist_add(ls, pars, mkacc(ls, TK1(ls)->seminfo.ts, TK1(ls)->line));
  luaX_next(ls);
  while (!check(ls, ')')) {
    accept_err(ls, ',');
    if (accept(ls, TK_DOTS))
      return 1;
    check_err(ls, TK_NAME);
    nlist_add(ls, pars, mkacc(ls, TK1(ls)->seminfo.ts, TK1(ls)->line));
    luaX_next(ls);
  }
  return 0;
}


/*
** Parse a sequence of statements until token 'clo' (not consumed).
** Statements must be separated by newlines or ';'.
*/
static void parse_stmts (LexState *ls, NList *l, int clo) {
  int i = 0;
  while (!check(ls, clo)) {
    if (i > 0 && samesep(ls))
      err(ls, TK1(ls), "sequence error : expected ';' or new line");
    nlist_add(ls, l, parse_expr(ls));
    i++;
  }
}


static ANode *parse_block (LexState *ls) {
  int line = TK1(ls)->line;
  ANode *blk;
  accept_err(ls, '{');
  blk = mkblock(ls, line);
  parse_stmts(ls, &blk->u.es.es, '}');
  blk->line2 = TK1(ls)->line;
  accept_err(ls, '}');
  return blk;
}


/* parse a block and wrap it as a Lua closure (no parameters) */
static ANode *parse_luablock (LexState *ls) {
  return mklua(ls, parse_block(ls));
}


static ANode *parse_lambda (LexState *ls) {
  int line = TK1(ls)->line;
  accept_err(ls, '\\');
  if (!is_optoken(TK1(ls)->token)) {  /* normal lambda: \(){} */
    ANode *p = mkproto(ls, P_FUNC, NULL, line);
    if (accept(ls, '(')) {
      p->u.proto.dots = cast_byte(parse_dots_pars(ls, &p->u.proto.pars));
      accept_err(ls, ')');
    }
    else if (check(ls, TK_NAME)) {
      nlist_add(ls, &p->u.proto.pars,
                mkacc(ls, TK1(ls)->seminfo.ts, TK1(ls)->line));
      luaX_next(ls);
    }
    else {  /* implicit 'it' */
      nlist_add(ls, &p->u.proto.pars,
                mkacc(ls, luaX_newstring(ls, "it", 2), line));
    }
    check_err(ls, '{');
    p->u.proto.blk = parse_block(ls);
    return p;
  }
  else {  /* lambda operator: \- \++ */
    int op = TK1(ls)->token;
    ANode *p = mkproto(ls, P_FUNC, NULL, line);
    ANode *blk = mkblock(ls, line);
    TString *sa = luaX_newstring(ls, "a", 1);
    TString *sb = luaX_newstring(ls, "b", 1);
    if (is_binop(op)) {
      luaX_next(ls);
      nlist_add(ls, &p->u.proto.pars, mkacc(ls, sa, line));
      nlist_add(ls, &p->u.proto.pars, mkacc(ls, sb, line));
      nlist_add(ls, &blk->u.es.es,
                mkbin(ls, op, mkacc(ls, sa, line), mkacc(ls, sb, line), line));
    }
    else if (is_unop(op)) {
      luaX_next(ls);
      nlist_add(ls, &p->u.proto.pars, mkacc(ls, sa, line));
      nlist_add(ls, &blk->u.es.es, mkuno(ls, op, mkacc(ls, sa, line), line));
    }
    else
      err(ls, TK1(ls), "lambda error : invalid operator");
    p->u.proto.blk = blk;
    return p;
  }
}


/*
** @-qualifier (after '@' is consumed): @(e) | @num | @id | @:tag.
** If 'ret' is true, returns NULL on no-match (so the caller can
** handle the @# and @+ tip markers).
*/
static ANode *parse_at (LexState *ls, int ret) {
  if (accept(ls, '(')) {
    ANode *e = parse_expr(ls);
    accept_err(ls, ')');
    return e;
  }
  else if (check(ls, TK_TAG)) {  /* separate because of prim :X() */
    ANode *e = mktag(ls, TK1(ls)->seminfo.ts, TK1(ls)->line);
    e->text = toktext(ls, TK1(ls));
    luaX_next(ls);
    return e;
  }
  else if (check(ls, TK_INT) || check(ls, TK_FLT) || check(ls, TK_NAME)) {
    return parse_1_prim(ls);
  }
  else if (ret) {
    return NULL;
  }
  else {
    err(ls, TK1(ls), "expected name, number, tag, or '('");
    return NULL;  /* to avoid warnings */
  }
}


static int is_prefix (const ANode *e) {
  switch (e->tag) {
    case N_TAG: case N_ACC: case N_RT: case N_NAT: case N_CALL: case N_MET:
    case N_INDEX: case N_PARENS:
      return 1;
    default:
      return 0;
  }
}


/*
** tokens that can start a juxtaposed call argument (f "s", f [t], f :X);
** also gates what may start a bare await pattern
*/
static int check_call_arg (LexState *ls) {
  int tk = TK1(ls)->token;
  return (tk == '[' || tk == '\\' || tk == TK_STRING || tk == TK_TAG ||
          tk == TK_NAT || tk == TK_CLK);
}


static ANode *parse_2_suf (LexState *ls, ANode *pre) {
  int no = (check(ls, TK_EMIT) || check(ls, TK_SPAWN) ||
            check(ls, TK_TOGGLE) || check(ls, TK_THREAD));
  ANode *e = (pre != NULL) ? pre : parse_1_prim(ls);
  ANode *ret;
  int ok = (!no) && is_prefix(e) &&
           (samesep(ls) || check(ls, '@') || check(ls, '.') ||
            check(ls, TK_DBCOLON));
  if (!ok)
    return e;
  if (accept(ls, '@')) {
    const Token *tk0 = TK0(ls);  /* '@' */
    int line = tk0->line;
    ANode *idx = parse_at(ls, 1);  /* @(e) | @num | @id (or NULL) */
    if (idx == NULL) {
      if (accept(ls, '#'))  /* t@#  (last item) */
        idx = mkuno(ls, '#', e, line);
      else if (accept(ls, '+'))  /* t@+  (next item: #t+1) */
        idx = mkbin(ls, '+', mkuno(ls, '#', e, line), mkint(ls, 1, line), line);
      else
        err(ls, TK1(ls), "expected name, number, or '('");
    }
    ret = mkindex(ls, e, idx, line);
  }
  else if (accept(ls, '.')) {  /* (t) .id */
    int line = TK0(ls)->line;
    TString *id = accept_field_err(ls);
    ret = mkindex(ls, e, mktag(ls, id, line), line);
  }
  else if (accept(ls, TK_DBCOLON)) {  /* (o) ::m */
    int line = TK0(ls)->line;
    TString *id = accept_field_err(ls);
    ANode *m;
    if (!check_call_arg(ls))
      check_err(ls, '(');
    m = mknode(ls, N_MET, NULL);
    m->u.met.o = e;
    m->u.met.met = id;
    m->line = line;
    m->text = id;
    ret = m;
  }
  else if (check(ls, '(')) {  /* (f) (...) */
    int line = TK1(ls)->line;
    luaX_next(ls);
    ret = mkcall(ls, e, line);
    parse_exprlist(ls, &ret->u.call.es, ')');
    accept_err(ls, ')');
  }
  else if (check_call_arg(ls)) {
    int line = TK1(ls)->line;
    ANode *v = parse_1_prim(ls);
    ret = mkcall1(ls, e, v, line);
  }
  else {
    return e;  /* nothing consumed, not a suffix */
  }
  return parse_2_suf(ls, ret);
}


/*
** Insert argument 'e' into call 'f' (as first if 'pre', else as last);
** if 'f' is not a call, build the call f(e).
*/
static ANode *pipe (LexState *ls, ANode *f, ANode *e, int pre) {
  ANode *out = f;
  while (f->tag == N_PARENS)
    f = f->u.uno.e;
  if (f->tag == N_CALL) {
    if (pre)
      nlist_insert(ls, &f->u.call.es, 0, e);
    else
      nlist_add(ls, &f->u.call.es, e);
    return out;
  }
  else
    return mkcall1(ls, out, e, e->line);
}


static ANode *parse_3_met (LexState *ls, ANode *pre) {
  ANode *e = (pre != NULL) ? pre : parse_2_suf(ls, NULL);
  if (accept(ls, TK_RPIPE1))
    return parse_3_met(ls, pipe(ls, parse_2_suf(ls, NULL), e, 1));
  else if (accept(ls, TK_LPIPE1))
    return pipe(ls, e, parse_3_met(ls, parse_2_suf(ls, NULL)), 0);
  else
    return e;
}


static ANode *parse_4_pre (LexState *ls, ANode *pre) {
  if (pre != NULL || !is_unop(TK1(ls)->token))
    return parse_3_met(ls, pre);
  else {
    int op = TK1(ls)->token;
    int line = TK1(ls)->line;
    ANode *e;
    luaX_next(ls);
    enterlevel(ls);
    e = parse_4_pre(ls, NULL);
    leavelevel(ls);
    return mkuno(ls, op, e, line);
  }
}


static ANode *parse_5_bin (LexState *ls, ANode *pre) {
  ANode *e1 = (pre != NULL) ? pre : parse_4_pre(ls, NULL);
  for (;;) {
    int op, line;
    ANode *e2;
    if (!is_binop(TK1(ls)->token))
      return e1;
    op = TK1(ls)->token;
    line = TK1(ls)->line;
    if (e1->tag == N_BIN && e1 == pre && e1->sub != op)
      err(ls, TK1(ls), "operation error : use parentheses to disambiguate");
    luaX_next(ls);
    e2 = parse_4_pre(ls, NULL);
    e1 = mkbin(ls, op, e1, e2, line);
    pre = e1;  /* chained operators must be the same */
  }
}


static ANode *parse_6_pip (LexState *ls, ANode *pre) {
  ANode *e = (pre != NULL) ? pre : parse_5_bin(ls, NULL);
  ANode *ret;
  int op;
  if (check(ls, TK_RPIPE2)) op = TK_RPIPE2;
  else if (check(ls, TK_LPIPE2)) op = TK_LPIPE2;
  else op = 0;
  if (pre != NULL && op != 0 && pre->tag == N_CALL &&
      pre->u.call.pipeop != 0 && pre->u.call.pipeop != op)
    err(ls, TK1(ls), "operation error : use parentheses to disambiguate");
  if (accept(ls, TK_RPIPE2)) {
    ret = pipe(ls, parse_5_bin(ls, NULL), e, 1);
  }
  else if (accept(ls, TK_LPIPE2)) {  /* right to left */
    ANode *r = parse_6_pip(ls, NULL);
    if (r->tag == N_CALL && r->u.call.pipeop == TK_RPIPE2)
      err(ls, TK1(ls), "operation error : use parentheses to disambiguate");
    return pipe(ls, e, r, 0);
  }
  else
    return e;  /* nothing consumed, not an out */
  if (ret->tag == N_CALL)
    ret->u.call.pipeop = op;
  return parse_6_pip(ls, ret);
}


static ANode *parse_7_out (LexState *ls, ANode *pre) {
  ANode *e = (pre != NULL) ? pre : parse_6_pip(ls, NULL);
  if (accept(ls, TK_WHERE)) {
    int line = TK0(ls)->line;
    ANode *blk = mkblock(ls, line);
    ANode *f, *ret;
    accept_err(ls, '{');
    while (!check(ls, '}')) {
      ANode *d = mkdcl(ls, TK_VAL, TK1(ls)->line);
      parse_ids(ls, &d->u.dcl.ids);
      accept_err(ls, '=');
      d->u.dcl.set = parse_expr(ls);
      nlist_add(ls, &blk->u.es.es, d);
    }
    accept_err(ls, '}');
    nlist_add(ls, &blk->u.es.es, e);
    f = mklua(ls, blk);
    ret = mkcall(ls, f, line);
    ret->u.call.pipeop = TK_WHERE;
    return parse_7_out(ls, ret);
  }
  else
    return e;  /* nothing consumed, not an out */
}


static ANode *parse_expr (LexState *ls) {
  ANode *e;
  enterlevel(ls);
  e = parse_7_out(ls, NULL);
  leavelevel(ls);
  return e;
}


/*
** {======================================================
** Await patterns (see await.lua in the reference implementation)
**
** await / watching / loop-on / toggle-with take one of three tiers:
**      bare    : await P      -- one primary (a call spawns) + optional
**                                until (c) / while (c)
**      value   : await(E)     -- a plain value expression
**      pattern : await<PAT>   -- the combinator grammar
** =======================================================
*/

/*
** Builds the runtime combinator table {tag=name, [1]=..., n=count}.
** 'patname' marks compiler-built patterns.
*/
static ANode *mk_tagged (LexState *ls, const char *name, ANode **items, int n,
                        int line) {
  ANode *t = mknode(ls, N_TABLE, NULL);
  TString *tag = luaX_newstring(ls, "tag", 3);
  TString *nn = luaX_newstring(ls, "n", 1);
  TString *sname = luaX_newstring(ls, name, strlen(name));
  int i;
  t->line = line;
  t->flags |= 1;
  t->u.table.patname = sname;
  t->u.table.nitems = n;
  kvlist_add(ls, &t->u.table.es, mktag(ls, tag, line), mkstr(ls, sname, line));
  for (i = 0; i < n; i++)
    kvlist_add(ls, &t->u.table.es, mkint(ls, i + 1, line), items[i]);
  kvlist_add(ls, &t->u.table.es, mktag(ls, nn, line), mkint(ls, n, line));
  return t;
}


/*
** until/while predicate: '(E)' -- a \{} function, or an expression
** wrapped as \it -> e.
*/
static ANode *parse_pred (LexState *ls) {
  ANode *e;
  int line = TK1(ls)->line;
  accept_err(ls, '(');
  e = parse_expr(ls);
  accept_err(ls, ')');
  if (e->tag == N_PROTO) {  /* (\{...}) */
    return e;
  }
  else {  /* (x > 10) */
    ANode *blk = mkblock(ls, line);
    ANode *p = mkproto(ls, P_LUA, blk, line);
    nlist_add(ls, &blk->u.es.es, e);
    nlist_add(ls, &p->u.proto.pars, mkacc(ls, luaX_newstring(ls, "it", 2), line));
    return p;
  }
}


static ANode *parse_await_3_bin (LexState *ls);


/* level 1: leaf patterns */
static ANode *parse_await_1_prim (LexState *ls) {
  int line = TK1(ls)->line;
  if (check_tag(ls, "any") || check_tag(ls, "all")) {
    ANode *t = mknode(ls, N_TABLE, NULL);
    TString *mode = TK1(ls)->seminfo.ts;
    ANode *tsks;
    luaX_next(ls);
    t->line = line;
    t->flags |= 1;
    t->u.table.patname = luaX_newstring(ls, "tasks", 5);
    t->u.table.nitems = 0;
    tsks = parse_2_suf(ls, NULL);
    kvlist_add(ls, &t->u.table.es, mktag(ls, luaX_newstring(ls, "tag", 3), line),
               mkstr(ls, t->u.table.patname, line));
    kvlist_add(ls, &t->u.table.es, mktag(ls, luaX_newstring(ls, "mode", 4), line),
               mkstr(ls, mode, line));
    kvlist_add(ls, &t->u.table.es, mktag(ls, luaX_newstring(ls, "tasks", 5), line),
               tsks);
    return t;
  }
  else if (check(ls, TK_UNTIL) || check(ls, TK_WHILE)) {  /* bare predicate */
    const char *name = check(ls, TK_UNTIL) ? "until" : "while";
    ANode *items[1];
    luaX_next(ls);
    items[0] = parse_pred(ls);
    return mk_tagged(ls, name, items, 1, line);
  }
  else if (accept(ls, '<')) {  /* nested pattern group */
    ANode *p = parse_await_3_bin(ls);
    accept_err(ls, '>');
    return p;
  }
  else if (accept(ls, '(')) {  /* value leaf */
    ANode *e = parse_expr(ls);
    ANode *p;
    accept_err(ls, ')');
    p = mknode(ls, N_PARENS, NULL);
    p->u.uno.e = e;
    p->line = line;
    p->text = e->text;
    return p;
  }
  else {  /* bare primary; a lone call spawns */
    ANode *e = parse_2_suf(ls, NULL);
    if (e->tag == N_CALL &&
        !(e->u.call.f->tag == N_RT && e->u.call.f->u.rt == RT_TAG_DO)) {
      int n = e->u.call.es.n + 1;
      ANode **items = cast(ANode **, luaA_alloc(ls, cast_sizet(n) * sizeof(ANode *)));
      int i;
      items[0] = e->u.call.f;
      for (i = 0; i < e->u.call.es.n; i++)
        items[i + 1] = e->u.call.es.arr[i];
      return mk_tagged(ls, "spawn", items, n, line);
    }
    return e;
  }
}


/* level 2: !p */
static ANode *parse_await_2_pre (LexState *ls) {
  if (check(ls, TK_LNOT)) {
    int line = TK1(ls)->line;
    ANode *items[1];
    luaX_next(ls);
    items[0] = parse_await_2_pre(ls);
    return mk_tagged(ls, "not", items, 1, line);
  }
  else
    return parse_await_1_prim(ls);
}


/* level 3: p&&p | p||p | p until c | p while c (same-op chaining only) */
static ANode *parse_await_3_bin (LexState *ls) {
  ANode *e1 = parse_await_2_pre(ls);
  int op0 = 0;
  for (;;) {
    int op, line;
    if (check(ls, TK_LAND) || check(ls, TK_LOR)) {
      op = TK1(ls)->token;
    }
    else if (samesep(ls) && (check(ls, TK_UNTIL) || check(ls, TK_WHILE))) {
      op = TK1(ls)->token;
    }
    else
      return e1;
    if (op0 != 0 && op0 != op)
      err(ls, TK1(ls), "operation error : use parentheses to disambiguate");
    line = TK1(ls)->line;
    luaX_next(ls);
    op0 = op;
    if (op == TK_UNTIL || op == TK_WHILE) {
      ANode *items[2];
      items[0] = e1;
      items[1] = parse_pred(ls);
      e1 = mk_tagged(ls, (op == TK_UNTIL) ? "until" : "while", items, 2, line);
    }
    else {
      ANode *items[2];
      items[0] = e1;
      items[1] = parse_await_2_pre(ls);
      e1 = mk_tagged(ls, (op == TK_LAND) ? "and" : "or", items, 2, line);
    }
  }
}


static int ispat (const ANode *e, const char *name) {
  return (e->tag == N_TABLE && (e->flags & 1) &&
          strcmp(getstr(e->u.table.patname), name) == 0);
}


/*
** Parses one await pattern in one of three tiers:
**   value : await(E)
**   patt  : await<PAT>
**   bare  : await P
*/
static ANode *parse_await (LexState *ls) {
  int patt;
  ANode *pat;
  if (accept(ls, '(')) {  /* value */
    ANode *e = parse_expr(ls);
    accept_err(ls, ')');
    return e;
  }
  patt = accept(ls, '<');
  pat = parse_await_3_bin(ls);
  if (patt)
    accept_err(ls, '>');
  if (!patt) {  /* bare form: reject combinators and loose values */
    ANode *e = pat;
    int ok;
    if ((ispat(e, "until") || ispat(e, "while")) && e->u.table.nitems == 1)
      e = e->u.table.es.arr[1].v;  /* the predicate */
    if (e->tag == N_TABLE && (e->flags & 1))
      ok = !(ispat(e, "and") || ispat(e, "or") || ispat(e, "not"));
    else {
      switch (e->tag) {
        case N_TAG: case N_CLK: case N_STR: case N_NAT: case N_BOOL:
        case N_TABLE: case N_PROTO: case N_CALL:
          ok = 1; break;
        default:
          ok = 0; break;
      }
    }
    if (!ok)
      err(ls, TK1(ls), "invalid await : unexpected expression");
  }
  return pat;
}

/* }====================================================== */


/*
** spawn { ... } | spawn [@ts] T(...)
** Returns the resulting expression in '*out' and the actual spawn call
** in '*spw'.
*/
static void parse_spawn (LexState *ls, ANode **out, ANode **spw) {
  int line;
  accept_err(ls, TK_SPAWN);
  line = TK0(ls)->line;
  if (check(ls, '{')) {  /* spawn { ... } */
    ANode *f = mkrt(ls, RT_DO_SPAWN, line);
    ANode *call = mkcall1(ls, f, parse_luablock(ls), line);
    *out = *spw = call;
  }
  else {  /* spawn @ts T(...)  |  spawn @(e) T(...) */
    const Token tk = *TK0(ls);
    ANode *ts = NULL;
    ANode *call, *s;
    if (accept(ls, '@'))
      ts = parse_at(ls, 0);
    call = parse_6_pip(ls, NULL);
    if (call->tag != N_CALL)
      err(ls, &tk, "expected call syntax");
    nlist_insert(ls, &call->u.call.es, 0, call->u.call.f);
    if (ts != NULL) {
      nlist_insert(ls, &call->u.call.es, 0, ts);
      s = mkcall(ls, mkrt(ls, RT_SPAWN_IN, tk.line), tk.line);
    }
    else
      s = mkcall(ls, mkrt(ls, RT_SPAWN, tk.line), tk.line);
    s->u.call.es = call->u.call.es;
    *spw = s;
    *out = parse_7_out(ls, s);
  }
}


/* parse a case body: block, lambda, or single expression */
static ANode *parse_case_body (LexState *ls) {
  if (check(ls, '{'))
    return parse_luablock(ls);
  else if (check(ls, '\\'))
    return parse_lambda(ls);
  else {
    ANode *blk = mkblock(ls, TK1(ls)->line);
    nlist_add(ls, &blk->u.es.es, parse_expr(ls));
    return mklua(ls, blk);
  }
}


static ANode *parse_1_prim (LexState *ls) {
  const Token *t = TK1(ls);
  int line = t->line;
  switch (t->token) {
    /* literals */
    case TK_NIL: {
      ANode *n = mknode(ls, N_NIL, t);
      luaX_next(ls);
      return n;
    }
    case TK_TRUE: case TK_FALSE: {
      ANode *n = mknode(ls, N_BOOL, t);
      n->u.b = (t->token == TK_TRUE);
      luaX_next(ls);
      return n;
    }
    case TK_DOTS: {
      ANode *n = mknode(ls, N_DOTS, t);
      luaX_next(ls);
      return n;
    }
    case TK_INT: {
      ANode *n = mknode(ls, N_NUM, t);
      n->u.i = t->seminfo.i;
      luaX_next(ls);
      return n;
    }
    case TK_FLT: {
      ANode *n = mknode(ls, N_NUM, t);
      n->u.r = t->seminfo.r;
      n->flags |= NFLT;
      luaX_next(ls);
      return n;
    }
    case TK_STRING: {
      ANode *n = mknode(ls, N_STR, t);
      n->u.ts = t->seminfo.ts;
      luaX_next(ls);
      return n;
    }
    case TK_NAT: {
      ANode *n = mknode(ls, N_NAT, t);
      n->u.ts = t->seminfo.ts;
      luaX_next(ls);
      return n;
    }
    case TK_CLK: {
      ANode *n = mknode(ls, N_CLK, t);
      /* the lexer stores floats in 'r' and integers in 'i'; it cannot
         tell us which one was used, so it encodes floats by making
         the text contain a '.' */
      if (strchr(getstr(t->text), '.') != NULL) {
        n->u.r = t->seminfo.r;
        n->flags |= NFLT;
      }
      else
        n->u.i = t->seminfo.i;
      luaX_next(ls);
      return n;
    }
    /* id: x, __v */
    case TK_NAME: {
      ANode *n = mkacc(ls, t->seminfo.ts, line);
      luaX_next(ls);
      return n;
    }
    /* tag */
    case TK_TAG: {
      ANode *e = mktag(ls, t->seminfo.ts, line);
      e->text = toktext(ls, t);
      luaX_next(ls);
      if ((check(ls, '(') || check(ls, '[')) && samesep(ls)) {
        /* (:X) [...] */
        ANode *tb = parse_1_prim(ls);
        ANode *f = mkrt(ls, RT_TAG_DO, line);
        ANode *c = mkcall1(ls, f, e, line);
        nlist_add(ls, &c->u.call.es, tb);
        return c;
      }
      else
        return e;
    }
    /* table: [...] */
    case '[': {
      ANode *tb = mknode(ls, N_TABLE, t);
      lua_Integer idx = 1;
      luaX_next(ls);
      while (!check(ls, ']')) {
        ANode *key, *val;
        if (accept(ls, '@')) {  /* computed key: @(e)=v  @id=v  @5=v */
          key = parse_at(ls, 0);
          accept_err(ls, '=');
          val = parse_expr(ls);
        }
        else {
          ANode *e = parse_expr(ls);
          if (e->tag == N_ACC && accept(ls, '=')) {
            key = mktag(ls, e->u.ts, e->line);
            val = parse_expr(ls);
          }
          else {
            key = mkint(ls, idx++, e->line);
            val = e;
          }
        }
        kvlist_add(ls, &tb->u.table.es, key, val);
        if (!accept(ls, ','))
          break;
      }
      accept_err(ls, ']');
      return tb;
    }
    /* parens: (...) */
    case '(': {
      NList es;
      memset(&es, 0, sizeof(es));
      luaX_next(ls);
      parse_exprlist(ls, &es, ')');
      accept_err(ls, ')');
      if (es.n == 1) {
        ANode *p = mknode(ls, N_PARENS, t);
        p->u.uno.e = es.arr[0];
        return p;
      }
      else {
        ANode *p = mknode(ls, N_ES, t);
        p->u.es.es = es;
        return p;
      }
    }
    /* emit @t (...) */
    case TK_EMIT: {
      Token tk;
      ANode *to = NULL;
      ANode *cmd, *call;
      luaX_next(ls);
      tk = *TK0(ls);
      if (accept(ls, '@')) {
        to = parse_at(ls, 0);
        cmd = mkrt(ls, RT_EMIT_IN, line);
      }
      else
        cmd = mkrt(ls, RT_EMIT, line);
      call = parse_6_pip(ls, parse_5_bin(ls, parse_4_pre(ls,
               parse_3_met(ls, parse_2_suf(ls, cmd)))));
      if (call->tag != N_CALL)
        err(ls, &tk, "expected call syntax");
      if (call->u.call.es.n != 1)
        err(ls, &tk, "expected single argument");
      if (to != NULL)
        nlist_insert(ls, &call->u.call.es, 0, to);
      return parse_7_out(ls, call);
    }
    /* await P | await(E) | await<PAT> */
    case TK_AWAIT: {
      ANode *awt;
      luaX_next(ls);
      awt = parse_await(ls);
      return mkcall1(ls, mkrt(ls, RT_AWAIT, line), awt, line);
    }
    /* spawn {}, spawn T() */
    case TK_SPAWN: {
      ANode *out, *spw;
      parse_spawn(ls, &out, &spw);
      if (spw->u.call.f->u.rt != RT_SPAWN_IN) {
        /* force "pin" if no "in" target; 'spw' marks real spawns (not
           transparent do_spawn) so that the coder unwraps them at
           value-position block tails */
        ANode *d = mkdcl(ls, TK_PIN, line);
        nlist_add(ls, &d->u.dcl.ids, mkacc(ls, luaX_newstring(ls, "_", 1), line));
        d->u.dcl.set = out;
        d->u.dcl.spw = (spw->u.call.f->u.rt == RT_SPAWN) ? spw : NULL;
        return d;
      }
      return out;
    }
    /* toggle on :T {...} | toggle t(bool) [with ...] */
    case TK_TOGGLE: {
      luaX_next(ls);
      if (accept(ls, TK_ON)) {
        ANode *tag, *call;
        NList filter;
        int i;
        memset(&filter, 0, sizeof(filter));
        check_err(ls, TK_TAG);
        tag = mktag(ls, TK1(ls)->seminfo.ts, TK1(ls)->line);
        luaX_next(ls);
        if (accept(ls, TK_WITH)) {  /* optional filter pattern */
          const Token tk = *TK0(ls);
          do {
            if (check(ls, '{')) break;
            nlist_add(ls, &filter, parse_await(ls));
          } while (accept(ls, ','));
          if (filter.n == 0)
            err(ls, &tk, "unexpected empty list");
        }
        call = mkcall1(ls, mkrt(ls, RT_TOGGLE, line), tag, line);
        for (i = 0; i < filter.n; i++)
          nlist_add(ls, &call->u.call.es, filter.arr[i]);
        nlist_add(ls, &call->u.call.es, parse_luablock(ls));
        return call;
      }
      else {
        const Token tk = *TK0(ls);
        ANode *cmd = mkrt(ls, RT_TOGGLE, line);
        ANode *call = parse_6_pip(ls, NULL);
        ANode *ret;
        if (call->tag != N_CALL)
          err(ls, &tk, "expected call syntax");
        nlist_insert(ls, &call->u.call.es, 0, call->u.call.f);
        if (accept(ls, TK_WITH)) {  /* optional trailing filter pattern */
          const Token tk2 = *TK0(ls);
          int n0 = call->u.call.es.n;
          do {
            nlist_add(ls, &call->u.call.es, parse_await(ls));
          } while (accept(ls, ','));
          if (call->u.call.es.n == n0)
            err(ls, &tk2, "unexpected empty list");
        }
        ret = mkcall(ls, cmd, line);
        ret->u.call.es = call->u.call.es;
        return parse_7_out(ls, ret);
      }
    }
    /* func, task */
    case TK_TASK: case TK_FUNC: {
      int sub = (t->token == TK_TASK) ? P_TASK : P_FUNC;
      luaX_next(ls);
      /* bare 'task' = running instance ("me"): not a proto header when no
         same-line '(' or name follows */
      if (sub == P_TASK &&
          !((check(ls, '(') || check(ls, TK_NAME)) && samesep(ls)))
        return mkcall(ls, mkrt(ls, RT_XTASK, line), line);
      if (accept(ls, '(')) {  /* anonymous: func () { ... } */
        ANode *p = mkproto(ls, sub, NULL, line);
        p->u.proto.dots = cast_byte(parse_dots_pars(ls, &p->u.proto.pars));
        accept_err(ls, ')');
        p->u.proto.blk = parse_block(ls);
        return p;
      }
      else {  /* func f () / func M.f () / func o::f () */
        TString *id;
        NList idxs;
        TString *met = NULL;
        ANode *p, *dst, *s;
        int i;
        memset(&idxs, 0, sizeof(idxs));
        check_err(ls, TK_NAME);
        id = TK1(ls)->seminfo.ts;
        luaX_next(ls);
        while (accept(ls, '.'))
          nlist_add(ls, &idxs, mkstr(ls, accept_field_err(ls), TK0(ls)->line));
        if (sub == P_FUNC && accept(ls, TK_DBCOLON)) {
          met = accept_field_err(ls);
          nlist_add(ls, &idxs, mkstr(ls, met, TK0(ls)->line));
        }
        accept_err(ls, '(');
        p = mkproto(ls, sub, NULL, line);
        p->u.proto.dots = cast_byte(parse_dots_pars(ls, &p->u.proto.pars));
        accept_err(ls, ')');
        if (met != NULL)
          nlist_insert(ls, &p->u.proto.pars, 0,
                       mkacc(ls, luaX_newstring(ls, "self", 4), line));
        dst = mkacc(ls, id, line);
        for (i = 0; i < idxs.n; i++)
          dst = mkindex(ls, dst, idxs.arr[i], idxs.arr[i]->line);
        p->u.proto.blk = parse_block(ls);
        s = mknode(ls, N_SET, NULL);
        s->line = line;
        s->text = dst->text;
        nlist_add(ls, &s->u.set.dsts, dst);
        s->u.set.src = p;
        return s;
      }
    }
    /* lambda: \{} */
    case '\\': {
      return parse_lambda(ls);
    }
    /* val x = 10 */
    case TK_VAL: case TK_VAR: case TK_PIN: {
      int kind = t->token;
      const Token tk = *t;
      ANode *d;
      luaX_next(ls);
      if (kind == TK_VAL && (check(ls, TK_TASK) || check(ls, TK_FUNC))) {
        int sub = check(ls, TK_TASK) ? P_TASK : P_FUNC;
        ANode *p;
        TString *id;
        luaX_next(ls);
        check_err(ls, TK_NAME);
        id = TK1(ls)->seminfo.ts;
        luaX_next(ls);
        accept_err(ls, '(');
        p = mkproto(ls, sub, NULL, line);
        p->u.proto.dots = cast_byte(parse_dots_pars(ls, &p->u.proto.pars));
        accept_err(ls, ')');
        p->u.proto.blk = parse_block(ls);
        d = mkdcl(ls, kind, line);
        nlist_add(ls, &d->u.dcl.ids, mkacc(ls, id, line));
        d->u.dcl.set = p;
        return d;
      }
      d = mkdcl(ls, kind, line);
      parse_ids(ls, &d->u.dcl.ids);
      if (d->u.dcl.ids.n == 1 && check(ls, '*')) {
        if (kind != TK_PIN)
          err(ls, &tk, "invalid stream variable : expected pin declaration");
        err(ls, TK1(ls), "stream variables are not supported");
      }
      if (accept(ls, '=')) {
        if (check(ls, TK_SPAWN)) {
          ANode *set, *spw;
          parse_spawn(ls, &set, &spw);
          if (spw->u.call.f->u.rt == RT_DO_SPAWN) {
            /* pin t = spawn { ... } : promote the transparent block to an
               anonymous non-transparent task: spawn(task(atm_func(f))) */
            if (kind == TK_PIN) {
              spw->u.call.f->u.rt = RT_SPAWN;
              spw->u.call.es.arr[0]->sub = P_TASK;
            }
            else
              err(ls, &tk, "invalid assignment : unexpected transparent task");
          }
          d->u.dcl.set = set;
        }
        else if (accept(ls, TK_TASKS)) {
          ANode *c = mkcall(ls, mkrt(ls, RT_TASKS, TK0(ls)->line), TK0(ls)->line);
          accept_err(ls, '(');
          if (!check(ls, ')'))
            nlist_add(ls, &c->u.call.es, parse_expr(ls));
          accept_err(ls, ')');
          d->u.dcl.set = c;
        }
        else
          d->u.dcl.set = parse_expr(ls);
      }
      return d;
    }
    /* set x = 10 */
    case TK_SET: {
      ANode *s = mknode(ls, N_SET, t);
      luaX_next(ls);
      do {
        const Token tk = *TK1(ls);
        ANode *e = parse_expr(ls);
        if (!(e->tag == N_ACC || e->tag == N_INDEX || e->tag == N_NAT))
          err(ls, &tk, "expected assignable expression");
        nlist_add(ls, &s->u.set.dsts, e);
      } while (accept(ls, ','));
      accept_err(ls, '=');
      s->u.set.src = parse_expr(ls);
      return s;
    }
    /* do :X {...} | do(...) */
    case TK_DO: {
      luaX_next(ls);
      if (check(ls, TK_TAG) || check(ls, '{')) {
        ANode *d = mknode(ls, N_DO, t);
        if (check(ls, TK_TAG)) {
          d->u.do_.esc = TK1(ls)->seminfo.ts;
          luaX_next(ls);
        }
        d->u.do_.blk = parse_block(ls);
        return d;
      }
      else {
        const Token tk = *TK0(ls);
        ANode *cmd = mkrt(ls, RT_VOID, line);
        ANode *call = parse_6_pip(ls, parse_5_bin(ls, parse_4_pre(ls,
                       parse_3_met(ls, parse_2_suf(ls, cmd)))));
        if (call->tag != N_CALL)
          err(ls, &tk, "expected call syntax");
        return call;
      }
    }
    /* test { ... } */
    case TK_TEST: {
      ANode *d = mknode(ls, N_DO, t);
      ANode *blk;
      luaX_next(ls);
      blk = parse_block(ls);
      if (!getstate(ls)->test)
        blk->u.es.es.n = 0;  /* remove contents */
      d->u.do_.blk = blk;
      return d;
    }
    /* catch cnd { ... } */
    case TK_CATCH: {
      ANode *c = mknode(ls, N_CATCH, t);
      luaX_next(ls);
      c->u.catch_.cnd = parse_expr(ls);
      c->u.catch_.blk = parse_block(ls);
      return c;
    }
    /* defer { ... } */
    case TK_DEFER: {
      ANode *d = mknode(ls, N_DEFER, t);
      luaX_next(ls);
      d->u.defer.blk = parse_block(ls);
      return d;
    }
    /* if x {...} else {...} | if x => y => z */
    case TK_IF: {
      ANode *ifs = mknode(ls, N_IFS, t);
      ANode *cnd;
      luaX_next(ls);
      cnd = parse_expr(ls);
      if (check(ls, '{')) {
        caselist_add(ls, &ifs->u.ifs.cases, cnd, parse_luablock(ls), C_EXPR);
        if (accept(ls, TK_ELSE))
          caselist_add(ls, &ifs->u.ifs.cases, NULL, parse_luablock(ls), C_ELSE);
      }
      else {
        accept_err(ls, TK_ARROW);
        if (check(ls, '\\')) {
          caselist_add(ls, &ifs->u.ifs.cases, cnd, parse_lambda(ls), C_EXPR);
          if (accept(ls, TK_ELSE))
            caselist_add(ls, &ifs->u.ifs.cases, NULL, parse_luablock(ls), C_ELSE);
        }
        else {
          ANode *blk = mkblock(ls, TK1(ls)->line);
          nlist_add(ls, &blk->u.es.es, parse_expr(ls));
          caselist_add(ls, &ifs->u.ifs.cases, cnd, mklua(ls, blk), C_EXPR);
          if (accept(ls, TK_ARROW)) {
            ANode *blk2 = mkblock(ls, TK1(ls)->line);
            nlist_add(ls, &blk2->u.es.es, parse_expr(ls));
            caselist_add(ls, &ifs->u.ifs.cases, NULL, mklua(ls, blk2), C_ELSE);
          }
        }
      }
      return ifs;
    }
    /* ifs { x => a ; y => b ; else => c } */
    case TK_IFS: {
      ANode *ifs = mknode(ls, N_IFS, t);
      luaX_next(ls);
      accept_err(ls, '{');
      while (!check(ls, '}')) {
        int brk = 0;
        ANode *cnd = NULL;
        int kind = C_EXPR;
        if (accept(ls, TK_ELSE)) {
          brk = 1;
          kind = C_ELSE;
        }
        else
          cnd = parse_expr(ls);
        accept_err(ls, TK_ARROW);
        caselist_add(ls, &ifs->u.ifs.cases, cnd, parse_case_body(ls), kind);
        if (brk) break;
      }
      accept_err(ls, '}');
      return ifs;
    }
    /* match e { x => a ; y => b ; else => c } */
    case TK_MATCH: {
      ANode *ifs = mknode(ls, N_IFS, t);
      luaX_next(ls);
      ifs->u.ifs.match = parse_expr(ls);
      accept_err(ls, '{');
      while (!check(ls, '}')) {
        int brk = 0;
        ANode *cnd = NULL;
        int kind;
        if (accept(ls, TK_ELSE)) {
          brk = 1;
          kind = C_ELSE;
        }
        else if (check(ls, '\\')) {
          cnd = parse_lambda(ls);
          kind = C_MATCHFUNC;
        }
        else {
          cnd = parse_expr(ls);
          kind = C_MATCH;
        }
        accept_err(ls, TK_ARROW);
        caselist_add(ls, &ifs->u.ifs.cases, cnd, parse_case_body(ls), kind);
        if (brk) break;
      }
      accept_err(ls, '}');
      return ifs;
    }
    /* loop */
    case TK_LOOP: {
      ANode *lp = mknode(ls, N_LOOP, t);
      luaX_next(ls);
      if (check(ls, TK_NAME))
        parse_ids(ls, &lp->u.loop.ids);
      if (accept(ls, TK_ON)) {  /* loop { val IDS = await(PAT) ; BODY } */
        ANode *awt = parse_await(ls);
        ANode *blk = parse_block(ls);
        ANode *call = mkcall1(ls, mkrt(ls, RT_AWAIT, line), awt, line);
        ANode *bnd;
        if (lp->u.loop.ids.n == 0)
          bnd = call;
        else {
          bnd = mkdcl(ls, TK_VAL, line);
          bnd->u.dcl.ids = lp->u.loop.ids;
          bnd->u.dcl.set = call;
        }
        nlist_insert(ls, &blk->u.es.es, 0, bnd);
        memset(&lp->u.loop.ids, 0, sizeof(NList));
        lp->u.loop.itr = NULL;
        lp->u.loop.blk = blk;
        return lp;
      }
      else {
        if (accept(ls, TK_IN))
          lp->u.loop.itr = parse_expr(ls);
        lp->u.loop.blk = parse_block(ls);
        return lp;
      }
    }
    /* until(...) / while(...) as calls */
    case TK_UNTIL: case TK_WHILE: {
      ANode *n = mkrt(ls, (t->token == TK_UNTIL) ? RT_UNTIL : RT_WHILE, line);
      luaX_next(ls);
      return n;
    }
    /* par [:all|:any] {...} with {...} */
    case TK_PAR: {
      int rt = RT_PAR;
      ANode *call;
      luaX_next(ls);
      if (check(ls, TK_TAG)) {
        if (check_tag(ls, "all")) rt = RT_PAR_ALL;
        else if (check_tag(ls, "any")) rt = RT_PAR_ANY;
        else err(ls, TK1(ls), "invalid par : invalid tag");
        luaX_next(ls);
      }
      call = mkcall(ls, mkrt(ls, rt, line), line);
      nlist_add(ls, &call->u.call.es, parse_luablock(ls));
      while (accept(ls, TK_WITH))
        nlist_add(ls, &call->u.call.es, parse_luablock(ls));
      return call;
    }
    /* watching P {...} */
    case TK_WATCHING: {
      ANode *call, *awt;
      luaX_next(ls);
      awt = parse_await(ls);
      call = mkcall1(ls, mkrt(ls, RT_WATCHING, line), awt, line);
      nlist_add(ls, &call->u.call.es, parse_luablock(ls));
      return call;
    }
    /* thread {...} */
    case TK_THREAD: {
      ANode *call;
      luaX_next(ls);
      call = mkcall1(ls, mkrt(ls, RT_THREAD, line), parse_luablock(ls), line);
      return call;
    }
    default: {
      err(ls, t, "expected expression");
      return NULL;  /* to avoid warnings */
    }
  }
}

/* }====================================================== */


/*
** Parse a whole program: a sequence of statements up to <eof>.
** Returns a 'do' node with the program block.
*/
ANode *luaA_parse (LexState *ls, int flags) {
  AtmState as;
  ANode *blk, *d;
  as.cur = NULL;
  as.left = 0;
  as.test = (flags & ATM_TEST);
  ls->ast = &as;
  ls->atmos = 1;
  luaX_next(ls);  /* read first token */
  blk = mkblock(ls, 1);
  parse_stmts(ls, &blk->u.es.es, TK_EOS);
  check_err(ls, TK_EOS);
  d = mknode(ls, N_DO, NULL);
  d->line = 1;
  d->u.do_.blk = blk;
  ls->ast = NULL;
  return d;
}
