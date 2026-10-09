# -*- coding: utf-8 -*-
"""调查11：0x22B990 的虚表归属 + 全部引用（含间接）"""
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
def file2sec(off):
    for name, va, vs, ra, rs in secs:
        if ra <= off < ra + rs:
            return name
    return None

va_target = image_base + 0x22B990
print(f"扫描 8 字节指针 0x{va_target:X}（虚表项/引用）：")
needle = struct.pack("<Q", va_target)
start = 0
while True:
    i = exe.find(needle, start)
    if i < 0: break
    start = i + 1
    sec = file2sec(i)
    off_in = None
    for name, va, vs, ra, rs in secs:
        if ra <= i < ra + rs:
            off_in = va + (i - ra)
    print(f"  file=0x{i:X} sec={sec} rva=0x{off_in:X}")

# 对找到的 .rdata 虚表项：找谁引用这个虚表（lea rip -> vtable 附近）
