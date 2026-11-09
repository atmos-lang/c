#!/usr/bin/env python3
# Rewrites 'STMT where { a = e1  b = e2 }' into
#   val w_a_N = e1
#   val w_b_N = e2
#   STMT   (with a, b renamed; table keys and fields untouched)
# for benchmarking implementations without 'where'. Usage: unwhere.py FILE...
import re, sys

N = [0]

def ind(line):
    return len(line) - len(line.lstrip(' '))

def ren(text, names):
    for old, new in names.items():
        # not a field (.x, :x, @x), not a table key (x = ...), whole word
        text = re.sub(r'(?<![\w.:@])' + re.escape(old) + r'(?![\w])(?!\s*=(?!=))',
                      new, text)
    return text

def rewrite(lines):
    out = list(lines)
    i = 0
    while i < len(out):
        line = out[i]
        m = re.search(r'\s*where\s*\{\s*$', line)
        if not m or line.strip().startswith(';;'):
            i += 1
            continue
        I = ind(line)
        # statement start: this line if it has more than a closing bracket,
        # else the previous line with the same indentation
        head = line[:m.start()]
        if head.strip() in ('}', ']', ')'):
            s = i - 1
            while ind(out[s]) != I or out[s].strip() == '':
                s -= 1
        else:
            s = i
        # bindings until the closing '}' at indentation I
        j = i + 1
        binds = []
        while not (out[j].strip() == '}' and ind(out[j]) == I):
            bm = re.match(r'^\s*(\w+)\s*=\s*(.*)$', out[j])
            if bm and (not binds or ind(out[j]) <= binds[-1][2]):
                binds.append([bm.group(1), bm.group(2), ind(out[j])])
            else:
                binds[-1][1] += '\n' + out[j]
            j += 1
        N[0] += 1
        names = {b[0]: 'w_%s_%d' % (b[0], N[0]) for b in binds}
        pre = []
        for b in binds:
            pre.append(' ' * I + 'val %s = %s' % (names[b[0]], ren(b[1], names)))
        stmt = out[s:i] + [head]
        stmt = [ren(l, names) for l in stmt]
        out[s:j + 1] = pre + stmt
        i = s + len(pre) + len(stmt)
    return out

for path in sys.argv[1:]:
    src = open(path).read().split('\n')
    new = rewrite(src)
    if new != src:
        open(path, 'w').write('\n'.join(new))
        print('rewrote', path)
