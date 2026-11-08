/*
** $Id: llex.c $
** Lexical Analyzer (Atmos)
** See Copyright Notice in lua.h
*/

#define llex_c
#define LUA_CORE

#include "lprefix.h"


#include <locale.h>
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



#define next(ls)	(ls->current = zgetc(ls->z))


/* minimum size for string buffer */
#if !defined(LUA_MINBUFFER)
#define LUA_MINBUFFER   32
#endif


#define currIsNewline(ls)	(ls->current == '\n' || ls->current == '\r')


/* ORDER RESERVED */
static const char *const luaX_tokens [] = {
    "await", "break", "catch", "defer", "do",
    "else", "emit", "escape", "false", "func", "if", "ifs", "in",
    "loop", "match", "nil", "on", "par", "pin", "pub", "return",
    "set", "spawn", "task", "tasks", "test", "thread",
    "toggle", "true", "until", "val", "var", "watching", "where",
    "while", "with",
    "//", "**", "++", "...", "==", "!=", ">=", "<=",
    "&&", "||", "!", "??", "!?", "?>", "!>",
    "===", "=!=", "=>=", "=<=", "=>",
    "<-", "<--", "->", "-->", "::",
    "<eof>",
    "<number>", "<integer>", "<name>", "<string>", "<tag>", "<clock>"
};


#define save_and_next(ls) (save(ls, ls->current), next(ls))


static l_noret lexerror (LexState *ls, const char *msg, int token);


static void save (LexState *ls, int c) {
  Mbuffer *b = ls->buff;
  if (luaZ_bufflen(b) + 1 > luaZ_sizebuffer(b)) {
    size_t newsize = luaZ_sizebuffer(b);  /* get old size */;
    if (newsize >= (MAX_SIZE/3 * 2))  /* larger than MAX_SIZE/1.5 ? */
      lexerror(ls, "lexical element too long", 0);
    newsize += (newsize >> 1);  /* new size is 1.5 times the old one */
    luaZ_resizebuffer(ls->L, b, newsize);
  }
  b->buffer[luaZ_bufflen(b)++] = cast_char(c);
}


void luaX_init (lua_State *L) {
  int i;
  TString *e = luaS_newliteral(L, LUA_ENV);  /* create env name */
  luaC_fix(L, obj2gco(e));  /* never collect this name */
  for (i=0; i<NUM_RESERVED; i++) {
    TString *ts = luaS_new(L, luaX_tokens[i]);
    luaC_fix(L, obj2gco(ts));  /* reserved words are never collected */
    ts->extra = cast_byte(i+1);  /* reserved word */
  }
}


const char *luaX_token2str (LexState *ls, int token) {
  if (token < FIRST_RESERVED) {  /* single-byte symbols? */
    if (lisprint(token))
      return luaO_pushfstring(ls->L, "'%c'", token);
    else  /* control character */
      return luaO_pushfstring(ls->L, "'<\\%d>'", token);
  }
  else {
    const char *s = luaX_tokens[token - FIRST_RESERVED];
    if (token < TK_EOS)  /* fixed format (symbols and reserved words)? */
      return luaO_pushfstring(ls->L, "'%s'", s);
    else  /* names, strings, and numerals */
      return s;
  }
}


/*
** Name of a reserved word (as a string); used for field names
** that are keywords ('t.set').
*/
TString *luaX_tokenname (LexState *ls, int token) {
  lua_assert(token >= FIRST_RESERVED && token < TK_IDIV);
  return luaX_newstring(ls, luaX_tokens[token - FIRST_RESERVED],
                            strlen(luaX_tokens[token - FIRST_RESERVED]));
}


static const char *txtToken (LexState *ls, int token) {
  switch (token) {
    case TK_NAME: case TK_STRING:
    case TK_FLT: case TK_INT: case TK_TAG: case TK_CLK:
      save(ls, '\0');
      return luaO_pushfstring(ls->L, "'%s'", luaZ_buffer(ls->buff));
    default:
      return luaX_token2str(ls, token);
  }
}


static l_noret lexerror (LexState *ls, const char *msg, int token) {
  msg = luaG_addinfo(ls->L, msg, ls->source, ls->linenumber);
  if (token)
    luaO_pushfstring(ls->L, "%s near %s", msg, txtToken(ls, token));
  luaD_throw(ls->L, LUA_ERRSYNTAX);
}


l_noret luaX_syntaxerror (LexState *ls, const char *msg) {
  lexerror(ls, msg, ls->t.token);
}


/*
** Anchors a string in scanner's table so that it will not be collected
** until the end of the compilation; by that time it should be anchored
** somewhere. It also internalizes long strings, ensuring there is only
** one copy of each unique string.
*/
static TString *anchorstr (LexState *ls, TString *ts) {
  lua_State *L = ls->L;
  TValue oldts;
  int tag = luaH_getstr(ls->h, ts, &oldts);
  if (!tagisempty(tag))  /* string already present? */
    return tsvalue(&oldts);  /* use stored value */
  else {  /* create a new entry */
    TValue *stv = s2v(L->top.p++);  /* reserve stack space for string */
    setsvalue(L, stv, ts);  /* push (anchor) the string on the stack */
    luaH_set(L, ls->h, stv, stv);  /* t[string] = string */
    /* table is not a metatable, so it does not need to invalidate cache */
    luaC_checkGC(L);
    L->top.p--;  /* remove string from stack */
    return ts;
  }
}


/*
** Creates a new string and anchors it in scanner's table.
*/
TString *luaX_newstring (LexState *ls, const char *str, size_t l) {
  return anchorstr(ls, luaS_newlstr(ls->L, str, l));
}


/*
** increment line number and skips newline sequence (any of
** \n, \r, \n\r, or \r\n)
*/
static void inclinenumber (LexState *ls) {
  int old = ls->current;
  lua_assert(currIsNewline(ls));
  next(ls);  /* skip '\n' or '\r' */
  if (currIsNewline(ls) && ls->current != old)
    next(ls);  /* skip '\n\r' or '\r\n' */
  if (++ls->linenumber >= INT_MAX)
    lexerror(ls, "chunk has too many lines", 0);
  ls->sep++;  /* newline is a statement separator */
}


void luaX_setinput (lua_State *L, LexState *ls, ZIO *z, TString *source,
                    int firstchar) {
  ls->t.token = 0;
  ls->t.sep = 0;
  ls->L = L;
  ls->current = firstchar;
  ls->lookahead.token = TK_EOS;  /* no look-ahead token */
  ls->z = z;
  ls->fs = NULL;
  ls->linenumber = 1;
  ls->lastline = 1;
  ls->sep = 0;
  ls->lastsep = -1;
  ls->source = source;
  /* both strings here ("_ENV", "break") were fixed,
     so they cannot be collected */
  ls->envn = luaS_newliteral(L, LUA_ENV);  /* get env string */
  ls->brkn = luaS_newliteral(L, "break");  /* get "break" string */
  luaZ_resizebuffer(ls->L, ls->buff, LUA_MINBUFFER);  /* initialize buffer */
}



/*
** =======================================================
** LEXICAL ANALYZER
** =======================================================
*/


static int check_next1 (LexState *ls, int c) {
  if (ls->current == c) {
    next(ls);
    return 1;
  }
  else return 0;
}


/*
** Check whether current char is in set 'set' (with two chars) and
** saves it
*/
static int check_next2 (LexState *ls, const char *set) {
  lua_assert(set[2] == '\0');
  if (ls->current == set[0] || ls->current == set[1]) {
    save_and_next(ls);
    return 1;
  }
  else return 0;
}


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


/* LUA_NUMBER */
/*
** This function is quite liberal in what it accepts, as 'luaO_str2num'
** will reject ill-formed numerals. Roughly, it accepts the following
** pattern:
**
**   %d(%x|%.|([Ee][+-]?))* | 0[Xx](%x|%.|([Pp][+-]?))*
**
** The only tricky part is to accept [+-] only after a valid exponent
** mark, to avoid reading '3-4' or '0xe+1' as a single number.
** A numeral touching letters may be a clock duration ('1h30min').
*/
static int read_numeral (LexState *ls, SemInfo *seminfo) {
  TValue obj;
  const char *expo = "Ee";
  int first = ls->current;
  lua_assert(lisdigit(ls->current));
  save_and_next(ls);
  if (first == '0' && check_next2(ls, "xX"))  /* hexadecimal? */
    expo = "Pp";
  for (;;) {
    if (check_next2(ls, expo))  /* exponent mark? */
      check_next2(ls, "-+");  /* optional exponent sign */
    else if (lisxdigit(ls->current) || ls->current == '.')  /* '%x|%.' */
      save_and_next(ls);
    else break;
  }
  if (lislalpha(ls->current)) {  /* is numeral touching a letter? */
    do {  /* read the rest (clock duration or error) */
      save_and_next(ls);
    } while (lislalnum(ls->current) || ls->current == '.');
  }
  save(ls, '\0');
  if (luaO_str2num(luaZ_buffer(ls->buff), &obj) == 0) {  /* format error? */
    int isflt;
    if (read_clock(ls, seminfo, &isflt)) {  /* clock duration? */
      luaZ_buffremove(ls->buff, 1);  /* remove '\0' */
      return TK_CLK;
    }
    luaZ_buffremove(ls->buff, 1);  /* remove '\0' */
    lexerror(ls, "malformed number", TK_FLT);
  }
  luaZ_buffremove(ls->buff, 1);  /* remove '\0' */
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

/* }====================================================== */


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
    ls->sep++;  /* ';' is a statement separator */
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
        if (top >= 64) lexerror(ls, "comment nesting too deep", 0);
        stk[top++] = s;
      }
      do {
        while (ls->current != ';') {
          if (ls->current == EOZ) {
            const char *msg = luaO_pushfstring(ls->L,
              "unfinished long comment (starting at line %d)", line);
            lexerror(ls, msg, TK_EOS);
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
** Strings
** =======================================================
*/

static void esccheck (LexState *ls, int c, const char *msg) {
  if (!c) {
    if (ls->current != EOZ)
      save_and_next(ls);  /* add current to buffer for error message */
    lexerror(ls, msg, TK_STRING);
  }
}


static int gethexa (LexState *ls) {
  save_and_next(ls);
  esccheck (ls, lisxdigit(ls->current), "hexadecimal digit expected");
  return luaO_hexavalue(ls->current);
}


static int readhexaesc (LexState *ls) {
  int r = gethexa(ls);
  r = (r << 4) + gethexa(ls);
  luaZ_buffremove(ls->buff, 2);  /* remove saved chars from buffer */
  return r;
}


/*
** When reading a UTF-8 escape sequence, save everything to the buffer
** for error reporting in case of errors; 'i' counts the number of
** saved characters, so that they can be removed if case of success.
*/
static l_uint32 readutf8esc (LexState *ls) {
  l_uint32 r;
  int i = 4;  /* number of chars to be removed: start with #"\u{X" */
  save_and_next(ls);  /* skip 'u' */
  esccheck(ls, ls->current == '{', "missing '{'");
  r = cast_uint(gethexa(ls));  /* must have at least one digit */
  while (cast_void(save_and_next(ls)), lisxdigit(ls->current)) {
    i++;
    esccheck(ls, r <= (0x7FFFFFFFu >> 4), "UTF-8 value too large");
    r = (r << 4) + luaO_hexavalue(ls->current);
  }
  esccheck(ls, ls->current == '}', "missing '}'");
  next(ls);  /* skip '}' */
  luaZ_buffremove(ls->buff, i);  /* remove saved chars from buffer */
  return r;
}


static void utf8esc (LexState *ls) {
  char buff[UTF8BUFFSZ];
  int n = luaO_utf8esc(buff, readutf8esc(ls));
  for (; n > 0; n--)  /* add 'buff' to string */
    save(ls, buff[UTF8BUFFSZ - n]);
}


static int readdecesc (LexState *ls) {
  int i;
  int r = 0;  /* result accumulator */
  for (i = 0; i < 3 && lisdigit(ls->current); i++) {  /* read up to 3 digits */
    r = 10*r + ls->current - '0';
    save_and_next(ls);
  }
  esccheck(ls, r <= UCHAR_MAX, "decimal escape too large");
  luaZ_buffremove(ls->buff, i);  /* remove read digits from buffer */
  return r;
}


/*
** Single-line string, with escape sequences (as in Lua). The opening
** delimiter was already saved in the buffer.
*/
static void read_string (LexState *ls, int del, SemInfo *seminfo) {
  while (ls->current != del) {
    switch (ls->current) {
      case EOZ:
        lexerror(ls, "unfinished string", TK_EOS);
        break;  /* to avoid warnings */
      case '\n':
      case '\r':
        lexerror(ls, "unfinished string", TK_STRING);
        break;  /* to avoid warnings */
      case '\\': {  /* escape sequences */
        int c;  /* final character to be saved */
        save_and_next(ls);  /* keep '\\' for error messages */
        switch (ls->current) {
          case 'a': c = '\a'; goto read_save;
          case 'b': c = '\b'; goto read_save;
          case 'f': c = '\f'; goto read_save;
          case 'n': c = '\n'; goto read_save;
          case 'r': c = '\r'; goto read_save;
          case 't': c = '\t'; goto read_save;
          case 'v': c = '\v'; goto read_save;
          case 'x': c = readhexaesc(ls); goto read_save;
          case 'u': utf8esc(ls);  goto no_save;
          case '\n': case '\r':
            inclinenumber(ls); c = '\n'; goto only_save;
          case '\\': case '\"': case '\'':
            c = ls->current; goto read_save;
          case EOZ: goto no_save;  /* will raise an error next loop */
          case 'z': {  /* zap following span of spaces */
            luaZ_buffremove(ls->buff, 1);  /* remove '\\' */
            next(ls);  /* skip the 'z' */
            while (lisspace(ls->current)) {
              if (currIsNewline(ls)) inclinenumber(ls);
              else next(ls);
            }
            goto no_save;
          }
          default: {
            esccheck(ls, lisdigit(ls->current), "invalid escape sequence");
            c = readdecesc(ls);  /* digital escape '\ddd' */
            goto only_save;
          }
        }
       read_save:
         next(ls);
         /* go through */
       only_save:
         luaZ_buffremove(ls->buff, 1);  /* remove '\\' */
         save(ls, c);
         /* go through */
       no_save: break;
      }
      default:
        save_and_next(ls);
    }
  }
  save_and_next(ls);  /* skip delimiter */
  seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + 1,
                                   luaZ_bufflen(ls->buff) - 2);
}


/*
** Multi-line raw string, delimited by three quotes ('"""' or "'''").
** It has no escape sequences and ends at the first run of three or
** more matching quotes (the extra quotes belong to the content); a
** newline right after the opening delimiter is skipped. The three
** opening quotes were already read.
*/
static void read_raw_string (LexState *ls, int del, SemInfo *seminfo) {
  int line = ls->linenumber;  /* initial line (for error message) */
  luaZ_resetbuffer(ls->buff);
  if (currIsNewline(ls))  /* string starts with a newline? */
    inclinenumber(ls);  /* skip it */
  for (;;) {
    switch (ls->current) {
      case EOZ: {  /* error */
        const char *msg = luaO_pushfstring(ls->L,
                     "unfinished string (starting at line %d)", line);
        lexerror(ls, msg, TK_EOS);
        break;  /* to avoid warnings */
      }
      case '\n': case '\r': {
        save(ls, '\n');
        inclinenumber(ls);
        break;
      }
      default: {
        if (ls->current == del) {
          int n = 0;
          while (ls->current == del) { n++; next(ls); }
          if (n >= 3) {  /* closing delimiter (extra quotes are content) */
            while (n-- > 3) save(ls, del);
            goto endloop;
          }
          while (n-- > 0) save(ls, del);  /* shorter runs are content */
        }
        else save_and_next(ls);
      }
    }
  } endloop:
  seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff),
                                   luaZ_bufflen(ls->buff));
}


/*
** String literal: 'x' | "x" | '' | """..."""
*/
static int read_quoted (LexState *ls, int del, SemInfo *seminfo) {
  save_and_next(ls);  /* keep first delimiter (for error messages) */
  if (ls->current != del) {  /* single-line string? */
    read_string(ls, del, seminfo);
    return TK_STRING;
  }
  next(ls);  /* second delimiter */
  if (ls->current != del) {  /* empty string ('' or "")? */
    seminfo->ts = luaX_newstring(ls, "", 0);
    return TK_STRING;
  }
  next(ls);  /* third delimiter: raw multi-line string */
  read_raw_string(ls, del, seminfo);
  return TK_STRING;
}

/* }====================================================== */


static l_noret invalidop (LexState *ls) {
  save(ls, '\0');
  lexerror(ls, "invalid operator", 0);
}


static int llex (LexState *ls, SemInfo *seminfo) {
  luaZ_resetbuffer(ls->buff);
  for (;;) {
    ls->tokensep = ls->sep;  /* (a multi-line token spans separators) */
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
      case '=': {
        next(ls);
        if (check_next1(ls, '=')) {
          if (check_next1(ls, '=')) return TK_SEQ;  /* '===' */
          return TK_EQ;  /* '==' */
        }
        else if (check_next1(ls, '>')) {
          if (check_next1(ls, '=')) return TK_SGE;  /* '=>=' */
          return TK_ARROW;  /* '=>' */
        }
        else if (ls->current == '<') {
          next(ls);
          if (check_next1(ls, '=')) return TK_SLE;  /* '=<=' */
          save(ls, '='); save(ls, '<'); invalidop(ls);
        }
        else if (ls->current == '!') {
          next(ls);
          if (check_next1(ls, '=')) return TK_SNE;  /* '=!=' */
          save(ls, '='); save(ls, '!'); invalidop(ls);
        }
        else return '=';
      }
      case '!': {
        next(ls);
        if (check_next1(ls, '=')) return TK_NE;  /* '!=' */
        else if (check_next1(ls, '?')) return TK_ISNOT;  /* '!?' */
        else if (check_next1(ls, '>')) return TK_XNOTIN;  /* '!>' */
        else return TK_NOT;  /* '!' */
      }
      case '<': {
        next(ls);
        if (check_next1(ls, '=')) return TK_LE;  /* '<=' */
        else if (check_next1(ls, '-')) {
          if (check_next1(ls, '-')) return TK_LPIPE2;  /* '<--' */
          return TK_LPIPE1;  /* '<-' */
        }
        else return '<';
      }
      case '>': {
        next(ls);
        if (check_next1(ls, '=')) return TK_GE;  /* '>=' */
        else return '>';
      }
      case '-': {
        next(ls);
        if (check_next1(ls, '>')) return TK_RPIPE1;  /* '->' */
        else if (ls->current == '-') {
          next(ls);
          if (check_next1(ls, '>')) return TK_RPIPE2;  /* '-->' */
          save(ls, '-'); save(ls, '-'); invalidop(ls);
        }
        else return '-';
      }
      case '+': {
        next(ls);
        if (check_next1(ls, '+')) return TK_CAT;  /* '++' */
        else return '+';
      }
      case '*': {
        next(ls);
        if (check_next1(ls, '*')) return TK_POW;  /* '**' */
        else return '*';
      }
      case '/': {
        next(ls);
        if (check_next1(ls, '/')) return TK_IDIV;  /* '//' */
        else return '/';
      }
      case '&': {
        next(ls);
        if (check_next1(ls, '&')) return TK_AND;  /* '&&' */
        save(ls, '&'); invalidop(ls);
      }
      case '|': {
        next(ls);
        if (check_next1(ls, '|')) return TK_OR;  /* '||' */
        save(ls, '|'); invalidop(ls);
      }
      case '?': {
        next(ls);
        if (check_next1(ls, '?')) return TK_IS;  /* '??' */
        else if (check_next1(ls, '>')) return TK_XIN;  /* '?>' */
        save(ls, '?'); invalidop(ls);
      }
      case ':': {
        next(ls);
        if (check_next1(ls, ':')) return TK_DBCOLON;  /* '::' */
        else if (lislalnum(ls->current)) {  /* tag */
          do {
            save_and_next(ls);
          } while (lislalnum(ls->current) || ls->current == '.');
          seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff),
                                           luaZ_bufflen(ls->buff));
          return TK_TAG;
        }
        else return ':';
      }
      case '"': case '\'': {  /* strings */
        return read_quoted(ls, ls->current, seminfo);
      }
      case '.': {  /* '.' or '...' */
        next(ls);
        if (ls->current == '.') {
          next(ls);
          if (check_next1(ls, '.'))
            return TK_DOTS;   /* '...' */
          save(ls, '.'); save(ls, '.'); invalidop(ls);
        }
        else return '.';
      }
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9': {
        return read_numeral(ls, seminfo);
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
          /* find or create string */
          ts = luaS_newlstr(ls->L, luaZ_buffer(ls->buff),
                                   luaZ_bufflen(ls->buff));
          if (isreserved(ts))   /* reserved word? */
            return ts->extra - 1 + FIRST_RESERVED;
          else {
            seminfo->ts = anchorstr(ls, ts);
            return TK_NAME;
          }
        }
        else {  /* single-char tokens ('{', '}', '(', ')', '[', ']', ...) */
          int c = ls->current;
          next(ls);
          return c;
        }
      }
    }
  }
}


void luaX_next (LexState *ls) {
  ls->lastline = ls->linenumber;
  ls->lastsep = ls->t.sep;
  if (ls->lookahead.token != TK_EOS) {  /* is there a look-ahead token? */
    ls->t = ls->lookahead;  /* use this one */
    ls->lookahead.token = TK_EOS;  /* and discharge it */
  }
  else {
    ls->t.token = llex(ls, &ls->t.seminfo);  /* read next token */
    ls->t.sep = ls->tokensep;
  }
}


int luaX_lookahead (LexState *ls) {
  lua_assert(ls->lookahead.token == TK_EOS);
  ls->lookahead.token = llex(ls, &ls->lookahead.seminfo);
  ls->lookahead.sep = ls->tokensep;
  return ls->lookahead.token;
}
