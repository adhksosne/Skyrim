# -*- coding: utf-8 -*-
"""调查13：转储 0x1559930 虚表全部槽位 -> Address Library ID -> 供 NG 源码反查类名"""
import struct, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
exec(open(r"investigate\_idmap_loader.py", encoding="utf-8").read())
GAME = r"F:\iniRePather-Skyrim\ModOrganizer\StockGame"
EXE  = GAME + r"\SkyrimSE.exe"
exe = open(EXE, "rb").read()
pe_off = struct.unpack_from("<I", exe, 0x3C)[0]
nsec = struct.unpack_from("<H", exe, pe_off + 6)[0]
opt_size = struct.unpack_from("<H", exe, pe_off + 20)[0]
opt_off = pe_off + 24
image_base = struct.unpack_from("<Q", exe, opt_off + 24)[0]
secs = []
for i in range(nsec):
    so = opt_off + opt_size + i * 40
    name = exe[so:so+8].rstrip(b"\0").decode()
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", exe, so + 8)
    secs.append((name, vaddr, vsize, raddr, rsize))
def rva2file(rva):
    for name, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ra + (rva - va)

VT = 0x1559930
print(f"=== 虚表 @ rva 0x{VT:X}（0x22B990 = 槽 76），转储槽 0..79 ===")
ids = []
for slot in range(80):
    rva = VT + slot * 8
    fo = rva2file(rva)
    val = struct.unpack_from("<Q", exe, fo)[0]
    rva_val = val - image_base
    fid = off2id.get(rva_val)
    mark = " <== ID17486 目标" if rva_val == 0x22B990 else ""
    print(f"  slot {slot:3d} rva=0x{rva:X} -> 0x{rva_val:X} (id={fid}){mark}")
    if fid: ids.append(fid)

print("\n供反查的 ID 列表：")
print(ids)
