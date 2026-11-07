/*
** $Id: lparser.c $
** Atmos Parser (generates Lua bytecode through lcode.c)
** See Copyright Notice in lua.h
*/

#define lparser_c
#define LUA_CORE

#include "lprefix.h"


#include <limits.h>
#include <string.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"



/* maximum number of variable declarations per function (must be
   smaller than 250, due to the bytecode format) */
#define MAXVARS		200


#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)


/* because all strings are unified by the scanner, the parser
   can use pointer equality for string equality */
#define eqstr(a,b)	((a) == (b))


/*
** nodes for block list (list of active blocks)
*/
typedef struct BlockCnt {
  struct BlockCnt *previous;  /* chain */
  int firstlabel;  /* index of first label in this block */
  int firstgoto;  /* index of first pending goto in this block */
  short nactvar;  /* number of active declarations at block entry */
  lu_byte upval;  /* true if some variable in the block is an upvalue */
  lu_byte isloop;  /* 1 if 'block' is a loop; 2 if it has pending breaks */
  lu_byte insidetbc;  /* true if inside the scope of a to-be-closed var. */
  /* Atmos */
  TString *tag;  /* tag of a 'do :tag' block (NULL otherwise) */
  lu_byte hasescape;  /* true if block has pending escapes */
  short resreg;  /* register holding the block value (or -1) */
} BlockCnt;



/*
** prototypes for recursive non-terminal functions
*/
static int statement (LexState *ls, expdesc *v);
static void expr (LexState *ls, expdesc *v);
static int exprlist (LexState *ls, expdesc *v);
static void statlist (LexState *ls, expdesc *v, int *n);


static l_noret error_expected (LexState *ls, int token) {
  luaX_syntaxerror(ls,
      luaO_pushfstring(ls->L, "%s expected", luaX_token2str(ls, token)));
}


static l_noret errorlimit (FuncState *fs, int limit, const char *what) {
  lua_State *L = fs->ls->L;
  const char *msg;
  int line = fs->f->linedefined;
  const char *where = (line == 0)
                      ? "main function"
                      : luaO_pushfstring(L, "function at line %d", line);
  msg = luaO_pushfstring(L, "too many %s (limit is %d) in %s",
                             what, limit, where);
  luaX_syntaxerror(fs->ls, msg);
}


void luaY_checklimit (FuncState *fs, int v, int l, const char *what) {
  if (l_unlikely(v > l)) errorlimit(fs, l, what);
}


/*
** Test whether next token is 'c'; if so, skip it.
*/
static int testnext (LexState *ls, int c) {
  if (ls->t.token == c) {
    luaX_next(ls);
    return 1;
  }
  else return 0;
}


/*
** Check that next token is 'c'.
*/
static void check (LexState *ls, int c) {
  if (ls->t.token != c)
    error_expected(ls, c);
}


/*
** Check that next token is 'c' and skip it.
*/
static void checknext (LexState *ls, int c) {
  check(ls, c);
  luaX_next(ls);
}


#define check_condition(ls,c,msg)	{ if (!(c)) luaX_syntaxerror(ls, msg); }


/*
** Check that next token is 'what' and skip it. In case of error,
** raise an error that the expected 'what' should match a 'who'
** in line 'where' (if that is not the current line).
*/
static void check_match (LexState *ls, int what, int who, int where) {
  if (l_unlikely(!testnext(ls, what))) {
    if (where == ls->linenumber)  /* all in the same line? */
      error_expected(ls, what);  /* do not need a complex message */
    else {
      luaX_syntaxerror(ls, luaO_pushfstring(ls->L,
             "%s expected (to close %s at line %d)",
              luaX_token2str(ls, what), luaX_token2str(ls, who), where));
    }
  }
}


static TString *str_checkname (LexState *ls) {
  TString *ts;
  check(ls, TK_NAME);
  ts = ls->t.seminfo.ts;
  luaX_next(ls);
  return ts;
}


/*
** A field name: a name or a reserved word (e.g., 't.set').
*/
static TString *str_checkfield (LexState *ls) {
  TString *ts;
  if (ls->t.token == TK_NAME)
    ts = ls->t.seminfo.ts;
  else if (ls->t.token >= FIRST_RESERVED && ls->t.token < TK_IDIV)
    ts = luaX_tokenname(ls, ls->t.token);
  else {
    error_expected(ls, TK_NAME);
    return NULL;  /* to avoid warnings */
  }
  luaX_next(ls);
  return ts;
}


/*
** True if the current token starts at the same "separator level" as
** the last consumed token (same line, no ';' in between).
*/
#define sameline(ls)	((ls)->lastsep == (ls)->t.sep)


static void init_exp (expdesc *e, expkind k, int i) {
  e->f = e->t = NO_JUMP;
  e->k = k;
  e->u.info = i;
}


static void codestring (expdesc *e, TString *s) {
  e->f = e->t = NO_JUMP;
  e->k = VKSTR;
  e->u.strval = s;
}


static void codename (LexState *ls, expdesc *e) {
  codestring(e, str_checkname(ls));
}


/*
** Register a new local variable in the active 'Proto' (for debug
** information).
*/
static short registerlocalvar (LexState *ls, FuncState *fs,
                               TString *varname) {
  Proto *f = fs->f;
  int oldsize = f->sizelocvars;
  luaM_growvector(ls->L, f->locvars, fs->ndebugvars, f->sizelocvars,
                  LocVar, SHRT_MAX, "local variables");
  while (oldsize < f->sizelocvars)
    f->locvars[oldsize++].varname = NULL;
  f->locvars[fs->ndebugvars].varname = varname;
  f->locvars[fs->ndebugvars].startpc = fs->pc;
  luaC_objbarrier(ls->L, f, varname);
  return fs->ndebugvars++;
}


/*
** Create a new variable with the given 'name' and given 'kind'.
** Return its index in the function.
*/
static int new_varkind (LexState *ls, TString *name, lu_byte kind) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Dyndata *dyd = ls->dyd;
  Vardesc *var;
  luaM_growvector(L, dyd->actvar.arr, dyd->actvar.n + 1,
             dyd->actvar.size, Vardesc, SHRT_MAX, "variable declarations");
  var = &dyd->actvar.arr[dyd->actvar.n++];
  var->vd.kind = kind;  /* default */
  var->vd.name = name;
  return dyd->actvar.n - 1 - fs->firstlocal;
}


/*
** Create a new local variable with the given 'name' and regular kind.
*/
static int new_localvar (LexState *ls, TString *name) {
  return new_varkind(ls, name, VDKREG);
}

#define new_localvarliteral(ls,v) \
    new_localvar(ls,  \
      luaX_newstring(ls, "" v, (sizeof(v)/sizeof(char)) - 1));



/*
** Return the "variable description" (Vardesc) of a given variable.
** (Unless noted otherwise, all variables are referred to by their
** compiler indices.)
*/
static Vardesc *getlocalvardesc (FuncState *fs, int vidx) {
  return &fs->ls->dyd->actvar.arr[fs->firstlocal + vidx];
}


/*
** Convert 'nvar', a compiler index level, to its corresponding
** register. For that, search for the highest variable below that level
** that is in a register and uses its register index ('ridx') plus one.
*/
static lu_byte reglevel (FuncState *fs, int nvar) {
  while (nvar-- > 0) {
    Vardesc *vd = getlocalvardesc(fs, nvar);  /* get previous variable */
    if (varinreg(vd))  /* is in a register? */
      return cast_byte(vd->vd.ridx + 1);
  }
  return 0;  /* no variables in registers */
}


/*
** Return the number of variables in the register stack for the given
** function.
*/
lu_byte luaY_nvarstack (FuncState *fs) {
  return reglevel(fs, fs->nactvar);
}


/*
** Get the debug-information entry for current variable 'vidx'.
*/
static LocVar *localdebuginfo (FuncState *fs, int vidx) {
  Vardesc *vd = getlocalvardesc(fs,  vidx);
  if (!varinreg(vd))
    return NULL;  /* no debug info. for constants */
  else {
    int idx = vd->vd.pidx;
    lua_assert(idx < fs->ndebugvars);
    return &fs->f->locvars[idx];
  }
}


/*
** Create an expression representing variable 'vidx'
*/
static void init_var (FuncState *fs, expdesc *e, int vidx) {
  e->f = e->t = NO_JUMP;
  e->k = VLOCAL;
  e->u.var.vidx = cast_short(vidx);
  e->u.var.ridx = getlocalvardesc(fs, vidx)->vd.ridx;
}


/*
** Raises an error if variable described by 'e' is read only; moreover,
** if 'e' is t[exp] where t is the vararg parameter, change it to index
** a real table. (Virtual vararg tables cannot be changed.)
*/
static void check_readonly (LexState *ls, expdesc *e) {
  FuncState *fs = ls->fs;
  TString *varname = NULL;  /* to be set if variable is const */
  switch (e->k) {
    case VCONST: {
      varname = ls->dyd->actvar.arr[e->u.info].vd.name;
      break;
    }
    case VLOCAL: case VVARGVAR: {
      Vardesc *vardesc = getlocalvardesc(fs, e->u.var.vidx);
      if (vardesc->vd.kind != VDKREG)  /* not a regular variable? */
        varname = vardesc->vd.name;
      break;
    }
    case VUPVAL: {
      Upvaldesc *up = &fs->f->upvalues[e->u.info];
      if (up->kind != VDKREG)
        varname = up->name;
      break;
    }
    case VVARGIND: {
      needvatab(fs->f);  /* function will need a vararg table */
      e->k = VINDEXED;
    }  /* FALLTHROUGH */
    case VINDEXUP: case VINDEXSTR: case VINDEXED: {  /* global variable */
      if (e->u.ind.ro)  /* read-only? */
        varname = tsvalue(&fs->f->k[e->u.ind.keystr]);
      break;
    }
    default:
      lua_assert(e->k == VINDEXI);  /* this one doesn't need any check */
      return;  /* integer index cannot be read-only */
  }
  if (varname)
    luaK_semerror(ls, "attempt to assign to const variable '%s'",
                      getstr(varname));
}


/*
** Start the scope for the last 'nvars' created variables.
*/
static void adjustlocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  int reglevel = luaY_nvarstack(fs);
  int i;
  for (i = 0; i < nvars; i++) {
    int vidx = fs->nactvar++;
    Vardesc *var = getlocalvardesc(fs, vidx);
    var->vd.ridx = cast_byte(reglevel++);
    var->vd.pidx = registerlocalvar(ls, fs, var->vd.name);
    luaY_checklimit(fs, reglevel, MAXVARS, "local variables");
  }
}


/*
** Close the scope for all variables up to level 'tolevel'.
** (debug info.)
*/
static void removevars (FuncState *fs, int tolevel) {
  fs->ls->dyd->actvar.n -= (fs->nactvar - tolevel);
  while (fs->nactvar > tolevel) {
    LocVar *var = localdebuginfo(fs, --fs->nactvar);
    if (var)  /* does it have debug information? */
      var->endpc = fs->pc;
  }
}


/*
** Emit a CLOSE for register level 'reg' if some enclosing block at that
** level or above has upvalues or to-be-closed variables (so that a
** value can be stored at 'reg' before leaving those blocks).
*/
static void closeupto (FuncState *fs, int reg) {
  BlockCnt *bl;
  for (bl = fs->bl; bl != NULL && reglevel(fs, bl->nactvar) >= reg;
       bl = bl->previous) {
    if (bl->upval) {
      luaK_codeABC(fs, OP_CLOSE, reg, 0, 0);
      return;
    }
  }
}


/*
** Store the value of 'e' into register 'reg' (nil if 'e' is void),
** closing enclosing blocks above 'reg' first.
*/
static void store_value (LexState *ls, int reg, expdesc *e) {
  FuncState *fs = ls->fs;
  if (e->k == VVOID) {
    closeupto(fs, reg);
    luaK_nil(fs, reg, 1);
  }
  else {
    expdesc var;
    luaK_exp2anyreg(fs, e);  /* evaluate before closing */
    closeupto(fs, reg);
    init_exp(&var, VLOCAL, 0);  /* fake local variable at 'reg' */
    var.u.var.ridx = cast_byte(reg);
    var.u.var.vidx = 0;
    luaK_storevar(fs, &var, e);
  }
}


/*
** The value of a compound statement is in register 'reg', the first
** free register when the statement started; describe it as a
** temporary on the top of the stack.
*/
static void value_result (LexState *ls, int reg, expdesc *v) {
  FuncState *fs = ls->fs;
  init_exp(v, VNONRELOC, reg);
  fs->freereg = cast_byte(reg + 1);
}


/*
** Search the upvalues of the function 'fs' for one
** with the given 'name'.
*/
static int searchupvalue (FuncState *fs, TString *name) {
  int i;
  Upvaldesc *up = fs->f->upvalues;
  for (i = 0; i < fs->nups; i++) {
    if (eqstr(up[i].name, name)) return i;
  }
  return -1;  /* not found */
}


static Upvaldesc *allocupvalue (FuncState *fs) {
  Proto *f = fs->f;
  int oldsize = f->sizeupvalues;
  luaY_checklimit(fs, fs->nups + 1, MAXUPVAL, "upvalues");
  luaM_growvector(fs->ls->L, f->upvalues, fs->nups, f->sizeupvalues,
                  Upvaldesc, MAXUPVAL, "upvalues");
  while (oldsize < f->sizeupvalues)
    f->upvalues[oldsize++].name = NULL;
  return &f->upvalues[fs->nups++];
}


static int newupvalue (FuncState *fs, TString *name, expdesc *v) {
  Upvaldesc *up = allocupvalue(fs);
  FuncState *prev = fs->prev;
  if (v->k == VLOCAL) {
    up->instack = 1;
    up->idx = v->u.var.ridx;
    up->kind = getlocalvardesc(prev, v->u.var.vidx)->vd.kind;
    lua_assert(eqstr(name, getlocalvardesc(prev, v->u.var.vidx)->vd.name));
  }
  else {
    up->instack = 0;
    up->idx = cast_byte(v->u.info);
    up->kind = prev->f->upvalues[v->u.info].kind;
    lua_assert(eqstr(name, prev->f->upvalues[v->u.info].name));
  }
  up->name = name;
  luaC_objbarrier(fs->ls->L, fs->f, name);
  return fs->nups - 1;
}


/*
** Look for an active variable with the name 'n' in the
** function 'fs'. If found, initialize 'var' with it and return
** its expression kind; otherwise return -1.
*/
static int searchvar (FuncState *fs, TString *n, expdesc *var) {
  int i;
  for (i = cast_int(fs->nactvar) - 1; i >= 0; i--) {
    Vardesc *vd = getlocalvardesc(fs, i);
    if (eqstr(n, vd->vd.name)) {  /* found? */
      if (vd->vd.kind == RDKCTC)  /* compile-time constant? */
        init_exp(var, VCONST, fs->firstlocal + i);
      else {  /* local variable */
        init_var(fs, var, i);
        if (vd->vd.kind == RDKVAVAR)  /* vararg parameter? */
          var->k = VVARGVAR;
      }
      return cast_int(var->k);
    }
  }
  return -1;  /* not found */
}


/*
** Mark block where variable at given level was defined
** (to emit close instructions later).
*/
static void markupval (FuncState *fs, int level) {
  BlockCnt *bl = fs->bl;
  while (bl->nactvar > level)
    bl = bl->previous;
  bl->upval = 1;
  fs->needclose = 1;
}


/*
** Mark that current block has a to-be-closed variable.
*/
static void marktobeclosed (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  bl->upval = 1;
  bl->insidetbc = 1;
  fs->needclose = 1;
}


/*
** Find a variable with the given name 'n'. If it is an upvalue, add
** this upvalue into all intermediate functions. If it is a global, set
** 'var' as 'void' as a flag.
*/
static void singlevaraux (FuncState *fs, TString *n, expdesc *var, int base) {
  int v = searchvar(fs, n, var);  /* look up variables at current level */
  if (v >= 0) {  /* found? */
    if (!base) {
      if (var->k == VVARGVAR)  /* vararg parameter? */
        luaK_vapar2local(fs, var);  /* change it to a regular local */
      if (var->k == VLOCAL)
        markupval(fs, var->u.var.vidx);  /* will be used as an upvalue */
    }
    /* else nothing else to be done */
  }
  else {  /* not found at current level; try upvalues */
    int idx = searchupvalue(fs, n);  /* try existing upvalues */
    if (idx < 0) {  /* not found? */
      if (fs->prev != NULL)  /* more levels? */
        singlevaraux(fs->prev, n, var, 0);  /* try upper levels */
      if (var->k == VLOCAL || var->k == VUPVAL)  /* local or upvalue? */
        idx  = newupvalue(fs, n, var);  /* will be a new upvalue */
      else  /* it is a global or a constant */
        return;  /* don't need to do anything at this level */
    }
    init_exp(var, VUPVAL, idx);  /* new or old upvalue */
  }
}


static void buildglobal (LexState *ls, TString *varname, expdesc *var) {
  FuncState *fs = ls->fs;
  expdesc key;
  init_exp(var, VGLOBAL, -1);  /* global by default */
  singlevaraux(fs, ls->envn, var, 1);  /* get environment variable */
  if (var->k == VGLOBAL)
    luaK_semerror(ls, "%s is global when accessing variable '%s'",
                      LUA_ENV, getstr(varname));
  luaK_exp2anyregup(fs, var);  /* _ENV could be a constant */
  codestring(&key, varname);  /* key is variable name */
  luaK_indexed(fs, var, &key);  /* 'var' represents _ENV[varname] */
}


/*
** Find a variable with the given name 'n', handling global variables
** too.
*/
static void buildvar (LexState *ls, TString *varname, expdesc *var) {
  FuncState *fs = ls->fs;
  init_exp(var, VGLOBAL, -1);  /* global by default */
  singlevaraux(fs, varname, var, 1);
  if (var->k == VGLOBAL)  /* global name? */
    buildglobal(ls, varname, var);
}


static void singlevar (LexState *ls, expdesc *var) {
  buildvar(ls, str_checkname(ls), var);
}


/*
** Adjust the number of results from an expression list 'e' with 'nexps'
** expressions to 'nvars' values.
*/
static void adjust_assign (LexState *ls, int nvars, int nexps, expdesc *e) {
  FuncState *fs = ls->fs;
  int needed = nvars - nexps;  /* extra values needed */
  luaK_checkstack(fs, needed);
  if (hasmultret(e->k)) {  /* last expression has multiple returns? */
    int extra = needed + 1;  /* discount last expression itself */
    if (extra < 0)
      extra = 0;
    luaK_setreturns(fs, e, extra);  /* last exp. provides the difference */
  }
  else {
    if (e->k != VVOID)  /* at least one expression? */
      luaK_exp2nextreg(fs, e);  /* close last expression */
    if (needed > 0)  /* missing values? */
      luaK_nil(fs, fs->freereg, needed);  /* complete with nils */
  }
  if (needed > 0)
    luaK_reserveregs(fs, needed);  /* registers for extra values */
  else  /* adding 'needed' is actually a subtraction */
    fs->freereg = cast_byte(fs->freereg + needed);  /* remove extra values */
}


#define enterlevel(ls)	luaE_incCstack(ls->L)


#define leavelevel(ls) ((ls)->L->nCcalls--)


/*
** Closes the goto at index 'g' to given 'label' and removes it
** from the list of pending gotos.
** The goto needs a CLOSE if it jumps out of a block with upvalues,
** or out of the scope of some variable and the block has upvalues
** (signaled by parameter 'bup').
*/
static void closegoto (LexState *ls, int g, Labeldesc *label, int bup) {
  int i;
  FuncState *fs = ls->fs;
  Labellist *gl = &ls->dyd->gt;  /* list of gotos */
  Labeldesc *gt = &gl->arr[g];  /* goto to be resolved */
  lua_assert(eqstr(gt->name, label->name));
  lua_assert(gt->nactvar >= label->nactvar);
  if (gt->close ||
      (label->nactvar < gt->nactvar && bup)) {  /* needs close? */
    lu_byte stklevel = reglevel(fs, label->nactvar);
    /* move jump to CLOSE position */
    fs->f->code[gt->pc + 1] = fs->f->code[gt->pc];
    /* put CLOSE instruction at original position */
    fs->f->code[gt->pc] = CREATE_ABCk(OP_CLOSE, stklevel, 0, 0, 0);
    gt->pc++;  /* must point to jump instruction */
  }
  luaK_patchlist(ls->fs, gt->pc, label->pc);  /* goto jumps to label */
  for (i = g; i < gl->n - 1; i++)  /* remove goto from pending list */
    gl->arr[i] = gl->arr[i + 1];
  gl->n--;
}


/*
** Search for an active label with the given name, starting at
** index 'ilb' (so that it can search for all labels in current block
** or all labels in current function).
*/
static Labeldesc *findlabel (LexState *ls, TString *name, int ilb) {
  Dyndata *dyd = ls->dyd;
  for (; ilb < dyd->label.n; ilb++) {
    Labeldesc *lb = &dyd->label.arr[ilb];
    if (eqstr(lb->name, name))  /* correct label? */
      return lb;
  }
  return NULL;  /* label not found */
}


/*
** Adds a new label/goto in the corresponding list.
*/
static int newlabelentry (LexState *ls, Labellist *l, TString *name,
                          int line, int pc) {
  int n = l->n;
  luaM_growvector(ls->L, l->arr, n, l->size,
                  Labeldesc, SHRT_MAX, "labels/gotos");
  l->arr[n].name = name;
  l->arr[n].line = line;
  l->arr[n].nactvar = ls->fs->nactvar;
  l->arr[n].close = 0;
  l->arr[n].pc = pc;
  l->n = n + 1;
  return n;
}


/*
** Create an entry for the goto and the code for it. As it is not known
** at this point whether the goto may need a CLOSE, the code has a jump
** followed by an CLOSE. (As the CLOSE comes after the jump, it is a
** dead instruction; it works as a placeholder.) When the goto is closed
** against a label, if it needs a CLOSE, the two instructions swap
** positions, so that the CLOSE comes before the jump.
*/
static int newgotoentry (LexState *ls, TString *name, int line) {
  FuncState *fs = ls->fs;
  int pc = luaK_jump(fs);  /* create jump */
  luaK_codeABC(fs, OP_CLOSE, 0, 1, 0);  /* spaceholder, marked as dead */
  return newlabelentry(ls, &ls->dyd->gt, name, line, pc);
}


/*
** Create a new label with the given 'name' at the end of a block
** (used for 'break' and 'escape' jumps). Solves all pending gotos to
** this new label.
*/
static void createlabel (LexState *ls, TString *name) {
  FuncState *fs = ls->fs;
  Labellist *ll = &ls->dyd->label;
  int l = newlabelentry(ls, ll, name, 0, luaK_getlabel(fs));
  /* locals are already out of scope */
  ll->arr[l].nactvar = fs->bl->nactvar;
}


/*
** Traverse the pending gotos of the finishing block checking whether
** each match some label of that block. Those that do not match are
** "exported" to the outer block, to be solved there. In particular,
** its 'nactvar' is updated with the level of the inner block,
** as the variables of the inner block are now out of scope.
*/
static void solvegotos (FuncState *fs, BlockCnt *bl) {
  LexState *ls = fs->ls;
  Labellist *gl = &ls->dyd->gt;
  int outlevel = reglevel(fs, bl->nactvar);  /* level outside the block */
  int igt = bl->firstgoto;  /* first goto in the finishing block */
  while (igt < gl->n) {   /* for each pending goto */
    Labeldesc *gt = &gl->arr[igt];
    /* search for a matching label in the current block */
    Labeldesc *lb = findlabel(ls, gt->name, bl->firstlabel);
    if (lb != NULL)  /* found a match? */
      closegoto(ls, igt, lb, bl->upval);  /* close and remove goto */
    else {  /* adjust 'goto' for outer block */
      /* block has variables to be closed and goto escapes the scope of
         some variable? */
      if (bl->upval && reglevel(fs, gt->nactvar) > outlevel)
        gt->close = 1;  /* jump may need a close */
      gt->nactvar = bl->nactvar;  /* correct level for outer block */
      igt++;  /* go to next goto */
    }
  }
  ls->dyd->label.n = bl->firstlabel;  /* remove local labels */
}


static void enterblock (FuncState *fs, BlockCnt *bl, lu_byte isloop) {
  bl->isloop = isloop;
  bl->nactvar = fs->nactvar;
  bl->firstlabel = fs->ls->dyd->label.n;
  bl->firstgoto = fs->ls->dyd->gt.n;
  bl->upval = 0;
  /* inherit 'insidetbc' from enclosing block */
  bl->insidetbc = (fs->bl != NULL && fs->bl->insidetbc);
  bl->tag = NULL;
  bl->hasescape = 0;
  bl->resreg = -1;
  bl->previous = fs->bl;  /* link block in function's block list */
  fs->bl = bl;
  lua_assert(fs->freereg == luaY_nvarstack(fs));
}


/*
** generates an error for an undefined 'escape'.
*/
static l_noret undefgoto (LexState *ls, Labeldesc *gt) {
  /* breaks are checked when created, cannot be undefined */
  lua_assert(!eqstr(gt->name, ls->brkn));
  luaK_semerror(ls, "no enclosing block ':%s' for <escape> at line %d",
                    getstr(gt->name), gt->line);
}


static void leaveblock (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  LexState *ls = fs->ls;
  lu_byte stklevel = reglevel(fs, bl->nactvar);  /* level outside block */
  if (bl->previous && bl->upval)  /* need a 'close'? */
    luaK_codeABC(fs, OP_CLOSE, stklevel, 0, 0);
  fs->freereg = stklevel;  /* free registers */
  removevars(fs, bl->nactvar);  /* remove block locals */
  lua_assert(bl->nactvar == fs->nactvar);  /* back to level on entry */
  if (bl->isloop == 2)  /* has to fix pending breaks? */
    createlabel(ls, ls->brkn);
  if (bl->tag != NULL && bl->hasescape)  /* has pending escapes? */
    createlabel(ls, bl->tag);
  solvegotos(fs, bl);
  if (bl->previous == NULL) {  /* was it the last block? */
    if (bl->firstgoto < ls->dyd->gt.n)  /* still pending gotos? */
      undefgoto(ls, &ls->dyd->gt.arr[bl->firstgoto]);  /* error */
  }
  fs->bl = bl->previous;  /* current block now is previous one */
}


/*
** adds a new prototype into list of prototypes
*/
static Proto *addprototype (LexState *ls) {
  Proto *clp;
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;  /* prototype of current function */
  if (fs->np >= f->sizep) {
    int oldsize = f->sizep;
    luaM_growvector(L, f->p, fs->np, f->sizep, Proto *, MAXARG_Bx, "functions");
    while (oldsize < f->sizep)
      f->p[oldsize++] = NULL;
  }
  f->p[fs->np++] = clp = luaF_newproto(L);
  luaC_objbarrier(L, f, clp);
  return clp;
}


/*
** codes instruction to create new closure in parent function.
** The OP_CLOSURE instruction uses the last available register,
** so that, if it invokes the GC, the GC knows which registers
** are in use at that time.

*/
static void codeclosure (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs->prev;
  init_exp(v, VRELOC, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np - 1));
  luaK_exp2nextreg(fs, v);  /* fix it at the last register */
}


static void open_func (LexState *ls, FuncState *fs, BlockCnt *bl) {
  lua_State *L = ls->L;
  Proto *f = fs->f;
  fs->prev = ls->fs;  /* linked list of funcstates */
  fs->ls = ls;
  ls->fs = fs;
  fs->pc = 0;
  fs->previousline = f->linedefined;
  fs->iwthabs = 0;
  fs->lasttarget = 0;
  fs->freereg = 0;
  fs->nk = 0;
  fs->nabslineinfo = 0;
  fs->np = 0;
  fs->nups = 0;
  fs->ndebugvars = 0;
  fs->nactvar = 0;
  fs->needclose = 0;
  fs->firstlocal = ls->dyd->actvar.n;
  fs->firstlabel = ls->dyd->label.n;
  fs->bl = NULL;
  f->source = ls->source;
  luaC_objbarrier(L, f, f->source);
  f->maxstacksize = 2;  /* registers 0/1 are always valid */
  fs->kcache = luaH_new(L);  /* create table for function */
  luaD_anchorobj(L, ls->h, obj2gco(fs->kcache));  /* anchor it */
  enterblock(fs, bl, 0);
}


static void close_func (LexState *ls) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  TValue temp;
  luaK_ret(fs, luaY_nvarstack(fs), 0);  /* final return */
  leaveblock(fs);
  lua_assert(fs->bl == NULL);
  luaK_finish(fs);
  luaM_shrinkvector(L, f->code, f->sizecode, fs->pc, Instruction);
  luaM_shrinkvector(L, f->lineinfo, f->sizelineinfo, fs->pc, ls_byte);
  luaM_shrinkvector(L, f->abslineinfo, f->sizeabslineinfo,
                       fs->nabslineinfo, AbsLineInfo);
  luaM_shrinkvector(L, f->k, f->sizek, fs->nk, TValue);
  luaM_shrinkvector(L, f->p, f->sizep, fs->np, Proto *);
  luaM_shrinkvector(L, f->locvars, f->sizelocvars, fs->ndebugvars, LocVar);
  luaM_shrinkvector(L, f->upvalues, f->sizeupvalues, fs->nups, Upvaldesc);
  /* remove kcache table from scanner table ("weigh" its anchor) */
  sethvalue(L, &temp, fs->kcache);  /* key to be set to nil */
  luaH_set(L, ls->h, &temp, &G(L)->nilvalue);
  ls->fs = fs->prev;
  luaC_checkGC(L);
}


/*
** {======================================================================
** GRAMMAR RULES
** =======================================================================
*/


/*
** check whether current token is in the follow set of a block.
*/
static int block_follow (LexState *ls) {
  switch (ls->t.token) {
    case '}': case TK_EOS:
      return 1;
    default: return 0;
  }
}


/*
** Discard the value of a statement that is not the last one of its
** block (calls use no results; other expressions are evaluated).
*/
static void discard (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  if (v->k == VCALL) {
    Instruction *inst = &getinstruction(fs, v);
    SETARG_C(*inst, 1);  /* call statement uses no results */
  }
  else if (v->k != VVOID)
    luaK_exp2anyreg(fs, v);  /* evaluate expression (for side effects) */
}


/*
** Parse a sequence of statements. The value of the last statement is
** left in 'v' ('n' values in consecutive registers, the last one
** described by 'v'); 'v' is void for an empty sequence.
*/
static void statlist (LexState *ls, expdesc *v, int *n) {
  /* statlist -> { stat } */
  FuncState *fs = ls->fs;
  int nstats = 0;
  init_exp(v, VVOID, 0);
  *n = 0;
  while (!block_follow(ls)) {
    if (nstats > 0 && sameline(ls))  /* statement in the same line? */
      luaX_syntaxerror(ls, "';' or new line expected");
    *n = statement(ls, v);
    nstats++;
    if (!block_follow(ls)) {  /* not the last statement? */
      discard(ls, v);
      init_exp(v, VVOID, 0);
      *n = 0;
      fs->freereg = luaY_nvarstack(fs);  /* free registers */
    }
  }
}


/*
** Parse a block ('{' statlist '}') whose value goes to register
** 'resreg' (or is discarded if 'resreg' is -1).
*/
static void block (LexState *ls, int resreg) {
  FuncState *fs = ls->fs;
  BlockCnt bl;
  expdesc v;
  int n;
  int line = ls->linenumber;
  enterblock(fs, &bl, 0);
  checknext(ls, '{');
  statlist(ls, &v, &n);
  if (resreg >= 0)
    store_value(ls, resreg, &v);
  else
    discard(ls, &v);
  ls->blockline = ls->lastline;  /* line of last statement */
  check_match(ls, '}', '{', line);
  leaveblock(fs);
}


/*
** Parse a branch of a conditional ('{' block '}' or '=>' expression),
** storing its value in register 'resreg'.
*/
static void branch (LexState *ls, int resreg) {
  if (ls->t.token == '{')
    block(ls, resreg);
  else {
    expdesc v;
    checknext(ls, TK_ARROW);
    expr(ls, &v);
    store_value(ls, resreg, &v);
    ls->blockline = ls->lastline;
  }
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/

typedef struct ConsControl {
  expdesc v;  /* last list item read */
  expdesc *t;  /* table descriptor */
  int nh;  /* total number of 'record' elements */
  int na;  /* number of array elements already stored */
  int tostore;  /* number of array elements pending to be stored */
  int maxtostore;  /* maximum number of pending elements */
} ConsControl;


/*
** Maximum number of elements in a constructor, to control the following:
** * counter overflows;
** * overflows in 'extra' for OP_NEWTABLE and OP_SETLIST;
** * overflows when adding multiple returns in OP_SETLIST.
*/
#define MAX_CNST	(INT_MAX/2)
#if MAX_CNST/(MAXARG_vC + 1) > MAXARG_Ax
#undef MAX_CNST
#define MAX_CNST	(MAXARG_Ax * (MAXARG_vC + 1))
#endif


/*
** Parse an '@' qualifier (after the '@'): '(' expr ')' | NUM | NAME | TAG
*/
static void atkey (LexState *ls, expdesc *key) {
  switch (ls->t.token) {
    case '(': {
      int line = ls->linenumber;
      luaX_next(ls);
      expr(ls, key);
      check_match(ls, ')', '(', line);
      luaK_exp2val(ls->fs, key);
      break;
    }
    case TK_INT: {
      init_exp(key, VKINT, 0);
      key->u.ival = ls->t.seminfo.i;
      luaX_next(ls);
      break;
    }
    case TK_FLT: {
      init_exp(key, VKFLT, 0);
      key->u.nval = ls->t.seminfo.r;
      luaX_next(ls);
      break;
    }
    case TK_TAG: {
      codestring(key, ls->t.seminfo.ts);
      luaX_next(ls);
      break;
    }
    case TK_NAME: {
      singlevar(ls, key);
      luaK_exp2val(ls->fs, key);
      break;
    }
    default:
      luaX_syntaxerror(ls, "name, number, tag, or '(' expected");
  }
}


static void recfield (LexState *ls, ConsControl *cc) {
  /* recfield -> ('@' atkey | NAME) = exp */
  FuncState *fs = ls->fs;
  lu_byte reg = ls->fs->freereg;
  expdesc tab, key, val;
  if (ls->t.token == TK_NAME)
    codename(ls, &key);
  else {  /* ls->t.token == '@' */
    luaX_next(ls);
    atkey(ls, &key);
  }
  cc->nh++;
  checknext(ls, '=');
  tab = *cc->t;
  luaK_indexed(fs, &tab, &key);
  expr(ls, &val);
  luaK_storevar(fs, &tab, &val);
  fs->freereg = reg;  /* free registers */
}


static void closelistfield (FuncState *fs, ConsControl *cc) {
  lua_assert(cc->tostore > 0);
  luaK_exp2nextreg(fs, &cc->v);
  cc->v.k = VVOID;
  if (cc->tostore >= cc->maxtostore) {
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);  /* flush */
    cc->na += cc->tostore;
    cc->tostore = 0;  /* no more items pending */
  }
}


static void lastlistfield (FuncState *fs, ConsControl *cc) {
  if (cc->tostore == 0) return;
  if (hasmultret(cc->v.k)) {
    luaK_setmultret(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, LUA_MULTRET);
    cc->na--;  /* do not count last expression (unknown number of elements) */
  }
  else {
    if (cc->v.k != VVOID)
      luaK_exp2nextreg(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);
  }
  cc->na += cc->tostore;
}


static void listfield (LexState *ls, ConsControl *cc) {
  /* listfield -> exp */
  expr(ls, &cc->v);
  cc->tostore++;
}


static void field (LexState *ls, ConsControl *cc) {
  /* field -> listfield | recfield */
  switch(ls->t.token) {
    case TK_NAME: {  /* may be 'listfield' or 'recfield' */
      if (luaX_lookahead(ls) != '=')  /* expression? */
        listfield(ls, cc);
      else
        recfield(ls, cc);
      break;
    }
    case '@': {
      recfield(ls, cc);
      break;
    }
    default: {
      listfield(ls, cc);
      break;
    }
  }
}


/*
** Compute a limit for how many registers a constructor can use before
** emitting a 'SETLIST' instruction, based on how many registers are
** available.
*/
static int maxtostore (FuncState *fs) {
  int numfreeregs = MAX_FSTACK - fs->freereg;
  if (numfreeregs >= 160)  /* "lots" of registers? */
    return numfreeregs / 5;  /* use up to 1/5 of them */
  else if (numfreeregs >= 80)  /* still "enough" registers? */
    return 10;  /* one 'SETLIST' instruction for each 10 values */
  else  /* save registers for potential more nesting */
    return 1;
}


static void constructor (LexState *ls, expdesc *t) {
  /* constructor -> '[' [ field { ',' field } [','] ] ']' */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int pc = luaK_codevABCk(fs, OP_NEWTABLE, 0, 0, 0, 0);
  ConsControl cc;
  luaK_code(fs, 0);  /* space for extra arg. */
  cc.na = cc.nh = cc.tostore = 0;
  cc.t = t;
  init_exp(t, VNONRELOC, fs->freereg);  /* table will be at stack top */
  luaK_reserveregs(fs, 1);
  init_exp(&cc.v, VVOID, 0);  /* no value (yet) */
  checknext(ls, '[');
  cc.maxtostore = maxtostore(fs);
  do {
    if (ls->t.token == ']') break;
    if (cc.v.k != VVOID)  /* is there a previous list item? */
      closelistfield(fs, &cc);  /* close it */
    field(ls, &cc);
    luaY_checklimit(fs, cc.tostore + cc.na + cc.nh, MAX_CNST,
                    "items in a constructor");
  } while (testnext(ls, ','));
  check_match(ls, ']', '[', line);
  lastlistfield(fs, &cc);
  luaK_settablesize(fs, pc, t->u.info, cc.na, cc.nh);
}


/*
** Tagged constructor ':X [ ... ]': a table with field 'tag' = "X".
*/
static void tagconstructor (LexState *ls, expdesc *t, TString *tag) {
  FuncState *fs = ls->fs;
  expdesc tab, key, val;
  constructor(ls, t);
  tab = *t;
  codestring(&key, luaX_newstring(ls, "tag", 3));
  luaK_indexed(fs, &tab, &key);
  codestring(&val, tag);
  luaK_storevar(fs, &tab, &val);
}

/* }====================================================================== */


static void setvararg (FuncState *fs) {
  fs->f->flag |= PF_VAHID;  /* by default, use hidden vararg arguments */
  luaK_codeABC(fs, OP_VARARGPREP, 0, 0, 0);
}


static void parlist (LexState *ls) {
  /* parlist -> [ {NAME ','} (NAME | '...') ] */
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int nparams = 0;
  int varargk = 0;
  if (ls->t.token != ')') {  /* is 'parlist' not empty? */
    do {
      switch (ls->t.token) {
        case TK_NAME: {
          new_localvar(ls, str_checkname(ls));
          nparams++;
          break;
        }
        case TK_DOTS: {
          varargk = 1;
          luaX_next(ls);  /* skip '...' */
          new_localvarliteral(ls, "(vararg table)");
          break;
        }
        default: luaX_syntaxerror(ls, "<name> or '...' expected");
      }
    } while (!varargk && testnext(ls, ','));
  }
  adjustlocalvars(ls, nparams);
  f->numparams = cast_byte(fs->nactvar);
  if (varargk) {
    setvararg(fs);  /* declared vararg */
    adjustlocalvars(ls, 1);  /* vararg parameter */
  }
  /* reserve registers for parameters (plus vararg parameter, if present) */
  luaK_reserveregs(fs, fs->nactvar);
}


/*
** Return the value of the last statement of a function body (or chunk).
*/
static void tailreturn (LexState *ls, expdesc *e, int n) {
  FuncState *fs = ls->fs;
  int first = luaY_nvarstack(fs);  /* first slot to be returned */
  int nret;
  if (e->k == VVOID)
    return;  /* no value: final 'return' comes from 'close_func' */
  if (n == 1)
    luaK_setoneret(fs, e);  /* single expression evaluates to one value */
  if (hasmultret(e->k)) {
    luaK_setmultret(fs, e);
    nret = LUA_MULTRET;  /* return all values */
  }
  else {
    if (n == 1)  /* only one single value? */
      first = luaK_exp2anyreg(fs, e);  /* can use original slot */
    else {  /* values must go to the top of the stack */
      luaK_exp2nextreg(fs, e);
      lua_assert(n == fs->freereg - first);
    }
    nret = n;
  }
  luaK_ret(fs, first, nret);
}


/*
** Function body block: '{' statlist '}', returning the last value.
*/
static void funcblock (LexState *ls, int line) {
  expdesc v;
  int n;
  checknext(ls, '{');
  statlist(ls, &v, &n);
  tailreturn(ls, &v, n);
  ls->fs->f->lastlinedefined = ls->linenumber;
  check_match(ls, '}', '{', line);
}


static void body (LexState *ls, expdesc *e, int ismethod, int line) {
  /* body ->  '(' parlist ')' funcblock */
  FuncState new_fs;
  BlockCnt bl;
  enterlevel(ls);  /* control recursion depth of nested functions */
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  open_func(ls, &new_fs, &bl);
  checknext(ls, '(');
  if (ismethod) {
    new_localvarliteral(ls, "self");  /* create 'self' parameter */
    adjustlocalvars(ls, 1);
  }
  parlist(ls);
  checknext(ls, ')');
  funcblock(ls, line);
  codeclosure(ls, e);
  close_func(ls);
  leavelevel(ls);
}


/*
** Lambda: '\' [NAME | '(' parlist ')'] funcblock
*/
static void lambda (LexState *ls, expdesc *e, int line) {
  FuncState new_fs;
  BlockCnt bl;
  enterlevel(ls);  /* control recursion depth of nested functions */
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  open_func(ls, &new_fs, &bl);
  if (testnext(ls, '(')) {
    parlist(ls);
    checknext(ls, ')');
  }
  else {
    FuncState *fs = ls->fs;
    if (ls->t.token == TK_NAME)
      new_localvar(ls, str_checkname(ls));
    else
      new_localvarliteral(ls, "it");
    adjustlocalvars(ls, 1);
    fs->f->numparams = 1;
    luaK_reserveregs(fs, 1);
  }
  funcblock(ls, line);
  codeclosure(ls, e);
  close_func(ls);
  leavelevel(ls);
}


static int explist (LexState *ls, expdesc *v) {
  /* explist -> expr { ',' expr } */
  int n = 1;  /* at least one expression */
  expr(ls, v);
  while (testnext(ls, ',')) {
    luaK_exp2nextreg(ls->fs, v);
    expr(ls, v);
    n++;
  }
  return n;
}


/*
** True if the current token can start a juxtaposed call argument
** (f "s", f [t], f :X, f \{}).
*/
static int check_call_arg (LexState *ls) {
  switch (ls->t.token) {
    case '[': case '\\': case TK_STRING: case TK_TAG:
      return 1;
    default:
      return 0;
  }
}


static void funcargs (LexState *ls, expdesc *f) {
  FuncState *fs = ls->fs;
  expdesc args;
  int base, nparams;
  int line = ls->linenumber;
  switch (ls->t.token) {
    case '(': {  /* funcargs -> '(' [ explist ] ')' */
      luaX_next(ls);
      if (ls->t.token == ')')  /* arg list is empty? */
        args.k = VVOID;
      else {
        explist(ls, &args);
        if (hasmultret(args.k))
          luaK_setmultret(fs, &args);
      }
      check_match(ls, ')', '(', line);
      break;
    }
    case '[': {  /* funcargs -> constructor */
      constructor(ls, &args);
      break;
    }
    case TK_STRING: {  /* funcargs -> STRING */
      codestring(&args, ls->t.seminfo.ts);
      luaX_next(ls);  /* must use 'seminfo' before 'next' */
      break;
    }
    case TK_TAG: {  /* funcargs -> TAG [constructor] */
      TString *tag = ls->t.seminfo.ts;
      luaX_next(ls);
      if (ls->t.token == '[' && sameline(ls))
        tagconstructor(ls, &args, tag);
      else
        codestring(&args, tag);
      break;
    }
    case '\\': {  /* funcargs -> lambda */
      luaX_next(ls);
      lambda(ls, &args, line);
      break;
    }
    default: {
      luaX_syntaxerror(ls, "function arguments expected");
      return;  /* to avoid warnings */
    }
  }
  lua_assert(f->k == VNONRELOC);
  base = f->u.info;  /* base register for call */
  if (hasmultret(args.k))
    nparams = LUA_MULTRET;  /* open call */
  else {
    if (args.k != VVOID)
      luaK_exp2nextreg(fs, &args);  /* close last argument */
    nparams = fs->freereg - (base+1);
  }
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams+1, 2));
  luaK_fixline(fs, line);
  /* call removes function and arguments and leaves one result (unless
     changed later) */
  fs->freereg = cast_byte(base + 1);
}




/*
** {======================================================================
** Expression parsing
** =======================================================================
*/


static int ifstat (LexState *ls, expdesc *v);
static int ifsstat (LexState *ls, expdesc *v);
static int dostat (LexState *ls, expdesc *v);
static int loopstat (LexState *ls, expdesc *v);


/*
** Parenthesized list: '(' explist ')' (the '(' is current).
** Returns the number of expressions; a single expression is
** "parenthesized" (truncated to one value).
*/
static int parenlist (LexState *ls, expdesc *v) {
  int line = ls->linenumber;
  int n;
  luaX_next(ls);  /* skip '(' */
  if (ls->t.token == ')')
    luaX_syntaxerror(ls, "unexpected symbol");
  n = explist(ls, v);
  check_match(ls, ')', '(', line);
  if (n == 1)
    luaK_dischargevars(ls->fs, v);  /* single value */
  return n;
}


/*
** A multi-valued parenthesized list used as a single value: keep only
** its first value.
*/
static void tuple2value (LexState *ls, expdesc *v, int n) {
  FuncState *fs = ls->fs;
  int base;
  luaK_exp2nextreg(fs, v);  /* last value */
  base = fs->freereg - n;  /* first value */
  fs->freereg = cast_byte(base + 1);  /* drop the other values */
  init_exp(v, VNONRELOC, base);
}


static void primaryexp (LexState *ls, expdesc *v) {
  /* primaryexp -> NAME | '(' explist ')' | literal | constructor |
                   'func' body | lambda | compound statement */
  int line = ls->linenumber;
  switch (ls->t.token) {
    case '(': {
      int n = parenlist(ls, v);
      if (n > 1)
        tuple2value(ls, v, n);
      return;
    }
    case TK_NAME: {
      singlevar(ls, v);
      return;
    }
    case TK_FLT: {
      init_exp(v, VKFLT, 0);
      v->u.nval = ls->t.seminfo.r;
      break;
    }
    case TK_INT: {
      init_exp(v, VKINT, 0);
      v->u.ival = ls->t.seminfo.i;
      break;
    }
    case TK_STRING: {
      codestring(v, ls->t.seminfo.ts);
      break;
    }
    case TK_TAG: {
      TString *tag = ls->t.seminfo.ts;
      luaX_next(ls);
      if (ls->t.token == '[' && sameline(ls))
        tagconstructor(ls, v, tag);
      else
        codestring(v, tag);
      return;
    }
    case TK_NIL: {
      init_exp(v, VNIL, 0);
      break;
    }
    case TK_TRUE: {
      init_exp(v, VTRUE, 0);
      break;
    }
    case TK_FALSE: {
      init_exp(v, VFALSE, 0);
      break;
    }
    case TK_DOTS: {  /* vararg */
      FuncState *fs = ls->fs;
      check_condition(ls, isvararg(fs->f),
                      "cannot use '...' outside a vararg function");
      init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, fs->f->numparams, 1));
      break;
    }
    case '[': {  /* constructor */
      constructor(ls, v);
      return;
    }
    case TK_FUNC: {
      luaX_next(ls);
      body(ls, v, 0, line);
      return;
    }
    case '\\': {
      luaX_next(ls);
      lambda(ls, v, line);
      return;
    }
    case TK_IF: ifstat(ls, v); return;
    case TK_IFS: ifsstat(ls, v); return;
    case TK_DO: dostat(ls, v); return;
    case TK_LOOP: loopstat(ls, v); return;
    default: {
      luaX_syntaxerror(ls, "unexpected symbol");
    }
  }
  luaX_next(ls);
}


static int is_prefix (expdesc *v) {
  switch (v->k) {
    case VLOCAL: case VUPVAL: case VCONST: case VVARGVAR:
    case VINDEXED: case VINDEXUP: case VINDEXI: case VINDEXSTR: case VVARGIND:
    case VCALL: case VNONRELOC: case VKSTR: case VRELOC:
      return 1;
    default:
      return 0;
  }
}


/*
** Suffixes: '.' NAME | '@' index | '::' NAME funcargs | funcargs
*/
static void suffixloop (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  for (;;) {
    switch (ls->t.token) {
      case '.': {  /* fieldsel */
        expdesc key;
        luaK_exp2anyregup(fs, v);
        luaX_next(ls);  /* skip the dot */
        codestring(&key, str_checkfield(ls));
        luaK_indexed(fs, v, &key);
        break;
      }
      case '@': {  /* index */
        expdesc key;
        luaK_exp2anyregup(fs, v);
        luaX_next(ls);  /* skip '@' */
        if (ls->t.token == '#' || ls->t.token == '+') {  /* t@# | t@+ */
          int plus = (ls->t.token == '+');
          int line = ls->linenumber;
          luaX_next(ls);
          luaK_exp2anyreg(fs, v);
          key = *v;  /* copy of table */
          luaK_prefix(fs, OPR_LEN, &key, line);  /* #t */
          if (plus) {  /* #t + 1 */
            expdesc one;
            init_exp(&one, VKINT, 0);
            one.u.ival = 1;
            luaK_infix(fs, OPR_ADD, &key);
            luaK_posfix(fs, OPR_ADD, &key, &one, line);
          }
          luaK_exp2val(fs, &key);
        }
        else
          atkey(ls, &key);
        luaK_indexed(fs, v, &key);
        break;
      }
      case TK_DBCOLON: {  /* '::' NAME funcargs */
        expdesc key;
        luaX_next(ls);
        codestring(&key, str_checkfield(ls));
        luaK_self(fs, v, &key);
        if (ls->t.token != '(' && !check_call_arg(ls))
          error_expected(ls, '(');
        funcargs(ls, v);
        break;
      }
      case '(': case TK_STRING: case '[': case TK_TAG: case '\\': {
        if (!sameline(ls) || !is_prefix(v))
          return;  /* a new statement */
        luaK_exp2nextreg(fs, v);
        funcargs(ls, v);
        break;
      }
      default: return;
    }
  }
}


static void suffixedexp (LexState *ls, expdesc *v) {
  primaryexp(ls, v);
  suffixloop(ls, v);
}


static UnOpr getunopr (int op) {
  switch (op) {
    case TK_NOT: return OPR_NOT;
    case '-': return OPR_MINUS;
    case '#': return OPR_LEN;
    default: return OPR_NOUNOPR;
  }
}


static BinOpr getbinopr (int op) {
  switch (op) {
    case '+': return OPR_ADD;
    case '-': return OPR_SUB;
    case '*': return OPR_MUL;
    case '%': return OPR_MOD;
    case TK_POW: return OPR_POW;
    case '/': return OPR_DIV;
    case TK_IDIV: return OPR_IDIV;
    case TK_CAT: return OPR_CONCAT;
    case TK_NE: return OPR_NE;
    case TK_EQ: return OPR_EQ;
    case '<': return OPR_LT;
    case TK_LE: return OPR_LE;
    case '>': return OPR_GT;
    case TK_GE: return OPR_GE;
    case TK_AND: return OPR_AND;
    case TK_OR: return OPR_OR;
    default: return OPR_NOBINOPR;
  }
}


/*
** unaryexp -> (suffixedexp | unop unaryexp)
*/
static void unaryexp (LexState *ls, expdesc *v) {
  UnOpr uop;
  enterlevel(ls);
  uop = getunopr(ls->t.token);
  if (uop != OPR_NOUNOPR) {  /* prefix (unary) operator? */
    int line = ls->linenumber;
    luaX_next(ls);  /* skip operator */
    unaryexp(ls, v);
    luaK_prefix(ls->fs, uop, v, line);
  }
  else suffixedexp(ls, v);
  leavelevel(ls);
}


/*
** binloop -> { binop unaryexp }
** All binary operators have the same precedence and are left
** associative; mixing different operators requires parentheses.
*/
static void binloop (LexState *ls, expdesc *v) {
  BinOpr op = getbinopr(ls->t.token);
  BinOpr first = op;
  while (op != OPR_NOBINOPR) {
    expdesc v2;
    int line = ls->linenumber;
    if (op != first)
      luaX_syntaxerror(ls, "mixed operators require parentheses");
    luaX_next(ls);  /* skip operator */
    luaK_infix(ls->fs, op, v);
    unaryexp(ls, &v2);
    luaK_posfix(ls->fs, op, v, &v2, line);
    op = getbinopr(ls->t.token);
  }
  switch (ls->t.token) {  /* unsupported operators */
    case TK_IS: case TK_ISNOT: case TK_XIN: case TK_XNOTIN:
    case TK_SEQ: case TK_SNE: case TK_SGE: case TK_SLE:
    case TK_LPIPE1: case TK_LPIPE2: case TK_RPIPE1: case TK_RPIPE2:
    case TK_WHERE:
      luaX_syntaxerror(ls, "operator not implemented");
    default: break;
  }
}


static void expr (LexState *ls, expdesc *v) {
  unaryexp(ls, v);
  binloop(ls, v);
}


/*
** An expression that may be a parenthesized list of values:
** returns the number of values (as 'explist').
*/
static int exprlist (LexState *ls, expdesc *v) {
  if (ls->t.token == '(') {
    int n = parenlist(ls, v);
    if (n > 1)
      return n;  /* multiple values */
    suffixloop(ls, v);  /* continue the expression */
    binloop(ls, v);
    return 1;
  }
  else {
    expr(ls, v);
    return 1;
  }
}

/* }==================================================================== */



/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/


/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
struct LHS_assign {
  struct LHS_assign *prev;
  expdesc v;  /* variable (global, local, upvalue, or indexed) */
};


/*
** check whether, in an assignment to an upvalue/local variable, the
** upvalue/local variable is begin used in a previous assignment to a
** table. If so, save original upvalue/local value in a safe place and
** use this safe copy in the previous assignment.
*/
static void check_conflict (LexState *ls, struct LHS_assign *lh, expdesc *v) {
  FuncState *fs = ls->fs;
  lu_byte extra = fs->freereg;  /* eventual position to save local variable */
  int conflict = 0;
  for (; lh; lh = lh->prev) {  /* check all previous assignments */
    if (vkisindexed(lh->v.k)) {  /* assignment to table field? */
      if (lh->v.k == VINDEXUP) {  /* is table an upvalue? */
        if (v->k == VUPVAL && lh->v.u.ind.t == v->u.info) {
          conflict = 1;  /* table is the upvalue being assigned now */
          lh->v.k = VINDEXSTR;
          lh->v.u.ind.t = extra;  /* assignment will use safe copy */
        }
      }
      else {  /* table is a register */
        if (v->k == VLOCAL && lh->v.u.ind.t == v->u.var.ridx) {
          conflict = 1;  /* table is the local being assigned now */
          lh->v.u.ind.t = extra;  /* assignment will use safe copy */
        }
        /* is index the local being assigned? */
        if (lh->v.k == VINDEXED && v->k == VLOCAL &&
            lh->v.u.ind.idx == v->u.var.ridx) {
          conflict = 1;
          lh->v.u.ind.idx = extra;  /* previous assignment will use safe copy */
        }
      }
    }
  }
  if (conflict) {
    /* copy upvalue/local value to a temporary (in position 'extra') */
    if (v->k == VLOCAL)
      luaK_codeABC(fs, OP_MOVE, extra, v->u.var.ridx, 0);
    else
      luaK_codeABC(fs, OP_GETUPVAL, extra, v->u.info, 0);
    luaK_reserveregs(fs, 1);
  }
}


/* Create code to store the "top" register in 'var' */
static void storevartop (FuncState *fs, expdesc *var) {
  expdesc e;
  init_exp(&e, VNONRELOC, fs->freereg - 1);
  luaK_storevar(fs, var, &e);  /* will also free the top register */
}


/*
** Parse and compile a multiple assignment. The first "variable"
** (a 'suffixedexp') was already read by the caller.
**
** assignment -> suffixedexp restassign
** restassign -> ',' suffixedexp restassign | '=' exprlist
*/
static void restassign (LexState *ls, struct LHS_assign *lh, int nvars) {
  expdesc e;
  check_condition(ls, vkisvar(lh->v.k), "assignable expression expected");
  check_readonly(ls, &lh->v);
  if (testnext(ls, ',')) {  /* restassign -> ',' suffixedexp restassign */
    struct LHS_assign nv;
    nv.prev = lh;
    suffixedexp(ls, &nv.v);
    if (!vkisindexed(nv.v.k))
      check_conflict(ls, lh, &nv.v);
    enterlevel(ls);  /* control recursion depth */
    restassign(ls, &nv, nvars+1);
    leavelevel(ls);
  }
  else {  /* restassign -> '=' exprlist */
    int nexps;
    checknext(ls, '=');
    nexps = exprlist(ls, &e);
    if (nexps != nvars)
      adjust_assign(ls, nvars, nexps, &e);
    else {
      luaK_setoneret(ls->fs, &e);  /* close last expression */
      luaK_storevar(ls->fs, &lh->v, &e);
      return;  /* avoid default */
    }
  }
  storevartop(ls->fs, &lh->v);  /* default assignment */
}


static void setstat (LexState *ls) {
  /* setstat -> SET suffixedexp restassign */
  struct LHS_assign v;
  luaX_next(ls);  /* skip 'set' */
  suffixedexp(ls, &v.v);
  v.prev = NULL;
  restassign(ls, &v, 1);
}


/*
** Read a condition and generate code to jump when it is false.
*/
static int cond (LexState *ls) {
  /* cond -> exp */
  expdesc v;
  expr(ls, &v);  /* read condition */
  if (v.k == VNIL) v.k = VFALSE;  /* 'falses' are all equal here */
  luaK_goiftrue(ls->fs, &v);
  return v.f;
}


/*
** Find the innermost enclosing loop block.
*/
static BlockCnt *loopblock (LexState *ls, const char *what) {
  BlockCnt *bl;
  for (bl = ls->fs->bl; bl != NULL; bl = bl->previous) {
    if (bl->isloop)  /* found one? */
      return bl;
  }
  luaX_syntaxerror(ls, luaO_pushfstring(ls->L, "%s outside loop", what));
  return NULL;  /* to avoid warnings */
}


/*
** Jump to the end of loop block 'bl' (a 'goto break').
*/
static void breakjump (LexState *ls, BlockCnt *bl, int line) {
  bl->isloop = 2;  /* signal that block has pending breaks */
  newgotoentry(ls, ls->brkn, line);
}


/*
** Parse the arguments of 'break'/'until'/'while'/'escape' after the
** first one: ',' explist ')'. Stores the first extra value (or 'def'
** when there is none) into the block's value register.
*/
static void storeextra (LexState *ls, BlockCnt *bl, expdesc *def) {
  expdesc e;
  if (testnext(ls, ',')) {
    expr(ls, &e);
    while (testnext(ls, ',')) {  /* extra values are evaluated and dropped */
      luaK_exp2nextreg(ls->fs, &e);
      expr(ls, &e);
    }
  }
  else
    e = *def;
  if (bl->resreg >= 0)
    store_value(ls, bl->resreg, &e);
  else
    discard(ls, &e);
}


static void breakstat (LexState *ls, int line) {
  /* breakstat -> BREAK '(' [explist] ')' */
  BlockCnt *bl = loopblock(ls, "break");
  expdesc nilv;
  luaX_next(ls);  /* skip 'break' */
  checknext(ls, '(');
  init_exp(&nilv, VNIL, 0);
  if (ls->t.token != ')') {
    expdesc e;
    expr(ls, &e);
    while (testnext(ls, ',')) {
      luaK_exp2nextreg(ls->fs, &e);
      expr(ls, &e);
    }
    if (bl->resreg >= 0) store_value(ls, bl->resreg, &e);
    else discard(ls, &e);
  }
  else if (bl->resreg >= 0)
    store_value(ls, bl->resreg, &nilv);
  checknext(ls, ')');
  breakjump(ls, bl, line);
}


static void untilstat (LexState *ls, int line) {
  /* untilstat -> UNTIL '(' cond [',' explist] ')'  (break when true) */
  FuncState *fs = ls->fs;
  BlockCnt *bl = loopblock(ls, "until");
  expdesc e, c;
  int jf;
  luaX_next(ls);  /* skip 'until' */
  checknext(ls, '(');
  expr(ls, &e);
  luaK_exp2anyreg(fs, &e);  /* condition value (to be the loop value) */
  c = e;
  luaK_goiftrue(fs, &c);  /* jump if false (frees the register) */
  luaK_reserveregs(fs, 1);  /* keep the condition value for the store */
  jf = c.f;
  storeextra(ls, bl, &e);  /* true: loop value is extra or condition */
  breakjump(ls, bl, line);
  luaK_patchtohere(fs, jf);
  checknext(ls, ')');
}


static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE '(' cond [',' explist] ')'  (break when false) */
  FuncState *fs = ls->fs;
  BlockCnt *bl = loopblock(ls, "while");
  expdesc nilv;
  int jf, jt;
  luaX_next(ls);  /* skip 'while' */
  checknext(ls, '(');
  jf = cond(ls);  /* jump if false */
  jt = luaK_jump(fs);  /* true: continue loop */
  luaK_patchtohere(fs, jf);
  init_exp(&nilv, VNIL, 0);
  storeextra(ls, bl, &nilv);  /* false: loop value is extra or nil */
  breakjump(ls, bl, line);
  luaK_patchtohere(fs, jt);
  checknext(ls, ')');
}


static void escapestat (LexState *ls, int line) {
  /* escapestat -> ESCAPE '(' TAG [constructor] [',' explist] ')' */
  FuncState *fs = ls->fs;
  BlockCnt *bl;
  TString *tag;
  expdesc tv;
  luaX_next(ls);  /* skip 'escape' */
  checknext(ls, '(');
  check(ls, TK_TAG);
  tag = ls->t.seminfo.ts;
  for (bl = fs->bl; bl != NULL; bl = bl->previous) {
    if (bl->tag == tag)  /* found matching block? */
      break;
  }
  if (bl == NULL)
    luaK_semerror(ls, "no enclosing block ':%s' for <escape>", getstr(tag));
  luaX_next(ls);  /* skip tag */
  if (ls->t.token == '[' && sameline(ls))
    tagconstructor(ls, &tv, tag);
  else
    codestring(&tv, tag);
  storeextra(ls, bl, &tv);
  checknext(ls, ')');
  bl->hasescape = 1;
  newgotoentry(ls, tag, line);
}


/*
** if -> IF cond ('{' block '}' | '=>' expr) [ELSE ('{' block '}' | '=>' expr)]
*/
static int ifstat (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int reg = luaY_nvarstack(fs);  /* register for the value */
  int condfalse, escape;
  luaX_next(ls);  /* skip 'if' */
  condfalse = cond(ls);
  branch(ls, reg);  /* 'then' part */
  escape = luaK_jump(fs);
  luaK_fixline(fs, ls->blockline);  /* jump "belongs" to the branch */
  luaK_patchtohere(fs, condfalse);
  if (testnext(ls, TK_ELSE))
    branch(ls, reg);  /* 'else' part */
  else if (testnext(ls, TK_ARROW)) {  /* if c => a => b */
    expdesc e;
    expr(ls, &e);
    store_value(ls, reg, &e);
  }
  else {
    expdesc e;
    init_exp(&e, VVOID, 0);
    store_value(ls, reg, &e);  /* no else: nil */
  }
  luaK_patchtohere(fs, escape);
  value_result(ls, reg, v);
  return 1;
}


/*
** ifs -> IFS '{' { cond '=>' branch } [ELSE '=>' branch] '}'
*/
static int ifsstat (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int reg = luaY_nvarstack(fs);  /* register for the value */
  int escapelist = NO_JUMP;
  int haselse = 0;
  int line = ls->linenumber;
  luaX_next(ls);  /* skip 'ifs' */
  checknext(ls, '{');
  while (ls->t.token != '}') {
    if (testnext(ls, TK_ELSE)) {
      checknext(ls, TK_ARROW);
      if (ls->t.token == '{')
        block(ls, reg);
      else {
        expdesc e;
        expr(ls, &e);
        store_value(ls, reg, &e);
      }
      haselse = 1;
      break;
    }
    else {
      int condfalse = cond(ls);
      checknext(ls, TK_ARROW);
      if (ls->t.token == '{')
        block(ls, reg);
      else {
        expdesc e;
        expr(ls, &e);
        store_value(ls, reg, &e);
      }
      luaK_concat(fs, &escapelist, luaK_jump(fs));
      luaK_fixline(fs, ls->blockline);  /* jump "belongs" to the branch */
      luaK_patchtohere(fs, condfalse);
    }
  }
  if (!haselse) {
    expdesc e;
    init_exp(&e, VVOID, 0);
    store_value(ls, reg, &e);  /* no case: nil */
  }
  check_match(ls, '}', '{', line);
  luaK_patchtohere(fs, escapelist);
  value_result(ls, reg, v);
  return 1;
}


/*
** do -> DO [TAG] '{' block '}' | DO '(' explist ')'
*/
static int dostat (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  luaX_next(ls);  /* skip 'do' */
  if (ls->t.token == '(') {  /* do(e): the expression itself */
    int n = parenlist(ls, v);
    if (n > 1) tuple2value(ls, v, n);
    return 1;
  }
  else {
    int reg = luaY_nvarstack(fs);  /* register for the value */
    BlockCnt bl;
    expdesc e;
    int n;
    int line;
    TString *tag = NULL;
    if (ls->t.token == TK_TAG) {
      tag = ls->t.seminfo.ts;
      luaX_next(ls);
    }
    line = ls->linenumber;
    enterblock(fs, &bl, 0);
    bl.tag = tag;
    bl.resreg = cast_short(reg);
    checknext(ls, '{');
    statlist(ls, &e, &n);
    store_value(ls, reg, &e);
    check_match(ls, '}', '{', line);
    leaveblock(fs);
    value_result(ls, reg, v);
    return 1;
  }
}


/*
** Fix for instruction at position 'pc' to jump to 'dest'.
** (Jump addresses are relative in Lua). 'back' true means
** a back jump.
*/
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
** Generate code for a 'for' loop body.
*/
static void forbody (LexState *ls, int base, int line, int nvars, int isgen) {
  /* forbody -> '{' block '}' */
  static const OpCode forprep[2] = {OP_FORPREP, OP_TFORPREP};
  static const OpCode forloop[2] = {OP_FORLOOP, OP_TFORLOOP};
  BlockCnt bl;
  FuncState *fs = ls->fs;
  int prep, endfor;
  check(ls, '{');
  prep = luaK_codeABx(fs, forprep[isgen], base, 0);
  fs->freereg--;  /* both 'forprep' remove one register from the stack */
  enterblock(fs, &bl, 0);  /* scope for declared variables */
  adjustlocalvars(ls, nvars);
  luaK_reserveregs(fs, nvars);
  block(ls, -1);
  leaveblock(fs);  /* end of scope for declared variables */
  fixforjump(fs, prep, luaK_getlabel(fs), 0);
  if (isgen) {  /* generic for? */
    luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
    luaK_fixline(fs, line);
  }
  endfor = luaK_codeABx(fs, forloop[isgen], base, 0);
  fixforjump(fs, endfor, prep + 1, 1);
  luaK_fixline(fs, line);
}


/*
** Numeric loop: the three values (initial, limit, step) are already
** in registers 'base', 'base+1', 'base+2'.
*/
static void fornum (LexState *ls, TString *varname, int base, int line) {
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  new_varkind(ls, varname, RDKCONST);  /* control variable */
  adjustlocalvars(ls, 2);  /* start scope for internal variables */
  forbody(ls, base, line, 1, 0);
}


/*
** Generic loop: the iterator values are already in registers starting
** at 'base' (adjusted to 4 values).
*/
static void forlist (LexState *ls, TString *names[], int nnames, int base,
                     int line) {
  FuncState *fs = ls->fs;
  int i;
  /* create internal variables */
  new_localvarliteral(ls, "(for state)");  /* iterator function */
  new_localvarliteral(ls, "(for state)");  /* state */
  new_localvarliteral(ls, "(for state)");  /* closing var. (after swap) */
  new_varkind(ls, names[0], RDKCONST);  /* control variable */
  for (i = 1; i < nnames; i++)
    new_localvar(ls, names[i]);
  adjustlocalvars(ls, 3);  /* start scope for internal variables */
  marktobeclosed(fs);  /* last internal var. must be closed */
  luaK_checkstack(fs, 2);  /* extra space to call iterator */
  forbody(ls, base, line, nnames, 1);
}


#define MAXLOOPVARS	32

/*
** loop -> LOOP '{' block '}'
**       | LOOP NAME '{' block '}'
**       | LOOP NAME {',' NAME} IN exprlist '{' block '}'
*/
static int loopstat (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int reg = luaY_nvarstack(fs);  /* register for the value */
  int line = ls->linenumber;
  BlockCnt bl;
  luaX_next(ls);  /* skip 'loop' */
  enterblock(fs, &bl, 1);  /* loop block */
  bl.resreg = cast_short(reg);
  if (ls->t.token == '{') {  /* infinite loop */
    int loopinit = luaK_getlabel(fs);
    block(ls, -1);
    luaK_jumpto(fs, loopinit);
    luaK_fixline(fs, ls->blockline);  /* jump "belongs" to the body */
  }
  else {
    TString *names[MAXLOOPVARS];
    int nnames = 0;
    int base = fs->freereg;
    do {
      if (nnames >= MAXLOOPVARS)
        luaX_syntaxerror(ls, "too many loop variables");
      names[nnames++] = str_checkname(ls);
    } while (testnext(ls, ','));
    if (ls->t.token == '{') {  /* numeric infinite loop */
      if (nnames != 1)
        luaX_syntaxerror(ls, "'in' expected");
      luaK_int(fs, fs->freereg, 1); luaK_reserveregs(fs, 1);
      luaK_int(fs, fs->freereg, LUA_MAXINTEGER); luaK_reserveregs(fs, 1);
      luaK_int(fs, fs->freereg, 1); luaK_reserveregs(fs, 1);
      fornum(ls, names[0], base, line);
    }
    else {
      expdesc e;
      int n;
      checknext(ls, TK_IN);
      line = ls->linenumber;
      n = exprlist(ls, &e);
      if (nnames == 1 && (n == 2 || n == 3)) {
        /* numeric loop */
        luaK_exp2nextreg(fs, &e);
        if (n == 2) {  /* default step = 1 */
          luaK_int(fs, fs->freereg, 1);
          luaK_reserveregs(fs, 1);
        }
        fornum(ls, names[0], base, line);
      }
      else {  /* generic loop */
        adjust_assign(ls, 4, n, &e);
        forlist(ls, names, nnames, base, line);
      }
    }
  }
  luaK_nil(fs, reg, 1);  /* normal exit: loop value is nil */
  leaveblock(fs);  /* loop scope ('break' jumps to this point) */
  value_result(ls, reg, v);
  return 1;
}


static void localfunc (LexState *ls, int line) {
  expdesc b;
  FuncState *fs = ls->fs;
  int fvar = fs->nactvar;  /* function's variable index */
  new_varkind(ls, str_checkname(ls), RDKCONST);  /* new local variable */
  adjustlocalvars(ls, 1);  /* enter its scope */
  body(ls, &b, 0, line);  /* function created in next register */
  /* debug information will only see the variable after this point! */
  localdebuginfo(fs, fvar)->startpc = fs->pc;
}


static void checktoclose (FuncState *fs, int level) {
  if (level != -1) {  /* is there a to-be-closed variable? */
    marktobeclosed(fs);
    luaK_codeABC(fs, OP_TBC, reglevel(fs, level), 0, 0);
  }
}


static void localstat (LexState *ls, int line) {
  /* stat -> (VAL | VAR | PIN) NAME { ',' NAME } ['=' exprlist] */
  FuncState *fs = ls->fs;
  int toclose = -1;  /* index of to-be-closed variable (if any) */
  int nvars = 0;
  int nexps;
  expdesc e;
  lu_byte kind;
  switch (ls->t.token) {
    case TK_VAL: kind = RDKCONST; break;
    case TK_PIN: kind = RDKTOCLOSE; break;
    default: kind = VDKREG; break;
  }
  luaX_next(ls);  /* skip 'val'/'var'/'pin' */
  if (kind == RDKCONST && ls->t.token == TK_FUNC) {  /* val func? */
    luaX_next(ls);
    localfunc(ls, line);
    return;
  }
  do {  /* for each variable */
    TString *vname = str_checkname(ls);  /* get its name */
    new_varkind(ls, vname, kind);  /* predeclare it */
    if (kind == RDKTOCLOSE) {  /* to-be-closed? */
      if (toclose != -1)  /* one already present? */
        luaK_semerror(ls, "multiple to-be-closed variables in local list");
      toclose = fs->nactvar + nvars;
    }
    nvars++;
  } while (testnext(ls, ','));
  if (testnext(ls, '='))  /* initialization? */
    nexps = exprlist(ls, &e);
  else {
    e.k = VVOID;
    nexps = 0;
  }
  adjust_assign(ls, nvars, nexps, &e);
  adjustlocalvars(ls, nvars);
  checktoclose(fs, toclose);
}


static int funcname (LexState *ls, expdesc *v) {
  /* funcname -> NAME {'.' NAME} ['::' NAME] */
  int ismethod = 0;
  singlevar(ls, v);
  while (ls->t.token == '.' || ls->t.token == TK_DBCOLON) {
    expdesc key;
    luaK_exp2anyregup(ls->fs, v);
    if (ls->t.token == TK_DBCOLON) ismethod = 1;
    luaX_next(ls);  /* skip the dot or '::' */
    codestring(&key, str_checkfield(ls));
    luaK_indexed(ls->fs, v, &key);
    if (ismethod) break;
  }
  return ismethod;
}


static void funcstat (LexState *ls, int line) {
  /* funcstat -> FUNC funcname body */
  int ismethod;
  expdesc v, b;
  luaX_next(ls);  /* skip 'func' */
  ismethod = funcname(ls, &v);
  check_readonly(ls, &v);
  body(ls, &b, ismethod, line);
  luaK_storevar(ls->fs, &v, &b);
  luaK_fixline(ls->fs, line);  /* definition "happens" in the first line */
}


static void retstat (LexState *ls) {
  /* stat -> RETURN '(' [explist] ')' */
  FuncState *fs = ls->fs;
  expdesc e;
  int nret;  /* number of values being returned */
  int first = luaY_nvarstack(fs);  /* first slot to be returned */
  int line = ls->linenumber;
  luaX_next(ls);  /* skip 'return' */
  checknext(ls, '(');
  if (ls->t.token == ')')
    nret = 0;  /* return no values */
  else {
    nret = explist(ls, &e);  /* optional return values */
    if (hasmultret(e.k)) {
      luaK_setmultret(fs, &e);
      if (e.k == VCALL && nret == 1 && !fs->bl->insidetbc) {  /* tail call? */
        SET_OPCODE(getinstruction(fs,&e), OP_TAILCALL);
        lua_assert(GETARG_A(getinstruction(fs,&e)) == luaY_nvarstack(fs));
      }
      nret = LUA_MULTRET;  /* return all values */
    }
    else {
      if (nret == 1)  /* only one single value? */
        first = luaK_exp2anyreg(fs, &e);  /* can use original slot */
      else {  /* values must go to the top of the stack */
        luaK_exp2nextreg(fs, &e);
        lua_assert(nret == fs->freereg - first);
      }
    }
  }
  check_match(ls, ')', '(', line);
  luaK_ret(fs, first, nret);
}


/*
** Parse a statement; its value (if any) is left in 'v'.
** Returns the number of values (0 for no value).
*/
static int statement (LexState *ls, expdesc *v) {
  int line = ls->linenumber;  /* may be needed for error messages */
  int n = 0;
  init_exp(v, VVOID, 0);
  /* (expression statements control recursion depth in 'unaryexp') */
  switch (ls->t.token) {
    case TK_VAL: case TK_VAR: case TK_PIN: {
      localstat(ls, line);
      break;
    }
    case TK_SET: {
      setstat(ls);
      break;
    }
    case TK_FUNC: {
      if (luaX_lookahead(ls) == '(') {  /* anonymous function? */
        n = exprlist(ls, v);  /* expression statement */
        break;
      }
      funcstat(ls, line);
      break;
    }
    case TK_RETURN: {
      retstat(ls);
      break;
    }
    case TK_BREAK: {
      breakstat(ls, line);
      break;
    }
    case TK_UNTIL: {
      untilstat(ls, line);
      break;
    }
    case TK_WHILE: {
      whilestat(ls, line);
      break;
    }
    case TK_ESCAPE: {
      escapestat(ls, line);
      break;
    }
    case TK_DEFER: case TK_CATCH: case TK_AWAIT:
    case TK_EMIT: case TK_SPAWN: case TK_TOGGLE: case TK_PAR:
    case TK_WATCHING: case TK_THREAD: case TK_TASK: case TK_TASKS:
    case TK_TEST: case TK_MATCH: case TK_PUB: {
      luaX_syntaxerror(ls, "statement not implemented");
      break;
    }
    default: {  /* expression statement */
      n = exprlist(ls, v);
      break;
    }
  }
  lua_assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
             ls->fs->freereg >= luaY_nvarstack(ls->fs));
  return n;
}

/* }====================================================================== */


/*
** compiles the main function, which is a regular vararg function with an
** upvalue named LUA_ENV
*/
static void mainfunc (LexState *ls, FuncState *fs) {
  BlockCnt bl;
  Upvaldesc *env;
  expdesc v;
  int n;
  open_func(ls, fs, &bl);
  setvararg(fs);  /* main function is always vararg */
  env = allocupvalue(fs);  /* ...set environment upvalue */
  env->instack = 1;
  env->idx = 0;
  env->kind = VDKREG;
  env->name = ls->envn;
  luaC_objbarrier(ls->L, fs->f, env->name);
  luaX_next(ls);  /* read first token */
  statlist(ls, &v, &n);  /* parse main body */
  tailreturn(ls, &v, n);
  check(ls, TK_EOS);
  close_func(ls);
}


LClosure *luaY_parser (lua_State *L, ZIO *z, Table *anchor, Mbuffer *buff,
                       Dyndata *dyd, const char *name, int firstchar) {
  LexState lexstate;
  FuncState funcstate;
  LClosure *cl;
  lexstate.h = anchor;  /* table for scanner */
  cl = luaF_newLclosure(L, 1);  /* create main closure */
  luaD_anchorobj(L, anchor, obj2gco(cl));  /* anchor it in scanner table */
  funcstate.f = cl->p = luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  funcstate.f->source = luaS_new(L, name);  /* create and anchor TString */
  luaC_objbarrier(L, funcstate.f, funcstate.f->source);
  lexstate.buff = buff;
  lexstate.dyd = dyd;
  dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
  luaX_setinput(L, &lexstate, z, funcstate.f->source, firstchar);
  mainfunc(&lexstate, &funcstate);
  lua_assert(!funcstate.prev && funcstate.nups == 1 && !lexstate.fs);
  /* all scopes should be correctly finished */
  lua_assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  return cl;
}
