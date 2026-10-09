# -*- coding: utf-8 -*-
"""调查16：反汇编 GetDisplayFullName(19354 @ 0x2961F0) 与 TESNPC::24212(0x361640)
验证 19354 是否经 vtable 槽76 (call [rax+0x260]) 调用 24212"""
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

# .pdata 找函数边界
def func_bounds(rva):
    # .pdata: RUNTIME_FUNCTION { BeginRVA, EndRVA, UnwindRVA } * n
    for name, va, vs, ra, rs in secs:
        if name == ".pdata":
            n = rs // 12
            lo, hi = 0, n - 1
            while lo <= hi:
                mid = (lo + hi) // 2
                b, e, u = struct.unpack_from("<III", exe, ra + mid * 12)
                if rva < b: hi = mid - 1
                elif rva >= e: lo = mid + 1
                else: return b, e
    return None

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False

def disasm(rva, label, max_insns=400):
    b, e = func_bounds(rva)
    print(f"\n=== {label} @ 0x{rva:X} (id={off2id.get(rva)}) 范围 [0x{b:X}, 0x{e:X}) 大小 {e-b} ===")
    fo = rva2file(rva)
    code = exe[fo:fo + (e - rva)]
    for ins in md.disasm(code, rva):
        print(f"  {ins.address:X}: {ins.mnemonic} {ins.op_str}")
        max_insns -= 1
        if max_insns <= 0:
            print("  ... (截断)")
            break

disasm(0x2961F0, "GetDisplayFullName (ID 19354)")
disasm(0x196E10, "疑似 TESForm::GetFullNameWithOwner (ID 14548)")
disasm(0x361640, "TESNPC::槽76 (ID 24212)", 500)
