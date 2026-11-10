"""Builds the player release: the launcher and the build trees it uses to make
the games from the player's own copies (launcher/setup.js). Nothing of the
games goes in: no game files, no recompiled code.

  python tools/make_release.py [out_dir]                 one exe (default)
  python tools/make_release.py [out_dir] --folder [--zip]  a folder

One exe (default out_dir build/release): BattlefieldLauncher-<version>.exe,
electron-builder's portable target (npm run dist in launcher/) with src/ and
src-bfbc1/ below inside it (staged in build/release_sources); it installs
in a BattlefieldRecomp folder next to itself (launcher/main.js).

A folder (default out_dir build/release/BF1943-PC):
  Launcher/        the packed launcher (npm run pack in launcher/)
  src/             this tree's sources (Battlefield 1943 and the code both games
                   share), the compiled shaders (build/xenia_shaders, so players
                   need no shader compiler), XenonRecomp.exe (tools/bin) and the
                   recompiler config (generated/*.toml and the switch tables)
  src-bfbc1/       ../bfbc1-recomp: Battlefield: Bad Company's sources and config
  README.txt
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

# Left out of src/ (relative to the repo, '/' separated, prefixes).
SKIP = [
    'original/', 'generated/ppc/', 'generated/analysis/', 'build/', 'research/', 'userdata/',
    'frames/', 'shader_cache/', 'launcher/node_modules/', 'launcher/dist/',
    'server/arcadia360/bin/', 'server/arcadia360/obj/', 'tests/',
]
SKIP_NAMES = {'.git', '.git-upstream', '__pycache__', '.vs'}
SKIP_EXT = ('.log', '.out', '.wav', '.pdb', '.pipelinecache')
# Kept from the excluded folders.
KEEP = [
    'generated/bf1943_config.toml',
    'generated/analysis/bf1943_switch_tables_all.toml',
]
# Bad Company's tree: its sources and recompiler config only.
BC_ROOT = os.path.join(ROOT, '..', 'bfbc1-recomp')
BC_SKIP = ['original/', 'generated/', 'build/', 'userdata/', 'userdata2/']
BC_KEEP = [
    'generated/bfbc1_config.toml',
    'generated/analysis/bfbc1_switch_tables_all.toml',
]


def copy_tree(src_root, dst_root, skip=SKIP):
    for dirpath, dirnames, filenames in os.walk(src_root):
        rel_dir = os.path.relpath(dirpath, src_root).replace('\\', '/')
        rel_dir = '' if rel_dir == '.' else rel_dir + '/'
        dirnames[:] = [d for d in dirnames if d not in SKIP_NAMES and not any(
            (rel_dir + d + '/').startswith(s) for s in skip)]
        for f in filenames:
            rel = rel_dir + f
            if f.endswith(SKIP_EXT) or any(rel.startswith(s) for s in skip):
                continue
            os.makedirs(os.path.join(dst_root, rel_dir), exist_ok=True)
            shutil.copy2(os.path.join(dirpath, f), os.path.join(dst_root, rel))


def stage(out):
    """The build trees: <out>/src and <out>/src-bfbc1."""
    if os.path.exists(out):
        shutil.rmtree(out)
    src = os.path.join(out, 'src')
    print('sources ->', src)
    copy_tree(ROOT, src)
    for k in KEEP:
        os.makedirs(os.path.dirname(os.path.join(src, k)), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, k), os.path.join(src, k))
    bc = os.path.join(out, 'src-bfbc1')
    print('Bad Company ->', bc)
    copy_tree(BC_ROOT, bc, BC_SKIP)
    for k in BC_KEEP:
        os.makedirs(os.path.dirname(os.path.join(bc, k)), exist_ok=True)
        shutil.copy2(os.path.join(BC_ROOT, k), os.path.join(bc, k))
    shaders = os.path.join(ROOT, 'build', 'xenia_shaders')
    if not os.path.isdir(shaders):
        sys.exit('build/xenia_shaders missing: run tools/build_xenia_shaders.py first')
    shutil.copytree(shaders, os.path.join(src, 'build', 'xenia_shaders'))
    recomp = os.path.join(ROOT, 'build', 'xenonrecomp-clang', 'XenonRecomp', 'XenonRecomp.exe')
    if not os.path.isfile(recomp):
        sys.exit('XenonRecomp.exe missing: build thirdparty/XenonRecomp first (see the README)')
    os.makedirs(os.path.join(src, 'tools', 'bin'), exist_ok=True)
    shutil.copy2(recomp, os.path.join(src, 'tools', 'bin', 'XenonRecomp.exe'))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    launcher = os.path.join(ROOT, 'launcher')
    if '--folder' not in sys.argv:
        out = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build', 'release'))
        stage(os.path.join(ROOT, 'build', 'release_sources'))  # launcher/package.json extraResources
        print('launcher -> one exe')
        dist = os.path.join(launcher, 'dist')
        if os.path.exists(dist):
            shutil.rmtree(dist)
        subprocess.run('npm run dist', cwd=launcher, shell=True, check=True)
        os.makedirs(out, exist_ok=True)
        for f in os.listdir(dist):
            if f.startswith('BattlefieldLauncher-') and f.endswith('.exe'):
                shutil.copy2(os.path.join(dist, f), os.path.join(out, f))
                print('done:', os.path.join(out, f))
        return

    out = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build', 'release', 'BF1943-PC'))
    stage(out)
    print('launcher -> Launcher/')
    subprocess.run('npm run pack', cwd=launcher, shell=True, check=True)
    shutil.copytree(os.path.join(launcher, 'dist', 'BF1943Launcher-win32-x64'), os.path.join(out, 'Launcher'))
    with open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8') as f:
        f.write('Battlefield 1943 and Battlefield: Bad Company for PC\n\n'
                'Start Launcher\\BF1943Launcher.exe and pick a game at the top. The first time, it\n'
                'builds that game from your own copy:\n'
                '  Battlefield 1943: the Xbox 360 package (version 1.06) or default.xex.\n'
                '  Battlefield: Bad Company: the disc image (.iso, USA) or default.xex.\n'
                'Choose the file, press INSTALL and keep the launcher open. It installs the build tools\n'
                'it needs (LLVM, CMake, Visual Studio C++ build tools, Vulkan SDK) through winget.\n'
                'Each game takes about 20-60 minutes to build.\n')
    if '--zip' in sys.argv:
        shutil.make_archive(out, 'zip', os.path.dirname(out), os.path.basename(out))
        print('zip ->', out + '.zip')
    print('done:', out)


if __name__ == '__main__':
    main()
