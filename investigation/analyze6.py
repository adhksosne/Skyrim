# -*- coding: utf-8 -*-
"""调查6：独立 's 字符串 + 0x13CC20 / 0x13CE20 内容"""
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
def file2rva(off):
    for name, va, vs, ra, rs in secs:
        if ra <= off < ra + rs:
            return va + (off - ra)
    return None

# 1) 所有 b"'s" 出现点且后随 \0 或空格（standalone 短串）
print("===== 独立 \"'s\" 短字符串 =====")
short_strs = []
start = 0
while True:
    i = exe.find(b"'s", start)
    if i < 0: break
    start = i + 1
    # 检查是否字符串开头
    if i > 0 and exe[i-1] != 0: continue
    # 提取到 \0
    j = exe.index(b"\0", i)
    s = exe[i:j]
    if len(s) <= 8:
        r = file2rva(i)
        if r: short_strs.append((r, s.decode("latin1")))
for r, s in short_strs:
    print(f"  rva=0x{r:X}: {s!r}")

# 2) 对这些短串的 lea xref
text = [s for s in secs if s[0] == ".text"][0]
_, tva, tvs, tra, trs = text
tb = exe[tra:tra+trs]
targets = set(r for r, _ in short_strs)
print("===== 对独立 \"'s\" 串的 .text lea xref =====")
for i in range(len(tb)-7):
    if tb[i] in (0x48,0x49,0x4C) and tb[i+1] == 0x8D and (tb[i+2] & 0xC7) == 0x05:
        disp = struct.unpack_from("<i", tb, i+3)[0]
        tgt = tva + i + 7 + disp
        if tgt in targets:
            print(f"  code rva=0x{tva+i:X} -> str rva=0x{tgt:X} {dict(short_strs)[tgt]!r}")

# 3) 反汇编 0x13CC20 与 0x13CE20 的 lea rip 目标
def dump_calls(func, size):
    print(f"===== 0x{func:X} lea/call =====")
    fo = rva2file(func)
    for insn in MD.disasm(exe[fo:fo+size], func):
        if insn.mnemonic in ("call",) or "rip" in insn.op_str:
            extra = ""
            if "rip" in insn.op_str and insn.mnemonic == "lea":
                try:
                    disp = int(insn.op_str.split("+ ")[-1].rstrip("]").replace(" ", ""), 16)
                    tgt = insn.address + insn.size + disp
                    extra = f"   ; ->0x{tgt:X}"
                except Exception:
                    pass
            print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}{extra}")
dump_calls(0x13CC20, 0x120)
dump_calls(0x13CE20, 0x100)
