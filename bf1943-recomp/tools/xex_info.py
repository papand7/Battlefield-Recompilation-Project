"""Dump XEX2 header facts (no decryption). Usage: python -I xex_info.py <default.xex>"""
import struct
import sys

KNOWN = {
    0x000002FF: "RESOURCE_INFO", 0x000003FF: "FILE_FORMAT_INFO", 0x00000405: "DELTA_PATCH_DESCRIPTOR",
    0x000005FF: "BASE_REFERENCE", 0x00004004: "BOUNDING_PATH", 0x00008105: "DEVICE_ID",
    0x00010001: "ORIGINAL_BASE_ADDRESS", 0x00010100: "ENTRY_POINT", 0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES", 0x00018002: "CHECKSUM_TIMESTAMP", 0x00018102: "ENABLED_FOR_CALLCAP",
    0x00018200: "ENABLED_FOR_FASTCAP", 0x000183FF: "ORIGINAL_PE_NAME", 0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO", 0x00020200: "DEFAULT_STACK_SIZE", 0x00020301: "DEFAULT_FILESYSTEM_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE", 0x00028002: "PAGE_HEAP_SIZE_AND_FLAGS", 0x00030000: "SYSTEM_FLAGS",
    0x00040006: "EXECUTION_INFO", 0x00040201: "TITLE_WORKSPACE_SIZE", 0x00040310: "GAME_RATINGS",
    0x00040404: "LAN_KEY", 0x000405FF: "XBOX360_LOGO", 0x000406FF: "MULTIDISC_MEDIA_IDS",
    0x000407FF: "ALTERNATE_TITLE_IDS", 0x00040801: "ADDITIONAL_TITLE_MEMORY", 0x00E10402: "EXPORTS_BY_NAME",
}


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def main(path):
    d = open(path, "rb").read()
    print(f"file: {path}  size: {len(d)}")
    magic = d[0:4]
    print(f"magic: {magic!r}")
    if magic != b"XEX2":
        print("not XEX2")
        return
    flags, hdr_size, _res, sec_off, nopt = struct.unpack_from(">IIIII", d, 4)
    print(f"module_flags: 0x{flags:08X}  header_size: 0x{hdr_size:X}  security_offset: 0x{sec_off:X}  opt_headers: {nopt}")
    opts = {}
    for i in range(nopt):
        key, val = struct.unpack_from(">II", d, 0x18 + i * 8)
        opts[key] = val
        print(f"  opt 0x{key:08X} {KNOWN.get(key, '?'):<30} value/offset 0x{val:08X}")

    # security info
    s = sec_off
    img_size = u32(d, s + 4)
    load_addr = u32(d, s + 0x110)
    print(f"security: image_size 0x{img_size:X}  load_address 0x{load_addr:08X}  image_flags 0x{u32(d, s + 0x10C):08X}")
    print(f"          allowed_media 0x{u32(d, s + 0x17C):08X}  game_regions 0x{u32(d, s + 0x178):08X}")
    nsect = u32(d, s + 0x180)
    print(f"          page descriptors: {nsect}")
    kinds = {1: "code", 2: "data", 3: "readonly"}
    # each descriptor: (page_count << 4 | type), 64 KiB pages; coalesce runs of the same type
    addr, runs = load_addr, []
    for i in range(nsect):
        info = u32(d, s + 0x184 + i * 24)
        kind, size = kinds.get(info & 0xF, str(info & 0xF)), (info >> 4) * 0x10000
        if runs and runs[-1][0] == kind:
            runs[-1][2] += size
        else:
            runs.append([kind, addr, size])
        addr += size
    for kind, a, sz in runs:
        print(f"            {kind:<8} 0x{a:08X}-0x{a + sz:08X} (0x{sz:X})")

    if 0x00010100 in opts:
        print(f"entry_point: 0x{opts[0x00010100]:08X}")
    if 0x00010201 in opts:
        print(f"image_base (opt): 0x{opts[0x00010201]:08X}")
    if 0x00010001 in opts:
        print(f"original_base: 0x{opts[0x00010001]:08X}")
    if 0x00020200 in opts:
        print(f"default_stack_size: 0x{opts[0x00020200]:X}")
    if 0x00030000 in opts:
        print(f"system_flags: 0x{opts[0x00030000]:08X}")

    if 0x00040006 in opts:
        o = opts[0x00040006]
        media_id, ver, base_ver, title_id = struct.unpack_from(">IIII", d, o)
        plat, etype, discn, discc = d[o + 16:o + 20]

        def v(x):
            return f"{x >> 28}.{(x >> 24) & 0xF}.{(x >> 8) & 0xFFFF}.{x & 0xFF}"
        print(f"execution: title_id {title_id:08X} media_id {media_id:08X} version {v(ver)} base_version {v(base_ver)} disc {discn}/{discc}")

    if 0x000183FF in opts:
        o = opts[0x000183FF]
        n = u32(d, o)
        print(f"original_pe_name: {d[o + 4:o + n].split(b'\0')[0].decode(errors='replace')}")

    if 0x000003FF in opts:
        o = opts[0x000003FF]
        info_size = u32(d, o)
        enc, comp = u16(d, o + 4), u16(d, o + 6)
        print(f"file_format: encryption {enc} ({['none', 'normal(AES)'][enc] if enc < 2 else '?'})  "
              f"compression {comp} ({['none', 'basic', 'normal(LZX)', 'delta'][comp] if comp < 4 else '?'})")
        if comp == 2:
            print(f"  LZX window_size 0x{u32(d, o + 8):X}  first block size 0x{u32(d, o + 12):X}")
        elif comp == 1:
            nb = (info_size - 8) // 8
            print(f"  basic blocks: {nb}")

    if 0x000200FF in opts:
        o = opts[0x000200FF]
        n = (u32(d, o) - 4) // 16
        print(f"static_libraries ({n}):")
        for i in range(n):
            e = o + 4 + i * 16
            name = d[e:e + 8].split(b"\0")[0].decode(errors="replace")
            a, b_, c, fl = struct.unpack_from(">HHHH", d, e + 8)
            print(f"  {name:<10} {a}.{b_}.{c}.{fl & 0x7FFF}")

    if 0x000103FF in opts:
        o = opts[0x000103FF]
        _size, strtab_size, nlibs = struct.unpack_from(">III", d, o)
        strs = d[o + 12:o + 12 + strtab_size].split(b"\0")
        names = [x.decode() for x in strs if x]
        print(f"import_libraries ({nlibs}):")
        p = o + 12 + strtab_size
        for i in range(nlibs):
            lsize = u32(d, p)
            name_idx = u16(d, p + 0x24 - 2) if False else None
            # layout: size(4) next_import_digest(20) id(4) version(4) version_min(4) name_index(2) count(2)
            ver = u32(d, p + 28)
            vmin = u32(d, p + 32)
            name_index, count = struct.unpack_from(">HH", d, p + 36)

            def vv(x):
                return f"{x >> 28}.{(x >> 24) & 0xF}.{(x >> 8) & 0xFFFF}.{x & 0xFF}"
            nm = names[name_index] if name_index < len(names) else f"#{name_index}"
            print(f"  {nm:<14} version {vv(ver)} (min {vv(vmin)})  import records {count}")
            p += lsize


if __name__ == "__main__":
    for a in sys.argv[1:]:
        main(a)
        print()
