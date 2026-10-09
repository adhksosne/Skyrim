# -*- coding: utf-8 -*-
"""调查4：GetDisplayFullName 的 extra-data 返回分支 + %s's %s 附近字符串簇"""
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

print("===== GetDisplayFullName 0x29642d-0x2964c0 =====")
foff = rva2file(0x29642D)
for insn in MD.disasm(exe[foff:foff+0x90], 0x29642D):
    print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}")

print("\n===== 字符串簇 0x15A0600-0x15A0680 =====")
foff = rva2file(0x15A0600)
chunk = exe[foff:foff+0x80]
parts = chunk.split(b"\0")
off = 0x15A0600
for p in chunk.split(b"\0")[:-1]:
    try:
        s = p.decode("ascii")
        if s:
            print(f"  0x{off:X}: {s!r}")
    except UnicodeDecodeError:
        pass
    off += len(p) + 1

print("\n===== GetDisplayFullName 全函数 return 段 0x2965A0-0x296680 =====")
foff = rva2file(0x2965A0)
for insn in MD.disasm(exe[foff:foff+0xE0], 0x2965A0):
    print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}")
