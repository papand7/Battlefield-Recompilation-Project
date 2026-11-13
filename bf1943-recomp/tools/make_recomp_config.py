"""Makes the XenonRecomp config for Battlefield 1943 (v1.06) or, with --title
bfbc1, Battlefield: Bad Company (the US disc) from the player's own
default.xex, for a build from a fresh clone (build_linux.sh): the repo keeps no
config, switch tables or other game-derived data (generated/ is not tracked).

  python3 -I tools/make_recomp_config.py --xex <default.xex> --gen <generated dir>
      --analyse <XenonAnalyse> --recomp <XenonRecomp> --xex-dump <xex_dump>
      --ppc-context <thirdparty/XenonRecomp/XenonUtils/ppc_context.h> [--title bfbc1]

The same steps the hand-made configs went through (docs/ARCHITECTURE.md):
  1. xex_dump: the flat memory image (analysis/default_v106_image.bin,
     Bad Company's analysis/bfbc1_image.bin);
  2. XenonAnalyse: jump tables (analysis/<title>_switch_tables.toml), plus the
     ones it misses (tools/find_offset_switches.py), together in
     analysis/<title>_switch_tables_all.toml;
  3. the register save/restore helpers and setjmp/longjmp, by their code
     (the patterns of tools/find_xenon_helpers.py);
  4. XenonRecomp, then tools/fix_truncated_functions.py extends the functions
     cut short (`// ERROR` targets) in the config's `functions` list; repeated
     until no `// ERROR` is left.
Writes <gen>/<title>_config.toml and leaves <gen>/ppc as the last run made it.
Prints "progress config <done> <total>" lines.
"""
import argparse
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from find_xenon_helpers import PATTERNS  # noqa: E402

BASE = 0x82000000
MAX_PASSES = 12
STEPS = 4 + MAX_PASSES

# Per game: the header's first line and the memory image's name.
TITLES = {
    'bf1943': ('Battlefield 1943 (XBLA, title 5841097E) -- XenonRecomp config, v1.0.6 XEX.',
               'default_v106_image.bin'),
    'bfbc1': ('Battlefield: Bad Company (Xbox 360, title 454107F9) -- XenonRecomp config, US disc XEX.',
              'bfbc1_image.bin'),
}

HEADER = '''# {title}
# Made by tools/make_recomp_config.py from your own default.xex.
# Paths are relative to this file (generated/).

[main]
file_path = "{xex}"
out_directory_path = "ppc"
switch_table_file_path = "analysis/{name}_switch_tables_all.toml"  # XenonAnalyse + tools/find_offset_switches.py

skip_lr = true
skip_msr = true
ctr_as_local = true
xer_as_local = true
reserved_as_local = true
cr_as_local = true
non_argument_as_local = false  # SEH funclets get the frame in r12
non_volatile_as_local = false  # funclets read the parent's r14-r31

'''


def progress(done):
    print(f'progress config {done} {STEPS}', flush=True)


def run(args, cwd=None, log=None):
    r = subprocess.run([str(a) for a in args], cwd=cwd, capture_output=True, text=True, errors='replace')
    if log:
        pathlib.Path(log).write_text(r.stdout + r.stderr)
    if r.returncode != 0:
        tail = '\n'.join((r.stdout + r.stderr).strip().splitlines()[-8:])
        sys.exit(f'{pathlib.Path(str(args[0])).name} failed (exit {r.returncode}):\n{tail}')
    return r.stdout


def find_helpers(img, text):
    """The helper addresses, each found once in .text, and setjmp/longjmp."""
    t0, t1 = text[0] - BASE, text[1] - BASE
    n = (t1 - t0) // 4
    words = struct.unpack_from(f'>{n}I', img, t0)
    addr = lambda i: BASE + t0 + i * 4  # noqa: E731
    out = {}
    for name, pat in PATTERNS.items():
        hits = [addr(i) for i in range(n - len(pat)) if words[i] == pat[0] and list(words[i:i + len(pat)]) == pat]
        if len(hits) != 1:
            sys.exit(f'{name}: {len(hits)} matches in the code (one expected): is this the right default.xex?')
        out[name] = hits[0]

    def seq(opbase):
        """Starts of runs of 18 loads/stores of r14..r31 relative to one register."""
        found = []
        for i in range(n - 18):
            w = words[i]
            if (w & 0xFC1F0003) == opbase and ((w >> 21) & 31) == 14 and all(
                    (words[i + k] & 0xFC1F0003) == opbase and ((words[i + k] >> 21) & 31) == 14 + k
                    for k in range(1, 18)):
                found.append(i)
        return found

    def func_start(i):
        """Back from i to just after the previous blr / bctr (and its padding): the function's start."""
        j = i
        while j > 0 and words[j - 1] not in (0x4E800020, 0x4E800420) and i - j < 512:
            j -= 1
        while j < i and words[j] == 0:
            j += 1
        return j

    # setjmp: std r14..r31,d(r3) (into the jmp_buf).
    sj = seq(0xF8030000)
    # longjmp: `mr r7,r3` then ld r14..r31,d(r7) (from the jmp_buf).
    lj = seq(0xE8070000)
    if len(sj) != 1 or len(lj) != 1:
        sys.exit(f'setjmp/longjmp: {len(sj)}/{len(lj)} candidates (one each expected)')
    out['setjmp'] = addr(func_start(sj[0]))
    # longjmp calls an unwind helper (bl) before its loads: back to `mr r7,r3`,
    # then to its prologue's `mflr r0`.
    j = lj[0]
    while j > 0 and words[j] != 0x7C671B78 and lj[0] - j < 128:
        j -= 1
    while j > 0 and words[j] != 0x7C0802A6 and lj[0] - j < 160:
        j -= 1
    if words[j] != 0x7C0802A6:
        sys.exit('longjmp: no `mr r7,r3` / `mflr r0` before its register loads')
    out['longjmp'] = addr(j)
    return out


def write_config(path, name, xex, helpers, funcs):
    h = helpers
    body = HEADER.format(title=TITLES[name][0], name=name, xex=xex.replace('\\', '/'))
    for k in ('restgprlr_14', 'savegprlr_14', 'restfpr_14', 'savefpr_14',
              'restvmx_14', 'savevmx_14', 'restvmx_64', 'savevmx_64'):
        body += f'{k}_address = 0x{h[k]:08X}\n'
    body += f'\nlongjmp_address = 0x{h["longjmp"]:08X}\nsetjmp_address = 0x{h["setjmp"]:08X}\n\n'
    body += '# Functions with no .pdata entry XenonRecomp cut short (tools/fix_truncated_functions.py).\n'
    body += 'functions = [\n' + ''.join(f'    {{ address = 0x{a:08X}, size = 0x{s:X} }},\n'
                                       for a, s in sorted(funcs.items())) + ']\n'
    path.write_text(body)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    for a in ('--xex', '--gen', '--analyse', '--recomp', '--xex-dump', '--ppc-context'):
        ap.add_argument(a, required=True)
    ap.add_argument('--title', choices=sorted(TITLES), default='bf1943')
    a = ap.parse_args()
    name = a.title
    xex = os.path.abspath(a.xex)
    gen = pathlib.Path(a.gen).resolve()
    ana = gen / 'analysis'
    ana.mkdir(parents=True, exist_ok=True)
    progress(0)

    # 1. The memory image and the .text range.
    img_path = ana / TITLES[name][1]
    out = run([a.xex_dump, xex, img_path, ana / 'xex_symbols.txt'])
    m = re.search(r'section \.text\s+0x([0-9A-F]+) size 0x([0-9A-F]+)', out)
    if not m:
        sys.exit('xex_dump: no .text section in its output')
    text = (int(m.group(1), 16), int(m.group(1), 16) + int(m.group(2), 16))
    img = img_path.read_bytes()
    progress(1)

    # 2. Jump tables.
    tables = ana / f'{name}_switch_tables.toml'
    run([a.analyse, xex, tables], log=ana / 'xenonanalyse_stdout.txt')
    extra = run([sys.executable, '-I', TOOLS / 'find_offset_switches.py', img_path, tables])
    (ana / f'{name}_offset_switches.toml').write_text(extra)
    (ana / f'{name}_switch_tables_all.toml').write_text(tables.read_text() + extra)
    progress(2)

    # 3. Helpers.
    helpers = find_helpers(img, text)
    print('helpers: ' + ', '.join(f'{k} 0x{v:08X}' for k, v in helpers.items()), flush=True)
    progress(3)

    # 4. XenonRecomp until no function is cut short.
    cfg = gen / f'{name}_config.toml'
    ppc = gen / 'ppc'
    funcs = {}
    nested = set()
    for n in range(1, MAX_PASSES + 1):
        write_config(cfg, name, xex, helpers, funcs)
        if ppc.exists():
            shutil.rmtree(ppc)
        ppc.mkdir()
        run([a.recomp, cfg.name, os.path.relpath(a.ppc_context, gen)], cwd=gen, log=ana / 'xenonrecomp_log.txt')
        fixes = run([sys.executable, '-I', TOOLS / 'fix_truncated_functions.py', ppc])
        new = {}
        for adr, size in re.findall(r'address = 0x([0-9A-F]+), size = 0x([0-9A-F]+)', fixes):
            adr, size = int(adr, 16), int(size, 16)
            if adr not in nested and size > funcs.get(adr, 0):
                new[adr] = size
        errors = sum(f.read_text(errors='replace').count('// ERROR') for f in ppc.glob('ppc_recomp.*.cpp'))
        print(f'pass {n}: {len(funcs)} functions declared, {errors} cut-short targets, {len(new)} to extend',
              flush=True)
        progress(3 + n)
        if not errors:
            break
        funcs.update(new)
        # Never one declared inside another: the outer one covers it (and a
        # declared fragment turns the outer one's jump-table data into "code").
        starts = sorted(funcs)
        inner = {b for a in starts for b in starts if a < b < a + funcs[a]}
        for b in inner:
            del funcs[b]
        nested |= inner
        if not new:
            sys.exit(f'{errors} `// ERROR` targets left that tools/fix_truncated_functions.py cannot fix')
    else:
        sys.exit(f'still `// ERROR` targets after {MAX_PASSES} passes')
    progress(STEPS)
    print(f'config: {cfg} ({len(funcs)} functions declared)', flush=True)


if __name__ == '__main__':
    main()
