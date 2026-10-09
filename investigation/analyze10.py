# -*- coding: utf-8 -*-
"""调查10：0x22B990 的 ID 与调用者"""
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
secs = []
for i in range(nsec):
    so = opt_off + opt_size + i * 40
    name = exe[so:so+8].rstrip(b"\0").decode()
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", exe, so + 8)
    secs.append((name, vaddr, vsize, raddr, rsize))
tname, tva, tvs, tra, trs = [s for s in secs if s[0] == ".text"][0]
tb = exe[tra:tra+trs]

print(f"rva 0x22B990 -> ids {off2id.get(0x22B990, '无')}")
print(f"rva 0x22B98F-> ids {off2id.get(0x22B98F, '无')}")
target = 0x22B990
print(f"===== call 0x{target:X} 的调用者 =====")
for i in range(len(tb)-5):
    if tb[i] == 0xE8:
        rel = struct.unpack_from("<i", tb, i+1)[0]
        if tva + i + 5 + rel == target:
            print(f"  caller at rva=0x{tva+i:X}, caller func ids={off2id.get(tva+i, '未知')}")
