"""Flicker scan of consecutive presented frames (raw dumps from *_FRAME_DUMP_TRIGGER).

A flicker is the A-B-A pattern: a pixel changes for one frame and returns in the next. Camera
motion rarely returns in one frame, so it is not counted. Exact repeats (a 30 FPS game presented
at 60) are dropped first.

    python tools/flicker_scan.py DUMP_DIR [--report REGION.png]

Prints per-triple ABA percentages and the bounding box of every ABA pixel; exit code 0.
"""
import argparse
import re
from pathlib import Path

import numpy as np

CHANGE = 40   # luma step that counts as a visible change
RETURN = 6    # luma tolerance for "came back"


def load(path, step=2):
    m = re.search(r'(\d+)x(\d+)', path.name)
    w, h = int(m.group(1)), int(m.group(2))
    a = np.fromfile(path, dtype=np.uint8)[: w * h * 4].reshape(h, w, 4)[::step, ::step, :3]
    return (a[..., ::-1] if 'bgra' in path.name else a).astype(np.int16)


def frames_in(dump_dir):
    files = sorted(Path(dump_dir).glob('*.raw'),
                   key=lambda f: int(re.search(r'_(\d+)_', f.name).group(1)))
    return [load(f) for f in files]


def distinct(frames):
    out = frames[:1]
    for f in frames[1:]:
        if np.abs(f - out[-1]).max() > 0:
            out.append(f)
    return out


def aba_masks(frames):
    for a, b, c in zip(frames, frames[1:], frames[2:]):
        la, lb, lc = (x.mean(axis=2) for x in (a, b, c))
        yield (np.abs(la - lc) < RETURN) & (np.abs(lb - la) > CHANGE)


def scan(frames):
    frames = distinct(frames)
    masks = list(aba_masks(frames))
    union = np.logical_or.reduce(masks) if masks else None
    return {
        'distinct': len(frames),
        'triples': len(masks),
        'per_triple': [float(m.mean() * 100) for m in masks],
        'union': union,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('dump_dir', type=Path)
    args = parser.parse_args()
    result = scan(frames_in(args.dump_dir))
    pct = result['per_triple']
    print(f"{result['distinct']} distinct frames, {result['triples']} triples; ABA % max "
          f"{max(pct, default=0):.3f} mean {np.mean(pct) if pct else 0:.3f}; "
          f"triples > 0.05 %: {sum(p > 0.05 for p in pct)}")
    if result['union'] is not None and result['union'].any():
        ys, xs = np.nonzero(result['union'])
        print(f'ABA pixels: {int(result["union"].sum())} (half resolution), '
              f'bbox x {xs.min() * 2}-{xs.max() * 2} y {ys.min() * 2}-{ys.max() * 2} (full resolution)')


if __name__ == '__main__':
    main()
