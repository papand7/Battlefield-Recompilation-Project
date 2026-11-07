"""Finds functions XenonRecomp cut short (branch or switch targets past the end
show up as `// ERROR: 0x...`) and prints `{ address, size }` entries that extend
each one over its targets, for the config's `functions` list.

Usage: fix_truncated_functions.py <generated/ppc dir>
Run XenonRecomp again afterwards; repeat until no ERROR lines are left.
"""
import pathlib
import re
import sys

ppc = pathlib.Path(sys.argv[1])
starts = sorted(int(a, 16) for a in re.findall(r"\{ 0x([0-9A-F]+), sub_",
                                               (ppc / "ppc_func_mapping.cpp").read_text()))
code_end = max(starts) + 0x10000
targets = {}
for f in ppc.glob("ppc_recomp.*.cpp"):
    fn = None
    for line in f.read_text().splitlines():
        m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)", line)
        if m:
            fn = int(m.group(1), 16)
            continue
        m = re.search(r"// ERROR:? (?:0x)?([0-9A-F]{8})", line)
        if m and fn is not None:
            targets.setdefault(fn, set()).add(int(m.group(1), 16))

import bisect
out = []
backward = []
for fn, ts in sorted(targets.items()):
    if min(ts) < fn:
        backward.append(fn)  # a fragment of an earlier function; extending that one covers it
        continue
    need = max(ts) + 4
    # End at the next function start at or after the furthest target.
    i = bisect.bisect_left(starts, need)
    end = starts[i] if i < len(starts) else need
    if end == need and need in starts:
        end = need
    # A target that is itself a function start still has to be inside: step past it.
    if max(ts) in starts:
        j = bisect.bisect_right(starts, max(ts))
        end = starts[j] if j < len(starts) else need
    out.append((fn, end - fn))
for fn, size in out:
    print(f"    {{ address = 0x{fn:08X}, size = 0x{size:X} }},")
print(f"# {len(out)} functions extended, {len(backward)} with targets before their start: "
      + " ".join(f"{b:08X}" for b in backward[:20]), file=sys.stderr)
