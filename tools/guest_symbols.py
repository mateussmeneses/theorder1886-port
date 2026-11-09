"""Function starts of the linked guest image, for naming "guest offset 0x..." frames.

    python tools/guest_symbols.py <game folder> [--out out/guest_symbols.json]

The probe image is the eboot at 0 followed by the linked system modules at the bases in
out/link.json (scripts/link_modules.py). Eboot functions are the .eh_frame_hdr starts named
sub_<A> as in scripts/eboot_pe.py (so the names match the REA/Ghidra wrapper); module functions
are their .eh_frame_hdr starts too, named after the export at that address when there is one
(NIDs resolved through src/import_names.inc), else <module>+0x<offset>.
Output: a JSON list of [start, name, module] sorted by start, in guest-offset coordinates.
"""
import argparse
import bisect
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))


def eh_frame_starts(image, hdr):
    """Initial locations of an .eh_frame_hdr search table at image offset hdr (eboot_pe's)."""
    from eboot_pe import function_starts
    return function_starts(image, hdr)


def module_image(m):
    """Unrelocated load image of a parsed module (only code addresses are needed)."""
    from prepare import span
    loads = [p for p in m['ph'] if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(m['elf'], p['offset'], p['filesz'])
    return image


def module_functions(m, base, label, names):
    """[(guest offset, name)] for one module placed at base."""
    from link_modules import canonical
    starts = set()
    hdr = next((p for p in m['ph'] if p['type'] == 0x6474e550), None)
    if hdr:
        try:
            starts = eh_frame_starts(module_image(m), hdr['vaddr'])
        except ValueError:
            starts = set()
    exported = {}
    for s in m['symbols']:
        if s['section'] and s['type'] == 2 and s['identity']:  # STT_FUNC defined here
            key = canonical(s['identity'])
            exported.setdefault(s['value'], names.get(key, key))
    out = {a: exported.get(a, f'{label}+0x{a:x}') for a in starts}
    out.update(exported)
    return [(base + a, n) for a, n in out.items()]


def build(game):
    from eboot_pe import load
    from link_modules import module
    names = dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', (ROOT / 'src/import_names.inc').read_text()))
    _, _, _, _, exports = load(game)
    table = [(a, n, 'eboot') for a, n in exports.items()]
    link = json.loads((ROOT / 'out/link.json').read_text())
    for entry in link['modules']:
        m = module(game / 'sce_module' / entry['file'])
        label = entry['file'].rsplit('.', 1)[0]
        table += [(a, n, label) for a, n in module_functions(m, int(entry['base'], 16), label, names)]
    return sorted(table)


class Resolver:
    """Names a guest offset by the function start at or below it."""

    def __init__(self, table):
        self.table = table
        self.starts = [t[0] for t in table]

    def __call__(self, offset):
        i = bisect.bisect_right(self.starts, offset) - 1
        if i < 0:
            return None
        start, name, mod = self.table[i]
        return name if mod == 'eboot' else f'{mod}:{name}'


def load_resolver(path):
    return Resolver([tuple(t) for t in json.loads(Path(path).read_text())])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, default=ROOT / 'out/guest_symbols.json')
    args = parser.parse_args()
    table = build(args.game)
    args.out.write_text(json.dumps(table))
    counts = {}
    for _, _, mod in table:
        counts[mod] = counts.get(mod, 0) + 1
    print(f'{args.out}: ' + ', '.join(f'{m} {c}' for m, c in counts.items()))


if __name__ == '__main__':
    main()
