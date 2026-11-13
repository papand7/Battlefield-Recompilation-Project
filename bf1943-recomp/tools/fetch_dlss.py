"""Fetches NVIDIA's DLSS SDK (v310.9.16, from github.com/NVIDIA/DLSS) into
thirdparty/dlss: the NGX headers, the import library and nvngx_dlss.dll, each
checked against its SHA-256. Not kept in this repo: NVIDIA's licence
(thirdparty/dlss/LICENSE.txt once fetched) doesn't allow it in an open one.
The Windows build runs this by itself when the SDK is missing.

  python tools/fetch_dlss.py [--to <dir>]
"""
import hashlib
import os
import sys
import urllib.request

TAG = 'v310.9.16'
URL = 'https://raw.githubusercontent.com/NVIDIA/DLSS/' + TAG + '/'
# (path in NVIDIA's repo, path here, SHA-256)
FILES = [
    ('include/nvsdk_ngx.h', 'include/nvsdk_ngx.h',
     'dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848'),
    ('include/nvsdk_ngx_defs.h', 'include/nvsdk_ngx_defs.h',
     'ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e'),
    ('include/nvsdk_ngx_defs_dlssd.h', 'include/nvsdk_ngx_defs_dlssd.h',
     '2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716'),
    ('include/nvsdk_ngx_defs_dlssg.h', 'include/nvsdk_ngx_defs_dlssg.h',
     'c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1'),
    ('include/nvsdk_ngx_defs_vk.h', 'include/nvsdk_ngx_defs_vk.h',
     '0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031'),
    ('include/nvsdk_ngx_helpers.h', 'include/nvsdk_ngx_helpers.h',
     '5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471'),
    ('include/nvsdk_ngx_helpers_cuda.h', 'include/nvsdk_ngx_helpers_cuda.h',
     'f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042'),
    ('include/nvsdk_ngx_helpers_d3d.h', 'include/nvsdk_ngx_helpers_d3d.h',
     'ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba'),
    ('include/nvsdk_ngx_helpers_dlssd.h', 'include/nvsdk_ngx_helpers_dlssd.h',
     'a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532'),
    ('include/nvsdk_ngx_helpers_dlssd_cuda.h', 'include/nvsdk_ngx_helpers_dlssd_cuda.h',
     '7e35cde214d905346a487b9a28c05d35e1c55b480e237fe3de69580a7e332730'),
    ('include/nvsdk_ngx_helpers_dlssd_d3d.h', 'include/nvsdk_ngx_helpers_dlssd_d3d.h',
     '6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293'),
    ('include/nvsdk_ngx_helpers_dlssd_vk.h', 'include/nvsdk_ngx_helpers_dlssd_vk.h',
     '11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b'),
    ('include/nvsdk_ngx_helpers_dlssg.h', 'include/nvsdk_ngx_helpers_dlssg.h',
     '6881d21419c40c78ec948b326902ece09fc112b0223144d51c4905f3fced7a1f'),
    ('include/nvsdk_ngx_helpers_dlssg_d3d.h', 'include/nvsdk_ngx_helpers_dlssg_d3d.h',
     '6335ebcb95de39a361e87fe0257d6ce0723e5d8deaa15db3b35afacf2b3f4220'),
    ('include/nvsdk_ngx_helpers_dlssg_vk.h', 'include/nvsdk_ngx_helpers_dlssg_vk.h',
     'b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316'),
    ('include/nvsdk_ngx_helpers_vk.h', 'include/nvsdk_ngx_helpers_vk.h',
     'c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623'),
    ('include/nvsdk_ngx_loader.h', 'include/nvsdk_ngx_loader.h',
     'abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75'),
    ('include/nvsdk_ngx_params.h', 'include/nvsdk_ngx_params.h',
     '943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc'),
    ('include/nvsdk_ngx_params_dlssd.h', 'include/nvsdk_ngx_params_dlssd.h',
     'd41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e'),
    ('include/nvsdk_ngx_params_dlssg.h', 'include/nvsdk_ngx_params_dlssg.h',
     'c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad'),
    ('include/nvsdk_ngx_standalone_common.h', 'include/nvsdk_ngx_standalone_common.h',
     '38413c3338b0aea0202c07da95d426bfee7080a42913823fa2d9dd585b087b53'),
    ('include/nvsdk_ngx_standalone_cuda.h', 'include/nvsdk_ngx_standalone_cuda.h',
     'b23436ed5040051ac8e81554c9af080f2463c941435bbc3b82222e5431a09499'),
    ('include/nvsdk_ngx_vk.h', 'include/nvsdk_ngx_vk.h',
     '2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d'),
    ('lib/Windows_x86_64/x64/nvsdk_ngx_d.lib', 'lib/nvsdk_ngx_d.lib',
     'bc43df9ec032004fde681d3e3afd01cde239e03a0125b822e88674c84287f99c'),
    ('lib/Windows_x86_64/rel/nvngx_dlss.dll', 'bin/nvngx_dlss.dll',
     'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'),
    ('LICENSE.txt', 'LICENSE.txt',
     'd4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0'),
    ('README.md', 'README.md',
     '323a99117be4a1b6595ab0d721e9c613750c278366615b57168b3ea9967afac5'),
]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    to = os.path.abspath(sys.argv[sys.argv.index('--to') + 1] if '--to' in sys.argv else
                         os.path.join(here, '..', 'thirdparty', 'dlss'))
    for remote, local, sha in FILES:
        dest = os.path.join(to, local)
        if os.path.isfile(dest) and hashlib.sha256(open(dest, 'rb').read()).hexdigest() == sha:
            continue
        print('fetching', remote, flush=True)
        req = urllib.request.Request(URL + remote, headers={'User-Agent': 'bf-recomp-fetch-dlss'})
        with urllib.request.urlopen(req, timeout=120) as r:
            data = r.read()
        got = hashlib.sha256(data).hexdigest()
        if got != sha:
            sys.exit('error: %s is not the expected file (SHA-256 %s)' % (remote, got))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest + '.part', 'wb') as f:
            f.write(data)
        os.replace(dest + '.part', dest)
    print('DLSS SDK', TAG, 'in', to)


if __name__ == '__main__':
    main()
