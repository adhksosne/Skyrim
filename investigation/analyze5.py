# -*- coding: utf-8 -*-
"""调查5：0x13c740 是否为所有格合成点"""
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
pdata = [s for s in secs if s[0] == ".pdata"][0]
_, pva, pvs, pra, prs = pdata
pb = exe[pra:pra+prs]
def find_func(rva):
    lo, hi = 0, len(pb)//12
    while lo < hi:
        mid = (lo+hi)//2
        s, e = struct.unpack_from("<II", pb, mid*12)
        if rva < s: hi = mid
        elif rva >= e: lo = mid + 1
        else: return s, e
    return None

s, e = find_func(0x13C740)
print(f"func 0x13C740 bounds: 0x{s:X}-0x{e:X} size=0x{e-s:X}")
foff = rva2file(s)
insns = list(MD.disasm(exe[foff:foff+(e-s)], s))
# 只打印含 lea rip 或 call 的行，减小输出
for insn in insns:
    if "rip" in insn.op_str or insn.mnemonic in ("call", "jmp"):
        print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}")
# 计算 lea rip 目标，找指向 0x15A0638 的
for insn in insns:
    if "rip +" in insn.op_str or "rip -" in insn.op_str:
        try:
            disp = int(insn.op_str.split("rip")[1].split("]")[0].replace(" ", ""), 16) if False else None
        except Exception:
            pass
# 直接扫字节
fo = rva2file(s)
blob = exe[fo:fo+(e-s)]
str_rva = 0x15A0638
hits = []
for i in range(len(blob)-7):
    if blob[i] in (0x48,0x49,0x4C) and blob[i+1] == 0x8D and (blob[i+2] & 0xC7) == 0x05:
        disp = struct.unpack_from("<i", blob, i+3)[0]
        if s + i + 7 + disp == str_rva:
            hits.append(s+i)
print(f"lea->0x{str_rva:X} in func: {[hex(h) for h in hits]}")
