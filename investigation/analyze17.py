# -*- coding: utf-8 -*-
"""调查17：读取 24212/17486 使用的全部格式串 + xref 计数 + 0xf9e60 输出类型"""
import struct, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
exec(open(r"investigate\_idmap_loader.py", encoding="utf-8").read())
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
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
def read_cstr(rva, maxlen=120):
    fo = rva2file(rva)
    raw = b""
    while len(raw) < maxlen:
        b = exe[fo + len(raw)]
        if b == 0: break
        raw += bytes([b])
    try:
        return raw.decode("utf-8"), raw
    except UnicodeDecodeError:
        return raw.decode("latin1"), raw

print("=== 24212 (TESNPC 槽76) 中的格式串 ===")
# 361703: lea rdx,[rip+0x123ef2e]; next=36170A -> 0x159F638
for insn_rva, disp, label in [
    (0x361703, 0x123EF2E, "3616EE 分支 al!=0（owner,actor 顺序）"),
    (0x36171F, 0x123EF1A, "3616EE 分支 al==0（actor,owner 顺序）"),
    (0x361892, 0x11F627F, "偷窃/交互修饰分支"),
    (0x361985, 0x11EFB90, "偷窃修饰分支2"),
]:
    tgt = insn_rva + 7 + disp
    s, raw = read_cstr(tgt)
    print(f"  lea@0x{insn_rva:X} -> 0x{tgt:X}: {raw!r}  ({label})")

print("\n=== 17486 (TESObjectCONT 槽76) 的 \"'s \" 独立串 ===")
s, raw = read_cstr(0x1559E84)
print(f"  0x1559E84: {raw!r}")

print("\n=== xref 扫描（lea rip 指向这些串的指令数） ===")
text = [s2 for s2 in secs if s2[0] == ".text"][0]
_, tva, tvs, tra, trs = text
targets = {0x159F638: "%s's %s?(a)", 0x159F640: "strB(b)", 0x1559E84: "'s ", 0x1557B18: "stealFmt", 0x36198C + 0x11EFB90: "stealFmt2"}
data = exe[tra:tra+trs]
hits = {t: [] for t in targets}
i = 0
while i + 7 <= len(data):
    if data[i] in (0x48, 0x4C) and data[i+1] == 0x8D:
        modrm = data[i+2]
        if (modrm >> 6) == 0 and (modrm & 7) == 5:
            disp = struct.unpack_from("<i", data, i+3)[0]
            tgt = tva + i + 7 + disp
            if tgt in hits:
                hits[tgt].append(tva + i)
    i += 1
for t, name in targets.items():
    print(f"  0x{t:X} ({name}): {len(hits[t])} 处 lea -> {[hex(x) for x in hits[t][:8]]}")

print("\n=== 0xf9e60 / 0xf9e90 的 ID ===")
print(f"  0xf9e60 id={off2id.get(0xf9e60)}")
print(f"  0xf9e90 id={off2id.get(0xf9e90)}")

md = Cs(CS_ARCH_X86, CS_MODE_64)
print("\n=== 0xf9e60 反汇编（前 50 条，判断输出参数类型）===")
fo = rva2file(0xf9e60)
cnt = 0
for ins in md.disasm(exe[fo:fo+400], 0xf9e60):
    print(f"  {ins.address:X}: {ins.mnemonic} {ins.op_str}")
    cnt += 1
    if cnt >= 50: break
