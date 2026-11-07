"""Locate MSVC/XDK register save/restore helpers and setjmp/longjmp candidates
in a flat Xbox 360 memory image dumped by tools/xex_dump.
Usage: python -I find_xenon_helpers.py <image.bin> [base=0x82000000] [text_start text_end]
"""
import struct
import sys

PATTERNS = {
    # name: (first words, tail words that must follow within the helper)
    "savegprlr_14": [0xF9C1FF68, 0xF9E1FF70],                 # std r14,-0x98(r1); std r15,-0x90(r1)
    "restgprlr_14": [0xE9C1FF68, 0xE9E1FF70],                 # ld  r14,-0x98(r1); ld  r15,-0x90(r1)
    "savefpr_14":   [0xD9CCFF70, 0xD9ECFF78],                 # stfd f14,-0x90(r12); stfd f15,-0x88(r12)
    "restfpr_14":   [0xC9CCFF70, 0xC9ECFF78],                 # lfd  f14,-0x90(r12); lfd  f15,-0x88(r12)
    "savevmx_14":   [0x3960FEE0, 0x7DCB61CE],                 # li r11,-0x120; stvx v14,r11,r12
    "restvmx_14":   [0x3960FEE0, 0x7DCB60CE],                 # li r11,-0x120; lvx  v14,r11,r12
    "savevmx_64":   [0x3960FC00, 0x100B61CB],                 # li r11,-0x400; stvx128 v64,r11,r12
    "restvmx_64":   [0x3960FC00, 0x100B60CB],                 # li r11,-0x400; lvx128  v64,r11,r12
}


def main():
    img = open(sys.argv[1], "rb").read()
    base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x82000000
    t0 = int(sys.argv[3], 0) - base if len(sys.argv) > 3 else 0
    t1 = int(sys.argv[4], 0) - base if len(sys.argv) > 4 else len(img)
    n = (t1 - t0) // 4
    words = struct.unpack_from(f">{n}I", img, t0)

    def addr(i):
        return base + t0 + i * 4

    for name, pat in PATTERNS.items():
        hits = [addr(i) for i in range(n - len(pat)) if words[i] == pat[0] and list(words[i:i + len(pat)]) == pat]
        print(f"{name:<14} " + (", ".join(f"0x{h:08X}" for h in hits) if hits else "NOT FOUND"))

    # setjmp: stores r14..r31 relative to r3 (std rN,d(r3) => 0xF8030000 | rN<<21) in sequence.
    # longjmp: loads r14..r31 relative to r3 (ld rN,d(r3) => 0xE8030000 | rN<<21).
    def seq_hits(opbase, label):
        out = []
        for i in range(n - 18):
            w = words[i]
            if (w & 0xFC1F0003) == opbase and ((w >> 21) & 31) == 14:
                ok = all((words[i + k] & 0xFC1F0003) == opbase and ((words[i + k] >> 21) & 31) == 14 + k for k in range(1, 18))
                if ok:
                    # walk back to function start (previous blr + 4) as a hint
                    j = i
                    while j > 0 and words[j - 1] != 0x4E800020 and i - j < 64:
                        j -= 1
                    out.append((addr(i), addr(j)))
        print(f"{label}: " + (", ".join(f"r14-31 seq at 0x{a:08X} (func start ~0x{s:08X})" for a, s in out) if out else "NOT FOUND"))

    seq_hits(0xF8030000, "setjmp candidates (std r14..r31,d(r3))")
    seq_hits(0xE8030000, "longjmp candidates (ld r14..r31,d(r3))")


if __name__ == "__main__":
    main()
