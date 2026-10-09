# -*- coding: utf-8 -*-
"""调查3：0x361640 函数全貌 + GetDisplayFullName 的 0x296400 段 + 0x361640 是否控制台命令"""
import struct, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

GAME = r"F:\iniRePather-Skyrim\ModOrganizer\StockGame"
EXE  = GAME + r"\SkyrimSE.exe"
MD = Cs(CS_ARCH_X86, CS_MODE_64)
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
def rva2file(rva):
    for name, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ra + (rva - va)
    return None

def disasm(rva, size, label):
    print(f"===== {label} @0x{rva:X} =====")
    foff = rva2file(rva)
    for insn in MD.disasm(exe[foff:foff+size], rva):
        print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}")

disasm(0x361640, 0x3C3, "含 %s's %s 的函数")
disasm(0x296400, 0xE0, "GetDisplayFullName 尾段")
