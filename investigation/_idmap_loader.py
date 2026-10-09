# 共享：解码地址库到 idmap
def _load_idmap():
    import struct
    GAME = r"F:\iniRePather-Skyrim\ModOrganizer\StockGame"
    BIN  = GAME + r"\..\mods\插件地址库-Address Library All in One\SKSE\Plugins\version-1-5-97-0.bin"
    data = open(BIN, "rb").read()
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
    fmt = i32(); assert fmt == 1
    version = [i32() for _ in range(4)]
    nameLen = i32(); pos += nameLen
    ptrSize = i32(); count = i32()
    out = {}
    prevID = 0; prevOff = 0
    for _ in range(count):
        t = u8(); lo = t & 0xF; hi = t >> 4
        if lo == 0: idv = u64()
        elif lo == 1: idv = prevID + 1
        elif lo == 2: idv = prevID + u8()
        elif lo == 3: idv = prevID - u8()
        elif lo == 4: idv = prevID + u16()
        elif lo == 5: idv = prevID - u16()
        elif lo == 6: idv = u16()
        elif lo == 7: idv = u32()
        else: raise ValueError
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
        else: raise ValueError
        if hi & 8: off *= ptrSize
        out[idv] = off
        prevID, prevOff = idv, off
    return out
idmap = _load_idmap()
off2id = {}
for _i, _o in idmap.items():
    off2id.setdefault(_o, []).append(_i)
