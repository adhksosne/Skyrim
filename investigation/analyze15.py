# -*- coding: utf-8 -*-
"""调查15：0x361640 (ID 24212) 的虚表归属 + RTTI 类名"""
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
def file2rva(off):
    for name, va, vs, ra, rs in secs:
        if ra <= off < ra + rs:
            return va + (off - ra)
def sec_of_rva(rva):
    for name, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return name
def rd_q(rva):
    fo = rva2file(rva)
    if fo is None: return None
    return struct.unpack_from("<Q", exe, fo)[0]
def rd_d(rva):
    fo = rva2file(rva)
    if fo is None: return None
    return struct.unpack_from("<i", exe, fo)[0]
def rd_bd(rva):
    fo = rva2file(rva)
    if fo is None: return None
    return struct.unpack_from("<I", exe, fo)[0]
def demangle(name):
    for p in (".?AV", ".?AU"):
        if name.startswith(p):
            n = name[len(p):]
            if n.endswith("@@"): n = n[:-2]
            return n.replace("@", "::")
    return name
def rtti_for_vtable(vt_rva, label):
    print(f"--- 虚表 @ 0x{vt_rva:X} ({label}) ---")
    col_rva = rd_q(vt_rva - 8)
    if col_rva is None or not (image_base < col_rva < image_base + 0x3000000):
        print(f"  [-1]=0x{col_rva if col_rva else 0:X} 无 RTTI COL"); return
    col = col_rva - image_base
    sig = rd_d(col); off = rd_d(col+4)
    td_rva = rd_bd(col + 12)
    fo = rva2file(td_rva + 16)
    raw = b""
    while True:
        b = exe[fo + len(raw)]
        if b == 0: break
        raw += bytes([b])
        if len(raw) > 300: break
    print(f"  COL sig={sig} off={off}  name={raw.decode('latin1')}")
    print(f"  >>> 类名: {demangle(raw.decode('latin1'))}")

# 1) 找指向 0x361640 的 8 字节指针
va_target = image_base + 0x361640
needle = struct.pack("<Q", va_target)
locs = []
start = 0
while True:
    i = exe.find(needle, start)
    if i < 0: break
    start = i + 1
    rva = file2rva(i)
    sec = sec_of_rva(rva)
    locs.append((i, rva, sec))
    print(f"指针 0x361640 @ file=0x{i:X} rva=0x{rva:X} sec={sec}")

# 2) 对每个 .rdata 位置：向前找虚表起点（连续 .text 指针），解 RTTI
for i, rva, sec in locs:
    if sec != ".rdata":
        continue
    vt_start = rva
    while True:
        prev = vt_start - 8
        fo = rva2file(prev)
        val = struct.unpack_from("<Q", exe, fo)[0]
        rv = val - image_base
        if sec_of_rva(rv) == ".text" and rv > 0x1000:
            vt_start = prev
        else:
            break
    slot = (rva - vt_start) // 8
    print(f"\n虚表起点 0x{vt_start:X}，0x361640 = 槽 {slot}")
    rtti_for_vtable(vt_start, f"含 0x361640 槽{slot}")
