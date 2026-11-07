"""Fetches a Debian 12 (bookworm) sysroot for building the Linux server on
Windows (game/linux/cross-from-windows.cmake): glibc, the Linux headers,
libstdc++ and libgcc, from deb.debian.org. Symlinks become copies.

  python tools/fetch_linux_sysroot.py build/linux-sysroot
"""
import io
import lzma
import os
import re
import shutil
import sys
import tarfile
import urllib.request

MIRROR = 'http://deb.debian.org/debian/'
PACKAGES = ['libc6', 'libc6-dev', 'linux-libc-dev', 'libgcc-12-dev', 'libgcc-s1', 'libstdc++-12-dev', 'libstdc++6']


def main():
    dest = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build/linux-sysroot')
    index = lzma.decompress(urllib.request.urlopen(MIRROR + 'dists/bookworm/main/binary-amd64/Packages.xz').read())
    files = {}
    for block in index.decode('utf-8', 'replace').split('\n\n'):
        name = re.search(r'^Package: (.+)$', block, re.M)
        if name and name.group(1) in PACKAGES:
            files[name.group(1)] = re.search(r'^Filename: (.+)$', block, re.M).group(1)
    links = []
    for pkg in PACKAGES:
        print('fetching', files[pkg])
        data = urllib.request.urlopen(MIRROR + files[pkg]).read()
        pos = 8
        while pos < len(data):
            hdr = data[pos:pos + 60]
            name, size = hdr[:16].decode().strip(), int(hdr[48:58])
            if name.startswith('data.tar'):
                with tarfile.open(fileobj=io.BytesIO(data[pos + 60:pos + 60 + size]), mode='r:*') as t:
                    for m in t.getmembers():
                        if '/share/' in m.name:
                            continue
                        if m.issym() or m.islnk():
                            links.append((m.name, m.linkname, m.issym()))
                        else:
                            t.extract(m, dest, filter='data')
            pos += 60 + size + (size & 1)
    for _ in range(3):
        for name, target, sym in links:
            p = os.path.normpath(os.path.join(dest, name))
            if sym and not target.startswith('/'):
                t = os.path.normpath(os.path.join(os.path.dirname(p), target))
            else:
                t = os.path.normpath(os.path.join(dest, target.lstrip('/')))
            if os.path.exists(p) or not os.path.exists(t):
                continue
            os.makedirs(os.path.dirname(p), exist_ok=True)
            (shutil.copytree if os.path.isdir(t) else shutil.copy2)(t, p)
    print('sysroot ready:', dest)


if __name__ == '__main__':
    main()
