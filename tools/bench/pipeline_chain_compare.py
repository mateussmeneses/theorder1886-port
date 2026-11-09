"""P1: compare pipeline binary misses across sessions of the chain.

usage: python compare.py s1.log s2.log [s3.log]
A miss line (THEORDER_BINARY_MISS_LOG=1) names the emulator pipeline key hash and the driver's
pipeline binary key (the .bin name). For each later session: misses whose driver key already
missed (and was saved) in an earlier session = binaries that did not survive; misses whose
emulator key missed earlier under another driver key = unstable SPIR-V/state for that key.
"""
import re
import sys
from collections import Counter
from pathlib import Path

MISS = re.compile(r'^Binary miss: pipeline ([0-9a-f]{16}) driver key ([0-9a-f]+)', re.M)
SUMMARY = re.compile(r'^Pipeline binary cache: (\d+) hits, (\d+) compiled, (\d+) rejected, (\d+) saved, (\d+) cache errors', re.M)
SLOW = re.compile(r'^Pipeline: ([0-9.]+) ms in the driver (in the background|on the calling thread) \(([^,]+), key ([0-9a-f]{16})', re.M)

sessions = []
for path in sys.argv[1:]:
    text = Path(path).read_text(encoding='utf-8', errors='replace')
    misses = MISS.findall(text)
    summary = SUMMARY.findall(text)
    sessions.append(dict(name=Path(path).stem, misses=misses, summary=summary[-1] if summary else None,
                         slow=SLOW.findall(text), stalled=len(re.findall(r' STALLED$', text, re.M))))

seen_driver, seen_emu = {}, {}
for s in sessions:
    emu = Counter(e for e, _ in s['misses'])
    drv = Counter(d for _, d in s['misses'])
    print(f"## {s['name']}: {len(s['misses'])} misses ({len(emu)} emulator keys, {len(drv)} driver keys); "
          f"summary hits/compiled/rejected/saved/errors {s['summary']}; STALLED draws {s['stalled']}; "
          f"slow driver creates {len(s['slow'])}")
    if seen_driver:
        again_driver = [d for d in drv if d in seen_driver]
        again_emu_new_driver = sorted({e for e, d in s['misses'] if e in seen_emu and d not in seen_emu[e]})
        new = [d for d in drv if d not in seen_driver]
        print(f"   driver keys that missed in an earlier session too: {len(again_driver)}")
        print(f"   emulator keys seen earlier, now with a different driver key: {len(again_emu_new_driver)}")
        print(f"   driver keys never seen before: {len(new)}")
        for e in again_emu_new_driver[:10]:
            print(f"     {e}: earlier {sorted(seen_emu[e])[:3]} now {sorted({d for x, d in s['misses'] if x == e})[:3]}")
    for e, d in s['misses']:
        seen_driver.setdefault(d, s['name'])
        seen_emu.setdefault(e, set()).add(d)
