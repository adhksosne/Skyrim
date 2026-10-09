# -*- coding: utf-8 -*-
"""调查2：GetDisplayFullName 完整逻辑 + STRINGS/GMST 中的所有格格式串 + 引用函数归属"""
import struct, sys, io, os, glob
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

# --- A) GetDisplayFullName 完整反汇编（0x400 字节） ---
print("===== A) GetDisplayFullName @0x2961F0 =====")
foff = rva2file(0x2961F0)
code = exe[foff:foff+0x400]
for insn in list(MD.disasm(code, 0x2961F0)):
    print(f"  0x{insn.address:X}: {insn.mnemonic} {insn.op_str}")
    if insn.address > 0x2961F0 + 0x180: break

# --- C) .pdata 找包含给定 RVA 的函数边界 ---
print("\n===== C) .pdata 函数边界 =====")
pdata = [s for s in secs if s[0] == ".pdata"][0]
_, pva, pvs, pra, prs = pdata
pdata_bytes = exe[pra:pra+prs]
def find_func(rva):
    lo, hi = 0, len(pdata_bytes)//12
    while lo < hi:
        mid = (lo+hi)//2
        start, end = struct.unpack_from("<II", pdata_bytes, mid*12)
        if rva < start: hi = mid
        elif rva >= end: lo = mid + 1
        else: return start, end
    return None
for r in (0x361703, 0x2DF012, 0xA0C0F4):
    r_ = find_func(r)
    print(f"  code 0x{r:X} in func 0x{r_[0]:X}-0x{r_[1]:X} (size 0x{r_[1]-r_[0]:X})" if r_ else f"  code 0x{r:X} not found")

# --- B) STRINGS 文件解析 ---
print("\n===== B) STRINGS 文件搜索 possessive 格式 =====")
def parse_strings(path):
    data = open(path, "rb").read()
    n, ds = struct.unpack_from("<II", data, 0)
    ids = struct.unpack_from(f"<{n}I", data, 8)
    offs = struct.unpack_from(f"<{n}I", data, 8 + 4*n)
    table = 8 + 8*n
    out = {}
    for i in range(n):
        s = data[table+offs[i]:data.index(b"\0", table+offs[i])]
        out[ids[i]] = s.decode("utf-8", "replace")
    return out

def parse_dlstrings(path):
    data = open(path, "rb").read()
    n, ds = struct.unpack_from("<II", data, 0)
    ids = struct.unpack_from(f"<{n}I", data, 8)
    pos = 8 + 4*n
    lens = struct.unpack_from(f"<{n}I", data, pos)
    table = pos + 4*n
    out = {}
    for i in range(n):
        s = data[table+offs_probe] if False else data[table + sum(lens[:i]):table + sum(lens[:i+1])]
        out[ids[i]] = s.decode("utf-8", "replace")
    return out

for f in glob.glob(GAME + r"\Data\Strings\skyrim_chinese.STRINGS") + glob.glob(GAME + r"\Data\Strings\skyrim_english.STRINGS"):
    tbl = parse_strings(f)
    hits = [(i, v) for i, v in tbl.items() if "%s's" in v or "'s %s" in v]
    print(f"  {os.path.basename(f)}: {len(tbl)} 条, possessive 命中 {len(hits)} 条")
    for i, v in hits[:20]:
        print(f"    id=0x{i:08X}: {v!r}")

# 也搜中文文件里含 's 的字符串（看是否被原样保留）
f = GAME + r"\Data\Strings\skyrim_chinese.STRINGS"
tbl = parse_strings(f)
hits2 = [(i, v) for i, v in tbl.items() if "'s " in v]
print(f"  {os.path.basename(f)}: 含 \"'s \" 的条目 {len(hits2)} 条，示例：")
for i, v in hits2[:10]:
    print(f"    id=0x{i:08X}: {v!r}")
# 编码确认：取一条含中文的字符串打印 UTF-8 字节
for i, v in tbl.items():
    if any(ord(c) > 0x4E00 for c in v):
        print(f"  编码样本 id=0x{i:08X}: {v!r} utf8_bytes={v.encode('utf-8')[:24].hex(' ')}")
        break
