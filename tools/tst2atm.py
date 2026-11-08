#!/usr/bin/env python3
"""Convert reference test cases (atmos-lang/atmos tst/*.lua) to Atmos.

The reference tests are Atmos sources inside Lua strings, run through
`atm_test(src)` and compared with `assertx`/`assertfx`/`assert`:

    local src = [[ ... ]]
    print("Testing...", "name")
    local out = atm_test(src)
    assertx(out, "expected\n")

This script extracts (name, src, expectation) triples and writes an
Atmos test file made of calls to a harness (see testes/tasks1.atm):

    T("name", <src as a raw string>, "expected\n")  ;; exact output
    T("name", <src>, TX("expected"))   ;; trimmed output
    T("name", <src>, P("pattern"))     ;; Lua pattern (assertfx)
    T("name", <src>, F("text"))        ;; substring (string.find)
    T("name", <src>, E("pre", "msg"))  ;; error 'msg' after output 'pre'
    T("name", <src>, EP("pre", "pat")) ;; error matching pattern

Usage: tst2atm.py [--commented] [--enabled NAMESFILE] FILE.lua [FIRST [LAST]]
With --commented every case is emitted as a ';; ' comment block, to be
uncommented when it is expected to pass; with --enabled, only the cases
whose names are listed in NAMESFILE (one per line) are emitted active.
Unrecognized shapes are emitted as ';; TODO(tst2atm)' comments.
"""
import re
import sys


def lua_string(text, pos):
    """Parse a Lua string literal starting at text[pos]; returns
    (value, end) or None."""
    c = text[pos]
    if c in '"\'':
        i = pos + 1
        out = []
        while i < len(text) and text[i] != c:
            if text[i] == '\\':
                nxt = text[i + 1]
                esc = {'n': '\n', 't': '\t', '\\': '\\', '"': '"',
                       "'": "'", 'a': '\a', 'r': '\r', '0': '\0'}
                if nxt in esc:
                    out.append(esc[nxt])
                    i += 2
                    continue
                if nxt.isdigit():
                    j = i + 1
                    while j < i + 4 and text[j].isdigit():
                        j += 1
                    out.append(chr(int(text[i + 1:j])))
                    i = j
                    continue
                out.append(nxt)
                i += 2
                continue
            out.append(text[i])
            i += 1
        return ''.join(out), i + 1
    m = re.match(r'\[(=*)\[', text[pos:])
    if m:
        eq = m.group(1)
        start = pos + len(m.group(0))
        if text[start] == '\n':
            start += 1
        end = text.find(']' + eq + ']', start)
        return text[start:end], end + len(eq) + 2
    return None


def trim(s):
    s = re.sub(r'^\s*', '', s)
    s = re.sub(r'\n\s*', '\n', s)
    s = re.sub(r'\s*$', '', s)
    return s


def atm_string(s):
    """Atmos string literal for s: a raw string when possible (the
    newline after the opening delimiter is skipped, so one is added)."""
    if '\n' in s or '"' in s:
        if '"""' not in s and not s.endswith('"'):
            return '"""\n' + s + '"""'
        if "'''" not in s and not s.endswith("'"):
            return "'''\n" + s + "'''"
    out = s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
    out = out.replace('\t', '\\t')
    return '"' + out + '"'


def expectation(text, pos):
    """Parse the assertion after atm_test; returns (kind, value, end)."""
    while True:
        m = re.compile(r'[ \t]*(--[^\n]*\n|\n)').match(text, pos)
        if not m:
            break
        pos = m.end()
    m = re.compile(r'[ \t]*(assertx|assertfx|assert)\s*\(').match(text, pos)
    if not m:
        return None
    fn = m.group(1)
    p = m.end()
    rest = text[p:]
    if fn == 'assert':
        m2 = re.match(r'\s*string\.find\s*\(\s*out\s*,\s*', rest)
        if m2:
            s, e = lua_string(text, p + m2.end())
            return ('F', s, text.find('\n', e))
        m2 = re.match(r'\s*out\s*==\s*', rest)
        if m2:
            s, e = lua_string(text, p + m2.end())
            return ('X', s, text.find('\n', e))
        return None
    m2 = re.match(r'\s*(trim\s*\(\s*out\s*\)|out)\s*,\s*(trim\s*)?', rest)
    if not m2:
        return None
    trimmed = m2.group(1).startswith('trim')
    s, e = lua_string(text, p + m2.end())
    if trimmed:
        s = trim(s)
    kind = 'P' if fn == 'assertfx' else 'X'
    if '==> ERROR:' in s:
        pre = s[:s.index('==> ERROR:')].strip()
        last = s.strip().split('\n')[-1]
        assert last.startswith('==> ')
        return ('EP' if kind == 'P' else 'E', (pre, last[4:]),
                text.find('\n', e))
    if trimmed:
        kind = 'TP' if kind == 'P' else 'TX'
    return (kind, s, text.find('\n', e))


def comment(block):
    return '\n'.join(';; ' + l if l else ';;' for l in block.split('\n'))


def convert(text, first, last, commented=False, enabled=None):
    lines = text.split('\n')
    sub = '\n'.join(lines[first - 1:last])
    base = sum(len(l) + 1 for l in lines[:first - 1])
    out = []
    pos = 0
    rx = re.compile(r'(?m)^(?P<sec>\s*print\s*[\'"]--- (?P<name>.*?) ---[\'"])|'
                    r'(?P<case>^[ \t]*local src = )')
    while True:
        m = rx.search(sub, pos)
        if not m:
            break
        if m.group('sec'):
            out.append('\n;; ---- ' + m.group('name') + '\n')
            pos = m.end()
            continue
        p = m.end()
        src, e = lua_string(sub, p)
        pos = e
        mt = re.compile(r'\s*print\s*\(\s*"Testing\.\.\."\s*,\s*').match(sub, e)
        if not mt:
            continue  # parser-only or other test
        r = lua_string(sub, mt.end())
        if r is None:  # print("Testing...", src): name is the source itself
            m3 = re.compile(r'[A-Za-z_][A-Za-z_0-9]*').match(sub, mt.end())
            name = src.strip().split('\n')[0][:60]
            e2 = m3.end()
        else:
            name, e2 = r
        ma = re.compile(r'\s*\)\s*\n\s*local out = atm_test\(src(, true)?\)\s*\n').match(sub, e2)
        if not ma:
            out.append(';; TODO(tst2atm): unrecognized case ' + repr(name))
            continue
        exp = expectation(sub, ma.end())
        lineno = first + sub.count('\n', 0, p)
        if exp is None:
            out.append(';; TODO(tst2atm): unrecognized expectation for %r (line %d)'
                       % (name, lineno))
            continue
        kind, val, e3 = exp
        pos = e3
        body = atm_string(src)
        if kind == 'X':
            ev = atm_string(val)
        elif kind in ('E', 'EP'):
            ev = '%s(%s, %s)' % (kind, atm_string(val[0]), atm_string(val[1]))
        else:
            ev = '%s(%s)' % (kind, atm_string(val))
        case = 'T(%s, %s,\n  %s)' % (atm_string(name), body, ev)
        active = not commented if enabled is None else (name in enabled)
        out.append(';; line %d' % lineno)
        out.append(case if active else comment(case))
    return '\n'.join(out)


def main():
    args = sys.argv[1:]
    commented = False
    enabled = None
    while args and args[0].startswith('--'):
        if args[0] == '--commented':
            commented = True
            args = args[1:]
        elif args[0] == '--enabled':
            enabled = set(l.rstrip('\n') for l in open(args[1]) if l.strip())
            args = args[2:]
        else:
            raise SystemExit('unknown option ' + args[0])
    path = args[0]
    text = open(path).read()
    n = text.count('\n') + 1
    first = int(args[1]) if len(args) > 1 else 1
    last = int(args[2]) if len(args) > 2 else n
    base = path.split('/')[-1]
    sys.stdout.write(';; Generated by tools/tst2atm.py from the reference tests\n'
                     ';; (atmos-lang/atmos tst/%s, copied as is in testes/ref/tst/).\n'
                     ';; Commented cases are not expected to pass yet.\n\n'
                     'dofile("ref/harness.atm")\n'
                     'harness_begin(%s)\n' % (base, atm_string(base)))
    sys.stdout.write(convert(text, first, last, commented, enabled))
    sys.stdout.write('\n\nharness_end()\n')


if __name__ == '__main__':
    main()
