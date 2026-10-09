# -*- coding: utf-8 -*-
"""调查18：全 exe 扫描 b"%s's" 与 b"'s \\x00" 全部出现位置 + 正确地址的 xref 计数"""
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
def file2rva(off):
    for name, va, vs, ra, rs in secs:
        if ra <= off < ra + rs:
            return va + (off - ra)
def sec_of_rva(rva):
    for name, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return name
def rva2file(rva):
    for name, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ra + (rva - va)

print("=== 全文件扫描 b\"%s's\" ===")
start = 0
while True:
    i = exe.find(b"%s's", start)
    if i < 0: break
    start = i + 1
    rva = file2rva(i)
    ctx = exe[i:i+24].split(b"\0")[0]
    print(f"  file=0x{i:X} rva=0x{rva:X} sec={sec_of_rva(rva)} ctx={ctx!r}")

print("\n=== 全文件扫描 b\"'s \\x00\"（独立短串，尾随空格+NUL）===")
start = 0
cnt = 0
while True:
    i = exe.find(b"'s \x00", start)
    if i < 0: break
    start = i + 1
    rva = file2rva(i)
    sec = sec_of_rva(rva)
    if sec in (".rdata", ".data"):
        cnt += 1
        print(f"  file=0x{i:X} rva=0x{rva:X} sec={sec} ctx={exe[i-8:i+12]!r}")
print(f"  共 {cnt} 处（仅 .rdata/.data）")

print("\n=== lea-rip xref（正确地址 0x15A0638 / 0x15A0640）===")
text = [s2 for s2 in secs if s2[0] == ".text"][0]
_, tva, tvs, tra, trs = text
data = exe[tra:tra+trs]
for TGT in (0x15A0638, 0x15A0640):
    hits = []
    i = 0
    while i + 7 <= len(data):
        if data[i] in (0x48, 0x4C) and data[i+1] == 0x8D:
            modrm = data[i+2]
            if (modrm >> 6) == 0 and (modrm & 7) == 5:
                disp = struct.unpack_from("<i", data, i+3)[0]
                if tva + i + 7 + disp == TGT:
                    hits.append(tva + i)
        i += 1
    print(f"  0x{TGT:X}: {len(hits)} 处 -> {[hex(h) for h in hits]}")
