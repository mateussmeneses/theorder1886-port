"""Extract the original bytes of eboot 01.02 sub_8a7380 for tests/test_task_idle.c.

    python tools/task_idle_bytes.py <game folder> [--out out/sub_8a7380.bin]

The differential half of task-idle-test runs this machine code against the native callback
(src/runtime_task_idle.c). The bytes come from the user's own game and stay under out/.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
OFFSET, LENGTH, FNV1A = 0x8a7380, 54, 0x15685b7f227bc923  # src/runtime_task_idle.c


def fnv1a(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff
    return h


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, default=ROOT / 'out/sub_8a7380.bin')
    args = parser.parse_args()
    from eboot_pe import load
    data = bytes(load(args.game)[0][OFFSET:OFFSET + LENGTH])
    if fnv1a(data) != FNV1A:
        sys.exit(f'{args.game}: sub_{OFFSET:x} differs from eboot 01.02 (other build?)')
    args.out.write_bytes(data)
    print(f'{args.out}: {LENGTH} bytes of sub_{OFFSET:x}')


if __name__ == '__main__':
    main()
