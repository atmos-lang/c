/*
** $Id: latmcoder.c $
** Atmos Code Generator: generates Lua bytecode from the Atmos AST
** See Copyright Notice in lua.h
*/

#define latmcoder_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "latmos.h"
#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"


#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)

#define enterlevel(ls)	luaE_incCstack(ls->L)
#define leavelevel(ls)	((ls)->L->nCcalls--)

#define init_exp(e,k,i)		luaY_init_exp(e,k,i)
#define codestring(e,s)		luaY_codestring(e,s)


static void code_exp (LexState *ls, ANode *e, expdesc *v);
static void code_stat (LexState *ls, ANode *e);
static void code_stmts (LexState *ls, NList *es, int noret);
static void code_closure (LexState *ls, ANode *p, expdesc *v);

/* flag for temporary prototypes: do not return the last expression */
#define PF_NORET	4


/*
** {======================================================
** Helpers
** =======================================================
*/

static l_noret coderror (LexState *ls, ANode *e, const char *msg) {
  luaA_error(ls, e->line, e->text, msg);
}


/* set current line (for debug information) */
static void setline (LexState *ls, ANode *e) {
  ls->lastline = e->line;
  ls->linenumber = e->line;
}


static TString *newstr (LexState *ls, const char *s) {
  return luaX_newstring(ls, s, strlen(s));
}


/*
** Expression for the runtime function '_ATM.<name>'.
*/
static void code_rt (LexState *ls, int rt, expdesc *v) {
  FuncState *fs = ls->fs;
  expdesc key;
  init_exp(v, VGLOBAL, -1);
  luaY_singlevaraux(fs, ls->atmn, v, 1);  /* hidden upvalue */
  lua_assert(v->k == VUPVAL || v->k == VLOCAL);
  luaK_exp2anyregup(fs, v);
  codestring(&key, newstr(ls, luaA_rtnames[rt]));
  luaK_indexed(fs, v, &key);
}


/*
** Emit the call instruction for a call whose function is at register
** 'base' and whose arguments follow it. 'multret' tells whether the
** last argument is open (multiple results).
*/
static void emit_call (LexState *ls, int base, int multret, int line,
                       expdesc *v) {
  FuncState *fs = ls->fs;
  int nparams;
  if (multret)
    nparams = LUA_MULTRET;  /* open call */
  else
    nparams = fs->freereg - (base + 1);
  init_exp(v, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams+1, 2));
  luaK_fixline(fs, line);
  fs->freereg = cast_byte(base + 1);  /* call leaves one result */
}


/*
** Start a call to a runtime function: puts '_ATM.<rt>' in the next
** register and returns its index (the call base).
*/
static int start_rtcall (LexState *ls, int rt) {
  FuncState *fs = ls->fs;
  expdesc f;
  int base = fs->freereg;
  code_rt(ls, rt, &f);
  luaK_exp2nextreg(fs, &f);
  return base;
}


/* push an expression as the next argument of a call being built */
static void push_arg (LexState *ls, expdesc *e) {
  luaK_exp2nextreg(ls->fs, e);
}


/* push a node as the next argument of a call being built */
static void push_node (LexState *ls, ANode *e) {
  expdesc v;
  code_exp(ls, e, &v);
  luaK_exp2nextreg(ls->fs, &v);
}


/*
** Flatten expression lists: '(a, b)' nodes expand into their items.
*/
static int count_exps (NList *es) {
  int i, n = 0;
  for (i = 0; i < es->n; i++) {
    if (es->arr[i]->tag == N_ES)
      n += count_exps(&es->arr[i]->u.es.es);
    else
      n++;
  }
  return n;
}



/*
** Code a list of expressions (as 'explist'): all but the last go to
** consecutive registers; the last one stays in 'v'. Returns the number
** of expressions (0 with v->k == VVOID for an empty list).
*/
static void code_explist_aux (LexState *ls, NList *es, expdesc *v,
                              int *first) {
  int i;
  for (i = 0; i < es->n; i++) {
    ANode *e = es->arr[i];
    if (e->tag == N_ES) {
      code_explist_aux(ls, &e->u.es.es, v, first);
    }
    else {
      if (!*first)
        luaK_exp2nextreg(ls->fs, v);  /* close previous expression */
      code_exp(ls, e, v);
      *first = 0;
    }
  }
}


static int code_explist (LexState *ls, NList *es, expdesc *v) {
  int first = 1;
  int n = count_exps(es);
  if (n == 0) {
    v->k = VVOID;
    return 0;
  }
  code_explist_aux(ls, es, v, &first);
  return n;
}


/*
** Code the arguments 'es' of a call whose function is already in
** register 'base'; then emit the call.
*/
static void code_args (LexState *ls, int base, NList *es, int line,
                       expdesc *v) {
  FuncState *fs = ls->fs;
  expdesc args;
  int n = code_explist(ls, es, &args);
  int multret = 0;
  if (n > 0) {
    if (hasmultret(args.k)) {
      luaK_setmultret(fs, &args);
      multret = 1;
    }
    else
      luaK_exp2nextreg(fs, &args);  /* close last argument */
  }
  emit_call(ls, base, multret, line, v);
}


/*
** Is 'e' statically known not to be a task (so no need to check for
** pinning)?
*/
static int notask (ANode *e) {
  switch (e->tag) {
    case N_NIL: case N_BOOL: case N_NUM: case N_STR: case N_TAG: case N_CLK:
    case N_TABLE: case N_PROTO: case N_UNO:
      return 1;
    case N_BIN:
      return (e->sub != TK_LAND && e->sub != TK_LOR);
    default:
      return 0;
  }
}


/*
** Code '_ATM.pin_chk_set(chk, pin, e...)': checks pinning of task
** values on assignments and declarations.
*/
static void code_pin_chk_set (LexState *ls, int chk, int pin, ANode *e,
                              expdesc *v) {
  FuncState *fs = ls->fs;
  int base = start_rtcall(ls, RT_PIN_CHK_SET);
  expdesc a;
  NList es;
  ANode *arr[1];
  int n;
  init_exp(&a, chk ? VTRUE : VFALSE, 0);
  push_arg(ls, &a);
  init_exp(&a, pin ? VTRUE : VFALSE, 0);
  push_arg(ls, &a);
  es.arr = arr; es.n = 1; es.size = 1; arr[0] = e;
  n = code_explist(ls, &es, &a);  /* flattens '(a, b, ...)' lists */
  if (n > 0) {
    if (hasmultret(a.k)) {
      luaK_setmultret(fs, &a);
      emit_call(ls, base, 1, e->line, v);
      return;
    }
    luaK_exp2nextreg(fs, &a);
  }
  emit_call(ls, base, 0, e->line, v);
}

/* }====================================================== */


/*
** {======================================================
** Natives: Lua code embedded in Atmos
** =======================================================
*/

typedef struct NatReader {
  const char *s;
  size_t size;
} NatReader;


static const char *natreader (lua_State *L, void *ud, size_t *size) {
  NatReader *nr = cast(NatReader *, ud);
  UNUSED(L);
  if (nr->size == 0) return NULL;
  *size = nr->size;
  nr->size = 0;
  return nr->s;
}


/*
** Parse the native text in 'e' as a Lua expression ('v' != NULL) or as
** a Lua statement list ('v' == NULL), using a nested Lua lexer that
** shares the current function state.
*/
static void code_native (LexState *ls, ANode *e, expdesc *v) {
  FuncState *fs = ls->fs;
  LexState sub;
  ZIO z;
  NatReader nr;
  size_t len;
  nr.s = getlstr(e->u.ts, len);
  nr.size = len;
  luaZ_init(ls->L, &z, natreader, &nr);
  sub = *ls;  /* share tables, buffer, source, etc. */
  sub.z = &z;
  sub.atmos = 0;  /* Lua mode */
  sub.npback = 0;
  sub.linenumber = sub.lastline = e->line;
  sub.lookahead.token = TK_EOS;
  sub.t.token = 0;
  sub.current = zgetc(&z);
  sub.fs = fs;
  fs->ls = &sub;
  luaX_next(&sub);  /* read first token */
  if (v != NULL)
    luaY_expr(&sub, v);
  else
    luaY_statlist(&sub);
  if (sub.t.token != TK_EOS)
    luaX_syntaxerror(&sub, "unexpected symbol in native");
  fs->ls = ls;  /* restore lexer */
  ls->linenumber = ls->lastline = e->line;
}

/* }====================================================== */


/*
** {======================================================
** Functions and blocks
** =======================================================
*/

/*
** Code a closure for prototype 'p' (a function with parameters and a
** block whose last expression is returned).
*/
static void code_closure (LexState *ls, ANode *p, expdesc *v) {
  FuncState new_fs;
  BlockCnt bl;
  FuncState *fs;
  int i;
  int nparams = 0;
  new_fs.f = luaY_addprototype(ls);
  new_fs.f->linedefined = p->line;
  luaY_open_func(ls, &new_fs, &bl);
  fs = ls->fs;
  for (i = 0; i < p->u.proto.pars.n; i++) {
    luaY_new_localvar(ls, p->u.proto.pars.arr[i]->u.ts);
    nparams++;
  }
  if (p->u.proto.dots)
    luaY_new_localvar(ls, newstr(ls, "(vararg table)"));
  luaY_adjustlocalvars(ls, nparams);
  fs->f->numparams = cast_byte(fs->nactvar);
  if (p->u.proto.dots) {
    luaY_setvararg(fs);
    luaY_adjustlocalvars(ls, 1);
  }
  luaK_reserveregs(fs, fs->nactvar);
  code_stmts(ls, &p->u.proto.blk->u.es.es, (p->flags & PF_NORET));
  new_fs.f->lastlinedefined = p->u.proto.blk->line2
                              ? p->u.proto.blk->line2 : p->line;
  luaY_codeclosure(ls, v);
  luaY_close_func(ls);
}


/*
** Code a prototype node: lua closure, atm_func(closure), or
** task(atm_func(closure)).
*/
static void code_proto (LexState *ls, ANode *p, expdesc *v) {
  if (p->sub == P_LUA)
    code_closure(ls, p, v);
  else {
    int base = -1;
    int base2;
    expdesc f;
    if (p->sub == P_TASK)
      base = start_rtcall(ls, RT_TASK);  /* _ATM.task( */
    base2 = start_rtcall(ls, RT_FUNC);  /* _ATM.func( */
    code_closure(ls, p, &f);
    push_arg(ls, &f);
    emit_call(ls, base2, 0, p->line, v);  /* result is the argument */
    if (p->sub == P_TASK)
      emit_call(ls, base, 0, p->line, v);
  }
}


/*
** Code '(function () blk end)()' : a block as an expression.
*/
static void code_block_call (LexState *ls, ANode *blk, expdesc *v) {
  FuncState *fs = ls->fs;
  int base = fs->freereg;
  ANode p;
  expdesc f;
  memset(&p, 0, sizeof(p));
  p.tag = N_PROTO;
  p.sub = P_LUA;
  p.line = blk->line;
  p.u.proto.blk = blk;
  code_closure(ls, &p, &f);
  luaK_exp2nextreg(fs, &f);
  lua_assert(f.u.info == base);
  emit_call(ls, base, 0, blk->line, v);
}


/*
** Code '_ATM.<rt>(args..., closure)' where closure is built from 'blk'.
*/
static ANode *mkproto_tmp (ANode *tmp, ANode *blk) {
  memset(tmp, 0, sizeof(ANode));
  tmp->tag = N_PROTO;
  tmp->sub = P_LUA;
  tmp->line = blk->line;
  tmp->u.proto.blk = blk;
  return tmp;
}


/*
** Return the value of expression 'e' from the current function
** (a 'return e' statement, without tail calls to preserve traces).
*/
static void code_return (LexState *ls, ANode *e) {
  FuncState *fs = ls->fs;
  expdesc v;
  int nret;
  int first = luaY_nvarstack(fs);
  NList es;
  ANode *arr[1];
  es.arr = arr; es.n = 1; es.size = 1;
  arr[0] = e;
  nret = code_explist(ls, &es, &v);
  if (nret == 0)
    luaK_ret(fs, first, 0);
  else if (hasmultret(v.k)) {
    luaK_setmultret(fs, &v);
    luaK_ret(fs, first, LUA_MULTRET);
  }
  else {
    if (nret == 1)
      first = luaK_exp2anyreg(fs, &v);
    else {
      luaK_exp2nextreg(fs, &v);
      lua_assert(nret == fs->freereg - first);
    }
    luaK_ret(fs, first, nret);
  }
}


/* expressions that cannot be returned as values (statements only) */
static int is_stmt (ANode *e) {
  return ((e->tag == N_DCL && e->u.dcl.spw == NULL) ||
          e->tag == N_SET || e->tag == N_DEFER || e->tag == N_STMTS);
}


/*
** Code a sequence of statements; the last one is returned as the value
** of the enclosing function, unless 'noret'.
*/
static void code_stmts (LexState *ls, NList *es, int noret) {
  FuncState *fs = ls->fs;
  int i;
  for (i = 0; i < es->n; i++) {
    ANode *e = es->arr[i];
    enterlevel(ls);
    if (noret || i < es->n - 1 || is_stmt(e))
      code_stat(ls, e);
    else {
      /* an implicit "pin _ = spawn" at the tail of a value-position
         block is unwrapped to its plain spawn call */
      setline(ls, e);
      code_return(ls, (e->tag == N_DCL && e->u.dcl.spw) ? e->u.dcl.set : e);
    }
    lua_assert(fs->f->maxstacksize >= fs->freereg &&
               fs->freereg >= luaY_nvarstack(fs));
    fs->freereg = luaY_nvarstack(fs);  /* free registers */
    leavelevel(ls);
  }
}

/* }====================================================== */


/*
** {======================================================
** Statements
** =======================================================
*/

/*
** Declaration: val/var/pin ids = set
*/
static void code_dcl (LexState *ls, ANode *d) {
  FuncState *fs = ls->fs;
  int kind = d->sub;
  int nvars = d->u.dcl.ids.n;
  int i;
  int nexps;
  expdesc e;
  lu_byte vkind = (kind == TK_VAL) ? RDKCONST
                : (kind == TK_PIN) ? RDKTOCLOSE : VDKREG;
  int firstvar = fs->nactvar;
  int vidx = -1;
  ANode *set = d->u.dcl.set;
  for (i = 0; i < nvars; i++)
    vidx = luaY_new_varkind(ls, d->u.dcl.ids.arr[i]->u.ts, vkind);
  if (set == NULL) {
    e.k = VVOID;
    nexps = 0;
  }
  else if (kind == TK_PIN) {
    int chk = strcmp(getstr(d->u.dcl.ids.arr[0]->u.ts), "_") != 0;
    code_pin_chk_set(ls, chk, 1, set, &e);
    nexps = 1;
  }
  else if (notask(set)) {
    NList es; ANode *arr[1];
    es.arr = arr; es.n = 1; es.size = 1; arr[0] = set;
    nexps = code_explist(ls, &es, &e);
  }
  else {
    code_pin_chk_set(ls, 1, 0, set, &e);
    nexps = 1;
  }
  if (nvars == nexps && vkind == RDKCONST && nvars == 1) {
    Vardesc *var = luaY_getlocalvardesc(fs, vidx);
    if (luaK_exp2const(fs, &e, &var->k)) {  /* compile-time constant? */
      var->vd.kind = RDKCTC;
      luaY_adjustlocalvars(ls, nvars - 1);  /* exclude last variable */
      fs->nactvar++;  /* but count it */
      return;
    }
  }
  luaY_adjust_assign(ls, nvars, nexps, &e);
  luaY_adjustlocalvars(ls, nvars);
  if (kind == TK_PIN) {
    for (i = 0; i < nvars; i++)
      luaY_checktoclose(fs, firstvar + i);
  }
}


/*
** Assignment: set dsts = src
*/
static void code_assign (LexState *ls, ANode *s, int i,
                         struct LHS_assign *prev) {
  FuncState *fs = ls->fs;
  struct LHS_assign lh;
  int nvars = s->u.set.dsts.n;
  ANode *dst = s->u.set.dsts.arr[i];
  lh.prev = prev;
  if (dst->tag == N_NAT)
    code_native(ls, dst, &lh.v);
  else
    code_exp(ls, dst, &lh.v);
  if (!vkisvar(lh.v.k))
    coderror(ls, dst, "expected assignable expression");
  luaY_check_readonly(ls, &lh.v);
  if (prev != NULL && !vkisindexed(lh.v.k))
    luaY_check_conflict(ls, prev, &lh.v);
  if (i + 1 < nvars) {
    enterlevel(ls);
    code_assign(ls, s, i + 1, &lh);
    leavelevel(ls);
  }
  else {  /* last variable: evaluate source */
    ANode *src = s->u.set.src;
    expdesc e;
    int nexps;
    if (notask(src)) {
      NList es; ANode *arr[1];
      es.arr = arr; es.n = 1; es.size = 1; arr[0] = src;
      nexps = code_explist(ls, &es, &e);
    }
    else {
      code_pin_chk_set(ls, 1, 0, src, &e);
      nexps = 1;
    }
    if (nexps != nvars)
      luaY_adjust_assign(ls, nvars, nexps, &e);
    else {
      luaK_setoneret(fs, &e);  /* close last expression */
      luaK_storevar(fs, &lh.v, &e);
      return;  /* avoid default */
    }
  }
  luaY_storevartop(fs, &lh.v);  /* default assignment */
}


/*
** Defer: local (defer) <close> = _ATM.defer(function () blk end)
*/
static void code_defer (LexState *ls, ANode *d) {
  FuncState *fs = ls->fs;
  int level = fs->nactvar;
  int base;
  expdesc f, v;
  ANode tmp;
  luaY_new_varkind(ls, newstr(ls, "(defer)"), RDKTOCLOSE);
  base = start_rtcall(ls, RT_DEFER);
  mkproto_tmp(&tmp, d->u.defer.blk);
  tmp.flags |= PF_NORET;
  code_closure(ls, &tmp, &f);
  push_arg(ls, &f);
  emit_call(ls, base, 0, d->line, &v);
  luaY_adjust_assign(ls, 1, 1, &v);
  luaY_adjustlocalvars(ls, 1);
  luaY_checktoclose(fs, level);
}


static void code_stat (LexState *ls, ANode *e) {
  FuncState *fs = ls->fs;
  setline(ls, e);
  switch (e->tag) {
    case N_DCL: {
      code_dcl(ls, e);
      break;
    }
    case N_SET: {
      code_assign(ls, e, 0, NULL);
      break;
    }
    case N_DEFER: {
      code_defer(ls, e);
      break;
    }
    case N_NAT: {
      code_native(ls, e, NULL);
      break;
    }
    case N_STMTS: {
      int i;
      for (i = 0; i < e->u.es.es.n; i++)
        code_stat(ls, e->u.es.es.arr[i]);
      break;
    }
    default: {
      expdesc v;
      code_exp(ls, e, &v);
      if (v.k == VCALL) {
        Instruction *inst = &getinstruction(fs, &v);
        SETARG_C(*inst, 1);  /* call statement uses no results */
      }
      else
        coderror(ls, e, "unexpected symbol");
      break;
    }
  }
}

/* }====================================================== */


/*
** {======================================================
** Expressions
** =======================================================
*/

static BinOpr binopr (int tk) {
  switch (tk) {
    case '+': return OPR_ADD;
    case '-': return OPR_SUB;
    case '*': return OPR_MUL;
    case '%': return OPR_MOD;
    case TK_POW: return OPR_POW;
    case '/': return OPR_DIV;
    case TK_IDIV: return OPR_IDIV;
    case TK_EQ: return OPR_EQ;
    case TK_LNE: return OPR_NE;
    case '<': return OPR_LT;
    case TK_LE: return OPR_LE;
    case '>': return OPR_GT;
    case TK_GE: return OPR_GE;
    case TK_LAND: return OPR_AND;
    case TK_LOR: return OPR_OR;
    default: return OPR_NOBINOPR;
  }
}


/*
** Code a binary operation implemented by a runtime function:
** _ATM.<rt>(a, b) (or (b, a) if 'swap'), negated if 'neg'.
*/
static void code_rtbin (LexState *ls, int rt, ANode *a, ANode *b, int swap,
                        int neg, int line, expdesc *v) {
  int base = start_rtcall(ls, rt);
  push_node(ls, swap ? b : a);
  push_node(ls, swap ? a : b);
  emit_call(ls, base, 0, line, v);
  if (neg)
    luaK_prefix(ls->fs, OPR_NOT, v, line);
}


static void code_bin (LexState *ls, ANode *e, expdesc *v) {
  FuncState *fs = ls->fs;
  ANode *e1 = e->u.bin.e1;
  ANode *e2 = e->u.bin.e2;
  switch (e->sub) {
    case TK_DEQ: code_rtbin(ls, RT_EQ, e1, e2, 0, 0, e->line, v); break;
    case TK_DNE: code_rtbin(ls, RT_EQ, e1, e2, 0, 1, e->line, v); break;
    case TK_SUP: code_rtbin(ls, RT_GTE, e1, e2, 0, 0, e->line, v); break;
    case TK_SUB: code_rtbin(ls, RT_GTE, e1, e2, 1, 0, e->line, v); break;
    case TK_CAT: code_rtbin(ls, RT_CAT, e1, e2, 0, 0, e->line, v); break;
    case TK_IS: code_rtbin(ls, RT_IS, e1, e2, 0, 0, e->line, v); break;
    case TK_ISNOT: code_rtbin(ls, RT_IS, e1, e2, 0, 1, e->line, v); break;
    case TK_XIN: code_rtbin(ls, RT_XIN, e1, e2, 0, 0, e->line, v); break;
    case TK_XNOTIN: code_rtbin(ls, RT_XIN, e1, e2, 0, 1, e->line, v); break;
    default: {
      BinOpr op = binopr(e->sub);
      expdesc v2;
      lua_assert(op != OPR_NOBINOPR);
      code_exp(ls, e1, v);
      luaK_infix(fs, op, v);
      code_exp(ls, e2, &v2);
      luaK_posfix(fs, op, v, &v2, e->line);
      break;
    }
  }
}


static void code_uno (LexState *ls, ANode *e, expdesc *v) {
  UnOpr op = (e->sub == '-') ? OPR_MINUS
           : (e->sub == '#') ? OPR_LEN : OPR_NOT;
  code_exp(ls, e->u.uno.e, v);
  luaK_prefix(ls->fs, op, v, e->line);
}


static void code_index (LexState *ls, ANode *e, expdesc *v) {
  FuncState *fs = ls->fs;
  expdesc k;
  code_exp(ls, e->u.index.t, v);
  luaK_exp2anyregup(fs, v);
  code_exp(ls, e->u.index.idx, &k);
  luaK_exp2val(fs, &k);
  luaK_indexed(fs, v, &k);
}


static void code_table (LexState *ls, ANode *e, expdesc *t) {
  FuncState *fs = ls->fs;
  int pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
  int na = 0, nh = 0;
  int i;
  luaK_code(fs, 0);  /* space for extra arg. */
  init_exp(t, VNONRELOC, fs->freereg);  /* table will be at stack top */
  luaK_reserveregs(fs, 1);
  for (i = 0; i < e->u.table.es.n; i++) {
    KV *kv = &e->u.table.es.arr[i];
    lu_byte reg = fs->freereg;
    expdesc tab, key, val;
    if (kv->k->tag == N_NUM && !isnflt(kv->k)) na++; else nh++;
    code_exp(ls, kv->k, &key);
    luaK_exp2val(fs, &key);
    tab = *t;
    luaK_indexed(fs, &tab, &key);
    code_exp(ls, kv->v, &val);
    luaK_storevar(fs, &tab, &val);
    fs->freereg = reg;  /* free registers */
  }
  luaK_settablesize(fs, pc, t->u.info, na, nh);
}


static void code_call (LexState *ls, ANode *e, expdesc *v) {
  FuncState *fs = ls->fs;
  ANode *f = e->u.call.f;
  int base;
  if (f->tag == N_MET) {  /* o::m(args) */
    expdesc key;
    code_exp(ls, f->u.met.o, v);
    codestring(&key, f->u.met.met);
    luaK_self(fs, v, &key);
    base = v->u.info;
  }
  else {
    code_exp(ls, f, v);
    luaK_exp2nextreg(fs, v);
    base = v->u.info;
  }
  code_args(ls, base, &e->u.call.es, e->line, v);
}


static void code_acc (LexState *ls, ANode *e, expdesc *v) {
  const char *name = getstr(e->u.ts);
  if (strcmp(name, "pub") == 0) {  /* _ATM.pub().pub */
    FuncState *fs = ls->fs;
    int base = start_rtcall(ls, RT_PUB);
    expdesc key;
    emit_call(ls, base, 0, e->line, v);
    luaK_exp2anyregup(fs, v);
    codestring(&key, e->u.ts);
    luaK_indexed(fs, v, &key);
  }
  else if (strcmp(name, "break") == 0)
    code_rt(ls, RT_BREAK, v);
  else if (strcmp(name, "return") == 0)
    code_rt(ls, RT_RETURN, v);
  else
    luaY_buildvar(ls, e->u.ts, v);
}


/*
** if/ifs/match as a closure: each case tests its condition and returns
** the result of calling its body with the condition value.
*/
static void code_ifs (LexState *ls, ANode *e, expdesc *v) {
  FuncState *outer = ls->fs;
  FuncState new_fs;
  BlockCnt bl;
  FuncState *fs;
  int base = outer->freereg;
  int i;
  expdesc f;
  int hasmatch = (e->u.ifs.match != NULL);
  int mreg = 0;
  new_fs.f = luaY_addprototype(ls);
  new_fs.f->linedefined = e->line;
  luaY_open_func(ls, &new_fs, &bl);
  fs = ls->fs;
  if (hasmatch) {
    luaY_new_localvar(ls, newstr(ls, "(match)"));
    luaY_adjustlocalvars(ls, 1);
    mreg = 0;
  }
  fs->f->numparams = cast_byte(fs->nactvar);
  luaK_reserveregs(fs, fs->nactvar);
  for (i = 0; i < e->u.ifs.cases.n; i++) {
    Case *c = &e->u.ifs.cases.arr[i];
    expdesc cnd, body, arg, res;
    BlockCnt cbl;
    int cbase;
    int creg;
    luaY_enterblock(fs, &cbl, 0);
    setline(ls, c->f);
    /* local (cond) = <condition> */
    luaY_new_localvar(ls, newstr(ls, "(cond)"));
    if (c->kind == C_ELSE) {
      if (hasmatch) {  /* v or true */
        init_exp(&cnd, VNONRELOC, mreg);
        luaK_infix(fs, OPR_OR, &cnd);
        init_exp(&arg, VTRUE, 0);
        luaK_posfix(fs, OPR_OR, &cnd, &arg, c->f->line);
      }
      else
        init_exp(&cnd, VTRUE, 0);
      luaK_exp2nextreg(fs, &cnd);
    }
    else if (c->kind == C_EXPR) {
      code_exp(ls, c->cnd, &cnd);
      luaK_exp2nextreg(fs, &cnd);
    }
    else if (c->kind == C_MATCH) {  /* _ATM.is(v, cmp) and (v or true) */
      int b = start_rtcall(ls, RT_IS);
      expdesc t;
      init_exp(&arg, VNONRELOC, mreg);
      push_arg(ls, &arg);
      push_node(ls, c->cnd);
      emit_call(ls, b, 0, c->cnd->line, &cnd);
      luaK_infix(fs, OPR_AND, &cnd);
      init_exp(&arg, VNONRELOC, mreg);
      luaK_infix(fs, OPR_OR, &arg);
      init_exp(&t, VTRUE, 0);
      luaK_posfix(fs, OPR_OR, &arg, &t, c->cnd->line);
      luaK_posfix(fs, OPR_AND, &cnd, &arg, c->cnd->line);
      luaK_exp2nextreg(fs, &cnd);
    }
    else {  /* C_MATCHFUNC: f(v) */
      int b = fs->freereg;
      code_exp(ls, c->cnd, &cnd);
      luaK_exp2nextreg(fs, &cnd);
      init_exp(&arg, VNONRELOC, mreg);
      push_arg(ls, &arg);
      emit_call(ls, b, 0, c->cnd->line, &cnd);
      luaK_exp2nextreg(fs, &cnd);
    }
    creg = cnd.u.info;
    luaY_adjustlocalvars(ls, 1);  /* condition is now a local */
    /* if not cond then skip */
    init_exp(&cnd, VNONRELOC, creg);
    luaK_goiftrue(fs, &cnd);
    /* return body(cond) */
    cbase = fs->freereg;
    code_exp(ls, c->f, &body);
    luaK_exp2nextreg(fs, &body);
    init_exp(&arg, VNONRELOC, creg);
    luaK_exp2nextreg(fs, &arg);
    emit_call(ls, cbase, 0, c->f->line, &res);
    luaK_setmultret(fs, &res);
    luaK_ret(fs, cbase, LUA_MULTRET);
    luaK_patchtohere(fs, cnd.f);
    luaY_leaveblock(fs);
  }
  new_fs.f->lastlinedefined = e->line;
  luaY_codeclosure(ls, &f);
  luaY_close_func(ls);
  /* call the closure (with the match expression, if any) */
  luaK_exp2nextreg(outer, &f);
  lua_assert(f.u.info == base);
  if (hasmatch)
    push_node(ls, e->u.ifs.match);
  emit_call(ls, base, 0, e->line, v);
}


static void fixforjump (FuncState *fs, int pc, int dest, int back) {
  Instruction *jmp = &fs->f->code[pc];
  int offset = dest - (pc + 1);
  if (back)
    offset = -offset;
  if (l_unlikely(offset > MAXARG_Bx))
    luaX_syntaxerror(fs->ls, "control structure too long");
  SETARG_Bx(*jmp, offset);
}


/*
** loop: _ATM.loop(function () for ids in _ATM.iter(itr) do blk end end)
*/
static void code_loop (LexState *ls, ANode *e, expdesc *v) {
  FuncState *outer = ls->fs;
  FuncState new_fs;
  BlockCnt blf, bl, blb;
  FuncState *fs;
  int base, nvars, i, prep, endfor, itbase;
  int line = e->line;
  expdesc f, it;
  int rtbase = start_rtcall(ls, RT_LOOP);
  new_fs.f = luaY_addprototype(ls);
  new_fs.f->linedefined = e->line;
  luaY_open_func(ls, &new_fs, &blf);
  fs = ls->fs;
  luaY_enterblock(fs, &bl, 1);  /* scope for loop and control variables */
  base = fs->freereg;
  luaY_new_localvar(ls, newstr(ls, "(for state)"));
  luaY_new_localvar(ls, newstr(ls, "(for state)"));
  luaY_new_localvar(ls, newstr(ls, "(for state)"));
  if (e->u.loop.ids.n == 0) {
    luaY_new_varkind(ls, newstr(ls, "_"), RDKCONST);
    nvars = 4;
  }
  else {
    luaY_new_varkind(ls, e->u.loop.ids.arr[0]->u.ts, RDKCONST);
    nvars = 4;
    for (i = 1; i < e->u.loop.ids.n; i++) {
      luaY_new_localvar(ls, e->u.loop.ids.arr[i]->u.ts);
      nvars++;
    }
  }
  /* explist: _ATM.iter(itr) */
  itbase = start_rtcall(ls, RT_ITER);
  if (e->u.loop.itr != NULL) {
    NList es; ANode *arr[1];
    es.arr = arr; es.n = 1; es.size = 1; arr[0] = e->u.loop.itr;
    code_args(ls, itbase, &es, line, &it);
  }
  else
    emit_call(ls, itbase, 0, line, &it);
  luaY_adjust_assign(ls, 4, 1, &it);
  luaY_adjustlocalvars(ls, 3);  /* start scope for internal variables */
  luaY_marktobeclosed(fs);  /* last internal var. must be closed */
  luaK_checkstack(fs, 2);  /* extra space to call iterator */
  /* forbody */
  prep = luaK_codeABx(fs, OP_TFORPREP, base, 0);
  fs->freereg--;  /* 'forprep' removes one register from the stack */
  luaY_enterblock(fs, &blb, 0);  /* scope for declared variables */
  luaY_adjustlocalvars(ls, nvars - 3);
  luaK_reserveregs(fs, nvars - 3);
  {
    BlockCnt blk;
    luaY_enterblock(fs, &blk, 0);
    code_stmts(ls, &e->u.loop.blk->u.es.es, 1);
    luaY_leaveblock(fs);
  }
  luaY_leaveblock(fs);  /* end of scope for declared variables */
  fixforjump(fs, prep, luaK_getlabel(fs), 0);
  luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars - 3);
  luaK_fixline(fs, line);
  endfor = luaK_codeABx(fs, OP_TFORLOOP, base, 0);
  fixforjump(fs, endfor, prep + 1, 1);
  luaK_fixline(fs, line);
  luaY_leaveblock(fs);  /* loop scope */
  new_fs.f->lastlinedefined = e->u.loop.blk->line2;
  luaY_codeclosure(ls, &f);
  luaY_close_func(ls);
  luaK_exp2nextreg(outer, &f);
  emit_call(ls, rtbase, 0, line, v);
}


static void code_parens (LexState *ls, ANode *e, expdesc *v) {
  FuncState *fs = ls->fs;
  ANode *in = e->u.uno.e;
  if (in->tag == N_ES) {  /* multiple values: take the first */
    int base = fs->freereg;
    expdesc last;
    int n = code_explist(ls, &in->u.es.es, &last);
    if (n == 0)
      init_exp(v, VNIL, 0);
    else {
      luaK_exp2nextreg(fs, &last);
      fs->freereg = cast_byte(base + 1);
      init_exp(v, VNONRELOC, base);
    }
  }
  else {
    code_exp(ls, in, v);
    luaK_dischargevars(fs, v);  /* truncate to one value */
  }
}


static void code_exp (LexState *ls, ANode *e, expdesc *v) {
  FuncState *fs = ls->fs;
  enterlevel(ls);
  setline(ls, e);
  switch (e->tag) {
    case N_NIL: init_exp(v, VNIL, 0); break;
    case N_BOOL: init_exp(v, e->u.b ? VTRUE : VFALSE, 0); break;
    case N_NUM: case N_CLK: {
      if (isnflt(e)) {
        init_exp(v, VKFLT, 0);
        v->u.nval = e->u.r;
      }
      else {
        init_exp(v, VKINT, 0);
        v->u.ival = e->u.i;
      }
      break;
    }
    case N_STR: case N_TAG: {
      codestring(v, e->u.ts);
      break;
    }
    case N_NAT: {
      code_native(ls, e, v);
      break;
    }
    case N_DOTS: {
      if (!isvararg(fs->f))
        coderror(ls, e, "cannot use '...' outside a vararg function");
      init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, fs->f->numparams, 1));
      break;
    }
    case N_ACC: {
      code_acc(ls, e, v);
      break;
    }
    case N_RT: {
      code_rt(ls, e->u.rt, v);
      break;
    }
    case N_UNO: code_uno(ls, e, v); break;
    case N_BIN: code_bin(ls, e, v); break;
    case N_INDEX: code_index(ls, e, v); break;
    case N_TABLE: code_table(ls, e, v); break;
    case N_CALL: code_call(ls, e, v); break;
    case N_MET: {
      coderror(ls, e, "expected call syntax");
      break;
    }
    case N_PROTO: code_proto(ls, e, v); break;
    case N_PARENS: code_parens(ls, e, v); break;
    case N_ES: {  /* list in single-value context: take the first */
      ANode tmp;
      memset(&tmp, 0, sizeof(tmp));
      tmp.tag = N_PARENS;
      tmp.line = e->line;
      tmp.text = e->text;
      tmp.u.uno.e = e;
      code_parens(ls, &tmp, v);
      break;
    }
    case N_DO: {
      if (e->u.do_.esc != NULL) {  /* _ATM.do(tag, function () blk end) */
        int base = start_rtcall(ls, RT_DO);
        expdesc a;
        ANode tmp;
        codestring(&a, e->u.do_.esc);
        push_arg(ls, &a);
        code_closure(ls, mkproto_tmp(&tmp, e->u.do_.blk), &a);
        push_arg(ls, &a);
        emit_call(ls, base, 0, e->line, v);
      }
      else
        code_block_call(ls, e->u.do_.blk, v);
      break;
    }
    case N_IFS: code_ifs(ls, e, v); break;
    case N_LOOP: code_loop(ls, e, v); break;
    case N_CATCH: {  /* _ATM.catch(cnd, function () blk end) */
      int base = start_rtcall(ls, RT_CATCH);
      expdesc a;
      ANode tmp;
      push_node(ls, e->u.catch_.cnd);
      code_closure(ls, mkproto_tmp(&tmp, e->u.catch_.blk), &a);
      push_arg(ls, &a);
      emit_call(ls, base, 0, e->line, v);
      break;
    }
    case N_RETURN: {  /* _ATM.return(es) */
      int base = start_rtcall(ls, RT_RETURN);
      code_args(ls, base, &e->u.es.es, e->line, v);
      break;
    }
    case N_DCL: case N_SET: case N_DEFER: case N_STMTS: case N_BLOCK: {
      coderror(ls, e, "unexpected statement");
      break;
    }
    default: {
      coderror(ls, e, "unknown expression");
      break;
    }
  }
  leavelevel(ls);
}

/* }====================================================== */


/*
** Main function: a vararg function with upvalues _ENV and (atmos),
** whose body is 'return _ATM.func(function (...) <program> end)(...)'.
*/
void luaA_codemain (LexState *ls, FuncState *fs, ANode *ast) {
  BlockCnt bl;
  Upvaldesc *env, *atm;
  int base;
  expdesc f, v, dots;
  ANode p;
  luaY_open_func(ls, fs, &bl);
  luaY_setvararg(fs);  /* main function is always vararg */
  env = luaY_allocupvalue(fs);  /* ...set environment upvalue */
  env->instack = 1;
  env->idx = 0;
  env->kind = VDKREG;
  env->name = ls->envn;
  luaC_objbarrier(ls->L, fs->f, env->name);
  atm = luaY_allocupvalue(fs);  /* ...set runtime upvalue */
  atm->instack = 1;
  atm->idx = 1;
  atm->kind = RDKCONST;
  atm->name = ls->atmn;
  luaC_objbarrier(ls->L, fs->f, atm->name);
  /* return _ATM.func(function (...) <program> end)(...) */
  base = start_rtcall(ls, RT_FUNC);
  memset(&p, 0, sizeof(p));
  p.tag = N_PROTO;
  p.sub = P_LUA;
  p.line = ast->line;
  p.u.proto.dots = 1;
  p.u.proto.blk = ast->u.do_.blk;
  code_closure(ls, &p, &f);
  push_arg(ls, &f);
  emit_call(ls, base, 0, ast->line, &v);
  luaK_exp2nextreg(fs, &v);
  init_exp(&dots, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, fs->f->numparams, 1));
  luaK_setmultret(fs, &dots);
  emit_call(ls, base, 1, ast->line, &v);
  luaK_setmultret(fs, &v);
  luaK_ret(fs, base, LUA_MULTRET);
  luaY_close_func(ls);
}


/*
** Entry point: parse and compile an Atmos chunk (mirrors 'luaY_parser').
*/
LClosure *luaY_atmparser (lua_State *L, ZIO *z, Table *anchor, Mbuffer *buff,
                          Dyndata *dyd, const char *name, int firstchar) {
  LexState lexstate;
  FuncState funcstate;
  LClosure *cl;
  ANode *ast;
  int flags = 0;
  TValue tst;
  lexstate.h = anchor;  /* table for scanner */
  cl = luaF_newLclosure(L, 2);  /* create main closure */
  luaD_anchorobj(L, anchor, obj2gco(cl));  /* anchor it in scanner table */
  funcstate.f = cl->p = luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  funcstate.f->source = luaS_new(L, name);  /* create and anchor TString */
  luaC_objbarrier(L, funcstate.f, funcstate.f->source);
  lexstate.buff = buff;
  lexstate.dyd = dyd;
  dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
  luaX_setinput(L, &lexstate, z, funcstate.f->source, firstchar);
  /* 'test' blocks enabled? (registry field "atmos.test") */
  if (!tagisempty(luaH_getshortstr(hvalue(&G(L)->l_registry),
                                   luaS_newliteral(L, "atmos.test"), &tst))
      && !l_isfalse(&tst))
    flags |= ATM_TEST;
  ast = luaA_parse(&lexstate, flags);
  luaA_codemain(&lexstate, &funcstate, ast);
  lua_assert(!funcstate.prev && funcstate.nups == 2 && !lexstate.fs);
  /* all scopes should be correctly finished */
  lua_assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  return cl;
}
