# -*- coding: utf-8 -*-
"""调查12：0x22B990 所在虚表的边界 + 谁引用该虚表（lea rip）"""
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
def file2rva(off):
    for name, va, vs, ra, rs in secs:
        if ra <= off < ra + rs:
            return va + (off - ra)
    return None
def sec_of_rva(rva):
    for name, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return name
    return None

ENTRY_RVA = 0x1559B90  # 指向 0x22B990 的槽
# 1) 向上/向下转储 .rdata 中相邻指针，看虚表布局
print("=== .rdata 周边指针表（前后各 8 槽）===")
base_slot = ENTRY_RVA - 8 * 8
for k in range(-8, 9):
    rva = ENTRY_RVA + k * 8
    fo = rva2file(rva)
    if fo is None: continue
    val = struct.unpack_from("<Q", exe, fo)[0]
    tag = "  <== 含 0x22B990" if k == 0 else ""
    rva_val = val - image_base if image_base < val < image_base + 0x2000000 else 0
    print(f"  slot rva=0x{rva:X}: 0x{val:016X}" + (f" (rva=0x{rva_val:X})" if rva_val else "") + tag)

# 2) 找虚表起点：从 ENTRY_RVA 向上扫描，直到遇到非代码指针（通常 RTTI/collapse），
#    简单启发：连续的 .text 范围内指针视为同一虚表
vt_start = ENTRY_RVA
while True:
    prev = vt_start - 8
    fo = rva2file(prev)
    if fo is None: break
    val = struct.unpack_from("<Q", exe, fo)[0]
    rva_val = val - image_base
    if sec_of_rva(rva_val) in (".text",) and rva_val > 0x1000:
        vt_start = prev
    else:
        break
print(f"\n向上扫描得到虚表起点 rva=0x{vt_start:X}（含 0x22B990 的槽偏移 {ENTRY_RVA - vt_start} 字节 = 槽 {(ENTRY_RVA - vt_start)//8}）")

# 3) 扫 .text 中 lea reg,[rip+disp32] 指向 [vt_start, ENTRY_RVA] 区间的指令
print(f"\n=== .text 中引用虚表 [0x{vt_start:X}..0x{ENTRY_RVA:X}] 的 lea rip 指令 ===")
text = [s for s in secs if s[0] == ".text"][0]
_, tva, tvs, tra, trs = text
hits = []
i = 0
data = exe[tra:tra+trs]
target_lo, target_hi = vt_start, ENTRY_RVA
while i + 7 <= len(data):
    # 48 8D xx /r  或 4C 8D xx /r：lea r64
    if data[i] in (0x48, 0x4C) and i+1 < len(data) and data[i+1] == 0x8D:
        modrm = data[i+2]
        mod = modrm >> 6
        rm = modrm & 7
        if mod == 0 and rm == 5:  # rip-relative
            if i + 7 <= len(data):
                disp = struct.unpack_from("<i", data, i+3)[0]
                insn_rva = tva + i
                tgt = insn_rva + 7 + disp
                if target_lo <= tgt <= target_hi:
                    hits.append((insn_rva, tgt))
        i += 1
    else:
        i += 1
for insn_rva, tgt in hits[:40]:
    fid = off2id.get(insn_rva)
    slot_off = tgt - vt_start
    print(f"  0x{insn_rva:X} -> vtable+0x{slot_off:X} (id={fid})")
print(f"共 {len(hits)} 处")
