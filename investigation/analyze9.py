# -*- coding: utf-8 -*-
"""调查9：找 0x22B99F funclet 的父函数并反查 ID"""
import struct, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
GAME = r"F:\iniRePather-Skyrim\ModOrganizer\StockGame"
BIN  = GAME + r"\..\mods\插件地址库-Address Library All in One\SKSE\Plugins\version-1-5-97-0.bin"
EXE  = GAME + r"\SkyrimSE.exe"
MD = Cs(CS_ARCH_X86, CS_MODE_64)

# 载入 idmap
exec(open(r"investigate\_idmap_loader.py", encoding="utf-8").read())
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

# 反汇编 0x22B800-0x22B9A0 找 prologue（int3 填充后的 push 序列）
fo = rva2file(0x22B700)
print("===== 向前搜索函数头 =====")
for insn in MD.disasm(exe[fo:fo+0x2A0], 0x22B700):
    if insn.mnemonic == "int3":
        continue
    if insn.mnemonic == "push" and "rbp" in insn.op_str or (insn.mnemonic == "push" and "r15" in insn.op_str):
        print(f"  候选函数头 0x{insn.address:X}: {insn.mnemonic} {insn.op_str}")
    if insn.address > 0x22B9A0: break

# 打印 0x22B8C0 之后全部
print("===== 0x22B8C0-0x22B9A0 =====")
fo = rva2file(0x22B8C0)
for insn in MD.disasm(exe[fo:fo+0xE0], 0x22B8C0):
    print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}")
