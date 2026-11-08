"""Builds the player release: the launcher and the build tree it uses to make
the game from the player's own copy (launcher/setup.js). Nothing of the game
goes in: no game files, no recompiled code.

  python tools/make_release.py [out_dir] [--zip]

out_dir (default build/release/BF1943-PC):
  Launcher/        the packed launcher (npm run pack in launcher/)
  src/             this repo's sources, the compiled shaders (build/xenia_shaders,
                   so players need no shader compiler), XenonRecomp.exe
                   (tools/bin) and the recompiler config (generated/*.toml and
                   the switch tables)
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


def copy_tree(src_root, dst_root):
    for dirpath, dirnames, filenames in os.walk(src_root):
        rel_dir = os.path.relpath(dirpath, src_root).replace('\\', '/')
        rel_dir = '' if rel_dir == '.' else rel_dir + '/'
        dirnames[:] = [d for d in dirnames if d not in SKIP_NAMES and not any(
            (rel_dir + d + '/').startswith(s) for s in SKIP)]
        for f in filenames:
            rel = rel_dir + f
            if f.endswith(SKIP_EXT) or any(rel.startswith(s) for s in SKIP):
                continue
            os.makedirs(os.path.join(dst_root, rel_dir), exist_ok=True)
            shutil.copy2(os.path.join(dirpath, f), os.path.join(dst_root, rel))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build', 'release', 'BF1943-PC'))
    if os.path.exists(out):
        shutil.rmtree(out)
    src = os.path.join(out, 'src')
    print('sources ->', src)
    copy_tree(ROOT, src)
    for k in KEEP:
        os.makedirs(os.path.dirname(os.path.join(src, k)), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, k), os.path.join(src, k))
    shaders = os.path.join(ROOT, 'build', 'xenia_shaders')
    if not os.path.isdir(shaders):
        sys.exit('build/xenia_shaders missing: run tools/build_xenia_shaders.py first')
    shutil.copytree(shaders, os.path.join(src, 'build', 'xenia_shaders'))
    recomp = os.path.join(ROOT, 'build', 'xenonrecomp-clang', 'XenonRecomp', 'XenonRecomp.exe')
    if not os.path.isfile(recomp):
        sys.exit('XenonRecomp.exe missing: build thirdparty/XenonRecomp first (see the README)')
    os.makedirs(os.path.join(src, 'tools', 'bin'), exist_ok=True)
    shutil.copy2(recomp, os.path.join(src, 'tools', 'bin', 'XenonRecomp.exe'))

    print('launcher -> Launcher/')
    launcher = os.path.join(ROOT, 'launcher')
    subprocess.run('npm run pack', cwd=launcher, shell=True, check=True)
    shutil.copytree(os.path.join(launcher, 'dist', 'BF1943Launcher-win32-x64'), os.path.join(out, 'Launcher'))

    with open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8') as f:
        f.write('Battlefield 1943 for PC\n\n'
                'Start Launcher\\BF1943Launcher.exe. The first time, it builds the game from your own\n'
                'copy of Battlefield 1943 (Xbox 360, version 1.06): choose your game package or\n'
                'default.xex, press INSTALL and keep the launcher open. It installs the build tools it\n'
                'needs (LLVM, CMake, Visual Studio C++ build tools, Vulkan SDK) through winget.\n')
    if '--zip' in sys.argv:
        shutil.make_archive(out, 'zip', os.path.dirname(out), os.path.basename(out))
        print('zip ->', out + '.zip')
    print('done:', out)


if __name__ == '__main__':
    main()
