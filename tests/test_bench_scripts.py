"""The benchmark analysis scripts (tools/bench) stay valid Python; they run on batch data only."""
import py_compile
import unittest

from paths import ROOT


class BenchScriptsCompile(unittest.TestCase):
    def test_compile(self):
        scripts = sorted((ROOT / 'tools' / 'bench').glob('*.py'))
        self.assertGreaterEqual(len(scripts), 6)
        for script in scripts:
            with self.subTest(script=script.name):
                py_compile.compile(str(script), doraise=True)


if __name__ == '__main__':
    unittest.main()
