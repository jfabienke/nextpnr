#!/usr/bin/env python3
"""Join the oracle fit's locations (dump_loc.tcl) through cell_map.tsv into a --lab-hint file."""
import collections, re, sys
d = sys.argv[1]
names = {}
for line in open(f'{d}/cell_map.tsv'):
    short, orig, typ = line.rstrip('\n').split('\t')
    names[short] = (orig, typ)
hints, kinds, missing = {}, collections.Counter(), 0
for line in open(f'{d}/cell_locations.txt'):
    m = re.match(r'set_location_assignment (\w+?)_X(\d+)_Y(\d+)_N(\d+) -to (\S+)', line)
    if not m:
        continue
    kind, x, y, _, inst = m.groups()
    kinds[kind] += 1
    short = inst.split('~')[0].split('|')[-1]
    if kind in ('LABCELL', 'MLABCELL', 'FF') and short in names:
        hints[names[short][0]] = (x, y)
    elif kind in ('LABCELL', 'MLABCELL', 'FF'):
        missing += 1
with open(f'{d}/oracle.hints', 'w') as f:
    for orig, (x, y) in hints.items():
        f.write(f'{orig} {x} {y}\n')
labs = {v for v in hints.values()}
print(f'locations by kind {dict(kinds)}; hints {len(hints)} in {len(labs)} LABs; unmapped LAB cells {missing}')
