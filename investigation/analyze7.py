# -*- coding: utf-8 -*-
"""调查7：0x22BAAD 函数 - 's 合成器确认"""
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
s, e = find_func(0x22BAAD)
print(f"合成器函数边界: 0x{s:X}-0x{e:X} size=0x{e-s:X}")
fo = rva2file(s)
for insn in MD.disasm(exe[fo:fo+(e-s)], s):
    note = ""
    if insn.mnemonic == "lea" and "rip" in insn.op_str:
        try:
            disp = int(insn.op_str.split("+ ")[-1].rstrip("]"), 16)
            tgt = insn.address + insn.size + disp
            note = f"   ; ->0x{tgt:X}"
            if 0x1509000 <= tgt < 0x1DB0000:  # .rdata
                fo2 = rva2file(tgt)
                end = exe.index(b"\0", fo2)
                s2 = exe[fo2:end]
                if len(s2) < 64:
                    try: note += f" str={s2.decode('ascii')!r}"
                    except Exception: note += f" bytes={s2[:24].hex(' ')}"
        except Exception:
            pass
    print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}{note}")
