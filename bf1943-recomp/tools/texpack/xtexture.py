# Game file readers: FBRB archives and XenonTexture .res images.
#
# FBRB: "FbRB", BE u32 header length, gzip header (string table + entries),
# then the payload: gzip members, or raw (build_overlay-00.fbrb).
# XenonTexture: 92-byte BE header, then Xenos-tiled data, 8in16-swapped for
# block formats. The format enum is Frostbite 1's. Format 5 (16 bpp, used by two
# terrain textures, e.g. gravel_optimized) is not RGB565 and is left out.
import gzip
import struct
import zlib

import numpy as np


def _members(data):
    out = []
    while data:
        o = zlib.decompressobj(31)
        out.append(o.decompress(data))
        data = o.unused_data
    return b''.join(out)


def read_fbrb(path):
    """Returns [(name, ext, bytes)] for every entry."""
    b = open(path, 'rb').read()
    if b[:4] != b'FbRB':
        raise ValueError('not an FBRB archive')
    n = struct.unpack('>I', b[4:8])[0]
    h = gzip.decompress(b[8:8 + n])
    rest = b[8 + n:]
    payload = _members(rest) if rest[:2] == b'\x1f\x8b' else rest
    _, sl = struct.unpack('>II', h[:8])
    st = h[8:8 + sl]
    pos = 8 + sl
    cnt = struct.unpack('>I', h[pos:pos + 4])[0]
    pos += 4
    out = []
    for _ in range(cnt):
        no, _, off, sz, _, ext = struct.unpack('>6I', h[pos:pos + 24])
        pos += 24
        name = st[no:st.index(b'\0', no)].decode()
        ex = st[ext:st.index(b'\0', ext)].decode()
        out.append((name, ex, payload[off:off + sz]))
    return out


# Frostbite 1 texture formats: name -> (block size in pixels, bytes per block).
FORMATS = {
    0: ('DXT1', 4, 8), 1: ('DXT3', 4, 16), 2: ('DXT5', 4, 16), 3: ('DXT5A', 4, 8),
    4: ('DXN', 4, 16), 7: ('ARGB1555', 1, 2), 8: ('ARGB4444', 1, 2),
    9: ('ARGB8888', 1, 4), 10: ('L8', 1, 1), 11: ('L16', 1, 2), 13: ('ABGR16F', 1, 8),
    17: ('NormalDXN', 4, 16), 18: ('NormalDXT1', 4, 8), 19: ('NormalDXT5', 4, 16),
    20: ('NormalDXT5RGA', 4, 16),
}


class Texture:
    def __init__(self, data):
        w = struct.unpack('>9I', data[:36])
        self.cube = w[1] == 1
        self.format = w[2]
        self.width, self.height, self.mips = w[4], w[5], w[7]
        self.data = data[92:]

    @property
    def format_name(self):
        return FORMATS.get(self.format, ('fmt%d' % self.format,))[0]


def _tiled_offsets(bw, bh, log2_bpb):
    """Byte offset of each block (y, x) in a Xenos 2D-tiled surface."""
    pitch = (bw + 31) & ~31
    y, x = np.mgrid[0:bh, 0:bw].astype(np.int64)
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2_bpb + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F))


def _c565(v):
    r = ((v >> 11) & 31).astype(np.int32)
    g = ((v >> 5) & 63).astype(np.int32)
    b = (v & 31).astype(np.int32)
    return np.stack([(r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31], -1)


def _bc1(blk, force4=False):
    """blk: (..., 8) uint8 -> (..., 4, 4, 4) RGBA."""
    c0 = blk[..., 0].astype(np.uint16) | (blk[..., 1].astype(np.uint16) << 8)
    c1 = blk[..., 2].astype(np.uint16) | (blk[..., 3].astype(np.uint16) << 8)
    bits = blk[..., 4:8].astype(np.uint32)
    bits = bits[..., 0] | (bits[..., 1] << 8) | (bits[..., 2] << 16) | (bits[..., 3] << 24)
    p0, p1 = _c565(c0), _c565(c1)
    four = (c0 > c1)[..., None] | force4
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], -2)  # (..., 4, 3)
    alpha = np.stack([np.full(c0.shape, 255)] * 3 + [np.where(four[..., 0], 255, 0)], -1)
    idx = (bits[..., None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    rgb = np.take_along_axis(pal, idx[..., None].astype(np.int64), -2)
    a = np.take_along_axis(alpha, idx.astype(np.int64), -1)
    return np.concatenate([rgb, a[..., None]], -1).reshape(blk.shape[:-1] + (4, 4, 4))


def _bc4(blk):
    """blk: (..., 8) uint8 -> (..., 4, 4) channel."""
    a0 = blk[..., 0].astype(np.int32)
    a1 = blk[..., 1].astype(np.int32)
    bits = np.zeros(blk.shape[:-1], np.uint64)
    for i in range(6):
        bits |= blk[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    i7 = np.arange(1, 7)
    big = (a0 > a1)[..., None]
    p8 = ((7 - i7) * a0[..., None] + i7 * a1[..., None]) // 7
    i5 = np.arange(1, 5)
    p6 = ((5 - i5) * a0[..., None] + i5 * a1[..., None]) // 5
    p6 = np.concatenate([p6, np.zeros_like(a0)[..., None], np.full_like(a0, 255)[..., None]], -1)
    pal = np.concatenate([a0[..., None], a1[..., None], np.where(big, p8, p6)], -1)
    idx = (bits[..., None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)
    return np.take_along_axis(pal, idx.astype(np.int64), -1).reshape(blk.shape[:-1] + (4, 4))


def decode(tex, face=0):
    """Top mip of a 2D texture (or one cube face) as an (h, w, 4) uint8 RGBA array.

    Normal formats are returned with X in R, Y in G, reconstructed Z in B."""
    name, bs, bpb = FORMATS[tex.format]
    w, h = tex.width, tex.height
    bw, bh = (w + bs - 1) // bs, (h + bs - 1) // bs
    pitch_blocks = (bw + 31) & ~31
    face_size = pitch_blocks * ((bh + 31) & ~31) * bpb
    face_size = (face_size + 4095) & ~4095
    src = np.frombuffer(tex.data, np.uint8)[face * face_size:]
    offs = _tiled_offsets(bw, bh, {1: 0, 2: 1, 4: 2, 8: 3, 16: 4}[bpb])
    lin = src[offs[..., None] + np.arange(bpb)]  # (bh, bw, bpb)
    if bs == 4 or bpb == 2 or bpb == 8:
        lin = lin.reshape(bh, bw, bpb // 2, 2)[..., ::-1].reshape(bh, bw, bpb)  # 8in16
    if bs == 1:
        lin = lin.reshape(h, w, bpb)
        if name == 'ARGB8888':
            return lin[..., [1, 2, 3, 0]].copy()
        if name == 'L8':
            return np.concatenate([np.repeat(lin, 3, -1), np.full((h, w, 1), 255, np.uint8)], -1)
        v = lin[..., 0].astype(np.uint16) | (lin[..., 1].astype(np.uint16) << 8)  # now LE
        if name == 'RGB565':
            return np.concatenate([_c565(v).astype(np.uint8), np.full((h, w, 1), 255, np.uint8)], -1)
        if name == 'ARGB4444':
            ch = [(v >> s) & 15 for s in (8, 4, 0, 12)]
            return (np.stack(ch, -1) * 17).astype(np.uint8)
        if name == 'ARGB1555':
            ch = [((v >> s) & 31) * 255 // 31 for s in (10, 5, 0)] + [((v >> 15) & 1) * 255]
            return np.stack(ch, -1).astype(np.uint8)
        if name == 'L16':
            l = (v >> 8).astype(np.uint8)[..., None]
            return np.concatenate([l, l, l, np.full((h, w, 1), 255, np.uint8)], -1)
        if name == 'ABGR16F':
            f = lin.view('<f2').reshape(h, w, 4).astype(np.float32)
            f = np.clip(f / max(1.0, float(f[..., :3].max())), 0, 1)  # HDR: normalised preview
            return (f * 255 + 0.5).astype(np.uint8)
        raise ValueError('unsupported format ' + name)
    if name in ('DXT1', 'NormalDXT1'):
        px = _bc1(lin)
    elif name in ('DXT5', 'NormalDXT5', 'NormalDXT5RGA', 'DXT3'):
        px = _bc1(lin[..., 8:], force4=True)
        if name == 'DXT3':
            a = lin[..., :8]
            nib = np.stack([a & 15, a >> 4], -1).reshape(bh, bw, 16) * 17
            px[..., 3] = nib.reshape(bh, bw, 4, 4)
        else:
            px[..., 3] = _bc4(lin[..., :8])
    elif name == 'DXT5A':
        a = _bc4(lin)
        px = np.repeat(a[..., None], 4, -1)
    elif name in ('DXN', 'NormalDXN'):
        x, y = _bc4(lin[..., :8]), _bc4(lin[..., 8:])
        px = np.stack([x, y, np.zeros_like(x), np.full_like(x, 255)], -1)
    else:
        raise ValueError('unsupported format ' + name)
    img = px.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)[:h, :w].astype(np.uint8)
    if name.startswith('Normal') or name == 'DXN':
        img = normal_xy(img, name)
    return img


def normal_xy(img, name):
    """Moves the stored normal channels to R=X, G=Y and rebuilds B=Z."""
    if name in ('NormalDXT5',):
        x, y = img[..., 3], img[..., 1]  # DXT5nm: X in alpha, Y in green
    elif name == 'NormalDXT5RGA':
        x, y = img[..., 0], img[..., 1]
    else:
        x, y = img[..., 0], img[..., 1]
    fx = x.astype(np.float32) / 127.5 - 1
    fy = y.astype(np.float32) / 127.5 - 1
    fz = np.sqrt(np.clip(1 - fx * fx - fy * fy, 0, 1))
    out = np.stack([x, y, ((fz + 1) * 127.5 + 0.5).astype(np.uint8), np.full_like(x, 255)], -1)
    return out
