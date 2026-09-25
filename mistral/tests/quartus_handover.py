#!/usr/bin/env python3
"""Hand a nextpnr-mistral input netlist to Quartus as WYSIWYG primitives, keeping a map back to its cell names.

usage: quartus_handover.py NETLIST.json TECHMAP.v OUT_DIR

Design 20.0 (phase 0.1): Quartus's placement of our own netlist is the oracle the phase routes with nextpnr, so
every Quartus instance must map back to a nextpnr cell. Quartus's name builder crashes on Yosys's long escaped names,
and the earlier hand-over (tracker, 2026-09-17) renamed everything with `rename -enumerate`, losing the map. Here
each cell is renamed first to a short public name (`c0000001`, in sorted order of the original names) and the map is
written; Yosys then maps the cells to Quartus primitives (the techmap keeps an instance's name) and enumerates only
the wires. Writes OUT_DIR/handover.v, OUT_DIR/cell_map.tsv (short name, original name, type) and OUT_DIR/renamed.json.
"""
import json
import os
import subprocess
import sys


def main():
    netlist, techmap, out = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3]
    os.makedirs(out, exist_ok=True)
    data = json.load(open(netlist))
    top = max((m for m in data['modules'].values() if 'cells' in m), key=lambda m: len(m['cells']))
    names = sorted(top['cells'])
    short = {name: f'c{i:07d}' for i, name in enumerate(names, 1)}
    top['cells'] = {short[name]: top['cells'][name] for name in names}
    with open(os.path.join(out, 'cell_map.tsv'), 'w') as f:
        for name in names:
            f.write(f"{short[name]}\t{name}\t{top['cells'][short[name]]['type']}\n")
    renamed = os.path.join(out, 'renamed.json')
    json.dump(data, open(renamed, 'w'))
    script = (f'read_json {renamed}; techmap -D cyclonev -map {techmap}; '
              f'rename -hide w:*; rename -enumerate w:*; opt_clean; '
              f'write_verilog -noattr {os.path.join(out, "handover.v")}')
    subprocess.run(['yosys', '-q', '-p', script], check=True)
    print(f'{len(names)} cells renamed; {out}/handover.v, cell_map.tsv')


if __name__ == '__main__':
    main()
