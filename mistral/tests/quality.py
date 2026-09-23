#!/usr/bin/env python3
"""Multi-seed quality harness for nextpnr-mistral (design section 19).

Since 2026-09-23 a change may move placements and routes; it is judged over seeds instead of by
byte identity. This runs a binary over several seeds of a named configuration, collects the
quality and time of every run, and compares two such sets against the acceptance rule.

  quality.py run --bin BIN --tag TAG [--config core|probe] [--seeds 1-5] [--parallel 4]
                 [--determinism] [--drop=OPTION ...] [--timeout MIN] [-- extra nextpnr options]
  quality.py compare BASE_TAG CANDIDATE_TAG [--config core|probe] [--kind quality|speed]
  quality.py prepare --bin BIN --dir DIR [--config ...] [--seeds 1-5] [--parallel 4] [--drop ...]
  quality.py run ... --resume-dir DIR      (route from DIR's checkpoints; routing-only changes)

`prepare` writes a route-prepared checkpoint per seed (Stage 5 2a/2b); `run --resume-dir` routes
from them instead of placing. A resumed run equals the uninterrupted one to the byte, so its
quality is the full flow's, and resumed runs with `--parallel 1` give serial router times.

Outputs go to build/quality/<config>/<tag>/ (never committed): per seed the log, telemetry,
report, and routed JSON, and summary.json for the set. `--determinism` runs the first seed twice
and requires identical routed JSON (without its creator line) and report.

The acceptance rule (tracker decision 2026-09-23):
  every seed routes, no seed's Fmax below the baseline's worst, and
  quality: median Fmax above the baseline median by more than the baseline's spread (max - min);
  speed: median Fmax within the baseline's range, and median wall time lower.
Wall times from parallel runs are for orientation only; timing claims need serial runs.

A run that router2 cannot finish within its iteration cap goes on to router1, which never finishes
on the core; `--timeout` (minutes per run, 30 by default) stops it and records it as not routed.
Pass a dropped flag with `=` (`--drop=--router2-unit-cost`), or argparse reads it as an option.
"""
import argparse
import collections
import json
import os
import re
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CONFIGS = {
    # The recipe that routes the full Fabi386 core (CLAUDE.md, Stage 6 6h), inputs outside git.
    'core': {
        'inputs': ['--json', 'build/stage6-fullcore/f386_core_probe.json', '--qsf',
                   'build/stage6-fullcore/core_probe.qsf'],
        'options': ['--device', '5CSEBA6U23I7', '--threads', '1', '--placer', 'heap', '--router', 'router2',
                    '--freq', '33', '--router2-max-iter', '100', '--lab-controls', 'legacy', '--timing-allow-fail',
                    '--ignore-loops', '--alm-pairing', '1', '--spread-congestion', '--register-packing',
                    '--row-cost', '2.5', '--router2-unit-cost', '--router2-reroute', '20',
                    '--router2-reroute-contested'],
    },
    # The exec probe on the default path, as the gate runs it: a smoke test, not crowded.
    'probe': {
        'inputs': ['--json', 'build/fabi386-inputs/f386_exec_probe_nodsp.json', '--qsf',
                   'build/fabi386-inputs/exec_probe.qsf'],
        'options': ['--device', '5CSEBA6U23I7', '--threads', '1', '--placer', 'heap', '--router', 'router2',
                    '--freq', '12', '--router2-max-iter', '100', '--lab-controls', 'legacy'],
    },
}
LAB_BELS = ('MISTRAL_COMB', 'MISTRAL_MCOMB', 'MISTRAL_FF')
NET_FANOUT_MAX = 64  # nets with more sinks (clocks, resets, enables) do not enter the per-net shape


def seeds_of(text):
    out = []
    for part in text.split(','):
        if '-' in part:
            a, b = part.split('-')
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return out


def run_dir(config, tag):
    return os.path.join(ROOT, 'build', 'quality', config, tag)


def placement_shape(routed_json):
    """ALMs and LABs used, and per net (2 to NET_FANOUT_MAX sinks, all in LABs) the rows it
    touches and the LABs its sinks sit in other than the driver's: design 9.5's measures."""
    with open(routed_json) as f:
        module = next(iter(json.load(f)['modules'].values()))
    loc = {}
    alms, labs = set(), set()
    for name, cell in module['cells'].items():
        bel = cell.get('attributes', {}).get('NEXTPNR_BEL', '')
        parts = bel.split('.')
        if len(parts) != 4 or parts[0] not in LAB_BELS:
            continue
        x, y, z = int(parts[1]), int(parts[2]), int(parts[3])
        loc[name] = (x, y)
        alms.add((x, y, z // 6))
        labs.add((x, y))
    drivers, sinks = {}, collections.defaultdict(list)
    for name, cell in module['cells'].items():
        for port, bits in cell.get('connections', {}).items():
            direction = cell.get('port_directions', {}).get(port)
            for bit in bits:
                if not isinstance(bit, int):
                    continue
                if direction == 'output':
                    drivers[bit] = name
                elif direction == 'input':
                    sinks[bit].append(name)
    rows, entries, counted = 0, 0, 0
    for bit, driver in drivers.items():
        users = sinks.get(bit, [])
        if not 1 <= len(users) <= NET_FANOUT_MAX or driver not in loc or any(u not in loc for u in users):
            continue
        dx, dy = loc[driver]
        rows += len({dy} | {loc[u][1] for u in users})
        entries += len({loc[u] for u in users} - {(dx, dy)})
        counted += 1
    return {'alms': len(alms), 'labs': len(labs), 'nets_measured': counted,
            'rows_per_net': rows / counted if counted else None,
            'lab_entries_per_net': entries / counted if counted else None}


def parse_log(log_path):
    text = open(log_path, errors='replace').read()
    out = {}
    for key, pattern in (('heap_s', r'HeAP Placer Time: ([0-9.]+)s'), ('sa_s', r'SA placement time ([0-9.]+)s'),
                         ('router2_s', r'Router2 time ([0-9.]+)s')):
        m = re.findall(pattern, text)
        out[key] = float(m[-1]) if m else None
    iters = re.findall(r'iter=(\d+) wires=(\d+) overused=(\d+)', text)
    if iters:
        out['iterations'], out['wires'], out['overused'] = (int(v) for v in iters[-1])
    wl = re.findall(r'wirelen = (\d+)', text)
    out['sa_wirelen'] = int(wl[-1]) if wl else None
    checksums = re.findall(r'Checksum: (0x[0-9a-f]+)', text)
    out['checksums'] = checksums[-2:]
    out['finished'] = 'Program finished normally' in text
    return out


def config_options(config, drop):
    """The configuration's options without the dropped flags (a dropped flag that takes a value
    drops the value too)."""
    options, out, skip = CONFIGS[config]['options'], [], False
    for i, opt in enumerate(options):
        if skip:
            skip = False
            continue
        if opt in drop:
            skip = i + 1 < len(options) and not options[i + 1].startswith('--')
            continue
        out.append(opt)
    return out


def inputs_for(config, seed, resume_dir):
    inputs = list(CONFIGS[config]['inputs'])
    if resume_dir:
        i = inputs.index('--json')
        inputs[i:i + 2] = ['--resume', os.path.join(os.path.abspath(resume_dir), f's{seed}.prepared.json')]
    return inputs


def run_one(binary, config, tag, seed, extra, suffix='', drop=(), resume_dir=None, timeout_min=30):
    d = run_dir(config, tag)
    os.makedirs(d, exist_ok=True)
    stem = os.path.join(d, f's{seed}{suffix}')
    cmd = [binary] + config_options(config, drop) + inputs_for(config, seed, resume_dir) + [
        '--seed', str(seed), '--log', stem + '.log', '--telemetry', stem + '.telemetry.json', '--report',
        stem + '.report.json', '--write', stem + '.routed.json'] + extra
    try:
        proc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                              timeout=timeout_min * 60)
        returncode, stderr = proc.returncode, proc.stderr
    except subprocess.TimeoutExpired:
        returncode, stderr = 'timeout', ''
    result = {'seed': seed, 'exit': returncode}
    if os.path.exists(stem + '.log'):
        result.update(parse_log(stem + '.log'))
    result['routed'] = returncode == 0 and result.get('overused') == 0 and result.get('finished', False)
    if os.path.exists(stem + '.report.json'):
        report = json.load(open(stem + '.report.json'))
        result['fmax'] = {k: v['achieved'] for k, v in report.get('fmax', {}).items()}
    if os.path.exists(stem + '.telemetry.json'):
        phases = json.load(open(stem + '.telemetry.json')).get('phases', {})
        result['placement_s'], result['routing_s'] = phases.get('placement_s'), phases.get('routing_s')
    if os.path.exists(stem + '.routed.json'):
        result.update(placement_shape(stem + '.routed.json'))
    if returncode != 0:
        result['stderr_tail'] = stderr[-400:]
    return result


def cmd_prepare(args, extra):
    binary = os.path.abspath(args.bin)
    out = os.path.abspath(args.dir)
    os.makedirs(out, exist_ok=True)

    def one(seed):
        cmd = [binary] + config_options(args.config, args.drop) + CONFIGS[args.config]['inputs'] + [
            '--seed', str(seed), '--checkpoint', os.path.join(out, f's{seed}.prepared.json'), '--route-prepare-only',
            '--log', os.path.join(out, f's{seed}.prepare.log')] + extra
        return seed, subprocess.run(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode

    with ThreadPoolExecutor(max_workers=args.parallel) as pool:
        results = list(pool.map(one, seeds_of(args.seeds)))
    for seed, code in results:
        print(f"  seed {seed}: {'prepared' if code == 0 else f'FAILED (exit {code})'}")
    return 0 if all(code == 0 for _, code in results) else 1


def strip_creator(path):
    with open(path) as f:
        return [line for line in f if '"creator"' not in line]


def cmd_run(args, extra):
    binary = os.path.abspath(args.bin)
    seeds = seeds_of(args.seeds)
    jobs = [(s, '') for s in seeds] + ([(seeds[0], '_repeat')] if args.determinism else [])
    with ThreadPoolExecutor(max_workers=args.parallel) as pool:
        results = list(pool.map(lambda job: run_one(binary, args.config, args.tag, job[0], extra, job[1],
                                                    args.drop, args.resume_dir, args.timeout), jobs))
    runs = [r for (s, suffix), r in zip(jobs, results) if not suffix]
    summary = {'tag': args.tag, 'config': args.config, 'binary': binary, 'drop': args.drop, 'extra': extra,
               'resume_dir': args.resume_dir, 'runs': runs}
    if args.determinism:
        d = run_dir(args.config, args.tag)
        a, b = os.path.join(d, f's{seeds[0]}'), os.path.join(d, f's{seeds[0]}_repeat')
        same = (strip_creator(a + '.routed.json') == strip_creator(b + '.routed.json') and
                open(a + '.report.json').read() == open(b + '.report.json').read())
        summary['deterministic'] = same
    with open(os.path.join(run_dir(args.config, args.tag), 'summary.json'), 'w') as f:
        json.dump(summary, f, indent=1)
    print_table(summary)
    return 0 if all(r['routed'] for r in runs) and summary.get('deterministic', True) else 1


def primary_fmax(run):
    fmax = run.get('fmax') or {}
    return min(fmax.values()) if fmax else None


def print_table(summary):
    dropped = ' '.join('-' + d for d in summary.get('drop', []))
    print(f"== {summary['config']} / {summary['tag']}  {dropped} {' '.join(summary['extra'])}")
    print(f"  {'seed':>4} {'routed':>6} {'Fmax':>7} {'iters':>5} {'wires':>8} {'ALMs':>6} {'LABs':>5} "
          f"{'rows/net':>8} {'LABs/net':>8} {'place s':>7} {'route s':>7}")
    for r in summary['runs']:
        fmt = lambda v, spec: format(v, spec) if v is not None else '-'
        print(f"  {r['seed']:>4} {str(r['routed']):>6} {fmt(primary_fmax(r), '7.2f')} {fmt(r.get('iterations'), '5d')} "
              f"{fmt(r.get('wires'), '8d')} {fmt(r.get('alms'), '6d')} {fmt(r.get('labs'), '5d')} "
              f"{fmt(r.get('rows_per_net'), '8.3f')} {fmt(r.get('lab_entries_per_net'), '8.3f')} "
              f"{fmt(r.get('placement_s'), '7.0f')} {fmt(r.get('routing_s'), '7.0f')}")
    fm = [primary_fmax(r) for r in summary['runs'] if primary_fmax(r) is not None]
    if fm:
        print(f"  Fmax median {statistics.median(fm):.2f}, range {min(fm):.2f} to {max(fm):.2f} MHz")
    if 'deterministic' in summary:
        print(f"  determinism (seed repeated): {'identical' if summary['deterministic'] else 'DIFFERENT'}")


def load(config, tag):
    return json.load(open(os.path.join(run_dir(config, tag), 'summary.json')))


def cmd_compare(args):
    base, cand = load(args.config, args.base), load(args.config, args.candidate)
    print_table(base)
    print_table(cand)
    bf = [primary_fmax(r) for r in base['runs']]
    cf = [primary_fmax(r) for r in cand['runs']]
    if None in bf or None in cf:
        print('verdict: a run has no Fmax; not comparable')
        return 1
    b_med, c_med = statistics.median(bf), statistics.median(cf)
    spread = max(bf) - min(bf)
    all_route = all(r['routed'] for r in cand['runs'])
    floor = min(cf) >= min(bf)
    print(f"\n  median Fmax {b_med:.2f} -> {c_med:.2f} MHz ({c_med - b_med:+.2f}); baseline spread {spread:.2f}; "
          f"all candidate seeds route: {all_route}; worst seed {min(cf):.2f} vs baseline worst {min(bf):.2f}")
    if args.kind == 'quality':
        ok = all_route and floor and c_med - b_med > spread
        why = 'median gain exceeds the baseline spread' if ok else 'fails: needs every seed routed, the floor, ' \
              'and a median gain above the baseline spread'
    else:
        bt = statistics.median([(r.get('placement_s') or 0) + (r.get('routing_s') or 0) for r in base['runs']])
        ct = statistics.median([(r.get('placement_s') or 0) + (r.get('routing_s') or 0) for r in cand['runs']])
        inside = min(bf) <= c_med <= max(bf)
        ok = all_route and floor and inside and ct < bt
        print(f"  median wall (placement + routing) {bt:.0f} -> {ct:.0f} s (orientation only unless run serially)")
        why = 'quality inside the baseline range and faster' if ok else 'fails the speed rule'
    print(f"  verdict ({args.kind}): {'ACCEPT' if ok else 'REJECT'}: {why}")
    return 0 if ok else 1


def main():
    argv = sys.argv[1:]
    extra = []
    if '--' in argv:
        i = argv.index('--')
        argv, extra = argv[:i], argv[i + 1:]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    run = sub.add_parser('run')
    run.add_argument('--bin', required=True)
    run.add_argument('--tag', required=True)
    run.add_argument('--config', choices=sorted(CONFIGS), default='core')
    run.add_argument('--seeds', default='1-5')
    run.add_argument('--parallel', type=int, default=4)
    run.add_argument('--determinism', action='store_true')
    run.add_argument('--drop', action='append', default=[], help='remove a flag of the configuration')
    run.add_argument('--resume-dir', help='route from the route-prepared checkpoints in this directory')
    run.add_argument('--timeout', type=float, default=30, help='minutes per run before it counts as not routed')
    prep = sub.add_parser('prepare')
    prep.add_argument('--bin', required=True)
    prep.add_argument('--dir', required=True)
    prep.add_argument('--config', choices=sorted(CONFIGS), default='core')
    prep.add_argument('--seeds', default='1-5')
    prep.add_argument('--parallel', type=int, default=4)
    prep.add_argument('--drop', action='append', default=[])
    cmp = sub.add_parser('compare')
    cmp.add_argument('base')
    cmp.add_argument('candidate')
    cmp.add_argument('--config', choices=sorted(CONFIGS), default='core')
    cmp.add_argument('--kind', choices=['quality', 'speed'], default='quality')
    args = parser.parse_args(argv)
    if args.command == 'prepare':
        return cmd_prepare(args, extra)
    return cmd_run(args, extra) if args.command == 'run' else cmd_compare(args)


if __name__ == '__main__':
    sys.exit(main())
