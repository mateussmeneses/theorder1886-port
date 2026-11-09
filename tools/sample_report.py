"""Names the samples of THEORDER_SAMPLE (src/probe.c): self and inclusive time per function.

usage: python tools/sample_report.py [out/sample.txt] [--exe out/theorder-probe.exe] [--top 30]

Each sample line is "count;frame0;frame1;..." with frames as "module+0xoffset" or "guest offset
0x...". Frames in the port's executable are named with llvm-symbolizer (MSYS2 CLANG64) from its
DWARF; other modules count as one entry each. Guest frames are named by the function start at or
below them when --guest-symbols (default out/guest_symbols.json, from tools/guest_symbols.py)
exists: sub_<A> for the eboot, libc:<name> / libSceFios2:<name> for linked modules; else "guest".
"""
import argparse
import collections
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYMBOLIZER = Path('C:/msys64/clang64/bin/llvm-symbolizer.exe')


def image_base(exe):
    data = exe.read_bytes()[:4096]
    pe, = struct.unpack_from('<I', data, 0x3c)
    return struct.unpack_from('<Q', data, pe + 24 + 24)[0]


def parse(lines):
    """{thread: [(count, [frame, ...]), ...]} from the sample lines."""
    threads = collections.defaultdict(list)
    for line in lines:
        parts = line.rstrip('\n').split(';')
        if len(parts) >= 2 and parts[0].isdigit():
            named = parts[1].startswith('thread=')
            frames = parts[2:] if named else parts[1:]
            if frames:
                threads[parts[1][len('thread='):] if named else '?'].append((int(parts[0]), frames))
    return threads


def aggregate(samples, name_of):
    """Self (innermost frame) and inclusive (any of the frames, once per sample) counts by name."""
    own, inclusive = collections.Counter(), collections.Counter()
    for count, frames in samples:
        names = [name_of(frame) for frame in frames]
        own[names[0]] += count
        for name in set(names):
            inclusive[name] += count
    return own, inclusive


def symbolize(frames, exe):
    """{frame: function name} for the frames inside exe."""
    base = image_base(exe)
    prefix = exe.name.lower() + '+0x'
    ours = [f for f in frames if f.lower().startswith(prefix)]
    if not ours:
        return {}
    addresses = '\n'.join(hex(base + int(f.split('+0x')[1], 16)) for f in ours)
    out = subprocess.run([str(SYMBOLIZER), '--obj', str(exe), '--no-inlines', '--output-style=JSON'],
                         input=addresses, capture_output=True, text=True, check=True).stdout
    names = {}
    for frame, line in zip(ours, out.splitlines()):
        symbol = (json.loads(line).get('Symbol') or [{}])[0]
        names[frame] = symbol.get('FunctionName') or frame
    return names


GUEST = 'guest offset 0x'


def guest_offset(frame):
    return int(frame[len(GUEST):].split()[0], 16) if frame.startswith(GUEST) else None


def module_name(frame, resolve=None):
    offset = guest_offset(frame)
    if offset is None:
        return frame.split('+0x')[0]
    name = resolve(offset) if resolve else None
    return 'guest' if not name else 'guest:' + (name.split(':')[0] if ':' in name else 'eboot')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('samples', nargs='?', type=Path, default=ROOT / 'out/sample.txt')
    parser.add_argument('--exe', type=Path, default=ROOT / 'out/theorder-probe.exe')
    parser.add_argument('--top', type=int, default=30)
    parser.add_argument('--guest-symbols', type=Path, default=ROOT / 'out/guest_symbols.json')
    args = parser.parse_args()
    resolve = None
    if args.guest_symbols.exists():
        from guest_symbols import load_resolver
        resolve = load_resolver(args.guest_symbols)

    def guest_name(frame):
        offset = guest_offset(frame)
        return resolve(offset) if resolve and offset is not None else None
    threads = parse(args.samples.read_text(encoding='utf-8', errors='replace').splitlines())
    names = symbolize({f for samples in threads.values() for _, frames in samples for f in frames},
                      args.exe)
    for thread, samples in sorted(threads.items()):
        total = sum(count for count, _ in samples)
        own, inclusive = aggregate(samples, lambda f: names.get(f) or guest_name(f) or module_name(f))
        modules, _ = aggregate(samples, lambda f: module_name(f, resolve))
        print(f'=== {thread}: {total} samples')
        for title, counter in (('modules (innermost frame)', modules), ('self', own),
                               ('inclusive (sampled frames)', inclusive)):
            print(f'\n{title}:')
            for name, count in counter.most_common(args.top):
                print(f'  {100 * count / total:5.1f}%  {name[:150]}')
        print()


if __name__ == '__main__':
    main()
