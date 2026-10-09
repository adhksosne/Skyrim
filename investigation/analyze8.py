# -*- coding: utf-8 -*-
"""调查8：反查 0x22B99F 等函数的地址库 ID + 谁调用 0x22B99F"""
import struct, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
GAME = r"F:\iniRePather-Skyrim\ModOrganizer\StockGame"
BIN  = GAME + r"\..\mods\插件地址库-Address Library All in One\SKSE\Plugins\version-1-5-97-0.bin"
EXE  = GAME + r"\SkyrimSE.exe"

def decode_bin(path):
    data = open(path, "rb").read()
    pos = 0
    def i32():
        nonlocal pos
        v = struct.unpack_from("<i", data, pos)[0]; pos += 4; return v
    def u64():
        nonlocal pos
        v = struct.unpack_from("<Q", data, pos)[0]; pos += 8; return v
    def u8():
        nonlocal pos
        v = data[pos]; pos += 1; return v
    def u16():
        nonlocal pos
        v = struct.unpack_from("<H", data, pos)[0]; pos += 2; return v
    def u32():
        nonlocal pos
        v = struct.unpack_from("<I", data, pos)[0]; pos += 4; return v
    fmt = i32(); assert fmt == 1
    version = [i32() for _ in range(4)]
    nameLen = i32(); pos += nameLen
    ptrSize = i32(); count = i32()
    out = {}
    prevID = 0; prevOff = 0
    for _ in range(count):
        t = u8(); lo = t & 0xF; hi = t >> 4
        if lo == 0: idv = u64()
        elif lo == 1: idv = prevID + 1
        elif lo == 2: idv = prevID + u8()
        elif lo == 3: idv = prevID - u8()
        elif lo == 4: idv = prevID + u16()
        elif lo == 5: idv = prevID - u16()
        elif lo == 6: idv = u16()
        elif lo == 7: idv = u32()
        else: raise ValueError
        tmp = prevOff // ptrSize if (hi & 8) else prevOff
        h7 = hi & 7
        if h7 == 0: off = u64()
        elif h7 == 1: off = tmp + 1
        elif h7 == 2: off = tmp + u8()
        elif h7 == 3: off = tmp - u8()
        elif h7 == 4: off = tmp + u16()
        elif h7 == 5: off = tmp - u16()
        elif h7 == 6: off = u16()
        elif h7 == 7: off = u32()
        else: raise ValueError
        if hi & 8: off *= ptrSize
        out[idv] = off
        prevID, prevOff = idv, off
    return out

idmap = decode_bin(BIN)
off2id = {}
for i, o in idmap.items():
    off2id.setdefault(o, []).append(i)
for rva in (0x22B99F, 0x22BAFC, 0x13C740, 0x111420, 0x196e10, 0x2a6670, 0x29a330, 0x28e250, 0x361640):
    print(f"rva 0x{rva:X} -> ids {off2id.get(rva, '无(非函数起始或未收录)')}")

# 谁调用 0x22B99F（E8 rel32）
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
target = 0x22B99F
print(f"\n===== call 0x{target:X} 的调用者 =====")
for i in range(len(tb)-5):
    if tb[i] == 0xE8:
        rel = struct.unpack_from("<i", tb, i+1)[0]
        if tva + i + 5 + rel == target:
            print(f"  caller at rva=0x{tva+i:X}")
