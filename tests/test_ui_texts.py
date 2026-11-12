"""Every text the in-game menu shows has a translation key (gpu/shim/ui_language/port_ui_keys.h).

The menu translates what its helpers show (Row, CheckRow, ChoiceRow, InfoRow, SliderRow, Help, the
Choice labels) and what it passes to Tr(); a new English text without a key would stay English in
every language. Names that are the same everywhere (FSR 3.1, R2, resolutions) are allowed below.
"""
import re
import unittest

from paths import ROOT

OVERLAY = ROOT / 'gpu' / 'shim' / 'port_overlay.cpp'
KEYS = ROOT / 'gpu' / 'shim' / 'ui_language' / 'port_ui_keys.h'
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
CALLS = ('CheckRow(', 'ChoiceRow(', 'InfoRow(', 'SliderRow(', 'Row(', 'Help(', 'Tr(', 'PadText(')
# The same in every language: product names, button names, numbers and sizes.
UNTRANSLATED = re.compile(
    r'^(?:[A-Z0-9][A-Z0-9 .:/+\-]*|FSR [0-9.]+|TAA|DLSS|\d+ x \d+ \([^)]*\)|[0-9.]+|'
    r'%d%%|##\w+|###\w+|[A-Z]\d?|[A-Z]{1,3}\d?|R2|L2|RT|LT|ZR|ZL|[EQZ]|Tab|[+-] / [+-]|-|'
    r'\s*\(Insert[^)]*\))$')


def literals_in_calls(source):
    """The string literals inside the calls above and the static label arrays."""
    found = set()
    for name in CALLS:
        for match in re.finditer(r'(?<![\w:])' + re.escape(name), source):
            depth, i = 1, match.end()
            while depth and i < len(source):
                depth += {'(': 1, ')': -1}.get(source[i], 0)
                if source[i] == '"':
                    literal = LITERAL.match(source, i)
                    i = literal.end() - 1
                i += 1
            body = source[match.end():i - 1]
            # Adjacent literals are one C string.
            joined = re.sub(r'"\s*\n\s*"', '', body)
            found.update(LITERAL.findall(joined))
    for array in re.finditer(r'static const char\* const \w+\[(?:\w*)\] = \{(.*?)\};', source, re.S):
        found.update(LITERAL.findall(array.group(1)))
    return found


class UiTexts(unittest.TestCase):
    def test_every_shown_text_has_a_key(self):
        keys = set(LITERAL.findall(KEYS.read_text(encoding='utf-8')))
        texts = literals_in_calls(OVERLAY.read_text(encoding='utf-8'))
        missing = sorted(t for t in texts if t not in keys and not UNTRANSLATED.match(t))
        self.assertEqual(missing, [], 'menu texts without a translation key')

    def test_keys_are_used(self):
        # A key the menu no longer shows is dead weight in every language table.
        source = OVERLAY.read_text(encoding='utf-8') + (ROOT / 'gpu' / 'shim' / 'port_graphics.h').read_text(
            encoding='utf-8')
        source = re.sub(r'"\s*\n\s*"', '', source)  # adjacent literals are one C string
        keys = LITERAL.findall(KEYS.read_text(encoding='utf-8'))
        unused = [k for k in keys if '"' + k + '"' not in source]
        self.assertEqual(unused, [], 'translation keys the menu no longer shows')


if __name__ == '__main__':
    unittest.main()
