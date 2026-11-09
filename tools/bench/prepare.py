#!/usr/bin/env python3
# prepare.py SRC DST : copies the games (SRC holds the clones
# atmos-lang/pico-rocks, atmos-lang/pico-birds, kboltiz/snkrx) to DST and
# rewrites what this implementation does not accept yet (see README.md):
# 'where', statements after '=>', bare 'break', 'return [..]',
# 'until (c, v)', 'spawn v->f(..)'; and fixes SNKRX's card order (a hash
# table iteration, which differs between Lua 5.4 and 5.5). Both
# implementations run the rewritten copies.
import glob, os, re, shutil, subprocess, sys

SRC, DST = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))

def copy(src, dst):
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns('.git'))

def edit(path, f):
    s = open(path).read()
    t = f(s)
    if t != s:
        open(path, 'w').write(t)

copy(os.path.join(SRC, 'atmos-lang/pico-rocks'), os.path.join(DST, 'pico-rocks'))
copy(os.path.join(SRC, 'atmos-lang/pico-birds'), os.path.join(DST, 'pico-birds'))
os.makedirs(os.path.join(DST, 'snkrx'), exist_ok=True)
copy(os.path.join(SRC, 'kboltiz/snkrx/atmos'), os.path.join(DST, 'snkrx/atmos'))
assets = os.path.join(DST, 'snkrx/assets')
if not os.path.exists(assets):
    os.symlink(os.path.abspath(os.path.join(SRC, 'kboltiz/snkrx/assets')), assets)

rocks = os.path.join(DST, 'pico-rocks')
birds = os.path.join(DST, 'pico-birds')
snk = os.path.join(DST, 'snkrx/atmos')
snkfiles = glob.glob(snk + '/*.atm') + glob.glob(snk + '/arena/*.atm')

# 'where'
subprocess.check_call([sys.executable, os.path.join(HERE, 'unwhere.py'),
                       rocks + '/ts.atm', birds + '/birds-11.atm'] + snkfiles)

# statements after '=>'
edit(rocks + '/ts.atm', lambda s: re.sub(
    r'(?m)^(\s*V\.ctl\.move\.[lrud] => )(set .*)$', r'\1{ \2 }', s))
for p in (snk + '/main.atm', snk + '/menu.atm'):
    edit(p, lambda s: re.sub(
        r'(?m)^(\s*:[a-z.]+\s*=> )(break\([^)]*\))\s*$', r'\1{ \2 }', s))

# bare 'break'
edit(birds + '/birds-11.atm', lambda s: re.sub(
    r'(?m)^(\s*)break\s*$', r'\1break()', s))

# 'return [ .. ]' -> 'return([ .. ])'
def wrapreturn(s):
    i = 0
    while True:
        m = re.compile(r'\breturn \[').search(s, i)
        if not m:
            return s
        j = m.end() - 1
        depth, k = 0, j
        while True:
            c = s[k]
            if c == '[':
                depth += 1
            elif c == ']':
                depth -= 1
                if depth == 0:
                    break
            elif c in '"\'':
                k += 1
                while s[k] != c:
                    k += 1
            elif s.startswith(';;', k):
                while s[k] != '\n':
                    k += 1
            k += 1
        s = s[:m.start()] + 'return(' + s[j:k + 1] + ')' + s[k + 1:]
        i = m.start() + 8

# 'until (c, v)' / 'while (c, v)' -> 'if c { break(v) }' / 'if !(c) ...'
def untilvalue(s):
    i = 0
    while True:
        m = re.compile(r'\b(until|while) \(').search(s, i)
        if not m:
            return s
        j = m.end() - 1
        depth, k, comma = 0, j, None
        while True:
            c = s[k]
            if c in '([':
                depth += 1
            elif c in ')]':
                depth -= 1
                if depth == 0:
                    break
            elif c == ',' and depth == 1 and comma is None:
                comma = k
            k += 1
        if comma is None:
            i = k
            continue
        cnd, val = s[j + 1:comma].strip(), s[comma + 1:k].strip()
        if m.group(1) == 'until':
            rep = 'if %s { break(%s) }' % (cnd, val)
        else:
            rep = 'if !(%s) { break(%s) }' % (cnd, val)
        s = s[:m.start()] + rep + s[k + 1:]
        i = m.start() + len(rep)

for p in snkfiles:
    edit(p, wrapreturn)
    edit(p, untilvalue)
    edit(p, lambda s: s.replace('spawn task->Char_Icon(', 'spawn Char_Icon(task, '))

# SNKRX: card order independent of the hash tables
def sortchars(s):
    old = """            set out@+ = name
        }
    }
    return(out)"""
    assert old in s
    return s.replace(old, """            set out@+ = name
        }
    }
    table.sort(out)  ;; (benchmark: an order independent of the hash tables)
    return(out)""")
edit(snk + '/arena/shop.atm', sortchars)
print('prepared', DST)
