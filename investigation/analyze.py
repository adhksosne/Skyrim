# -*- coding: utf-8 -*-
"""
静态调查脚本：
1. 解码 Address Library version-1-5-97-0.bin (SSEv1)，查 ID 19354 (TESObjectREFR::GetDisplayFullName) 的 RVA
2. 反汇编该函数 prologue，判断前 5 字节是否可安全重定位（SKSE trampoline write_branch<5> 可行性）
3. 全 exe 扫描 "'s" 相关 ASCII 字符串，并定位 .text 中对它们的 rip-relative 引用
"""
import struct, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

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
    fmt = i32()
    assert fmt == 1, f"format={fmt}"
    version = [i32() for _ in range(4)]
    nameLen = i32(); pos += nameLen
    ptrSize = i32()
    count = i32()
    out = {}
    prevID = 0; prevOff = 0
    for _ in range(count):
        t = u8()
        lo = t & 0xF; hi = t >> 4
        if lo == 0: idv = u64()
        elif lo == 1: idv = prevID + 1
        elif lo == 2: idv = prevID + u8()
        elif lo == 3: idv = prevID - u8()
        elif lo == 4: idv = prevID + u16()
        elif lo == 5: idv = prevID - u16()
        elif lo == 6: idv = u16()
        elif lo == 7: idv = u32()
        else: raise ValueError("bad id type")
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
        else: raise ValueError("bad off type")
        if hi & 8: off *= ptrSize
        out[idv] = off
        prevID, prevOff = idv, off
    return version, count, out

version, count, idmap = decode_bin(BIN)
print(f"[bin] version={version} entries={count}")
for i in (19354, 19385):
    print(f"[bin] id {i} -> RVA 0x{idmap.get(i, -1):X}")

exe = open(EXE, "rb").read()
# 解析 PE：ImageBase 与节表
pe_off = struct.unpack_from("<I", exe, 0x3C)[0]
machine, nsec = struct.unpack_from("<HH", exe, pe_off + 4)[0:2]
opt_size = struct.unpack_from("<H", exe, pe_off + 20)[0]
opt_off = pe_off + 24
magic = struct.unpack_from("<H", exe, opt_off)[0]
image_base = struct.unpack_from("<Q", exe, opt_off + 24)[0]
print(f"[pe] ImageBase=0x{image_base:X} sections={nsec}")
secs = []
for i in range(nsec):
    so = opt_off + opt_size + i * 40
    name = exe[so:so+8].rstrip(b"\0").decode()
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", exe, so + 8)
    secs.append((name, vaddr, vsize, raddr, rsize))
    print(f"[pe] {name:8s} VA=0x{vaddr:06X} VS=0x{vsize:06X} RAW=0x{raddr:06X} RS=0x{rsize:06X}")

def rva2file(rva):
    for name, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ra + (rva - va)
    return None

def file2rva(off):
    for name, va, vs, ra, rs in secs:
        if ra <= off < ra + rs:
            return va + (off - ra)
    return None

MD = Cs(CS_ARCH_X86, CS_MODE_64)

# --- 1) GetDisplayFullName prologue ---
target_rva = idmap[19354]
foff = rva2file(target_rva)
print(f"\n[func] ID 19354 GetDisplayFullName RVA=0x{target_rva:X} file=0x{foff:X}")
code = exe[foff:foff+96]
for insn in list(MD.disasm(code, target_rva))[:20]:
    print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}   ; bytes={' '.join(f'{b:02X}' for b in insn.bytes)}")

# --- 2) 扫描字符串 ---
print("\n[strings] 含 \"'s\" 的 ASCII 串：")
needles = [b"'s ", b"%s's", b"'s\x00"]
found = []
for nd in needles:
    start = 0
    while True:
        i = exe.find(nd, start)
        if i < 0: break
        start = i + 1
        # 前后各取 24 字节上下文，要求周围是可打印 ASCII（排除误报）
        ctx = exe[max(0,i-24):i+28]
        if all(31 < c < 127 or c == 0 for c in ctx):
            rva = file2rva(i)
            if rva is not None:
                s = ctx.split(b"\0")
                frag = max(s, key=len).decode("ascii", "replace")
                found.append((nd, i, rva, frag))
for nd, i, rva, frag in found[:40]:
    print(f"  file=0x{i:08X} rva=0x{rva:08X} {nd!r} ctx=…{frag}…")
print(f"  total={len(found)}")

# --- 3) 定位 .text 中对这些字符串的 lea rip+disp32 引用 ---
text = [s for s in secs if s[0] == ".text"][0]
tname, tva, tvs, tra, trs = text
str_rvas = sorted(set(rva for _, _, rva, _ in found))
print("\n[xrefs] .text 中的 rip-relative 引用：")
refs = []
tb = exe[tra:tra+trs]
n = len(tb)
for i in range(n - 7):
    b0 = tb[i]
    if b0 in (0x48, 0x4C, 0x49) and tb[i+1] == 0x8D:
        modrm = tb[i+2]
        if (modrm & 0xC7) == 0x05:  # mod=00 rm=101 => [rip+disp32]
            disp = struct.unpack_from("<i", tb, i+3)[0]
            insn_rva = tva + i
            target = insn_rva + 7 + disp
            if target in str_rvas:
                refs.append((insn_rva, target))
for insn_rva, target in refs:
    print(f"  code rva=0x{insn_rva:08X} -> str rva=0x{target:08X}  (func19354=0x{target_rva:X}, delta={insn_rva-target_rva:+#x})")

# 反汇编每个引用点附近
for insn_rva, target in refs[:8]:
    f_off = rva2file(insn_rva - 32)
    print(f"\n[disasm] 引用点 0x{insn_rva:X} 附近：")
    for insn in list(MD.disasm(exe[f_off:f_off+64], insn_rva - 32))[:12]:
        mark = " <== 引用" if insn.address <= insn_rva < insn.address + insn.size else ""
        print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}{mark}")
