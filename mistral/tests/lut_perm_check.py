#!/usr/bin/env python3
"""Design 20.2 guard: every plain 5-input LUT computes its netlist function in the bitstream.

usage: lut_perm_check.py ROUTED.json DECODED.bt

Independent of nextpnr's compute_lut_mask: the physical pin each LUT input arrives on comes from the decoded
bitstream (`mistral-cv decomp`: `r <source> LAB.x.y.alm:PIN`), matched to nets through the routed JSON's ROUTING
wires (an input line) or the driving ALM half (a local output). The half's 32-bit mask comes from `LUT_MASK`.
For every assignment of the LUT's logical inputs, the stored bit at the physical index must equal the LUT's init
under the arch's conventions (inputs and mask inverted; A bit 0, B 1, C or D 2, E 3, F 4 in L5 mode). Plain L5
LUTs in LABs only (not LUT6, arithmetic, or MLAB tiles). Run it on a run without --lut-permutation first: it must
pass there completely, which validates the checker.
"""
import collections
import itertools
import json
import re
import sys

routed, decoded = sys.argv[1], sys.argv[2]
mod = list(json.load(open(routed))['modules'].values())[0]
bitname = {}
for name, n in mod['netnames'].items():
    for b in n['bits']:
        if isinstance(b, int):
            bitname.setdefault(b, name)
wire_net = {}
for name, n in mod['netnames'].items():
    for w in n['attributes'].get('ROUTING', '').split(';')[0::3]:
        if w:
            wire_net[w] = name


def norm(wire):
    parts = wire.split('.')
    return '.'.join([parts[0]] + [str(int(p)) for p in parts[1:]])


pins_of = collections.defaultdict(dict)   # (x, y, alm) -> {pin: source}
masks = {}
local_lut = set()                          # (x, y, alm, half) whose local output carries the LUT (xDFF1L NLUT)
for line in open(decoded):
    m = re.match(r's LAB\.(\d+)\.(\d+):([TB])DFF1L\.(\d+) NLUT$', line)
    if m:
        local_lut.add((int(m.group(1)), int(m.group(2)), int(m.group(4)), 0 if m.group(3) == 'T' else 1))
        continue
    m = re.match(r'r (\S+) LAB\.(\d+)\.(\d+)\.(\d+):(A|B|C|D|E0|E1|F0|F1)$', line)
    if m:
        pins_of[(int(m.group(2)), int(m.group(3)), int(m.group(4)))][m.group(5)] = m.group(1)
        continue
    m = re.match(r's LAB\.(\d+)\.(\d+):LUT_MASK\.(\d+) ([0-9a-f]+)\.([0-9a-f]+)$', line)
    if m:
        masks[(int(m.group(1)), int(m.group(2)), int(m.group(3)))] = (int(m.group(4), 16) << 32) | int(m.group(5), 16)

# Cells by ALM half, and each half's output nets (for local-line sources).
LUT_PINS = {'MISTRAL_ALUT5': 'ABCDE', 'MISTRAL_ALUT4': 'ABCD', 'MISTRAL_ALUT3': 'ABC', 'MISTRAL_ALUT2': 'AB',
            'MISTRAL_NOT': 'A', 'MISTRAL_BUF': 'A'}
lut_out = {}                               # (x, y, alm, half) -> the LUT's output net
second_ff_out = {}                         # (x, y, alm, half) -> the half's second register's output net
luts = []
for cname, c in mod['cells'].items():
    bel = c['attributes'].get('NEXTPNR_BEL', '').split('.')
    if len(bel) < 4 or bel[0] not in ('MISTRAL_COMB', 'MISTRAL_FF'):
        continue
    x, y, z = int(bel[1]), int(bel[2]), int(bel[3])
    alm = z // 6
    half = (z % 6) if bel[0] == 'MISTRAL_COMB' else (0 if (z % 6) in (2, 3) else 1)
    for port, bits in c['connections'].items():
        if c['port_directions'].get(port) == 'output' and bits and isinstance(bits[0], int):
            if bel[0] == 'MISTRAL_COMB':
                if port in ('Q', 'SO'):  # the LUT's output; an arithmetic cell's CO goes down the carry chain
                    lut_out[(x, y, alm, half)] = bitname[bits[0]]
            elif (z % 6) in (3, 5):
                second_ff_out[(x, y, alm, half)] = bitname[bits[0]]
    if bel[0] == 'MISTRAL_COMB' and c['type'] in LUT_PINS:
        luts.append((cname, c, x, y, alm, z % 6))

HW_BIT = {'A': 0, 'B': 1, 'C': 2, 'D': 2, 'E0': 3, 'E1': 3, 'F0': 4, 'F1': 4}
checked = passed = 0
failures = []
for cname, c, x, y, alm, half in luts:
    key = (x, y, alm)
    if key not in masks:
        continue
    # net arriving on each physical pin of this ALM
    pin_net = {}
    for pin, src in pins_of.get(key, {}).items():
        m = re.match(r'LAB\.(\d+)\.(\d+)\.(\d+):FF([TB])1L$', src)
        if m:
            h = (int(m.group(1)), int(m.group(2)), int(m.group(3)), 0 if m.group(4) == 'T' else 1)
            n = lut_out.get(h) if h in local_lut else second_ff_out.get(h)
            pin_net[pin] = {n} if n else set()
        else:
            n = wire_net.get(norm(src))
            pin_net[pin] = {n} if n else set()
    own = {'A', 'B', 'C' if half == 0 else 'D', 'E0' if half == 0 else 'E1', 'F0' if half == 0 else 'F1'}
    # Candidate physical pins per logical input. A net may arrive on two of the half's pins (one read by this half,
    # a shared A or B read by the other); the half's function must hold for one choice.
    candidates = []
    ok = True
    for k, p in enumerate(LUT_PINS[c['type']]):
        bits = c['connections'].get(p, [])
        if not bits or not isinstance(bits[0], int):
            continue
        net = bitname[bits[0]]
        where = sorted(pin for pin in own if net in pin_net.get(pin, set()))
        if not where:
            ok = False
            break
        candidates.append([(k, pin) for pin in where])
    if not ok:
        failures.append((cname, 'input pin not found in the bitstream'))
        continue
    init = int(c['parameters']['LUT'], 2) if c['type'] not in ('MISTRAL_NOT', 'MISTRAL_BUF') else (1 if c['type'] == 'MISTRAL_NOT' else 2)
    half_mask = (masks[key] >> (32 * half)) & 0xFFFFFFFF
    checked += 1

    def computes(logical):
        if len({pin for _, pin in logical}) != len(logical):
            return False  # two logical inputs cannot read one pin
        for assign in range(1 << len(logical)):
            # physical bits carry the inverted logical values; unused physical bits may be anything
            fixed = {}
            L = 0
            for i, (k, pin) in enumerate(logical):
                v = (assign >> i) & 1
                L |= v << k
                fixed[HW_BIT[pin]] = 1 - v
            for j in range(32):
                if any(((j >> b) & 1) != v for b, v in fixed.items()):
                    continue
                if ((half_mask >> j) & 1) != 1 - ((init >> L) & 1):
                    return False
        return True

    if any(computes(list(choice)) for choice in itertools.product(*candidates)):
        passed += 1
    else:
        failures.append((cname, 'function differs'))

print(f'plain L5 LUTs checked {checked}, correct {passed}, wrong or unresolved {len(failures)}')
for f in failures[:10]:
    print('  ', f[1], f[0][:100])
sys.exit(0 if not failures else 1)
