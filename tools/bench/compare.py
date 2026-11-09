#!/usr/bin/env python3
# compare.py A B N EVERY : compares the screenshots A-%05d.png and
# B-%05d.png every EVERY frames up to N; prints the differing frames
import sys
from PIL import Image, ImageChops
a, b, n, every = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
diff = []
for f in range(every, n + 1, every):
    x = Image.open('%s-%05d.png' % (a, f)).convert('RGB')
    y = Image.open('%s-%05d.png' % (b, f)).convert('RGB')
    if ImageChops.difference(x, y).getbbox():
        diff.append(f)
print('%d of %d frames differ%s' % (len(diff), n // every,
      (', first %d' % diff[0]) if diff else ''))
