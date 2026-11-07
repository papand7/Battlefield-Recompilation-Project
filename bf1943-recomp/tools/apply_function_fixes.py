"""Merges `{ address, size }` lines into the `functions = [...]` list of a
XenonRecomp config, keeping the larger size when an address is listed twice.
Usage: apply_function_fixes.py <config.toml> <entries.txt>"""
import re
import sys

cfg_path, entries_path = sys.argv[1:3]
cfg = open(cfg_path).read()
pat = r"\{ address = 0x([0-9A-Fa-f]+), size = 0x([0-9A-Fa-f]+) \}"
m = re.search(r"functions = \[\n(.*?)\]", cfg, re.S)
funcs = {int(a, 16): int(s, 16) for a, s in re.findall(pat, m.group(1))}
for a, s in re.findall(pat, open(entries_path).read()):
    a, s = int(a, 16), int(s, 16)
    funcs[a] = max(funcs.get(a, 0), s)
body = "".join(f"    {{ address = 0x{a:08X}, size = 0x{s:X} }},\n" for a, s in sorted(funcs.items()))
cfg = cfg[:m.start()] + "functions = [\n" + body + "]" + cfg[m.end():]
open(cfg_path, "w").write(cfg)
print(f"{len(funcs)} functions declared")
