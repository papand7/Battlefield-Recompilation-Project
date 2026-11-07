"""Make recompiled guest functions inlinable (run at CMake configure time).

XenonRecomp emits every function as a weak, noinline alias (sub_X) of its body
(__imp__sub_X), so the runtime can replace any of them. That blocks inlining
even inside one source file. Only the functions runtime/ actually replaces
(GAME_PATCH(addr) or PPC_FUNC(sub_X)) need that; this rewrites every other one
as a plain definition of sub_X, and restores the weak form for functions that
become patched later. Files are only written when they change, so a configure
without patch changes rebuilds nothing.

usage: strong_funcs.py <generated/ppc> <runtime> [--all-weak]
(--all-weak restores XenonRecomp's form for every function.)
"""
import pathlib
import re
import sys

ppc_dir, runtime_dir = map(pathlib.Path, sys.argv[1:3])
all_weak = '--all-weak' in sys.argv[3:]

patched = set()
for src in list(runtime_dir.glob('*.cpp')) + list(runtime_dir.glob('*.h')):
    text = src.read_text(encoding='utf-8', errors='replace')
    patched.update(re.findall(r'GAME_PATCH\(([0-9A-F]{8})\)', text))
    patched.update(re.findall(r'PPC_FUNC\(sub_([0-9A-F]{8})\)', text))

WEAK = re.compile(r'__attribute__\(\(alias\("__imp__sub_([0-9A-F]{8})"\)\)\) PPC_WEAK_FUNC\(sub_\1\);\n'
                  r'PPC_FUNC_IMPL\(__imp__sub_\1\) \{')
STRONG = re.compile(r'/\*strong\*/ PPC_FUNC\(sub_([0-9A-F]{8})\) \{')


def to_strong(m):
    a = m.group(1)
    return m.group(0) if all_weak or a in patched else f'/*strong*/ PPC_FUNC(sub_{a}) {{'


def to_weak(m):
    a = m.group(1)
    if not all_weak and a not in patched:
        return m.group(0)
    return (f'__attribute__((alias("__imp__sub_{a}"))) PPC_WEAK_FUNC(sub_{a});\n'
            f'PPC_FUNC_IMPL(__imp__sub_{a}) {{')


changed = 0
for f in sorted(ppc_dir.glob('ppc_recomp.*.cpp')):
    text = f.read_text(encoding='utf-8')
    new = STRONG.sub(to_weak, WEAK.sub(to_strong, text))
    if new != text:
        f.write_text(new, encoding='utf-8', newline='\n')
        changed += 1
print(f'strong_funcs: {len(patched)} patched functions kept weak, {changed} files rewritten')
