#!/usr/bin/env python3
"""Design 20.0: check Quartus's oracle placement, LAB by LAB, against nextpnr's LAB rules and count why it refuses.

usage: rules_gap.py NETLIST.json ORACLE_DIR

Quartus location N maps to nextpnr exactly (verified by ff2test and en3test): ALM = N // 6, LUT at N % 6 in {0, 3}
(half 0, 1), registers at N % 6 in {1, 2} (half 0, register 0 and 1) and {4, 5} (half 1). Rules as in mistral/lab.cc
(alm_legal_with, alm_input_count_with, check_lab_input_count) and the control model (mistral/lab_model.cc), with the
clock global (--lab-global-clocks).
"""
import collections
import json
import re
import sys

netlist, d = sys.argv[1], sys.argv[2]
mod = max((m for m in json.load(open(netlist))['modules'].values() if 'cells' in m), key=lambda m: len(m['cells']))
cells = mod['cells']
short = {}
for line in open(f'{d}/cell_map.tsv'):
    s, orig, _ = line.rstrip('\n').split('\t')
    short[s] = orig

LUT_PINS = {'MISTRAL_ALUT6': 'ABCDEF', 'MISTRAL_ALUT5': 'ABCDE', 'MISTRAL_ALUT4': 'ABCD', 'MISTRAL_ALUT3': 'ABC',
            'MISTRAL_ALUT2': 'AB', 'MISTRAL_NOT': 'A', 'MISTRAL_BUF': 'A'}
ARITH_PINS = ['A', 'B', 'C', 'D0', 'D1']


def net(c, port):
    b = cells[c]['connections'].get(port, [None])[0]
    return b if isinstance(b, int) else None


def lut_inputs(c):
    t = cells[c]['type']
    pins = ARITH_PINS if t == 'MISTRAL_ALUT_ARITH' else list(LUT_PINS.get(t, ''))
    return [n for n in (net(c, p) for p in pins) if n is not None]


def lut_out(c):
    t = cells[c]['type']
    return net(c, 'SO' if t == 'MISTRAL_ALUT_ARITH' else 'Q')


def lut_bits(c):
    t = cells[c]['type']
    return 32 if t == 'MISTRAL_ALUT_ARITH' else 2 ** len(LUT_PINS.get(t, 'A'))


# LAB -> ALM -> {'lut': [c0, c1], 'ff': [[r00, r01], [r10, r11]]}
labs = collections.defaultdict(lambda: collections.defaultdict(lambda: {'lut': [None, None], 'ff': [[None, None], [None, None]]}))
for line in open(f'{d}/cell_locations.txt'):
    m = re.match(r'set_location_assignment (\w+?)_X(\d+)_Y(\d+)_N(\d+) -to (\S+)', line)
    if not m or m.group(1) not in ('LABCELL', 'MLABCELL', 'FF'):
        continue
    kind, x, y, n, inst = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5).split('~')[0]
    c = short.get(inst)
    if c is None or c not in cells:
        continue
    alm, r = n // 6, n % 6
    a = labs[(x, y)][alm]
    if kind == 'FF':
        half, j = (0, r - 1) if r in (1, 2) else (1, r - 4)
        a['ff'][half][j] = c
    else:
        a['lut'][0 if r == 0 else 1] = c

reasons = collections.Counter()   # LABs failing each rule
cells_in = collections.Counter()  # cells in LABs failing each rule
first = collections.Counter()     # LABs by the first rule that fails, in the checker's order
lab_counts, lab_nets = [], []
for key, alms in labs.items():
    fails, count = set(), 0
    for alm in alms.values():
        luts = alm['lut']
        # ALM rule, bits and inputs
        bits = sum(lut_bits(c) for c in luts if c)
        if bits > 64:
            fails.add('ALM LUT bits > 64')
        ins = [set(lut_inputs(c)) if c else set() for c in luts]
        total = sum(len(s) for s in ins)
        shared = min(2, len(ins[0] & ins[1])) if luts[0] and luts[1] else 0
        if total - shared > 8:
            fails.add('ALM inputs > 8')
        if luts[0] and luts[1] and (cells[luts[0]]['type'] == 'MISTRAL_ALUT_ARITH') != (cells[luts[1]]['type'] == 'MISTRAL_ALUT_ARITH'):
            fails.add('carry and non-carry in one ALM')
        alm_count = total - shared
        for half in (0, 1):
            other = luts[1 - half]
            ef = not other or len(lut_inputs(other)) <= 2
            route_thru = luts[half] is None
            for j in (0, 1):
                ff = alm['ff'][half][j]
                if not ff:
                    continue
                if j == 1:
                    fails.add('second register of a half')
                if net(ff, 'SDATA') is not None:
                    alm_count += 1
                    if not ef:
                        fails.add('SDATA without E/F')
                    ef = False
                data = net(ff, 'DATAIN')
                if data is not None and (luts[half] is None or data != lut_out(luts[half])):
                    alm_count += 1
                    if route_thru:
                        route_thru = False
                    elif ef:
                        ef = False
                    else:
                        fails.add('register data from fabric, no E/F left')
        count += alm_count
    if count > 42:
        fails.add('LAB input count > 42')
    # Control model with a global clock: slots clk 1, sload 1, sclr 1, aclr 2, ena 3; DATAIN lines 8..11.
    sig = collections.defaultdict(set)
    for alm in alms.values():
        for half in alm['ff']:
            for ff in half:
                if ff:
                    for p in ('CLK', 'SLOAD', 'SCLR', 'ACLR', 'ENA'):
                        n = net(ff, p)
                        if n is not None:
                            sig[p].add(n)
    if len(sig['CLK']) > 1:
        fails.add('controls: two clocks')
    if len(sig['ACLR']) > 2 or len(sig['ENA']) > 3 or len(sig['SCLR']) > 1 or len(sig['SLOAD']) > 1:
        fails.add('controls: pool capacity')
    datain = len(sig['SLOAD']) + len(sig['SCLR']) + len(sig['ACLR']) + len(sig['ENA'])
    if datain > 4 or len(sig['ENA']) + len(sig['ACLR']) + len(sig['SCLR']) > 3 + (1 if not sig['SLOAD'] else 0):
        fails.add('controls: DATAIN lines')
    ncells = sum(1 for a in alms.values() for c in a['lut'] + a['ff'][0] + a['ff'][1] if c)
    lab_counts.append(count)
    for f in fails:
        reasons[f] += 1
        cells_in[f] += ncells
    if not fails:
        reasons['legal'] += 1
        cells_in['legal'] += ncells

print(f'{len(labs)} Quartus LABs, {sum(cells_in[k] for k in cells_in if k == "legal") } cells in legal LABs')
for k, v in reasons.most_common():
    print(f'  {k:42s} {v:5d} LABs  {cells_in[k]:6d} cells')
lab_counts.sort()
print(f'nextpnr input count of Quartus LABs: median {lab_counts[len(lab_counts)//2]}, 90th {lab_counts[len(lab_counts)*9//10]}, max {lab_counts[-1]}')
