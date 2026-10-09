# -*- coding: utf-8 -*-
"""调查14：解 MSVC RTTI —— 虚表[-1] -> Complete Object Locator -> TypeDescriptor -> 类名"""
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
def rd_q(rva):
    fo = rva2file(rva)
    if fo is None: return None
    return struct.unpack_from("<Q", exe, fo)[0]
def rd_d(rva):
    fo = rva2file(rva)
    if fo is None: return None
    return struct.unpack_from("<i", exe, fo)[0]
def rd_bd(rva):  # 32 位 RVA 字段（RTTI 内部用 4 字节 RVA）
    fo = rva2file(rva)
    if fo is None: return None
    return struct.unpack_from("<I", exe, fo)[0]

def demangle(name):
    # .?AVClassName@@ -> ClassName ; .?AV...嵌套用 @ 分隔
    if name.startswith(".?AV"):
        n = name[4:]
    elif name.startswith(".?AU"):
        n = name[4:]
    else:
        return name
    if n.endswith("@@"):
        n = n[:-2]
    return n.replace("@", "::")

def rtti_for_vtable(vt_rva, label):
    print(f"--- 虚表 @ 0x{vt_rva:X} ({label}) ---")
    col_rva = rd_q(vt_rva - 8)
    if col_rva is None:
        print("  读取失败"); return
    if not (image_base < col_rva < image_base + 0x3000000):
        print(f"  [-1] = 0x{col_rva:X} 不是指针，无 RTTI（或无 COL）")
        return
    col = col_rva - image_base
    sig = rd_d(col)
    off = rd_d(col + 4)
    cd  = rd_d(col + 8)
    td_rva = rd_bd(col + 12)   # pTypeDescriptor（4字节 RVA，image-relative）
    print(f"  COL @ 0x{col:X}: sig={sig} off={off} cd={cd} td_rva=0x{td_rva:X}")
    td = td_rva
    td_vftable = rd_q(td)
    spare = struct.unpack_from("<Q", exe, rva2file(td + 8))[0]
    raw = b""
    fo = rva2file(td + 16)
    while True:
        b = exe[fo + len(raw)]
        if b == 0: break
        raw += bytes([b])
        if len(raw) > 300: break
    print(f"  TypeDescriptor name: {raw.decode('latin1')}")
    print(f"  >>> 类名: {demangle(raw.decode('latin1'))}")

# 目标虚表
rtti_for_vtable(0x1559930, "含 0x22B990/槽76 的虚表")
