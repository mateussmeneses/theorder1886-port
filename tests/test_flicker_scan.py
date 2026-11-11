from paths import ROOT
import sys
import unittest

try:
    import numpy as np
except ImportError:  # the CI installs python-numpy; a local MSYS2 Python may not have it
    np = None

sys.path.insert(0, str(ROOT / 'tools'))
if np is not None:
    import flicker_scan


def scene(n, seed=1):
    if np is None:
        return []
    rng = np.random.default_rng(seed)
    base = rng.integers(20, 60, size=(54, 96, 3)).astype(np.int16)
    # slow camera drift: a gradient that moves one column per frame (never returns)
    return [np.roll(base, i, axis=1) for i in range(n)]


@unittest.skipIf(np is None, 'numpy not installed')
class FlickerScanTests(unittest.TestCase):
    def test_static_and_moving_scenes_have_no_aba(self):
        static = [scene(1)[0].copy() for _ in range(6)]
        self.assertEqual(flicker_scan.scan(static)['triples'], 0)  # all repeats dropped
        moving = flicker_scan.scan(scene(8))
        self.assertEqual(max(moving['per_triple']), 0.0)

    def test_object_missing_for_one_frame_is_found(self):
        frames = [f.copy() for f in [scene(1)[0]] * 5]
        for i, f in enumerate(frames):
            f[0, 0] += i  # keep frames distinct
            if i != 2:
                f[10:20, 30:40] += 100  # a lit object, gone in frame 2
        result = flicker_scan.scan(frames)
        self.assertGreater(result['per_triple'][1], 1.0)
        ys, xs = np.nonzero(result['union'])
        self.assertEqual((ys.min(), ys.max(), xs.min(), xs.max()), (10, 19, 30, 39))

    def test_repeated_frames_are_not_counted_as_changes(self):
        frames = scene(4)
        doubled = [f for f in frames for _ in range(2)]  # 30 FPS shown at 60
        self.assertEqual(flicker_scan.scan(doubled)['distinct'], 4)


if __name__ == '__main__':
    unittest.main()
