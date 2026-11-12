from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT))
import run


class RunLevelStartTests(unittest.TestCase):
    LEVEL = ['--guest-arg', '-startlevel', '--guest-arg', 'wch_020_slums']

    def test_level_start_adds_the_arguments_patch_and_sync_interval(self):
        args, patches = run.level_start(self.LEVEL, 'Aspect Ratio - 16:9')
        self.assertEqual(args, self.LEVEL + ['--guest-arg', '-syncinterval', '--guest-arg', '2'])
        self.assertEqual(patches, 'Aspect Ratio - 16:9;' + run.GAME_ARGS_PATCH)

    def test_existing_choices_are_kept(self):
        given = self.LEVEL + ['--guest-arg', '-syncinterval', '--guest-arg', '1']
        args, patches = run.level_start(given, run.GAME_ARGS_PATCH)
        self.assertEqual(args, given)
        self.assertEqual(patches, run.GAME_ARGS_PATCH)

    def test_without_a_level_nothing_changes(self):
        given = ['--guest-arg', '-disablecloth']
        self.assertEqual(run.level_start(given, ''), (given, ''))
        self.assertEqual(run.level_start([], None), ([], None))


if __name__ == '__main__':
    unittest.main()
