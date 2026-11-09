"""Logical processors per physical core with EfficiencyClass (GetLogicalProcessorInformationEx)."""
import ctypes, json, struct
k = ctypes.WinDLL('kernel32', use_last_error=True)
RelationProcessorCore = 0
size = ctypes.c_ulong(0)
k.GetLogicalProcessorInformationEx(RelationProcessorCore, None, ctypes.byref(size))
buf = ctypes.create_string_buffer(size.value)
assert k.GetLogicalProcessorInformationEx(RelationProcessorCore, buf, ctypes.byref(size)), ctypes.get_last_error()
raw, off, cores = buf.raw, 0, []
while off < size.value:
    rel, sz = struct.unpack_from('<II', raw, off)
    flags, eff = struct.unpack_from('<BB', raw, off + 8)
    gcount, = struct.unpack_from('<H', raw, off + 30)
    mask, group = struct.unpack_from('<QH', raw, off + 32)
    lps = [i for i in range(64) if mask >> i & 1]
    cores.append(dict(core=len(cores), efficiency_class=eff, smt=bool(flags & 1), group=group, logical=lps, mask=hex(mask)))
    off += sz
print(json.dumps(cores, indent=1))
