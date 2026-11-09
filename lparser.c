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
** Jumps out of task bodies ('break'/'until'/'while', 'escape',
** 'return' inside 'spawn { }', 'par' branches, 'toggle on' bodies)
** are throws of status LUA_ERRJUMP caught by a try region of the
** block that owns the task (or around the 'par'/'toggle' call), whose
** handler dispatches them to their targets. (See catch.md, option B.)
*/
#define JK_BREAK	1
#define JK_ESCAPE	2
#define JK_RETURN	3

#define MAXJUMPS	8

typedef struct JumpRec {
  int kind;
  TString *tag;  /* escape tag */
  struct BlockCnt *target;  /* loop or 'do :tag' block (NULL for return) */
} JumpRec;

typedef struct Owner {
  struct Owner *previous;  /* enclosing open owner region */
  int pc;  /* its OP_TRY */
  int reg;  /* its register (values at reg+1..) */
  int needtry;  /* some task body jumps out */
  int njumps;
  JumpRec jumps[MAXJUMPS];
} Owner;


/*
** Values of a compound statement ('do', 'loop', 'if', 'ifs', 'match',
** 'test'): each exit yields a known number of values (0, 1 or those
** of a tuple), stored at 'reg..'; exits are grouped by arity and the
** shorter ones padded with nils to the maximum at the end of the
** construct (see .claude/plans/expressions.md, "Several values").
*/
#define MAXARITY	15

typedef struct ResDesc {
  int reg;  /* first value register */
  unsigned int arities;  /* bit k set: some exit yields k values */
  int endarity;  /* arity of the normal end (falls through) */
  int jumps[MAXARITY + 1];  /* pending jumps of branch ends, per arity */
} ResDesc;


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
  lu_byte nregions;  /* try regions to pop when leaving the block */
  ResDesc *res;  /* values of the construct ('break'/'escape' target) */
  struct Owner *owner;  /* region owning the tasks spawned in the block */
  struct Owner ownerbuf;  /* its storage (no heap: parse errors longjmp) */
} BlockCnt;



/*
** prototypes for recursive non-terminal functions
*/
static int statement (LexState *ls, expdesc *v);
static void expr (LexState *ls, expdesc *v);
static int exprlist (LexState *ls, expdesc *v);
static void statlist (LexState *ls, expdesc *v, int *n);
static void primaryexp (LexState *ls, expdesc *v);
static int suffixloop (LexState *ls, expdesc *v, int nocall);
static void unaryexp (LexState *ls, expdesc *v);
static void binloop (LexState *ls, expdesc *v);
static void singlepipes (LexState *ls, expdesc *v);
static void lambda (LexState *ls, expdesc *e, int line);
static UnOpr getunopr (int op);
static BinOpr getbinopr (int op);
static int parenlist (LexState *ls, expdesc *v);
static void tuple2value (LexState *ls, expdesc *v, int n);
static void spawnexp (LexState *ls, expdesc *v, int promote);
static void ownspawn (LexState *ls, expdesc *v);
struct Owner;
static void blockowner (LexState *ls);
static void openowner (LexState *ls, struct Owner *o, int reg);
static void closeowner (LexState *ls, struct Owner *o, int line);
static void breakjump (LexState *ls, BlockCnt *bl, int n, int line);
static void createlabel (LexState *ls, TString *name);
static int newgotoentry (LexState *ls, TString *name, int line);
static void pattexp (LexState *ls, expdesc *e);
static void pattbin (LexState *ls, expdesc *e);
static int explist (LexState *ls, expdesc *v);
static void checktoclose (FuncState *fs, int level);
static void suffixedexp (LexState *ls, expdesc *v);
static void rotatecall (FuncState *fs, int r, int n, int k);
static int funcname (LexState *ls, expdesc *v);
static void discard (LexState *ls, expdesc *v);
static void check_readonly (LexState *ls, expdesc *e);


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
static void initres (ResDesc *rd, int reg) {
  int k;
  rd->reg = reg;
  rd->arities = 0;
  rd->endarity = 0;
  for (k = 0; k <= MAXARITY; k++)
    rd->jumps[k] = NO_JUMP;
}


/* the number of values of the construct (at least one: nil) */
static int maxarity (ResDesc *rd) {
  int k;
  for (k = MAXARITY; k > 1; k--)
    if (rd->arities & (1u << k)) return k;
  return 1;
}


/*
** Stores the 'n' values of 'e' (0: none; 1; or the last of 'n' values
** in consecutive registers, as a tuple) as an exit of 'rd': the values
** are evaluated first, enclosing upvalues are closed, then they are
** moved to 'rd->reg..'. Returns the arity.
*/
static int storevalues (LexState *ls, ResDesc *rd, expdesc *e, int n) {
  FuncState *fs = ls->fs;
  if (e->k == VVOID)
    n = 0;
  luaY_checklimit(fs, n, MAXARITY, "values of a block");
  if (n == 0) {  /* no value: nil, stored here (no padding to execute) */
    closeupto(fs, rd->reg);
    luaK_nil(fs, rd->reg, 1);
    n = 1;
  }
  else if (n == 1) {
    expdesc var;
    luaK_exp2anyreg(fs, e);  /* evaluate before closing */
    closeupto(fs, rd->reg);
    init_exp(&var, VLOCAL, 0);  /* fake local variable at 'reg' */
    var.u.var.ridx = cast_byte(rd->reg);
    var.u.var.vidx = 0;
    luaK_storevar(fs, &var, e);
  }
  else {
    int first, i;
    luaK_exp2nextreg(fs, e);
    first = fs->freereg - n;
    closeupto(fs, rd->reg);
    for (i = 0; i < n; i++) {
      if (first + i != rd->reg + i)
        luaK_codeABC(fs, OP_MOVE, rd->reg + i, first + i, 0);
    }
  }
  rd->arities |= (1u << n);
  return n;
}


/* a single value (or none) as an exit of 'rd' */
static void store_value (LexState *ls, ResDesc *rd, expdesc *e) {
  storevalues(ls, rd, e, 1);
}


/* label name of the exits of arity 'n' ('break#n', 'tag#n') */
static TString *aritylabel (LexState *ls, TString *base, int n) {
  lua_State *L = ls->L;
  const char *s = luaO_pushfstring(L, "%s#%d", getstr(base), n);
  TString *ts = luaX_newstring(ls, s, strlen(s));
  L->top.p--;  /* remove the string pushed by 'luaO_pushfstring' */
  return ts;
}


/*
** Padding of the exits of arity 'k' to 'max' values; exits of lower
** arity fall through the paddings of the higher ones (harmless).
*/
static void padexit (FuncState *fs, ResDesc *rd, int k, int max) {
  if (k < max)
    luaK_nil(fs, rd->reg + k, max - k);
}


/*
** End of the exits of a construct: the normal end (arity
** 'rd->endarity', falling through here) is padded and jumps over the
** stubs of the other arities, one per arity in increasing order, each
** a label 'name#k' (gotos of 'break'/'escape') or the pending jumps
** of branch ends ('name' NULL), followed by its padding.
*/
static void finishexits (LexState *ls, ResDesc *rd, TString *name) {
  FuncState *fs = ls->fs;
  int max = maxarity(rd);
  unsigned int others = rd->arities & ~(1u << rd->endarity);
  int jmp = NO_JUMP;
  int k;
  /* exits of the normal arity join the normal end before its padding */
  if (name == NULL)
    luaK_patchtohere(fs, rd->jumps[rd->endarity]);
  else
    createlabel(ls, aritylabel(ls, name, rd->endarity));
  padexit(fs, rd, rd->endarity, max);
  if (others != 0) {  /* exits of other arities? */
    jmp = luaK_jump(fs);  /* the normal end skips their stubs */
    for (k = 0; k <= MAXARITY; k++) {
      if (others & (1u << k)) {
        if (name != NULL)
          createlabel(ls, aritylabel(ls, name, k));
        else
          luaK_patchtohere(fs, rd->jumps[k]);
        padexit(fs, rd, k, max);
      }
    }
    luaK_patchtohere(fs, jmp);
  }
}


/*
** The values of a compound statement as an expression: one value at
** 'rd->reg' (a temporary), or 'max' values through OP_BLOCKRES
** (adjusted by the consumer as a call's results).
*/
static void finishres (LexState *ls, ResDesc *rd, expdesc *v) {
  FuncState *fs = ls->fs;
  int max = maxarity(rd);
  fs->freereg = cast_byte(rd->reg);
  luaK_reserveregs(fs, max);  /* (checks the frame size) */
  if (max == 1)
    init_exp(v, VNONRELOC, rd->reg);
  else {
    fs->freereg = cast_byte(rd->reg + 1);
    init_exp(v, VCALL, luaK_codeABC(fs, OP_BLOCKRES, rd->reg, max + 1, 2));
  }
}


/*
** The value of a compound statement is in register 'reg', the first
** free register when the statement started; describe it as a
** temporary on the top of the stack.
*/



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
** {======================================================================
** Atmos operators '??' '!?' '?>' '!>' '===' '=!=' '=>=' '=<=' are calls
** to functions registered in the global table under the operator's
** name: 'a ?? b' is '_ENV["??"](a, b)' (see lbaselib.c).
** =======================================================================
*/

typedef struct AtmOp {
  const char *name;  /* name of the function in '_ENV' */
  lu_byte swap;  /* call with the operands swapped */
  lu_byte negate;  /* negate the result */
} AtmOp;


static const AtmOp *getatmop (int token) {
  static const AtmOp ops[] = {
    {"??", 0, 0}, {"??", 0, 1}, {"?>", 0, 0}, {"?>", 0, 1},
    {"===", 0, 0}, {"===", 0, 1}, {"=>=", 0, 0}, {"=>=", 1, 0}
  };
  switch (token) {
    case TK_IS: return &ops[0];
    case TK_ISNOT: return &ops[1];
    case TK_XIN: return &ops[2];
    case TK_XNOTIN: return &ops[3];
    case TK_SEQ: return &ops[4];
    case TK_SNE: return &ops[5];
    case TK_SGE: return &ops[6];
    case TK_SLE: return &ops[7];
    default: return NULL;
  }
}


/* free register 'r' if it is the temporary on the top of the stack */
static void freetemp (FuncState *fs, int r) {
  if (r >= luaY_nvarstack(fs) && r == fs->freereg - 1)
    fs->freereg--;
}


static void movereg (FuncState *fs, int dst, int src) {
  if (dst != src)
    luaK_codeABC(fs, OP_MOVE, dst, src, 0);
}


/* store expression 'e' into register 'reg' */
static void storereg (FuncState *fs, int reg, expdesc *e) {
  expdesc var;
  init_exp(&var, VLOCAL, 0);  /* fake local variable at 'reg' */
  var.u.var.ridx = cast_byte(reg);
  var.u.var.vidx = 0;
  luaK_storevar(fs, &var, e);
}


/*
** Code the call '_ENV[op](a, b)' (or '(b, a)') for an Atmos operator
** with both operands already parsed; the result replaces 'a'. The
** temporaries of the operands are reused as the base of the call, so
** that no register is left allocated below the result.
*/
static void opcall (LexState *ls, expdesc *a, expdesc *b, const AtmOp *op,
                    int line) {
  FuncState *fs = ls->fs;
  expdesc fn;
  int ra = luaK_exp2anyreg(fs, a);
  int rb = luaK_exp2anyreg(fs, b);
  int base, pa, pb;
  freetemp(fs, rb);
  freetemp(fs, ra);
  base = fs->freereg;
  luaK_reserveregs(fs, 3);  /* function and two arguments */
  pa = op->swap ? base + 2 : base + 1;
  pb = op->swap ? base + 1 : base + 2;
  /* copy the operand higher in the stack first: its register may be
     the destination of the other copy */
  if (rb > ra) { movereg(fs, pb, rb); movereg(fs, pa, ra); }
  else { movereg(fs, pa, ra); movereg(fs, pb, rb); }
  buildvar(ls, luaX_newstring(ls, op->name, strlen(op->name)), &fn);
  storereg(fs, base, &fn);
  init_exp(a, VCALL, luaK_codeABC(fs, OP_CALL, base, 3, 2));
  luaK_fixline(fs, line);
  fs->freereg = cast_byte(base + 1);  /* one result */
  if (op->negate)
    luaK_prefix(fs, OPR_NOT, a, line);
}

/* }====================================================================== */


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
  bl->nregions = 0;
  bl->res = NULL;
  bl->owner = NULL;
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
  if (bl->owner != NULL) {  /* tasks spawned in the block? */
    bl->nregions--;  /* the handler runs with the region popped */
    closeowner(ls, bl->owner, ls->linenumber);
    bl->owner = NULL;
  }
  if (bl->previous && bl->upval)  /* need a 'close'? */
    luaK_codeABC(fs, OP_CLOSE, stklevel, 0, 0);
  fs->freereg = stklevel;  /* free registers */
  removevars(fs, bl->nactvar);  /* remove block locals */
  lua_assert(bl->nactvar == fs->nactvar);  /* back to level on entry */
  if (bl->isloop == 2)  /* has to fix pending breaks? */
    finishexits(ls, bl->res, ls->brkn);
  if (bl->tag != NULL && bl->hasescape)  /* has pending escapes? */
    finishexits(ls, bl->res, bl->tag);
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
  fs->iscatch = 0;
  fs->owner = NULL;
  fs->spawnval = 0;
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
    if (fs->spawnval) {  /* a 'spawn' statement? */
      int own = (fs->spawnval == 1);
      fs->spawnval = 0;
      if (own && !block_follow(ls)) {  /* not the block value: own it */
        ownspawn(ls, v);
        init_exp(v, VVOID, 0);
        *n = 0;
      }
    }
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
static int block (LexState *ls, ResDesc *rd) {
  FuncState *fs = ls->fs;
  BlockCnt bl;
  expdesc v;
  int n;
  int line = ls->linenumber;
  enterblock(fs, &bl, 0);
  checknext(ls, '{');
  statlist(ls, &v, &n);
  if (rd != NULL)
    n = storevalues(ls, rd, &v, n);
  else {
    discard(ls, &v);
    n = 0;
  }
  ls->blockline = ls->lastline;  /* line of last statement */
  check_match(ls, '}', '{', line);
  leaveblock(fs);
  return n;
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
  fs->freereg = cast_byte(t->u.info + 1);  /* free key register (if any) */
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
  if (n == 1 &&
      !(e->k == VCALL && GET_OPCODE(getinstruction(fs, e)) == OP_BLOCKRES))
    luaK_setoneret(fs, e);  /* single expression evaluates to one value
                               (a block with several values keeps them) */
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
** Operator as a function: '\+' is '\(a,b){ a + b }'; '\#' and '\!'
** take one parameter ('\-' is binary).
*/
static void oplambda (LexState *ls, int line) {
  FuncState *fs = ls->fs;
  int tok = ls->t.token;
  BinOpr bop = getbinopr(tok);
  const AtmOp *aop = getatmop(tok);
  UnOpr uop = (tok == '#' || tok == TK_NOT) ? getunopr(tok) : OPR_NOUNOPR;
  int nparams = (uop != OPR_NOUNOPR) ? 1 : 2;
  expdesc a, b;
  if (bop == OPR_NOBINOPR && aop == NULL && uop == OPR_NOUNOPR)
    luaX_syntaxerror(ls, "lambda error: invalid operator");
  luaX_next(ls);  /* skip operator */
  new_localvarliteral(ls, "a");
  if (nparams == 2)
    new_localvarliteral(ls, "b");
  adjustlocalvars(ls, nparams);
  fs->f->numparams = cast_byte(nparams);
  luaK_reserveregs(fs, nparams);
  init_exp(&a, VLOCAL, 0);
  a.u.var.ridx = 0; a.u.var.vidx = 0;
  init_exp(&b, VLOCAL, 0);
  b.u.var.ridx = 1; b.u.var.vidx = 1;
  if (uop != OPR_NOUNOPR)
    luaK_prefix(fs, uop, &a, line);
  else if (bop != OPR_NOBINOPR) {
    luaK_infix(fs, bop, &a);
    luaK_posfix(fs, bop, &a, &b, line);
  }
  else
    opcall(ls, &a, &b, aop, line);
  tailreturn(ls, &a, 1);
  fs->f->lastlinedefined = ls->linenumber;
}


/*
** Lambda: '\' [NAME | '(' parlist ')'] funcblock | '\' operator
*/
static void lambda (LexState *ls, expdesc *e, int line) {
  FuncState new_fs;
  BlockCnt bl;
  enterlevel(ls);  /* control recursion depth of nested functions */
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  open_func(ls, &new_fs, &bl);
  if (ls->t.token != '(' && ls->t.token != '{' && ls->t.token != TK_NAME)
    oplambda(ls, line);  /* operator as a function */
  else {
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
  }
  codeclosure(ls, e);
  close_func(ls);
  leavelevel(ls);
}


/*
** Body of a 'catch' or 'defer': a closure with no parameters.
** ('catch' bodies cannot 'return': that would return from the closure
** instead of from the enclosing function.)
*/
/* kinds of blocks compiled as closures ('FuncState.iscatch'): the
   bodies of tasks ('catch' and 'watching' bodies are inline, see
   .claude/plans/catch.md) */
#define CB_PAR		2
#define CB_TOGGLE	4
#define CB_SPAWN	5

static void closurebody (LexState *ls, expdesc *e, int line, int iscatch) {
  FuncState new_fs;
  BlockCnt bl;
  enterlevel(ls);  /* control recursion depth of nested functions */
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  open_func(ls, &new_fs, &bl);
  new_fs.iscatch = cast_byte(iscatch);
  funcblock(ls, line);
  codeclosure(ls, e);
  close_func(ls);
  leavelevel(ls);
}


/*
** catch -> 'catch' expr block
** An inline block under a try region (see .claude/plans/catch.md):
**
**     CND -> R[A]; OP_TRY A -> handler
**     body (value at R[A+1], locals above)
**     OP_ENDTRY A -> join           (R[A] := true)
**   handler: OP_CATCH A 0          (match, or rethrow)
**   join: OP_CATCHRES A C          (a VCALL-like expression at A)
**
** Hidden locals '(cnd)' and '(val)' keep the two slots below the body's
** locals (called through 'compoundexp', so live temporaries are kept
** too). Jumps out of the body pop the region ('popregions').
*/
static int catchstat (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int base = luaY_nvarstack(fs);
  int nactvar = fs->nactvar;
  int pctry, pcend, pcres, nvals;
  expdesc cnd;
  BlockCnt bl;
  luaX_next(ls);  /* skip 'catch' */
  lua_assert(base == fs->freereg);
  expr(ls, &cnd);  /* condition, evaluated before the body */
  luaK_exp2nextreg(fs, &cnd);  /* at 'base' */
  new_localvarliteral(ls, "(cnd)");
  new_localvarliteral(ls, "(val)");
  luaK_reserveregs(fs, 1);  /* the value slot */
  adjustlocalvars(ls, 2);
  pctry = luaK_codeAsBx(fs, OP_TRY, base, 0);
  luaK_fixline(fs, line);
  enterblock(fs, &bl, 0);
  bl.nregions = 1;
  checknext(ls, '{');
  {  /* the body values go to R[base+1]..: none, one, or a tuple's */
    expdesc bv;
    int n;
    statlist(ls, &bv, &n);
    if (bv.k == VVOID)
      nvals = 0;
    else if (n == 1) {
      ResDesc rd;
      initres(&rd, base + 1);
      store_value(ls, &rd, &bv);  /* (a call gives one value) */
      nvals = 1;
    }
    else {  /* tuple: values above the body locals; move them down */
      int first = luaY_nvarstack(fs);
      int i;
      luaK_exp2nextreg(fs, &bv);
      lua_assert(n == fs->freereg - first);
      for (i = 0; i < n; i++)
        luaK_codeABC(fs, OP_MOVE, base + 1 + i, first + i, 0);
      nvals = n;
    }
    ls->blockline = ls->lastline;
  }
  check_match(ls, '}', '{', line);
  leaveblock(fs);
  luaK_codeABCk(fs, OP_ENDTRY, base, nvals + 1, 0, 1);
  pcend = luaK_jump(fs);  /* to the join */
  SETARG_sBx(fs->f->code[pctry], luaK_getlabel(fs) - (pctry + 1));
  luaK_codeABC(fs, OP_CATCH, base, 0, 0);
  luaK_fixline(fs, line);
  luaK_patchtohere(fs, pcend);
  pcres = luaK_codeABC(fs, OP_CATCHRES, base, 0, 2);
  removevars(fs, nactvar);  /* the hidden locals */
  fs->freereg = cast_byte(base + 1);
  init_exp(v, VCALL, pcres);
  return 1;
}


/*
** {======================================================================
** Tasks: 'task', 'tasks', 'spawn', 'emit', 'await' and 'pub' compile to
** calls of the runtime functions of latmlib.c (see
** .claude/plans/tasks.md), as the reference implementation does.
** =======================================================================
*/

/* start a call to the global 'name': loads the function; its register */
static int callbegin (LexState *ls, const char *name) {
  FuncState *fs = ls->fs;
  expdesc f;
  buildvar(ls, luaX_newstring(ls, name, strlen(name)), &f);
  luaK_exp2nextreg(fs, &f);
  return f.u.info;
}


/* push the argument 'e' (all its values if open); returns 'open' */
static int callarg (LexState *ls, expdesc *e) {
  if (hasmultret(e->k)) {
    luaK_setmultret(ls->fs, e);
    return 1;
  }
  luaK_exp2nextreg(ls->fs, e);
  return 0;
}


/* finish the call at 'base' with the arguments above it */
static void callend (LexState *ls, expdesc *v, int base, int open, int line) {
  FuncState *fs = ls->fs;
  int nparams = open ? LUA_MULTRET : fs->freereg - (base + 1);
  init_exp(v, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams + 1, 2));
  luaK_fixline(fs, line);
  fs->freereg = cast_byte(base + 1);
}


/*
** '(' expr ')' as a predicate function: a function expression as is,
** otherwise '\it{ expr }'.
*/
static void predlambda (LexState *ls, expdesc *e) {
  int line = ls->linenumber;
  checknext(ls, '(');
  if (ls->t.token == TK_FUNC || ls->t.token == '\\')
    expr(ls, e);
  else {
    FuncState new_fs;
    BlockCnt bl;
    expdesc a;
    enterlevel(ls);
    new_fs.f = addprototype(ls);
    new_fs.f->linedefined = line;
    open_func(ls, &new_fs, &bl);
    new_localvarliteral(ls, "it");
    adjustlocalvars(ls, 1);
    new_fs.f->numparams = 1;
    luaK_reserveregs(ls->fs, 1);
    expr(ls, &a);
    tailreturn(ls, &a, 1);
    new_fs.f->lastlinedefined = ls->linenumber;
    codeclosure(ls, e);
    close_func(ls);
    leavelevel(ls);
  }
  checknext(ls, ')');
}


/* '@' target of spawn/emit: '(' expr ')' or a primary expression */
static void attarget (LexState *ls, expdesc *tg) {
  luaX_next(ls);  /* skip '@' */
  if (ls->t.token == '(') {
    int n = parenlist(ls, tg);
    if (n > 1) tuple2value(ls, tg, n);
  }
  else
    primaryexp(ls, tg);
  luaK_exp2nextreg(ls->fs, tg);
}


/*
** spawn -> SPAWN '{' block '}'                      (transparent task)
**        | SPAWN ['@' target] prefixexp '(' [explist] ')'
** 'promote': 'pin t = spawn { }' spawns the block as a prototype.
*/
static void spawnexp (LexState *ls, expdesc *v, int promote) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int base, open = 0;
  luaX_next(ls);  /* skip 'spawn' */
  if (ls->t.token == '{') {  /* spawn { ... } */
    expdesc f;
    if (promote) {
      int base2;
      base = callbegin(ls, "spawn");
      base2 = callbegin(ls, "task");
      closurebody(ls, &f, line, CB_SPAWN);
      luaK_exp2nextreg(fs, &f);
      callend(ls, &f, base2, 0, line);
      luaK_exp2nextreg(fs, &f);
    }
    else {
      base = callbegin(ls, "do_spawn");
      closurebody(ls, &f, line, CB_SPAWN);
      luaK_exp2nextreg(fs, &f);
    }
    callend(ls, v, base, 0, line);
    return;
  }
  if (ls->t.token == '@') {  /* spawn @target T(...) */
    expdesc tg;
    base = callbegin(ls, "spawn_in");
    attarget(ls, &tg);
  }
  else
    base = callbegin(ls, "spawn");
  {  /* the prototype and its arguments */
    expdesc f;
    primaryexp(ls, &f);
    suffixloop(ls, &f, 1);
    luaK_exp2nextreg(fs, &f);
    if (ls->t.token != '(')
      luaX_syntaxerror(ls, "expected call syntax");
    luaX_next(ls);
    if (ls->t.token != ')') {
      expdesc args;
      explist(ls, &args);
      open = callarg(ls, &args);
    }
    check_match(ls, ')', '(', line);
  }
  callend(ls, v, base, open, line);
}


/*
** Ownership check of the value of 'e' (already parsed), in place:
** OP_PINCHK calls the '__pinchk' metamethod of tasks and pools with
** 'mode' (PIN_CHK: must be owned or an ancestor; PIN_SET: 'pin'
** declaration, becomes owned; PIN_OWN: hidden pin of a statement
** 'spawn'). Other values are untouched.
*/
#define PIN_CHK		0
#define PIN_SET		1
#define PIN_OWN		2

static void pinop (LexState *ls, expdesc *e, int mode) {
  FuncState *fs = ls->fs;
  int r = luaK_exp2anyreg(fs, e);
  luaK_codeABC(fs, OP_PINCHK, r, mode, 0);
}


/*
** Check of the value of a declaration or assignment (the reference's
** 'atm_pin_chk_set'): a task or pool can only be assigned when it is
** owned by a pin or is an ancestor of the running task. Skipped for
** values that cannot be tasks (constants, closures, constructors,
** arithmetic).
*/
static void chkassign (LexState *ls, expdesc *e) {
  switch (e->k) {
    case VNIL: case VTRUE: case VFALSE: case VK: case VKFLT: case VKINT:
    case VKSTR: case VJMP: case VVOID:
      return;
    case VRELOC: {
      OpCode op = GET_OPCODE(getinstruction(ls->fs, e));
      if (op == OP_CLOSURE || op == OP_NEWTABLE || op == OP_CONCAT ||
          op == OP_NOT || op == OP_LEN || op == OP_UNM || op == OP_BNOT ||
          (op >= OP_ADDI && op <= OP_SHR))
        return;
      break;
    }
    default: break;
  }
  pinop(ls, e, PIN_CHK);
}


/*
** Statement 'spawn ...': its value is the task. Unless it is the last
** statement of its block (then the task flows out as the block value)
** or it is spawned into a pool, the enclosing block owns the task: see
** 'ownspawn', called by 'statlist'.
*/
static int spawnstat (LexState *ls, expdesc *v) {
  int la = luaX_lookahead(ls);
  int inpool = (la == '@');
  if (la == '{')  /* a block: the enclosing block owns it (jumps out) */
    blockowner(ls);
  spawnexp(ls, v, 0);
  ls->fs->spawnval = (lu_byte)(inpool ? 2 : 1);
  return 1;
}


/* the block owns the task in 'v': a hidden to-be-closed local */
static void ownspawn (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int toclose;
  pinop(ls, v, PIN_OWN);
  new_varkind(ls, luaX_newstring(ls, "(spawn)", 7), RDKTOCLOSE);
  toclose = fs->nactvar;
  adjust_assign(ls, 1, 1, v);
  adjustlocalvars(ls, 1);
  checktoclose(fs, toclose);
}


/* emit -> EMIT ['@' target] ('(' expr ')' | expr) */
static void emitexp (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int base;
  expdesc e;
  luaX_next(ls);  /* skip 'emit' */
  if (ls->t.token == '@') {
    expdesc tg;
    base = callbegin(ls, "emit_in");
    attarget(ls, &tg);
  }
  else
    base = callbegin(ls, "emit");
  if (!testnext(ls, TK_LPIPE1)) testnext(ls, TK_LPIPE2);  /* emit <- e */
  if (ls->t.token == '(') {
    int n = parenlist(ls, &e);
    if (n != 1)
      luaX_syntaxerror(ls, "expected single argument");
  }
  else
    expr(ls, &e);
  luaK_exp2nextreg(fs, &e);
  callend(ls, v, base, 0, line);
}


/*
** await -> AWAIT '(' expr ')'                            (value)
**        | AWAIT (':any' | ':all') suffixedexp           (pool)
**        | AWAIT ('until' | 'while') '(' predicate ')'   (condition)
**        | AWAIT prefixexp '(' [explist] ')'             (spawn and await)
**        | AWAIT primary                                 (value)
*/
/*
** Await patterns (after 'await', 'watching', 'loop .. on'; see
** .claude/plans/tasks.md, stage 2a). Three tiers: bare ('await P':
** one leaf, optionally 'until (c)' / 'while (c)'), value ('await(E)')
** and pattern ('await <PAT>': '!p', 'p && p', 'p || p', 'p until (c)',
** 'p while (c)', nested '<P>'). Leaves: tags and other primaries,
** '(E)' values, ':any ts' / ':all ts', bare 'until (c)' / 'while (c)',
** and a call 'T(args)', which spawns the task and awaits it. The
** combinators are calls of the constructors '_ENV["await.or"]' etc.
** of latmlib.c, which 'await' interprets.
*/

/* 'name(e, ...)': 'e' is already evaluated; starts the call, 'e' as
   the first argument; returns the base */
static int callwith (LexState *ls, expdesc *e, const char *name) {
  FuncState *fs = ls->fs;
  int base;
  luaK_exp2nextreg(fs, e);
  base = callbegin(ls, name);  /* above it */
  rotatecall(fs, base - 1, 1, 1);  /* function first */
  return base - 1;
}


/* leaf of a pattern */
static void pattprim (LexState *ls, expdesc *e) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  switch (ls->t.token) {
    case '(': {  /* value */
      int n = parenlist(ls, e);
      if (n > 1) tuple2value(ls, e, n);
      return;
    }
    case '<': {  /* nested pattern */
      luaX_next(ls);
      pattbin(ls, e);
      checknext(ls, '>');
      return;
    }
    case TK_UNTIL: case TK_WHILE: {  /* bare predicate */
      const char *k = (ls->t.token == TK_UNTIL) ? "await.until" : "await.while";
      int base;
      luaX_next(ls);
      base = callbegin(ls, k);
      predlambda(ls, e);
      luaK_exp2nextreg(fs, e);
      callend(ls, e, base, 0, line);
      return;
    }
    case TK_TAG: {
      const char *tag = getstr(ls->t.seminfo.ts);
      if (strcmp(tag, "any") == 0 || strcmp(tag, "all") == 0) {  /* pool */
        const char *k = (tag[0] == 'a' && tag[1] == 'n') ? "await.any"
                                                          : "await.all";
        int base;
        luaX_next(ls);
        base = callbegin(ls, k);
        suffixedexp(ls, e);
        luaK_exp2nextreg(fs, e);
        callend(ls, e, base, 0, line);
        return;
      }
    }  /* FALLTHROUGH */
    default: {  /* primary; a call spawns the task and awaits it */
      int isname = (ls->t.token == TK_NAME);
      primaryexp(ls, e);
      if (suffixloop(ls, e, 1) && ls->t.token == '(') {
        int base, open = 0;
        base = callwith(ls, e, "await.spawn");
        luaX_next(ls);  /* skip '(' */
        if (ls->t.token != ')') {
          expdesc args;
          explist(ls, &args);
          open = callarg(ls, &args);
        }
        check_match(ls, ')', '(', line);
        callend(ls, e, base, open, line);
      }
      else if (isname)  /* a variable: must be a '(value)' (as the reference) */
        luaX_syntaxerror(ls, "invalid await : unexpected expression");
    }
  }
}


/* '!p' or a leaf */
static void pattunary (LexState *ls, expdesc *e) {
  if (ls->t.token == TK_NOT) {
    int line = ls->linenumber;
    int base;
    luaX_next(ls);
    base = callbegin(ls, "await.not");
    pattprim(ls, e);
    luaK_exp2nextreg(ls->fs, e);
    callend(ls, e, base, 0, line);
  }
  else
    pattprim(ls, e);
}


static int pattop (int token) {
  return (token == TK_OR || token == TK_AND ||
          token == TK_UNTIL || token == TK_WHILE);
}


/* 'p until (c)' / 'p while (c)': 'p' already parsed */
static void pattpred (LexState *ls, expdesc *e) {
  const char *k = (ls->t.token == TK_UNTIL) ? "await.until" : "await.while";
  int line = ls->linenumber;
  int base;
  expdesc pred;
  luaX_next(ls);
  base = callwith(ls, e, k);
  predlambda(ls, &pred);
  luaK_exp2nextreg(ls->fs, &pred);
  callend(ls, e, base, 0, line);
}


/* combinators: one operator per level ('a || b || c'; mixing needs
   parentheses, as for binary operators) */
static void pattbin (LexState *ls, expdesc *e) {
  int line = ls->linenumber;
  int op;
  pattunary(ls, e);
  op = ls->t.token;
  if (op == TK_UNTIL || op == TK_WHILE)
    pattpred(ls, e);
  else if (op == TK_OR || op == TK_AND) {
    int base = callwith(ls, e, (op == TK_OR) ? "await.or" : "await.and");
    do {
      expdesc sub;
      luaX_next(ls);  /* skip the operator */
      pattunary(ls, &sub);
      luaK_exp2nextreg(ls->fs, &sub);
    } while (ls->t.token == op);
    callend(ls, e, base, 0, line);
  }
  if (pattop(ls->t.token))
    luaX_syntaxerror(ls,
      "operation error : use parentheses to disambiguate");
}


/* a pattern in the three tiers */
static void pattexp (LexState *ls, expdesc *e) {
  if (ls->t.token == '<') {  /* '<PAT>' */
    luaX_next(ls);
    pattbin(ls, e);
    checknext(ls, '>');
  }
  else {  /* bare or value: a leaf, optionally with a predicate */
    pattprim(ls, e);
    if (ls->t.token == TK_UNTIL || ls->t.token == TK_WHILE)
      pattpred(ls, e);
  }
}


/* await P -> await(P) */
static void awaitexp (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int base;
  expdesc e;
  luaX_next(ls);  /* skip 'await' */
  base = callbegin(ls, "await");
  if (!testnext(ls, TK_LPIPE1)) testnext(ls, TK_LPIPE2);  /* await <- p */
  pattexp(ls, &e);
  luaK_exp2nextreg(fs, &e);
  callend(ls, v, base, 0, line);
}


/*
** par [:any | :all] { } with { } ... -> par(\{ }, \{ }, ...)
** (par_any / par_all). Each block is a closure spawned as a
** transparent task.
*/
static void parexp (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  const char *name = "par";
  int base;
  luaX_next(ls);  /* skip 'par' */
  if (ls->t.token == TK_TAG) {
    const char *tag = getstr(ls->t.seminfo.ts);
    if (strcmp(tag, "any") == 0) name = "par_any";
    else if (strcmp(tag, "all") == 0) name = "par_all";
    else luaX_syntaxerror(ls, "invalid par : invalid tag");
    luaX_next(ls);
  }
  {  /* a region around the call, for the jumps out of the branches
        (its register is the call base: the values of a jump replace
        the call's) */
    Owner o;
    openowner(ls, &o, fs->freereg);
    base = callbegin(ls, name);
    do {
      expdesc f;
      check(ls, '{');
      closurebody(ls, &f, line, CB_PAR);
      luaK_exp2nextreg(fs, &f);
    } while (testnext(ls, TK_WITH));
    callend(ls, v, base, 0, line);
    closeowner(ls, &o, line);
  }
}


/*
** toggle on :T [with P {, P}] { } -> toggle(:T, P..., \{ })
** toggle f(args) [with P {, P}] -> toggle(f, args..., P...)
*/
static void togglepatts (LexState *ls) {
  do {
    expdesc p;
    pattexp(ls, &p);
    luaK_exp2nextreg(ls->fs, &p);
  } while (testnext(ls, ','));
}

static void toggleexp (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int base;
  luaX_next(ls);  /* skip 'toggle' */
  if (ls->t.token == TK_ON) {  /* block form, in a region for its jumps */
    expdesc tag, f;
    Owner o;
    openowner(ls, &o, fs->freereg);  /* region at the call base */
    base = callbegin(ls, "toggle");
    luaX_next(ls);  /* skip 'on' */
    check(ls, TK_TAG);
    codestring(&tag, ls->t.seminfo.ts);
    luaX_next(ls);
    luaK_exp2nextreg(fs, &tag);
    if (testnext(ls, TK_WITH))
      togglepatts(ls);
    check(ls, '{');
    closurebody(ls, &f, line, CB_TOGGLE);
    luaK_exp2nextreg(fs, &f);
    callend(ls, v, base, 0, line);
    closeowner(ls, &o, line);
    return;
  }
  base = callbegin(ls, "toggle");
  {  /* call form */
    expdesc t;
    primaryexp(ls, &t);
    if (!(suffixloop(ls, &t, 1) && ls->t.token == '('))
      luaX_syntaxerror(ls, "expected call syntax");
    luaK_exp2nextreg(fs, &t);
    luaX_next(ls);  /* skip '(' */
    if (ls->t.token != ')') {
      expdesc args;
      explist(ls, &args);
      luaK_exp2nextreg(fs, &args);
    }
    check_match(ls, ')', '(', line);
    if (testnext(ls, TK_WITH))
      togglepatts(ls);
  }
  callend(ls, v, base, 0, line);
}


/*
** watching P { body }: an inline body under a try region; a hidden
** watcher task awaits P and throws itself (with the event) at the
** body's pending await; the region catches it (see catch.md):
**
**     OP_TRY A -> handler
**     R[A] = _ENV["watching.new"](P); (watcher) <close> = R[A]
**     _ENV["watching.start"](R[A])   (inside the region: a pattern that
**                                     holds already throws at once)
**     body (value at R[A+1])
**     OP_ENDTRY A; JMP join
**   handler: OP_CATCH A 0         (e1 == watcher)
**   join: OP_CATCHRES A 4; value = R[A] ? R[A+1] : R[A+2]
*/
static int watchingstat (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int base = luaY_nvarstack(fs);
  int nactvar = fs->nactvar;
  int pctry, pcend, toclose, jf, jend;
  expdesc e, bv;
  BlockCnt bl;
  int n;
  luaX_next(ls);  /* skip 'watching' */
  new_localvarliteral(ls, "(cnd)");
  new_localvarliteral(ls, "(val)");
  luaK_reserveregs(fs, 2);
  adjustlocalvars(ls, 2);
  pctry = luaK_codeAsBx(fs, OP_TRY, base, 0);
  luaK_fixline(fs, line);
  enterblock(fs, &bl, 0);
  bl.nregions = 1;
  {  /* the watcher, pinned by the block, and the condition */
    expdesc w;
    int cb = callbegin(ls, "watching.new");
    pattexp(ls, &e);
    luaK_exp2nextreg(fs, &e);
    callend(ls, &e, cb, 0, line);
    luaK_exp2nextreg(fs, &e);  /* at base+2 */
    new_varkind(ls, luaX_newstring(ls, "(watcher)", 9), RDKTOCLOSE);
    toclose = fs->nactvar;
    adjustlocalvars(ls, 1);
    checktoclose(fs, toclose);
    luaK_codeABC(fs, OP_MOVE, base, base + 2, 0);  /* CND = the watcher */
    cb = callbegin(ls, "watching.start");  /* only now (CND is known) */
    luaK_codeABC(fs, OP_MOVE, cb + 1, base + 2, 0);
    luaK_reserveregs(fs, 1);
    callend(ls, &w, cb, 0, line);
    SETARG_C(getinstruction(fs, &w), 1);  /* no results */
    fs->freereg = cast_byte(cb);
  }
  checknext(ls, '{');
  statlist(ls, &bv, &n);
  {  /* the body value at base+1 (one value) */
    ResDesc rd;
    initres(&rd, base + 1);
    store_value(ls, &rd, &bv);
  }
  ls->blockline = ls->lastline;
  check_match(ls, '}', '{', line);
  leaveblock(fs);  /* closes the watcher */
  luaK_codeABCk(fs, OP_ENDTRY, base, 2, 0, 1);
  pcend = luaK_jump(fs);
  SETARG_sBx(fs->f->code[pctry], luaK_getlabel(fs) - (pctry + 1));
  luaK_codeABC(fs, OP_CATCH, base, 0, 0);
  luaK_fixline(fs, line);
  luaK_patchtohere(fs, pcend);
  luaK_codeABC(fs, OP_CATCHRES, base, 0, 4);  /* flag, v1, v2 */
  /* value: flag ? v1 (body) : v2 (event) */
  luaK_codeABCk(fs, OP_TEST, base, 0, 0, 0);
  jf = luaK_jump(fs);  /* taken when the flag is false */
  luaK_codeABC(fs, OP_MOVE, base, base + 1, 0);
  jend = luaK_jump(fs);
  luaK_patchtohere(fs, jf);
  luaK_codeABC(fs, OP_MOVE, base, base + 2, 0);
  luaK_patchtohere(fs, jend);
  removevars(fs, nactvar);
  fs->freereg = cast_byte(base + 1);
  init_exp(v, VNONRELOC, base);
  return 1;
}


/* pub -> xtask().pub */
static void pubexp (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  expdesc key;
  int base;
  luaX_next(ls);  /* skip 'pub' */
  base = callbegin(ls, "xtask");
  callend(ls, v, base, 0, line);
  luaK_exp2anyreg(fs, v);
  codestring(&key, luaX_newstring(ls, "pub", 3));
  luaK_indexed(fs, v, &key);
}


/* task -> TASK body (prototype) | TASK (the running instance) */
static void taskexp (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int base;
  luaX_next(ls);  /* skip 'task' */
  if (ls->t.token == '(' && sameline(ls)) {  /* prototype */
    expdesc f;
    base = callbegin(ls, "task");
    body(ls, &f, 0, line);
    luaK_exp2nextreg(fs, &f);
    callend(ls, v, base, 0, line);
  }
  else {  /* the running task */
    base = callbegin(ls, "xtask");
    callend(ls, v, base, 0, line);
  }
}


/* statement 'task NAME body': like 'func NAME' with a prototype */
static void taskstat (LexState *ls, int line) {
  FuncState *fs = ls->fs;
  int ismethod, base;
  expdesc v, b;
  luaX_next(ls);  /* skip 'task' */
  ismethod = funcname(ls, &v);
  check_readonly(ls, &v);
  base = callbegin(ls, "task");
  body(ls, &b, ismethod, line);
  luaK_exp2nextreg(fs, &b);
  callend(ls, &b, base, 0, line);
  luaK_storevar(fs, &v, &b);
  luaK_fixline(fs, line);  /* definition "happens" in the first line */
}


/* 'val task NAME body' */
static void localtask (LexState *ls, int line) {
  expdesc b;
  FuncState *fs = ls->fs;
  int fvar = fs->nactvar;  /* variable index */
  int base;
  new_varkind(ls, str_checkname(ls), RDKCONST);
  adjustlocalvars(ls, 1);  /* enter its scope */
  base = callbegin(ls, "task");  /* at the variable's register */
  body(ls, &b, 0, line);
  luaK_exp2nextreg(fs, &b);
  callend(ls, &b, base, 0, line);
  localdebuginfo(fs, fvar)->startpc = fs->pc;
}

/* }====================================================================== */


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
static int matchstat (LexState *ls, expdesc *v);
static int teststat (LexState *ls, expdesc *v);
static void callhead (LexState *ls, expdesc *f, int head);


/*
** Compound statement used as an expression. Its value register and
** the locals it declares are allocated at the level of the active
** locals ('luaY_nvarstack'), so temporaries that are live at this
** point (a function and arguments being collected, operands in
** registers) would be overwritten. Cover those temporaries with hidden
** locals while the statement is parsed, so that everything inside is
** allocated above them; the hidden locals disappear afterwards.
*/
static void compoundexp (LexState *ls, expdesc *v,
                         int (*stat) (LexState *ls, expdesc *v)) {
  FuncState *fs = ls->fs;
  int ntemps = fs->freereg - luaY_nvarstack(fs);
  if (ntemps == 0)  /* no live temporaries? */
    stat(ls, v);
  else {
    int nactvar = fs->nactvar;
    int i;
    for (i = 0; i < ntemps; i++)
      new_localvarliteral(ls, "(temp)");
    adjustlocalvars(ls, ntemps);  /* hidden locals in the temporaries */
    stat(ls, v);  /* value in 'v', above the temporaries */
    removevars(fs, nactvar);  /* remove hidden locals */
  }
}


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
    case TK_CATCH: compoundexp(ls, v, catchstat); return;
    case TK_TASK: taskexp(ls, v); return;
    case TK_TASKS: {
      luaX_next(ls);
      buildvar(ls, luaX_newstring(ls, "tasks", 5), v);
      return;
    }
    case TK_SPAWN: spawnexp(ls, v, 0); return;
    case TK_EMIT: emitexp(ls, v); return;
    case TK_AWAIT: awaitexp(ls, v); return;
    case TK_PAR: parexp(ls, v); return;
    case TK_WATCHING: compoundexp(ls, v, watchingstat); return;
    case TK_TOGGLE: toggleexp(ls, v); return;
    case TK_PUB: pubexp(ls, v); return;
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
    case TK_IF: compoundexp(ls, v, ifstat); return;
    case TK_IFS: compoundexp(ls, v, ifsstat); return;
    case TK_DO: compoundexp(ls, v, dostat); return;
    case TK_LOOP: compoundexp(ls, v, loopstat); return;
    case TK_MATCH: compoundexp(ls, v, matchstat); return;
    case TK_TEST: compoundexp(ls, v, teststat); return;
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
static int suffixloop (LexState *ls, expdesc *v, int nocall) {
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
        if (nocall) return 1;  /* stop at the call */
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
          return 0;  /* a new statement */
        if (nocall) return 1;  /* stop at the call */
        luaK_exp2nextreg(fs, v);
        funcargs(ls, v);
        break;
      }
      default: return 0;
    }
  }
}


static void suffixedexp (LexState *ls, expdesc *v) {
  primaryexp(ls, v);
  suffixloop(ls, v, 0);
}


/*
** {======================================================================
** Pipes: 'v -> f(args)' is 'f(v, args)'; 'f(args) <- v' is 'f(args, v)'.
** Single pipes ('->' '<-') bind tighter than unary and binary
** operators; double pipes ('-->' '<--') bind looser.
** =======================================================================
*/

/*
** Registers r..r+n-1 hold 'n' piped values and r+n..r+n+k-1 the
** function (and 'self') of the call: rotate them so that the function
** comes first ('k' registers above are used as temporaries).
*/
static void rotatecall (FuncState *fs, int r, int n, int k) {
  int i;
  luaK_reserveregs(fs, k);
  for (i = 0; i < k; i++)
    movereg(fs, r + n + k + i, r + n + i);
  for (i = n - 1; i >= 0; i--)
    movereg(fs, r + k + i, r + i);
  for (i = 0; i < k; i++)
    movereg(fs, r + i, r + n + k + i);
  fs->freereg = cast_byte(r + n + k);
}


/*
** 'v -> target': the 'n' piped values are on the top of the stack;
** parse the target and call it with the values before its arguments.
*/
static void pipeprepend (LexState *ls, expdesc *v, int n) {
  FuncState *fs = ls->fs;
  int r = fs->freereg - n;  /* first piped value */
  int line = ls->linenumber;
  expdesc f;
  int k;  /* 1 for a function, 2 for a method (with 'self') */
  switch (ls->t.token) {
    case TK_INT: case TK_FLT: case TK_STRING: case TK_NIL:
    case TK_TRUE: case TK_FALSE:
      luaX_syntaxerror(ls, "pipe target must be callable");
    default: break;
  }
  primaryexp(ls, &f);
  if (suffixloop(ls, &f, 1) && ls->t.token == TK_DBCOLON) {  /* method? */
    expdesc key;
    luaX_next(ls);
    codestring(&key, str_checkfield(ls));
    luaK_self(fs, &f, &key);  /* method and 'self' on the top */
    k = 2;
  }
  else {
    luaK_exp2nextreg(fs, &f);  /* function on the top */
    k = 1;
  }
  rotatecall(fs, r, n, k);
  init_exp(&f, VNONRELOC, r);  /* function at 'r', values after it */
  if (ls->t.token == '(' || (sameline(ls) && check_call_arg(ls)))
    funcargs(ls, &f);  /* more arguments */
  else {  /* call with the piped values only */
    init_exp(&f, VCALL, luaK_codeABC(fs, OP_CALL, r, n + k, 2));
    luaK_fixline(fs, line);
    fs->freereg = cast_byte(r + 1);
  }
  *v = f;
}


/*
** Operand of a double pipe: an expression without double pipes, or a
** parenthesized tuple; returns the number of values.
*/
static int pipeoperand (LexState *ls, expdesc *v) {
  if (ls->t.token == '(') {
    int n = parenlist(ls, v);
    if (n > 1)
      return n;
    suffixloop(ls, v, 0);
    singlepipes(ls, v);
    binloop(ls, v);
    return 1;
  }
  unaryexp(ls, v);
  binloop(ls, v);
  return 1;
}


/*
** 'target <- v': 'target' is already parsed (possibly a call just
** coded, which is reopened); parse the piped value(s) and call with
** them after the arguments.
*/
static void pipeappend (LexState *ls, expdesc *f, int multi) {
  FuncState *fs = ls->fs;
  int base, nparams;
  int line = ls->linenumber;
  expdesc e;
  if (f->k == VCALL && f->u.info == fs->pc - 1) {  /* just-coded call? */
    Instruction *inst = &fs->f->code[f->u.info];
    int nargs = GETARG_B(*inst) - 1;
    base = GETARG_A(*inst);
    if (nargs < 0) {  /* open call: truncate its last argument */
      Instruction *last = &fs->f->code[f->u.info - 1];
      lua_assert(GET_OPCODE(*last) == OP_CALL ||
                 GET_OPCODE(*last) == OP_VARARG);
      SETARG_C(*last, 2);  /* one value */
      nargs = GETARG_A(*last) - base;
    }
    luaK_removelastinstruction(fs);  /* reopen the call */
    fs->freereg = cast_byte(base + 1 + nargs);
  }
  else {
    luaK_exp2nextreg(fs, f);
    base = f->u.info;
  }
  if (multi)
    pipeoperand(ls, &e);
  else
    suffixedexp(ls, &e);
  if (hasmultret(e.k)) {
    luaK_setmultret(fs, &e);
    nparams = LUA_MULTRET;  /* open call */
  }
  else {
    luaK_exp2nextreg(fs, &e);
    nparams = fs->freereg - (base + 1);
  }
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams + 1, 2));
  luaK_fixline(fs, line);
  fs->freereg = cast_byte(base + 1);
}


static void singlepipes (LexState *ls, expdesc *v) {
  for (;;) {
    if (testnext(ls, TK_RPIPE1)) {
      luaK_exp2nextreg(ls->fs, v);
      pipeprepend(ls, v, 1);
    }
    else if (testnext(ls, TK_LPIPE1))
      pipeappend(ls, v, 0);
    else return;
  }
}


/* 'n' values on the top of the stack (the last described by 'v') */
static void doublepipes (LexState *ls, expdesc *v, int n) {
  for (;;) {
    if (testnext(ls, TK_RPIPE2)) {
      luaK_exp2nextreg(ls->fs, v);
      pipeprepend(ls, v, n);
      n = 1;
    }
    else if (testnext(ls, TK_LPIPE2)) {
      pipeappend(ls, v, 1);
      n = 1;
    }
    else return;
  }
}


static void pipeexp (LexState *ls, expdesc *v) {
  suffixedexp(ls, v);
  singlepipes(ls, v);
}


/*
** Keyword statements take their arguments as 'kw(...)' or piped:
** 'kw <- e' / 'kw <-- e'. A piped parenthesized argument is parsed as
** the call form. Returns 1 if the parentheses are there (consuming
** the opening one).
*/
static int kwopen (LexState *ls) {
  if (testnext(ls, TK_LPIPE1) || testnext(ls, TK_LPIPE2)) {
    if (ls->t.token != '(')
      return 0;  /* bare argument */
  }
  checknext(ls, '(');
  return 1;
}


static void kwclose (LexState *ls, int paren) {
  if (paren)
    checknext(ls, ')');
}

/* }====================================================================== */


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
  else pipeexp(ls, v);
  leavelevel(ls);
}


/*
** binloop -> { binop unaryexp }
** All binary operators have the same precedence and are left
** associative; mixing different operators requires parentheses.
*/
static void binloop (LexState *ls, expdesc *v) {
  int tok = ls->t.token;
  int first = tok;  /* all operators in a chain must be the same */
  for (;;) {
    BinOpr op = getbinopr(tok);
    const AtmOp *aop = getatmop(tok);
    expdesc v2;
    int line = ls->linenumber;
    if (op == OPR_NOBINOPR && aop == NULL)
      break;
    if (tok != first)
      luaX_syntaxerror(ls, "mixed operators require parentheses");
    luaX_next(ls);  /* skip operator */
    if (aop == NULL) {  /* Lua operator */
      luaK_infix(ls->fs, op, v);
      unaryexp(ls, &v2);
      luaK_posfix(ls->fs, op, v, &v2, line);
    }
    else {  /* Atmos operator: function call */
      unaryexp(ls, &v2);
      opcall(ls, v, &v2, aop, line);
    }
    tok = ls->t.token;
  }
  if (tok == TK_WHERE)
    luaX_syntaxerror(ls, "operator not implemented");
}


static void expr (LexState *ls, expdesc *v) {
  unaryexp(ls, v);
  binloop(ls, v);
  doublepipes(ls, v, 1);
}


/*
** An expression that may be a parenthesized list of values:
** returns the number of values (as 'explist').
*/
static int exprlist (LexState *ls, expdesc *v) {
  if (ls->t.token == '(') {
    int n = parenlist(ls, v);
    if (n > 1) {  /* multiple values */
      if (ls->t.token == TK_RPIPE2) {  /* piped into a call */
        luaK_exp2nextreg(ls->fs, v);
        doublepipes(ls, v, n);
        return 1;
      }
      return n;
    }
    suffixloop(ls, v, 0);  /* continue the expression */
    singlepipes(ls, v);
    binloop(ls, v);
    doublepipes(ls, v, 1);
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
    if (nvars == 1 && nexps == 1)
      chkassign(ls, &e);  /* tasks must be owned */
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
/*
** A jump leaving blocks with try regions ('catch' bodies) pops them
** first (OP_ENDTRY without a jump), like OP_CLOSE for 'pin's. 'target'
** is the block being jumped to (NULL: the function returns).
*/
static void popregions (FuncState *fs, BlockCnt *target) {
  BlockCnt *bl;
  for (bl = fs->bl; bl != target && bl != NULL; bl = bl->previous) {
    int i;
    for (i = 0; i < bl->nregions; i++)
      luaK_codeABC(fs, OP_ENDTRY, 0, 0, 0);  /* pop only */
  }
}


static BlockCnt *findloop (FuncState *fs) {
  BlockCnt *bl;
  for (bl = fs->bl; bl != NULL; bl = bl->previous) {
    if (bl->isloop)
      return bl;
  }
  return NULL;
}


/* a closure that is the body of a task: jumps out of it are throws */
#define istaskbody(fs) \
  ((fs)->iscatch == CB_SPAWN || (fs)->iscatch == CB_PAR || \
   (fs)->iscatch == CB_TOGGLE)


/*
** Registers a jump of 'kind' out of the task body being compiled: the
** target function is the first enclosing one that is not a task body;
** its innermost open owner region gets the dispatch (and the target is
** checked there, so a jump with no target is a compile error).
*/
static BlockCnt *findtag (FuncState *fs, TString *tag) {
  BlockCnt *bl;
  for (bl = fs->bl; bl != NULL; bl = bl->previous) {
    if (bl->tag == tag) return bl;
  }
  return NULL;
}

static void registerjump (LexState *ls, int kind, TString *tag) {
  FuncState *tf = ls->fs->prev;  /* the enclosing functions, outwards */
  Owner *o;
  BlockCnt *target = NULL;
  int i;
  for (;;) {  /* the first enclosing function with the target */
    if (kind == JK_BREAK) target = findloop(tf);
    else if (kind == JK_ESCAPE) target = findtag(tf, tag);
    if (target != NULL || !istaskbody(tf))
      break;  /* found, or a real function: it is the target of 'return' */
    tf = tf->prev;  /* a task body: look further out */
  }
  if (kind == JK_BREAK && target == NULL)
    luaK_semerror(ls, "break outside loop");
  if (kind == JK_ESCAPE && target == NULL)
    luaK_semerror(ls, "no enclosing block ':%s' for <escape>", getstr(tag));
  o = tf->owner;
  if (o == NULL)
    luaK_semerror(ls, "no owning block for a jump out of a task");
  o->needtry = 1;
  for (i = 0; i < o->njumps; i++) {
    if (o->jumps[i].kind == kind && o->jumps[i].tag == tag)
      return;  /* already registered */
  }
  if (o->njumps >= MAXJUMPS)
    luaK_semerror(ls, "too many kinds of jumps out of tasks in a block");
  o->jumps[o->njumps].kind = kind;
  o->jumps[o->njumps].tag = tag;
  o->jumps[o->njumps].target = target;
  o->njumps++;
}


/* starts a 'jump out' call: _ENV["atm.jump"](kind, [tag,] values) */
static int jumpbegin (LexState *ls, int kind, TString *tag) {
  FuncState *fs = ls->fs;
  int base = callbegin(ls, "atm.jump");
  luaK_int(fs, fs->freereg, kind);
  luaK_reserveregs(fs, 1);
  if (tag != NULL) {
    expdesc t;
    codestring(&t, tag);
    luaK_exp2nextreg(fs, &t);
  }
  return base;
}

static void jumpend (LexState *ls, int base, int open, int line) {
  expdesc v;
  callend(ls, &v, base, open, line);
  discard(ls, &v);
  ls->fs->freereg = cast_byte(base);
}


/*
** Opens an owner region at register 'reg' (speculative: patched into
** a no-op by 'closeowner' when no task body jumps out).
*/
static void openowner (LexState *ls, Owner *o, int reg) {
  FuncState *fs = ls->fs;
  o->previous = fs->owner;
  fs->owner = o;
  o->pc = luaK_codeAsBx(fs, OP_TRY, reg, 0);
  o->reg = reg;
  o->needtry = 0;
  o->njumps = 0;
}


/*
** A value left by the handler at register 'reg' as an expression: a
** copy in a fresh temporary (above all the handler's values, which
** the code generated here must not clobber), so the register
** discipline ('freeexp') holds.
*/
static void handlerval (FuncState *fs, expdesc *e, int reg) {
  int r = fs->freereg;
  luaK_reserveregs(fs, 1);
  luaK_codeABC(fs, OP_MOVE, r, reg, 0);
  init_exp(e, VNONRELOC, r);
}


/* dispatch of the jumps registered in 'o' (its handler code) */
static void jumpdispatch (LexState *ls, Owner *o, int line) {
  FuncState *fs = ls->fs;
  int oldfree = fs->freereg;
  int i;
  /* temporaries above the kind, tag and value of the handler */
  fs->freereg = cast_byte((o->reg + 4 > luaY_nvarstack(fs)) ? o->reg + 4
                                                             : luaY_nvarstack(fs));
  for (i = 0; i < o->njumps; i++) {
    JumpRec *j = &o->jumps[i];
    expdesc c, k;
    int vreg = o->reg + 2;
    handlerval(fs, &c, o->reg + 1);  /* the kind */
    init_exp(&k, VKINT, 0);
    k.u.ival = j->kind;
    luaK_infix(fs, OPR_EQ, &c);
    luaK_posfix(fs, OPR_EQ, &c, &k, line);
    luaK_goiftrue(fs, &c);
    if (j->kind == JK_ESCAPE) {  /* and the tag */
      expdesc t, s;
      handlerval(fs, &t, o->reg + 2);
      codestring(&s, j->tag);
      luaK_infix(fs, OPR_EQ, &t);
      luaK_posfix(fs, OPR_EQ, &t, &s, line);
      luaK_goiftrue(fs, &t);
      luaK_concat(fs, &c.f, t.f);
      vreg = o->reg + 3;
    }
    switch (j->kind) {
      case JK_BREAK: {
        expdesc v;
        handlerval(fs, &v, vreg);
        store_value(ls, j->target->res, &v);
        breakjump(ls, j->target, 1, line);
        break;
      }
      case JK_ESCAPE: {
        expdesc v;
        handlerval(fs, &v, vreg);
        store_value(ls, j->target->res, &v);
        popregions(fs, j->target);
        j->target->hasescape = 1;
        newgotoentry(ls, aritylabel(ls, j->tag, 1), line);
        break;
      }
      default: {  /* return: all the values */
        popregions(fs, NULL);
        luaK_codeABC(fs, OP_CATCHRES, o->reg, 0, 0);  /* open: 'top' after them */
        luaK_ret(fs, vreg, LUA_MULTRET);
        break;
      }
    }
    luaK_patchtohere(fs, c.f);
  }
  fs->freereg = cast_byte(oldfree);
}


/*
** Closes the owner region 'o': no jumps registered, its OP_TRY becomes
** a no-op; otherwise emits the pop of the region for the normal path
** and the handler (OP_CATCH mode 1: only jump throws) with the
** dispatch, ending in a rethrow (mode 2).
*/
static void closeowner (LexState *ls, Owner *o, int line) {
  FuncState *fs = ls->fs;
  lua_assert(fs->owner == o);
  fs->owner = o->previous;
  if (!o->needtry)
    fs->f->code[o->pc] = CREATE_ABCk(OP_MOVE, o->reg, o->reg, 0, 0);
  else {
    int jmp;
    luaK_codeABC(fs, OP_ENDTRY, o->reg, 0, 0);  /* normal path: pop */
    jmp = luaK_jump(fs);
    SETARG_sBx(fs->f->code[o->pc], luaK_getlabel(fs) - (o->pc + 1));
    luaK_codeABC(fs, OP_CATCH, o->reg, 1, 0);
    luaK_fixline(fs, line);
    jumpdispatch(ls, o, line);
    luaK_codeABC(fs, OP_CATCH, o->reg, 2, 0);  /* unmatched: rethrow */
    luaK_patchtohere(fs, jmp);
  }
}


/*
** Owner region of a block, for 'spawn { }' statements/declarations:
** opened by the first block spawn of the block at statement level (no
** live temporaries), with a hidden local '(try)' below the task's pin,
** so that leaving the block by a jump aborts the task.
*/
static void blockowner (LexState *ls) {
  FuncState *fs = ls->fs;
  BlockCnt *bl = fs->bl;
  if (bl->owner == NULL && fs->freereg == luaY_nvarstack(fs)) {
    Owner *o = &bl->ownerbuf;
    int reg = fs->freereg;
    new_localvarliteral(ls, "(try)");
    luaK_reserveregs(fs, 1);
    adjustlocalvars(ls, 1);
    openowner(ls, o, reg);
    bl->owner = o;
    bl->nregions++;
  }
}


/*
** Jump to the end of loop block 'bl' (a 'goto break').
*/
static void breakjump (LexState *ls, BlockCnt *bl, int n, int line) {
  popregions(ls->fs, bl);
  bl->isloop = 2;  /* signal that block has pending breaks */
  newgotoentry(ls, aritylabel(ls, ls->brkn, n), line);
}


/*
** Parse the arguments of 'break'/'until'/'while'/'escape' after the
** first one: ',' explist ')'. Stores the first extra value (or 'def'
** when there is none) into the block's value register.
*/
static int storeextra (LexState *ls, BlockCnt *bl, expdesc *def) {
  expdesc e;
  int n = 1;
  if (testnext(ls, ',')) {  /* the extra values are the exit's values */
    expr(ls, &e);
    while (testnext(ls, ',')) {
      luaK_exp2nextreg(ls->fs, &e);
      expr(ls, &e);
      n++;
    }
  }
  else
    e = *def;
  return storevalues(ls, bl->res, &e, n);
}


/* the loop targeted by 'what', or NULL inside a task body (a jump out) */
static BlockCnt *jumploop (LexState *ls, const char *what) {
  BlockCnt *bl = findloop(ls->fs);
  if (bl == NULL && !istaskbody(ls->fs))
    luaX_syntaxerror(ls, luaO_pushfstring(ls->L, "%s outside loop", what));
  return bl;
}


static void breakstat (LexState *ls, int line) {
  /* breakstat -> BREAK '(' [explist] ')' */
  FuncState *fs = ls->fs;
  BlockCnt *bl = jumploop(ls, "break");
  expdesc nilv;
  int paren;
  int base = -1;
  int nvals = 0;
  luaX_next(ls);  /* skip 'break' */
  if (bl == NULL) {  /* out of the task: atm.jump(JK_BREAK, value) */
    registerjump(ls, JK_BREAK, NULL);
    base = jumpbegin(ls, JK_BREAK, NULL);
  }
  paren = kwopen(ls);
  init_exp(&nilv, VNIL, 0);
  if (!paren || ls->t.token != ')') {
    expdesc e;
    int n = 1;
    expr(ls, &e);
    while (testnext(ls, ',')) {
      luaK_exp2nextreg(fs, &e);
      expr(ls, &e);
      n++;
    }
    if (bl == NULL) luaK_exp2nextreg(fs, &e);  /* (one value out of a task) */
    else n = storevalues(ls, bl->res, &e, n);
    nvals = n;
  }
  else if (bl == NULL) luaK_exp2nextreg(fs, &nilv);
  else {
    init_exp(&nilv, VVOID, 0);
    nvals = storevalues(ls, bl->res, &nilv, 0);  /* 'break()': no value */
  }
  kwclose(ls, paren);
  if (bl == NULL)
    jumpend(ls, base, 0, line);
  else
    breakjump(ls, bl, nvals, line);
}


/*
** 'until'/'while' inside a task body without a loop: the value (extra
** or 'def') goes to a jump out of the task.
*/
static void jumpextra (LexState *ls, expdesc *def, int line) {
  FuncState *fs = ls->fs;
  int base = jumpbegin(ls, JK_BREAK, NULL);
  expdesc e;
  if (testnext(ls, ',')) {
    expr(ls, &e);
    while (testnext(ls, ',')) {
      luaK_exp2nextreg(fs, &e);
      expr(ls, &e);
    }
  }
  else
    e = *def;
  luaK_exp2nextreg(fs, &e);
  jumpend(ls, base, 0, line);
}


static void untilstat (LexState *ls, int line) {
  /* untilstat -> UNTIL '(' cond [',' explist] ')'  (break when true) */
  FuncState *fs = ls->fs;
  BlockCnt *bl = jumploop(ls, "until");
  expdesc e, c;
  int jf;
  int paren;
  luaX_next(ls);  /* skip 'until' */
  if (bl == NULL) registerjump(ls, JK_BREAK, NULL);
  paren = kwopen(ls);
  expr(ls, &e);
  luaK_exp2anyreg(fs, &e);  /* condition value (to be the loop value) */
  c = e;
  luaK_goiftrue(fs, &c);  /* jump if false (frees the register) */
  luaK_reserveregs(fs, 1);  /* keep the condition value for the store */
  jf = c.f;
  if (bl == NULL)
    jumpextra(ls, &e, line);  /* out of the task */
  else {
    int n = storeextra(ls, bl, &e);  /* true: loop value is extra or condition */
    breakjump(ls, bl, n, line);
  }
  luaK_patchtohere(fs, jf);
  kwclose(ls, paren);
}


static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE '(' cond [',' explist] ')'  (break when false) */
  FuncState *fs = ls->fs;
  BlockCnt *bl = jumploop(ls, "while");
  expdesc nilv;
  int jf, jt;
  int paren;
  luaX_next(ls);  /* skip 'while' */
  if (bl == NULL) registerjump(ls, JK_BREAK, NULL);
  paren = kwopen(ls);
  jf = cond(ls);  /* jump if false */
  jt = luaK_jump(fs);  /* true: continue loop */
  luaK_patchtohere(fs, jf);
  init_exp(&nilv, VNIL, 0);
  if (bl == NULL)
    jumpextra(ls, &nilv, line);
  else {
    int n = storeextra(ls, bl, &nilv);  /* false: loop value is extra or nil */
    breakjump(ls, bl, n, line);
  }
  luaK_patchtohere(fs, jt);
  kwclose(ls, paren);
}


static void escapestat (LexState *ls, int line) {
  /* escapestat -> ESCAPE ('(' TAG [constructor] [',' explist] ')' |
                           TAG [constructor]) */
  FuncState *fs = ls->fs;
  BlockCnt *bl;
  TString *tag;
  expdesc tv;
  int paren;
  int base = -1;
  luaX_next(ls);  /* skip 'escape' */
  paren = (ls->t.token == TK_TAG) ? 0 : kwopen(ls);  /* 'escape :X' */
  check(ls, TK_TAG);
  tag = ls->t.seminfo.ts;
  for (bl = fs->bl; bl != NULL; bl = bl->previous) {
    if (bl->tag == tag)  /* found matching block? */
      break;
  }
  if (bl == NULL) {
    if (!istaskbody(fs))
      luaK_semerror(ls, "no enclosing block ':%s' for <escape>", getstr(tag));
    registerjump(ls, JK_ESCAPE, tag);  /* out of the task (checked there) */
    base = jumpbegin(ls, JK_ESCAPE, tag);
  }
  luaX_next(ls);  /* skip tag */
  if (ls->t.token == '[' && sameline(ls))
    tagconstructor(ls, &tv, tag);
  else
    codestring(&tv, tag);
  if (bl == NULL) {
    expdesc e;
    if (testnext(ls, ',')) {
      expr(ls, &e);
      while (testnext(ls, ',')) {
        luaK_exp2nextreg(fs, &e);
        expr(ls, &e);
      }
    }
    else
      e = tv;
    luaK_exp2nextreg(fs, &e);
    kwclose(ls, paren);
    jumpend(ls, base, 0, line);
    return;
  }
  {
    int n = storeextra(ls, bl, &tv);
    kwclose(ls, paren);
    popregions(fs, bl);
    bl->hasescape = 1;
    newgotoentry(ls, aritylabel(ls, tag, n), line);
  }
}


/*
** Condition of an 'if'/'ifs' case. When the branch is a lambda
** ('=> \\...'), it is called with the condition value, which is kept
** in register '*valreg' (-1 otherwise). Returns the false jump.
*/
static int condvalue (LexState *ls, int *valreg) {
  FuncState *fs = ls->fs;
  expdesc v;
  expr(ls, &v);  /* read condition */
  if (v.k == VNIL) v.k = VFALSE;  /* 'falses' are all equal here */
  if (ls->t.token == TK_ARROW && luaX_lookahead(ls) == '\\') {
    expdesc c;
    *valreg = luaK_exp2anyreg(fs, &v);  /* keep the value */
    c = v;
    luaK_goiftrue(fs, &c);  /* jump if false (may free the register) */
    if (fs->freereg == *valreg)
      luaK_reserveregs(fs, 1);  /* keep it reserved for the call */
    return c.f;
  }
  else {
    *valreg = -1;
    luaK_goiftrue(fs, &v);
    return v.f;
  }
}


/*
** '=>' branch of an 'if'/'ifs': a block, an expression, or a lambda,
** which is called with the condition value ('valreg', or 'true').
*/
/*
** Expression of a branch ('=> e'): an expression, or a parenthesized
** tuple giving all its values; returns the number of values.
*/
static int branchexpr (LexState *ls, expdesc *e) {
  if (ls->t.token == '(') {
    int n = parenlist(ls, e);
    if (n > 1)
      return n;  /* a tuple */
    suffixloop(ls, e, 0);  /* a parenthesized value: whole expression */
    singlepipes(ls, e);
    binloop(ls, e);
    doublepipes(ls, e, 1);
    return 1;
  }
  expr(ls, e);
  return 1;
}


static int arrowbranch (LexState *ls, ResDesc *rd, int valreg) {
  FuncState *fs = ls->fs;
  checknext(ls, TK_ARROW);
  if (ls->t.token == '{') {
    fs->freereg = cast_byte(luaY_nvarstack(fs));  /* drop the value */
    return block(ls, rd);
  }
  else {
    expdesc e;
    int line = ls->linenumber;
    int n = 1;
    if (testnext(ls, '\\')) {  /* lambda: called with the value */
      lambda(ls, &e, line);
      callhead(ls, &e, valreg);
    }
    else {
      fs->freereg = cast_byte(luaY_nvarstack(fs));
      n = branchexpr(ls, &e);
    }
    n = storevalues(ls, rd, &e, n);
    ls->blockline = ls->lastline;
    fs->freereg = cast_byte(luaY_nvarstack(fs));
    return n;
  }
}


/*
** if -> IF cond ('{' block '}' | '=>' branch) [ELSE ('{' block '}' | '=>' branch)]
*/
static int ifstat (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  ResDesc rd;
  int condfalse, valreg, n;
  initres(&rd, luaY_nvarstack(fs));  /* register for the values */
  luaX_next(ls);  /* skip 'if' */
  condfalse = condvalue(ls, &valreg);
  if (ls->t.token == '{')  /* 'then' part */
    n = block(ls, &rd);
  else
    n = arrowbranch(ls, &rd, valreg);
  luaK_concat(fs, &rd.jumps[n], luaK_jump(fs));
  luaK_fixline(fs, ls->blockline);  /* jump "belongs" to the branch */
  luaK_patchtohere(fs, condfalse);
  if (testnext(ls, TK_ELSE)) {  /* 'else' part */
    if (ls->t.token == '{')
      n = block(ls, &rd);
    else
      n = arrowbranch(ls, &rd, -1);
  }
  else if (ls->t.token == TK_ARROW)  /* if c => a => b */
    n = arrowbranch(ls, &rd, -1);
  else {
    expdesc e;
    init_exp(&e, VVOID, 0);
    n = storevalues(ls, &rd, &e, 0);  /* no else: nil */
  }
  rd.endarity = n;
  finishexits(ls, &rd, NULL);
  finishres(ls, &rd, v);
  return 1;
}


/*
** ifs -> IFS '{' { cond '=>' branch } [ELSE '=>' branch] '}'
*/
static int ifsstat (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  ResDesc rd;
  int haselse = 0;
  int n = 0;
  int line = ls->linenumber;
  initres(&rd, luaY_nvarstack(fs));  /* register for the values */
  luaX_next(ls);  /* skip 'ifs' */
  checknext(ls, '{');
  while (ls->t.token != '}') {
    if (testnext(ls, TK_ELSE)) {
      n = arrowbranch(ls, &rd, -1);
      haselse = 1;
      break;
    }
    else {
      int valreg;
      int condfalse = condvalue(ls, &valreg);
      n = arrowbranch(ls, &rd, valreg);
      luaK_concat(fs, &rd.jumps[n], luaK_jump(fs));
      luaK_fixline(fs, ls->blockline);  /* jump "belongs" to the branch */
      luaK_patchtohere(fs, condfalse);
    }
  }
  if (!haselse) {
    expdesc e;
    init_exp(&e, VVOID, 0);
    n = storevalues(ls, &rd, &e, 0);  /* no case: nil */
  }
  check_match(ls, '}', '{', line);
  rd.endarity = n;
  finishexits(ls, &rd, NULL);
  finishres(ls, &rd, v);
  return 1;
}


/*
** Branch of a 'match': '{' block '}', an expression, or a lambda
** called with the head (in register 'head').
*/
/* code the call 'f(head)' ('f(true)' when 'head' is -1) */
static void callhead (LexState *ls, expdesc *f, int head) {
  FuncState *fs = ls->fs;
  int base;
  luaK_exp2nextreg(fs, f);
  base = f->u.info;
  luaK_reserveregs(fs, 1);
  if (head >= 0)
    movereg(fs, base + 1, head);
  else {
    expdesc t;
    init_exp(&t, VTRUE, 0);
    storereg(fs, base + 1, &t);
  }
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, 2, 2));
  fs->freereg = cast_byte(base + 1);
}


/*
** Branch of a 'match': a block, an expression, or a lambda called with
** 'head' (the head of the match, or the result of a lambda case, which
** may be a temporary register above the locals).
*/
static int matchbranch (LexState *ls, ResDesc *rd, int head) {
  FuncState *fs = ls->fs;
  if (ls->t.token == '{') {
    fs->freereg = cast_byte(luaY_nvarstack(fs));  /* drop temporaries */
    return block(ls, rd);
  }
  else {
    expdesc e;
    int line = ls->linenumber;
    int n = 1;
    if (testnext(ls, '\\')) {  /* lambda: called with the head */
      lambda(ls, &e, line);
      callhead(ls, &e, head);
    }
    else {
      fs->freereg = cast_byte(luaY_nvarstack(fs));
      n = branchexpr(ls, &e);
    }
    n = storevalues(ls, rd, &e, n);
    ls->blockline = ls->lastline;
    fs->freereg = cast_byte(luaY_nvarstack(fs));
    return n;
  }
}


/*
** match -> MATCH expr '{' { case '=>' branch } [ELSE '=>' branch] '}'
** A case is an expression ('head ?? case') or a lambda (called with
** the head). The head is kept in a hidden local; the value of the
** chosen branch replaces it.
*/
static int matchstat (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  BlockCnt bl;
  ResDesc rd;
  expdesc h;
  int reg;  /* register for the head and then for the value */
  int haselse = 0;
  int n = 0;
  int line;
  luaX_next(ls);  /* skip 'match' */
  enterblock(fs, &bl, 0);
  reg = luaY_nvarstack(fs);
  initres(&rd, reg);
  expr(ls, &h);
  luaK_exp2nextreg(fs, &h);
  new_localvarliteral(ls, "(match)");  /* hidden local holding the head */
  adjustlocalvars(ls, 1);
  line = ls->linenumber;
  checknext(ls, '{');
  while (ls->t.token != '}') {
    if (testnext(ls, TK_ELSE)) {
      checknext(ls, TK_ARROW);
      n = matchbranch(ls, &rd, reg);
      haselse = 1;
      break;
    }
    else {
      expdesc c;
      int condfalse;
      int valreg = reg;  /* value given to a lambda branch */
      int cline = ls->linenumber;
      if (testnext(ls, '\\')) {  /* lambda case: its result is the value */
        expdesc cc;
        lambda(ls, &c, cline);
        callhead(ls, &c, reg);
        valreg = luaK_exp2anyreg(fs, &c);
        cc = c;
        luaK_goiftrue(fs, &cc);  /* (may free the register) */
        if (fs->freereg == valreg)
          luaK_reserveregs(fs, 1);  /* keep it for the branch */
        condfalse = cc.f;
      }
      else {  /* 'head ?? case' */
        expdesc hc;
        expr(ls, &c);
        init_exp(&hc, VNONRELOC, reg);
        opcall(ls, &hc, &c, getatmop(TK_IS), cline);
        luaK_goiftrue(fs, &hc);
        condfalse = hc.f;
      }
      checknext(ls, TK_ARROW);
      n = matchbranch(ls, &rd, valreg);
      luaK_concat(fs, &rd.jumps[n], luaK_jump(fs));
      luaK_fixline(fs, ls->blockline);  /* jump "belongs" to the branch */
      luaK_patchtohere(fs, condfalse);
    }
  }
  if (!haselse) {
    expdesc e;
    init_exp(&e, VVOID, 0);
    n = storevalues(ls, &rd, &e, 0);  /* no case: nil */
  }
  check_match(ls, '}', '{', line);
  rd.endarity = n;
  finishexits(ls, &rd, NULL);
  leaveblock(fs);
  finishres(ls, &rd, v);
  return 1;
}


/*
** test -> TEST '{' block '}': the block runs only when 'atmos.test' is
** true (nil otherwise).
*/
static int teststat (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  ResDesc rd;
  expdesc c, key, e;
  int condfalse, n;
  initres(&rd, luaY_nvarstack(fs));  /* register for the values */
  luaX_next(ls);  /* skip 'test' */
  buildvar(ls, luaX_newstring(ls, "atmos", 5), &c);
  luaK_exp2anyregup(fs, &c);
  codestring(&key, luaX_newstring(ls, "test", 4));
  luaK_indexed(fs, &c, &key);
  luaK_goiftrue(fs, &c);
  condfalse = c.f;
  check(ls, '{');
  n = block(ls, &rd);
  luaK_concat(fs, &rd.jumps[n], luaK_jump(fs));
  luaK_fixline(fs, ls->blockline);
  luaK_patchtohere(fs, condfalse);
  init_exp(&e, VVOID, 0);
  rd.endarity = storevalues(ls, &rd, &e, 0);  /* not testing: nil */
  finishexits(ls, &rd, NULL);
  finishres(ls, &rd, v);
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
    ResDesc rd;
    BlockCnt bl;
    expdesc e;
    int n;
    int line;
    TString *tag = NULL;
    initres(&rd, luaY_nvarstack(fs));  /* register for the values */
    if (ls->t.token == TK_TAG) {
      tag = ls->t.seminfo.ts;
      luaX_next(ls);
    }
    line = ls->linenumber;
    enterblock(fs, &bl, 0);
    bl.tag = tag;
    bl.res = &rd;
    checknext(ls, '{');
    statlist(ls, &e, &n);
    rd.endarity = storevalues(ls, &rd, &e, n);
    check_match(ls, '}', '{', line);
    leaveblock(fs);  /* (creates the escape labels and the padding) */
    if (!bl.hasescape)  /* no exits other than the end: pad here */
      finishexits(ls, &rd, NULL);
    finishres(ls, &rd, v);
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
  block(ls, NULL);
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
  ResDesc rd;
  int line = ls->linenumber;
  BlockCnt bl;
  initres(&rd, luaY_nvarstack(fs));  /* register for the values */
  luaX_next(ls);  /* skip 'loop' */
  enterblock(fs, &bl, 1);  /* loop block */
  bl.res = &rd;
  if (ls->t.token == '{') {  /* infinite loop */
    int loopinit = luaK_getlabel(fs);
    block(ls, NULL);
    luaK_jumpto(fs, loopinit);
    luaK_fixline(fs, ls->blockline);  /* jump "belongs" to the body */
  }
  else {
    TString *names[MAXLOOPVARS];
    int nnames = 0;
    int base = fs->freereg;
    while (ls->t.token != TK_ON) {
      if (nnames >= MAXLOOPVARS)
        luaX_syntaxerror(ls, "too many loop variables");
      names[nnames++] = str_checkname(ls);
      if (!testnext(ls, ',')) break;
    }
    if (ls->t.token == TK_ON) {  /* 'loop [ids] on P { }': await each turn */
      int loopinit;
      int callbase;
      expdesc e;
      BlockCnt bl2;
      luaX_next(ls);  /* skip 'on' */
      loopinit = luaK_getlabel(fs);
      enterblock(fs, &bl2, 0);  /* one scope per turn (fresh variables) */
      line = ls->linenumber;
      callbase = callbegin(ls, "await");
      pattexp(ls, &e);
      luaK_exp2nextreg(fs, &e);
      callend(ls, &e, callbase, 0, line);
      if (nnames == 0) {
        discard(ls, &e);
        fs->freereg = cast_byte(callbase);
      }
      else {
        int i;
        for (i = 0; i < nnames; i++)
          new_localvar(ls, names[i]);
        adjust_assign(ls, nnames, 1, &e);
        adjustlocalvars(ls, nnames);
      }
      block(ls, NULL);
      leaveblock(fs);
      luaK_jumpto(fs, loopinit);
      luaK_fixline(fs, ls->blockline);
    }
    else if (ls->t.token == '{') {  /* numeric infinite loop */
      if (nnames != 1)
        luaX_syntaxerror(ls, "'in' expected");
      luaK_int(fs, fs->freereg, 1); luaK_reserveregs(fs, 1);
      luaK_int(fs, fs->freereg, LUA_MAXINTEGER); luaK_reserveregs(fs, 1);
      luaK_int(fs, fs->freereg, 1); luaK_reserveregs(fs, 1);
      fornum(ls, names[0], base, line);
    }
    else {
      expdesc e;
      int n = 0;
      int numeric = 0;
      checknext(ls, TK_IN);
      line = ls->linenumber;
      if (nnames == 1 && ls->t.token == '(') {  /* maybe a numeric range */
        n = parenlist(ls, &e);
        if (n == 2 || n == 3)
          numeric = 1;
        else {  /* one value or a tuple of 'n' values: iterate them */
          expdesc f;
          if (n == 1) {  /* a single parenthesized value: whole expression */
            suffixloop(ls, &e, 0);
            singlepipes(ls, &e);
            binloop(ls, &e);
            doublepipes(ls, &e, 1);
          }
          luaK_exp2nextreg(fs, &e);  /* values at 'base'.. */
          buildvar(ls, luaX_newstring(ls, "loop.iter", 9), &f);
          luaK_exp2nextreg(fs, &f);  /* iterator function above them */
          rotatecall(fs, base, n, 1);  /* function first */
          init_exp(&e, VCALL, luaK_codeABC(fs, OP_CALL, base, n + 1, 0));
        }
      }
      else {  /* iterate the values of the expression: iter(e...) */
        expdesc f;
        int nparams;
        buildvar(ls, luaX_newstring(ls, "loop.iter", 9), &f);
        luaK_exp2nextreg(fs, &f);  /* iterator function at 'base' */
        n = exprlist(ls, &e);
        if (hasmultret(e.k)) {
          luaK_setmultret(fs, &e);
          nparams = LUA_MULTRET;
        }
        else {
          luaK_exp2nextreg(fs, &e);
          nparams = fs->freereg - (base + 1);
        }
        init_exp(&e, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams + 1, 0));
      }
      luaK_fixline(fs, line);
      if (numeric) {
        luaK_exp2nextreg(fs, &e);
        if (n == 2) {  /* default step = 1 */
          luaK_int(fs, fs->freereg, 1);
          luaK_reserveregs(fs, 1);
        }
        fornum(ls, names[0], base, line);
      }
      else {  /* generic loop over the four values of the iterator call */
        fs->freereg = cast_byte(base + 1);
        adjust_assign(ls, 4, 1, &e);
        forlist(ls, names, nnames, base, line);
      }
    }
  }
  {  /* normal exit: the loop value is nil */
    expdesc e;
    init_exp(&e, VVOID, 0);
    rd.endarity = storevalues(ls, &rd, &e, 0);
  }
  leaveblock(fs);  /* loop scope ('break' labels and padding) */
  if (bl.isloop != 2)  /* no breaks: pad here */
    finishexits(ls, &rd, NULL);
  finishres(ls, &rd, v);
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
  TString *names[MAXVARS];
  int i;
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
  if (kind == RDKCONST && ls->t.token == TK_TASK) {  /* val task? */
    luaX_next(ls);
    localtask(ls, line);
    return;
  }
  do {  /* for each variable */
    luaY_checklimit(fs, nvars + 1, MAXVARS, "local variables");
    names[nvars++] = str_checkname(ls);  /* get its name */
    if (kind == RDKTOCLOSE && nvars > 1)  /* to-be-closed list? */
      luaK_semerror(ls, "multiple to-be-closed variables in local list");
  } while (testnext(ls, ','));
  if (testnext(ls, '=')) {  /* initialization? */
    /* tasks and pools: 'pin' owns a fresh value ('pin.set'); other
       declarations of a spawn or pool require an owned one ('pin.chk') */
    if (ls->t.token == TK_SPAWN && luaX_lookahead(ls) == '{')
      blockowner(ls);  /* a spawned block: a region for its jumps */
    if (kind == RDKTOCLOSE) {  /* pin: owns the value */
      if (ls->t.token == TK_SPAWN) {
        spawnexp(ls, &e, 1);  /* 'pin t = spawn { }' promotes the block */
        nexps = 1;
      }
      else
        nexps = exprlist(ls, &e);
      if (nexps == 1)
        pinop(ls, &e, PIN_SET);
    }
    else {
      nexps = exprlist(ls, &e);
      if (nvars == 1 && nexps == 1)
        chkassign(ls, &e);
    }
  }
  else {
    e.k = VVOID;
    nexps = 0;
  }
  /* declare the variables only now: the expressions may be blocks
     declaring locals of their own in this function */
  for (i = 0; i < nvars; i++)
    new_varkind(ls, names[i], kind);
  if (kind == RDKTOCLOSE)
    toclose = fs->nactvar;
  adjust_assign(ls, nvars, nexps, &e);
  adjustlocalvars(ls, nvars);
  checktoclose(fs, toclose);
}


/*
** deferstat -> 'defer' funcblock
** The body becomes a closure stored in a hidden to-be-closed local;
** a function as a to-be-closed value is called when the variable goes
** out of scope ('lfunc.c').
*/
static void deferstat (LexState *ls, int line) {
  FuncState *fs = ls->fs;
  expdesc f;
  int toclose;
  luaX_next(ls);  /* skip 'defer' */
  check(ls, '{');
  new_varkind(ls, luaX_newstring(ls, "(defer)", 7), RDKTOCLOSE);
  toclose = fs->nactvar;
  closurebody(ls, &f, line, 0);
  adjust_assign(ls, 1, 1, &f);
  adjustlocalvars(ls, 1);
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
  int paren;
  luaX_next(ls);  /* skip 'return' */
  if (istaskbody(fs)) {  /* returns from the owner: a jump out */
    int base;
    int open = 0;
    registerjump(ls, JK_RETURN, NULL);
    base = jumpbegin(ls, JK_RETURN, NULL);
    paren = kwopen(ls);
    if (!(paren && ls->t.token == ')')) {
      if (paren)
        explist(ls, &e);
      else
        expr(ls, &e);
      open = callarg(ls, &e);
    }
    if (paren)
      check_match(ls, ')', '(', line);
    jumpend(ls, base, open, line);
    return;
  }
  popregions(fs, NULL);  /* leaving the function: pop its regions */
  paren = kwopen(ls);
  if (paren && ls->t.token == ')')
    nret = 0;  /* return no values */
  else {
    if (paren)
      nret = explist(ls, &e);  /* optional return values */
    else {
      expr(ls, &e);  /* 'return <- e' */
      nret = 1;
    }
    if (hasmultret(e.k)) {
      luaK_setmultret(fs, &e);
      if (e.k == VCALL && nret == 1 && !fs->bl->insidetbc &&
          GET_OPCODE(getinstruction(fs, &e)) == OP_CALL) {  /* tail call? */
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
  if (paren)
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
    case TK_DEFER: {
      deferstat(ls, line);
      break;
    }
    case TK_SPAWN: {
      n = spawnstat(ls, v);
      break;
    }
    case TK_TASK: {
      if (luaX_lookahead(ls) == TK_NAME) {  /* declaration? */
        taskstat(ls, line);
        break;
      }
      n = exprlist(ls, v);  /* expression statement */
      break;
    }
    case TK_THREAD: {
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
