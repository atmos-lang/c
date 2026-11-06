/*
** $Id: latmlex.c $
** Lexical Analyzer for Atmos
** See Copyright Notice in lua.h
*/

#define latmlex_c
#define LUA_CORE

#include "lprefix.h"


#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lobject.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"


/*
** The Atmos lexer shares 'LexState' with the Lua lexer, but it needs to
** push back characters when reading operators (longest match). 'next'
** consumes pushed-back characters first.
*/
#define next(ls) \
  (ls->current = (ls->npback > 0 ? ls->pback[--ls->npback] : zgetc(ls->z)))

#define save(ls,c)	luaX_save(ls,c)

#define save_and_next(ls)	(save(ls, ls->current), next(ls))

#define currIsNewline(ls)	(ls->current == '\n' || ls->current == '\r')


/*
** Push back the current character and make 'c' the current one.
*/
static void unread (LexState *ls, int c) {
  lua_assert(ls->npback < LUAX_MAXPBACK);
  ls->pback[ls->npback++] = ls->current;
  ls->current = c;
}


/*
** Atmos reserved words (a subset of all reserved words in 'luaX_tokens').
*/
int luaX_isatmword (int token) {
  switch (token) {
    case TK_AWAIT: case TK_CATCH: case TK_DEFER: case TK_DO: case TK_ELSE:
    case TK_EMIT: case TK_FALSE: case TK_FUNC: case TK_IF: case TK_IFS:
    case TK_IN: case TK_LOOP: case TK_MATCH: case TK_NIL: case TK_ON:
    case TK_PAR: case TK_PIN: case TK_SET: case TK_SPAWN: case TK_TASK:
    case TK_TASKS: case TK_TEST: case TK_THREAD: case TK_TOGGLE:
    case TK_TRUE: case TK_UNTIL: case TK_VAL: case TK_VAR: case TK_WATCHING:
    case TK_WHERE: case TK_WITH: case TK_WHILE:
      return 1;
    default:
      return 0;
  }
}


/*
** Text of a token for error messages ("near 'xxx'"), already quoted.
*/
const char *luaX_atmtoktext (LexState *ls, const Token *t) {
  switch (t->token) {
    case TK_NAME: case TK_STRING: case TK_FLT: case TK_INT:
    case TK_TAG: case TK_CLK: case TK_NAT: {
      if (t->text != NULL)
        return luaO_pushfstring(ls->L, "'%s'", getstr(t->text));
      else if (t->token == TK_NAME || t->token == TK_STRING ||
               t->token == TK_TAG || t->token == TK_NAT)
        return luaO_pushfstring(ls->L, "'%s'", getstr(t->seminfo.ts));
      else
        return luaX_token2str(ls, t->token);
    }
    case TK_EOS:
      return "'<eof>'";
    default:
      return luaX_token2str(ls, t->token);
  }
}


static l_noret lexerr (LexState *ls, const char *msg, const char *near) {
  luaX_atmerror(ls, ls->linenumber, near, msg);
}


static void inclinenumber (LexState *ls) {
  int old = ls->current;
  lua_assert(currIsNewline(ls));
  next(ls);  /* skip '\n' or '\r' */
  if (currIsNewline(ls) && ls->current != old)
    next(ls);  /* skip '\n\r' or '\r\n' */
  if (++ls->linenumber >= INT_MAX)
    lexerr(ls, "chunk has too many lines", NULL);
  ls->sep++;
}


/*
** Save current buffer as the raw text of the token (for messages).
*/
static TString *bufftext (LexState *ls) {
  return luaX_newstring(ls, luaZ_buffer(ls->buff), luaZ_bufflen(ls->buff));
}


/*
** {======================================================
** Comments:  ;; single line  |  ;;;(+) multi line ;;;(+)
** =======================================================
*/

/* read a run of ';' and return its length */
static int read_semis (LexState *ls) {
  int n = 0;
  while (ls->current == ';') {
    next(ls);
    ls->sep++;
    n++;
  }
  return n;
}


static void read_comment (LexState *ls, int n0) {
  int line = ls->linenumber;
  if (n0 == 2) {  /* single-line comment */
    while (!currIsNewline(ls) && ls->current != EOZ)
      next(ls);  /* skip until end of line (or end of file) */
  }
  else {  /* multi-line comment, with nesting by delimiter length */
    int stk[64];
    int top = 0;
    int s = n0;
    for (;;) {
      if (top > 0 && stk[top-1] == s) {
        top--;
        if (top == 0) break;
      }
      else {
        if (top >= 64) lexerr(ls, "comment nesting too deep", NULL);
        stk[top++] = s;
      }
      do {
        while (ls->current != ';') {
          if (ls->current == EOZ) {
            const char *msg = luaO_pushfstring(ls->L,
              "unterminated comment (starting at line %d)", line);
            lexerr(ls, msg, NULL);
          }
          if (currIsNewline(ls)) inclinenumber(ls);
          else next(ls);
        }
        s = read_semis(ls);
      } while (!(s > 2 && s >= stk[top-1]));
    }
  }
}

/* }====================================================== */


/*
** {======================================================
** Operators (longest match)
** =======================================================
*/

static int isopchar (int c) {
  switch (c) {
    case '+': case '-': case '*': case '/': case '%': case '>': case '<':
    case '=': case '|': case '&': case '?': case '!': case '#': case '~':
      return 1;
    default:
      return 0;
  }
}


/*
** Return the token for operator string 'op' of length 'n', or 0
*/
static int optoken (const char *op, int n) {
  if (n == 3) {
    if (memcmp(op, "===", 3) == 0) return TK_DEQ;
    if (memcmp(op, "=!=", 3) == 0) return TK_DNE;
    if (memcmp(op, "=>=", 3) == 0) return TK_SUP;
    if (memcmp(op, "=<=", 3) == 0) return TK_SUB;
    if (memcmp(op, "-->", 3) == 0) return TK_RPIPE2;
    if (memcmp(op, "<--", 3) == 0) return TK_LPIPE2;
    return 0;
  }
  else if (n == 2) {
    if (memcmp(op, "=>", 2) == 0) return TK_ARROW;
    if (memcmp(op, "==", 2) == 0) return TK_EQ;
    if (memcmp(op, "!=", 2) == 0) return TK_LNE;
    if (memcmp(op, ">=", 2) == 0) return TK_GE;
    if (memcmp(op, "<=", 2) == 0) return TK_LE;
    if (memcmp(op, "||", 2) == 0) return TK_LOR;
    if (memcmp(op, "&&", 2) == 0) return TK_LAND;
    if (memcmp(op, "**", 2) == 0) return TK_POW;
    if (memcmp(op, "//", 2) == 0) return TK_IDIV;
    if (memcmp(op, "++", 2) == 0) return TK_CAT;
    if (memcmp(op, "??", 2) == 0) return TK_IS;
    if (memcmp(op, "!?", 2) == 0) return TK_ISNOT;
    if (memcmp(op, "?>", 2) == 0) return TK_XIN;
    if (memcmp(op, "!>", 2) == 0) return TK_XNOTIN;
    if (memcmp(op, "->", 2) == 0) return TK_RPIPE1;
    if (memcmp(op, "<-", 2) == 0) return TK_LPIPE1;
    return 0;
  }
  else {
    switch (op[0]) {
      case '#': case '=': case '>': case '<': case '+': case '-':
      case '*': case '/': case '%':
        return op[0];
      case '!':
        return TK_LNOT;
      default:
        return 0;
    }
  }
}


static int read_operator (LexState *ls) {
  char op[4];
  int n = 0;
  int tk;
  while (n < 3 && isopchar(ls->current)) {
    op[n++] = cast_char(ls->current);
    next(ls);
  }
  op[n] = '\0';
  for (;;) {
    tk = optoken(op, n);
    if (tk != 0) return tk;
    if (n == 1) break;
    /* push back last character and try a shorter operator */
    unread(ls, op[--n]);
    op[n] = '\0';
  }
  lexerr(ls, "invalid operator",
         luaO_pushfstring(ls->L, "'%s'", luaZ_buffer(ls->buff)));
  return 0;  /* to avoid warnings */
}

/* }====================================================== */


/*
** {======================================================
** Numbers and clock durations
** =======================================================
*/

/*
** Clock units, in microseconds, in the order they must appear
*/
static const struct {
  const char *name;
  lua_Integer us;
} clkunits[] = {
  {"day", 86400000000LL}, {"h", 3600000000LL}, {"min", 60000000LL},
  {"s", 1000000LL}, {"ms", 1000LL}, {"us", 1LL}
};


/*
** Try to read a clock duration (e.g., "1h30min") from the buffer.
** Returns 1 and fills 'seminfo' on success.
*/
static int read_clock (LexState *ls, SemInfo *seminfo, int *isflt) {
  const char *s = luaZ_buffer(ls->buff);
  lua_Integer ival = 0;
  lua_Number fval = 0;
  int ncomps = 0;
  int u = 0;
  *isflt = 0;
  while (*s != '\0') {
    const char *start = s;
    lua_Number n;
    int flt = 0;
    size_t len;
    char numbuf[64];
    if (!lisdigit(cast_uchar(*s))) return 0;
    while (lisdigit(cast_uchar(*s))) s++;
    if (*s == '.') {
      flt = 1; s++;
      while (lisdigit(cast_uchar(*s))) s++;
    }
    len = cast_sizet(s - start);
    if (len >= sizeof(numbuf)) return 0;
    memcpy(numbuf, start, len); numbuf[len] = '\0';
    n = lua_str2number(numbuf, NULL);
    /* unit must be in descending order */
    for (;;) {
      size_t ul;
      if (u >= cast_int(sizeof(clkunits)/sizeof(clkunits[0]))) return 0;
      ul = strlen(clkunits[u].name);
      if (strncmp(s, clkunits[u].name, ul) == 0 &&
          !lislalpha(cast_uchar(s[ul]))) {  /* avoid 'm' matching 'ms' */
        s += ul;
        break;
      }
      u++;
    }
    if (flt) {
      *isflt = 1;
      fval += n * cast_num(clkunits[u].us);
    }
    else {
      lua_Integer iv = cast(lua_Integer, n);
      ival += iv * clkunits[u].us;
      fval += n * cast_num(clkunits[u].us);
    }
    u++;
    ncomps++;
  }
  if (ncomps == 0) return 0;
  if (*isflt) seminfo->r = fval;
  else seminfo->i = ival;
  return 1;
}


static int read_numeral (LexState *ls, SemInfo *seminfo) {
  TValue obj;
  int hasexp = 0;
  lua_assert(lisdigit(ls->current));
  /* read [%w%.]* ; after an exponent mark also accept [-+] */
  for (;;) {
    if (lislalnum(ls->current) || ls->current == '.') {
      if (ls->current == 'e' || ls->current == 'E' ||
          ls->current == 'p' || ls->current == 'P')
        hasexp = 1;
      save_and_next(ls);
    }
    else if (hasexp && (ls->current == '-' || ls->current == '+'))
      save_and_next(ls);
    else break;
  }
  save(ls, '\0');
  if (luaO_str2num(luaZ_buffer(ls->buff), &obj) != 0) {  /* a number? */
    luaZ_buffremove(ls->buff, 1);
    if (ttisinteger(&obj)) {
      seminfo->i = ivalue(&obj);
      return TK_INT;
    }
    else {
      lua_assert(ttisfloat(&obj));
      seminfo->r = fltvalue(&obj);
      return TK_FLT;
    }
  }
  else {  /* clock duration? */
    int isflt;
    if (read_clock(ls, seminfo, &isflt)) {
      luaZ_buffremove(ls->buff, 1);
      return TK_CLK;
    }
    luaZ_buffremove(ls->buff, 1);
    lexerr(ls, "invalid number",
           luaO_pushfstring(ls->L, "'%s'", luaZ_buffer(ls->buff)));
    return 0;  /* to avoid warnings */
  }
}

/* }====================================================== */


/*
** {======================================================
** Strings and natives
** =======================================================
*/

/*
** Decode Lua escape sequences in 'src' (length 'n') into the buffer.
*/
static void decode_escapes (LexState *ls, const char *src, size_t n,
                            int line) {
  size_t i = 0;
  luaZ_resetbuffer(ls->buff);
  while (i < n) {
    int c = cast_uchar(src[i++]);
    if (c != '\\') { save(ls, c); continue; }
    if (i >= n) lexerr(ls, "invalid escape sequence", NULL);
    c = cast_uchar(src[i++]);
    switch (c) {
      case 'a': save(ls, '\a'); break;
      case 'b': save(ls, '\b'); break;
      case 'f': save(ls, '\f'); break;
      case 'n': save(ls, '\n'); break;
      case 'r': save(ls, '\r'); break;
      case 't': save(ls, '\t'); break;
      case 'v': save(ls, '\v'); break;
      case '\\': case '"': case '\'': case '`': save(ls, c); break;
      case '\n': case '\r': save(ls, '\n'); break;
      case 'x': {
        int r = 0, k;
        for (k = 0; k < 2; k++) {
          if (i >= n || !lisxdigit(cast_uchar(src[i])))
            lexerr(ls, "hexadecimal digit expected", NULL);
          r = (r << 4) + luaO_hexavalue(cast_uchar(src[i++]));
        }
        save(ls, r);
        break;
      }
      case 'z': {
        while (i < n && lisspace(cast_uchar(src[i]))) i++;
        break;
      }
      case 'u': {
        l_uint32 r = 0;
        char buff[UTF8BUFFSZ];
        int k;
        if (i >= n || src[i] != '{') lexerr(ls, "missing '{'", NULL);
        i++;
        if (i >= n || !lisxdigit(cast_uchar(src[i])))
          lexerr(ls, "hexadecimal digit expected", NULL);
        while (i < n && lisxdigit(cast_uchar(src[i]))) {
          if (r > (0x7FFFFFFFu >> 4)) lexerr(ls, "UTF-8 value too large", NULL);
          r = (r << 4) + cast_uint(luaO_hexavalue(cast_uchar(src[i++])));
        }
        if (i >= n || src[i] != '}') lexerr(ls, "missing '}'", NULL);
        i++;
        k = luaO_utf8esc(buff, r);
        for (; k > 0; k--) save(ls, buff[UTF8BUFFSZ - k]);
        break;
      }
      default: {
        int r = 0, k;
        if (!lisdigit(c)) lexerr(ls, "invalid escape sequence", NULL);
        r = c - '0';
        for (k = 1; k < 3 && i < n && lisdigit(cast_uchar(src[i])); k++)
          r = 10*r + (src[i++] - '0');
        if (r > UCHAR_MAX) lexerr(ls, "decimal escape too large", NULL);
        save(ls, r);
        break;
      }
    }
  }
  (void)line;
}


/*
** Read a string ('"' or '\'') or a native ('`') literal.
** A literal is delimited by an odd number of matching quotes:
** one quote delimits a single-line literal; three or more delimit a
** multi-line raw literal. Two quotes is the empty literal.
*/
static int read_quoted (LexState *ls, int del, SemInfo *seminfo) {
  int line = ls->linenumber;
  int sep = ls->sep;
  int n1 = 0;
  int multi;
  int isnat = (del == '`');
  while (ls->current == del) {
    n1++;
    next(ls);
  }
  luaZ_resetbuffer(ls->buff);
  if (n1 == 2) {  /* empty literal */
    multi = 0;
  }
  else if (n1 == 1) {  /* single-line literal */
    multi = 0;
    for (;;) {
      if (ls->current == del) break;
      if (currIsNewline(ls) || ls->current == EOZ) {
        ls->linenumber = line; ls->sep = sep;
        lexerr(ls, isnat ? "unterminated native" : "unterminated string",
               luaO_pushfstring(ls->L, "'%c'", del));
      }
      if (ls->current == '\\' && !isnat) {  /* keep escapes raw */
        save_and_next(ls);
        if (ls->current == EOZ || currIsNewline(ls)) continue;
      }
      save_and_next(ls);
    }
    next(ls);  /* skip closing delimiter */
  }
  else {  /* multi-line literal (raw) */
    multi = 1;
    for (;;) {
      int n2;
      while (ls->current != del) {
        if (ls->current == EOZ) {
          const char *msg = luaO_pushfstring(ls->L,
            "%s (starting at line %d)",
            isnat ? "unterminated native" : "unterminated string", line);
          lexerr(ls, msg, NULL);
        }
        if (currIsNewline(ls)) { save(ls, '\n'); inclinenumber(ls); }
        else save_and_next(ls);
      }
      n2 = 0;
      while (ls->current == del) { n2++; next(ls); }
      if (n2 == n1) break;
      while (n2-- > 0) save(ls, del);  /* shorter/longer runs are content */
    }
  }
  if (isnat) {
    seminfo->ts = bufftext(ls);
    return TK_NAT;
  }
  else {
    TString *raw = bufftext(ls);
    if (!multi) {  /* decode escapes (as Lua does) */
      size_t l = tsslen(raw);
      decode_escapes(ls, getstr(raw), l, line);
      seminfo->ts = bufftext(ls);
    }
    else
      seminfo->ts = raw;
    return TK_STRING;
  }
}

/* }====================================================== */


/*
** Main lexer function: returns the next token, filling 'seminfo'.
*/
int luaX_atmlex (LexState *ls, Token *t) {
  SemInfo *seminfo = &t->seminfo;
  t->text = NULL;
  luaZ_resetbuffer(ls->buff);
  for (;;) {
    ls->tokline = ls->linenumber;
    ls->toksep = ls->sep;
    switch (ls->current) {
      case '\n': case '\r': {  /* line breaks */
        inclinenumber(ls);
        break;
      }
      case ' ': case '\f': case '\t': case '\v': {  /* spaces */
        next(ls);
        break;
      }
      case ';': {  /* separator or comment */
        int n = read_semis(ls);
        if (n >= 2)
          read_comment(ls, n);
        break;
      }
      case '{': case '}': case '(': case ')': case '[': case ']':
      case ',': case '\\': case '@': {  /* single-char symbols */
        int c = ls->current;
        next(ls);
        return c;
      }
      case '.': {  /* '.' or '...' ('..' is two dots) */
        next(ls);
        if (ls->current == '.') {
          next(ls);
          if (ls->current == '.') {
            next(ls);
            return TK_DOTS;
          }
          unread(ls, '.');  /* second dot is a separate token */
          return '.';
        }
        return '.';
      }
      case ':': {  /* '::' or tag */
        next(ls);
        if (ls->current == ':') {
          next(ls);
          return TK_DBCOLON;
        }
        else {
          save(ls, ':');
          while (lislalnum(ls->current) || ls->current == '.')
            save_and_next(ls);
          seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + 1,
                                           luaZ_bufflen(ls->buff) - 1);
          t->text = bufftext(ls);  /* raw text (with ':') for messages */
          return TK_TAG;
        }
      }
      case '"': case '\'': case '`': {  /* strings and natives */
        int tk = read_quoted(ls, ls->current, seminfo);
        t->text = seminfo->ts;
        return tk;
      }
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9': {
        int tk = read_numeral(ls, seminfo);
        t->text = bufftext(ls);
        return tk;
      }
      case EOZ: {
        return TK_EOS;
      }
      default: {
        if (lislalpha(ls->current)) {  /* identifier or reserved word? */
          TString *ts;
          do {
            save_and_next(ls);
          } while (lislalnum(ls->current));
          ts = luaS_newlstr(ls->L, luaZ_buffer(ls->buff),
                                   luaZ_bufflen(ls->buff));
          if (isreserved(ts) &&
              luaX_isatmword(ts->extra - 1 + FIRST_RESERVED))
            return ts->extra - 1 + FIRST_RESERVED;
          else {
            seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff),
                                             luaZ_bufflen(ls->buff));
            t->text = seminfo->ts;
            return TK_NAME;
          }
        }
        else if (isopchar(ls->current)) {
          return read_operator(ls);
        }
        else {
          int c = ls->current;
          const char *near = lisprint(c)
                ? luaO_pushfstring(ls->L, "'%c'", c)
                : luaO_pushfstring(ls->L, "'<\\%d>'", c);
          lexerr(ls, "invalid character", near);
          return 0;  /* to avoid warnings */
        }
      }
    }
  }
}
