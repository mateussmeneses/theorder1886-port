"""Per program: permutation indices with SPIR-V, with meta, and where LoadCachedProgram stops.

usage: python perms.py <cache/CUSA00035 dir> [pgm_hash ...]
LoadCachedProgram reads metas HashCombine(pgm, 0), (pgm, 1), ... and stops at the first missing
one; every permutation after a gap is rediscovered under a new index next session.
"""
import sys
from collections import defaultdict
from pathlib import Path

M = (1 << 64) - 1
def combine(seed, h):
    return (seed ^ ((h + 0x9e3779b9 + ((seed << 12) & M) + (seed >> 4)) & M)) & M

d = Path(sys.argv[1])
spv = defaultdict(set)
for p in d.glob('*.spv'):
    pgm, idx = p.stem.split('_')
    spv[int(pgm, 16)].add(int(idx))
metas = {int(p.stem, 16) for p in d.glob('*.meta')}
wanted = [int(x, 16) for x in sys.argv[2:]] or sorted(spv, key=lambda k: -len(spv[k]))
gaps_total = lost_total = 0
for pgm in wanted:
    idx = sorted(spv[pgm])
    with_meta = [i for i in idx if combine(pgm, i) in metas]
    stop = 0
    while combine(pgm, stop) in metas:
        stop += 1
    lost = len([i for i in with_meta if i >= stop])
    gaps = sorted(set(range(max(idx) + 1)) - set(idx)) if idx else []
    if sys.argv[2:] or lost or gaps:
        print(f'{pgm:#018x}: {len(idx)} spv (max index {max(idx) if idx else -1}), {len(with_meta)} metas, '
              f'load stops at {stop}, metas after the stop {lost}, indices without spv {gaps[:12]}')
    gaps_total += bool(gaps or lost)
    lost_total += lost
print(f'programs {len(spv)}, with a gap {gaps_total}, permutations unreachable at load {lost_total}')
