"""Find the --pbr taps of BF1943's lit pixel shaders.

Reads the shader disassembly written by `bf1943 --dump-shaders <dir>`
(shader_<hash>.ucode.frag) and prints data/pbr_shaders.txt lines for the lit
shaders it understands (see runtime/xenia_shims/pbr_shaders.h):

    python tools/pbr_taps.py build/run/shaders [hash ...]

It follows the data flow of each shader:
  n    the register dotted with c1 (the sun direction) in dp3_sat
  v    the camera vector, c7 - position
  env  the tfetchCube result
  lit  1 - the shadow dp4 over the point-sampled shadowmap fetches
  out  the colour read by the oC0 export
  a    the colour operand that multiplies the sunlight (c5) on the way to out,
       followed back past the environment map
  rm   the normal map fetch (the first non-shadow 2D fetch that n depends on)
  ao   a one-channel map on its own interpolated texture coordinates
  pow  the constant that multiplies log(N.H): the game's specular power
  env_scale  a constant that multiplies the cube map result
"""
import os
import re
import sys
from collections import OrderedDict

XYZW = 'xyzw'
VECTOR_OPS = {'add': 2, 'mul': 2, 'max': 2, 'min': 2, 'seq': 2, 'sgt': 2, 'sge': 2, 'sne': 2,
              'mad': 3, 'cndeq': 3, 'cndge': 3, 'cndgt': 3, 'dp4': 2, 'dp3': 2, 'dp2add': 3,
              'cube': 2, 'frc': 1, 'trunc': 1, 'floor': 1, 'mov': 1, 'max4': 1}


class Node:
    __slots__ = ('addr', 'op', 'ins', 'comp', '_tags')

    def __init__(self, addr, op, ins, comp=None):
        self.addr, self.op, self.ins, self.comp, self._tags = addr, op, ins, comp, None

    def tags(self):
        """Inputs this value depends on: 't<fetch address>', 'c<constant>', 'i...'."""
        if self._tags is None:
            self._tags = frozenset()  # cycle guard
            if self.op == 'const':
                t = {self.comp.split('.')[0]}
            elif self.op == 'in':
                t = {'i' + self.comp}
            else:
                t = {'t%d' % self.addr} if self.op.startswith(('tfetch', 'getWeights')) else set()
                for i in self.ins:
                    t |= i.tags()
            self._tags = frozenset(t)
        return self._tags


def parse_source(tok):
    tok = tok.strip()
    neg = tok.startswith('-')
    m = re.match(r'(r_abs\[(\d+)\]|r(\d+)|c(\d+))(?:\.([xyzw01_]+))?$', tok.lstrip('-'))
    if not m:
        return None
    if m.group(4) is not None:
        kind, idx = 'c', int(m.group(4))
    else:
        kind, idx = 'r', int(m.group(2) if m.group(2) is not None else m.group(3))
    return dict(kind=kind, idx=idx, sw=m.group(5) or 'xyzw', neg=neg)


class Shader:
    def __init__(self, path):
        self.consts = {}
        self.fetches = {}
        self.alu = OrderedDict()  # addr -> [(op, dest reg, dest comp or None, node, sources)]
        self.snap = OrderedDict()  # addr -> register state after it: (reg, comp) -> (node, addr)
        self.export = None
        self.ps = None
        self.regs = {}
        groups = OrderedDict()
        cur = None
        for ln in open(path, errors='replace').read().split('\n'):
            m = re.match(r'^/\*\s*(\d+)\s*\*/\s+(.*)$', ln)
            if m:
                cur = int(m.group(1))
                if m.group(2).strip() != 'serialize':
                    groups.setdefault(cur, []).append(m.group(2).strip())
                continue
            m = re.match(r'^\s+\+\s+(.*)$', ln)
            if m and cur is not None:
                groups.setdefault(cur, []).append(m.group(1).strip())
                continue
            m = re.match(r'^\s{10,}(\w.*)$', ln)
            if m and cur is not None:
                groups.setdefault(cur, []).append(m.group(1).strip())
        for addr, bodies in groups.items():
            writes = []
            for body in bodies:
                self._instruction(addr, body, writes)
            for k, n in writes:
                self.regs[k] = (n, addr)
            self.snap[addr] = dict(self.regs)

    def const(self, name):
        if name not in self.consts:
            self.consts[name] = Node(-1, 'const', [], name)
        return self.consts[name]

    def read(self, src, c):
        ch = src['sw'][min(c, len(src['sw']) - 1)]
        if src['kind'] == 'c':
            return self.const('c%d.%s' % (src['idx'], ch))
        if ch in '01':
            return self.const(ch)
        key = (src['idx'], XYZW.index(ch))
        if key not in self.regs:
            self.regs[key] = (Node(-1, 'in', [], 'r%d.%s' % key), -1)
        return self.regs[key][0]

    def _instruction(self, addr, body, writes):
        body = re.sub(r'^\(!?p0\)\s*', '', body)
        m = re.match(r'(\w+)\s+(.*)$', body)
        if not m:
            return
        op, rest = m.group(1), m.group(2)
        parts = [p.strip() for p in re.split(r',\s*(?![^\[]*\])', rest)]
        base = op.replace('_sat', '')
        if base.startswith(('tfetch', 'getWeights', 'getCompTexLOD', 'setTexLOD')):
            dm = re.match(r'r(\d+)\.([xyzw01_]{4})', parts[0])
            tf = re.search(r'tf(\d+)', rest)
            if base == 'setTexLOD' or not dm or not tf:
                return
            cm = re.match(r'(r_abs\[\d+\]|r\d+)\.([xyzw]+)', parts[1])
            coord_nodes = []
            if cm:
                reg = int(re.search(r'\d+', cm.group(1)).group(0))
                coord_nodes = [self.read(dict(kind='r', idx=reg, sw=ch), 0) for ch in cm.group(2)]
            self.fetches[addr] = dict(addr=addr, op=base, tf=int(tf.group(1)), dest=int(dm.group(1)),
                                      dsw=dm.group(2), point='MagFilter=point' in rest,
                                      coord=cm.group(0) if cm else None)
            for i, ch in enumerate(dm.group(2)):
                if ch in XYZW:
                    writes.append(((int(dm.group(1)), i), Node(addr, base, coord_nodes, XYZW.index(ch))))
            return
        if parts[0].startswith(('oC', 'oDepth')):
            if parts[0].startswith('oC0'):
                self.export = (addr, [parse_source(p) for p in parts[1:]])
            return
        dm = re.match(r'r(\d+)(?:\.([xyzw_]+))?$', parts[0])
        if not dm:
            return
        dreg, dmask = int(dm.group(1)), (dm.group(2) or 'xyzw').ljust(4, '_')
        srcs = [parse_source(p) for p in parts[1:]]
        if any(s is None for s in srcs):
            return
        if base in VECTOR_OPS and len(srcs) == VECTOR_OPS[base]:
            for i, ch in enumerate(dmask):
                if ch == '_':
                    continue
                if base in ('dp3', 'dp4', 'dp2add'):
                    k = {'dp3': 3, 'dp4': 4, 'dp2add': 2}[base]
                    ins = [self.read(srcs[0], j) for j in range(k)] + [self.read(srcs[1], j) for j in range(k)]
                    if base == 'dp2add':
                        ins.append(self.read(srcs[2], 0))
                elif base == 'cube':
                    ins = [self.read(s, j) for s in srcs for j in range(4)]
                else:
                    ins = [self.read(s, i) for s in srcs]
                n = Node(addr, base, ins, i)
                writes.append(((dreg, i), n))
                self.alu.setdefault(addr, []).append((op, dreg, i, n, srcs))
        else:  # scalar unit
            ins = [self.ps or self.const('ps')] if base.endswith('_prev') else []
            for s in srcs:
                for ch in (s['sw'][:2] if len(srcs) == 1 and len(s['sw']) >= 2 else s['sw'][:1]):
                    ins.append(self.read(dict(s, sw=ch), 0))
            n = Node(addr, base, ins, None)
            self.ps = n
            for i, ch in enumerate(dmask):
                if ch != '_':
                    writes.append(((dreg, i), n))
            self.alu.setdefault(addr, []).append((op, dreg, None, n, srcs))

    def state_before(self, addr):
        prev = {}
        for a, st in self.snap.items():
            if a >= addr:
                break
            prev = st
        return prev

    def tap(self, addr, reg, comps):
        """'r3.yzw@47' for register reg's comps as read by instruction addr."""
        st = self.state_before(addr)
        keys = [(reg, c) for c in comps]
        if any(k not in st for k in keys):
            return None, None
        at = max(st[k][1] for k in keys)
        return ('r%d.%s@%s' % (reg, ''.join(XYZW[c] for c in comps), at if at >= 0 else 's'),
                [st[k][0] for k in keys])

    def users(self, node):
        for a, lst in self.alu.items():
            for entry in lst:
                if node in entry[3].ins:
                    yield entry


def find_taps(sh):
    t, notes = {}, []
    nodes_n = nodes_out = None
    # N: dp3_sat with c1; the c1 swizzle pairs the components.
    for a, lst in sh.alu.items():
        for op, dreg, i, n, srcs in lst:
            if 'n' in t or op != 'dp3_sat' or len(srcs) != 2:
                continue
            c = [s for s in srcs if s['kind'] == 'c' and s['idx'] == 1]
            r = [s for s in srcs if s['kind'] == 'r']
            if c and r:
                comps = [XYZW.index(r[0]['sw'][c[0]['sw'][:3].index(ch)]) for ch in 'xyz']
                t['n'], nodes_n = sh.tap(a, r[0]['idx'], comps)
    # V: add r, -position, c7
    for a, lst in sh.alu.items():
        for op, dreg, i, n, srcs in lst:
            if 'v' in t or op != 'add' or len(srcs) != 2 or i is None:
                continue
            c7 = [s for s in srcs if s['kind'] == 'c' and s['idx'] == 7 and not s['neg']]
            pos = [s for s in srcs if s['kind'] == 'r' and s['neg']]
            if c7 and pos:
                comps = [c7[0]['sw'].index(ch) for ch in 'xyz']
                t['v'] = 'r%d.%s@%d' % (dreg, ''.join(XYZW[j] for j in comps), a)
    # Environment
    cubes = [f for f in sh.fetches.values() if f['op'] == 'tfetchCube']
    if cubes:
        f = cubes[0]
        comps = [f['dsw'].index(ch) for ch in 'xyz' if ch in f['dsw']]
        if len(comps) == 3:
            t['env'] = 'r%d.%s@%d' % (f['dest'], ''.join(XYZW[j] for j in comps), f['addr'])
            env_node = sh.regs.get((f['dest'], comps[0]))
            # env_scale: a constant multiplying the cube result (maybe after a mask)
            frontier = [sh.snap[f['addr']][(f['dest'], comps[0])][0]]
            for _ in range(3):
                nxt = []
                for node in frontier:
                    for op, dreg, i, n, srcs in sh.users(node):
                        if op in ('mul', 'mad') and 'env_scale' not in t:
                            for x in n.ins[:2]:
                                if x is not node and x.op == 'const' and x.comp[0] == 'c':
                                    t['env_scale'] = x.comp
                            nxt.append(n)
                frontier = nxt
    # Shadow: dp4 over point-sampled fetches, then 1 - it
    for a, lst in sh.alu.items():
        for op, dreg, i, n, srcs in lst:
            if 'lit' in t or op != 'dp4':
                continue
            if not any(x[0] == 't' and sh.fetches.get(int(x[1:]), {}).get('point')
                       for x in n.tags() if x[1:].isdigit()):
                continue
            for op2, d2, i2, n2, s2 in sh.users(n):
                if (op2 == 'add' and i2 is not None and n2.ins[0] is n and s2[0]['neg']
                        and s2[1]['kind'] == 'c' and 'lit' not in t):
                    t['lit'] = 'r%d.%s@%d' % (d2, XYZW[i2], [a2 for a2, l in sh.alu.items()
                                                             if any(e[3] is n2 for e in l)][0])
    # Output
    if sh.export:
        a, srcs = sh.export
        regs = [s for s in srcs if s and s['kind'] == 'r']
        colour = [s for s in regs if len(set(s['sw'][:3])) == 3] or regs
        if colour:
            s = colour[-1]
            t['out'], nodes_out = sh.tap(a, s['idx'], [XYZW.index(s['sw'][min(j, len(s['sw']) - 1)]) for j in range(3)])
    # Normal map: the first non-shadow 2D fetch N depends on
    nfetch = []
    for node in nodes_n or []:
        for x in node.tags():
            if x[0] == 't' and x[1:].isdigit():
                f = sh.fetches[int(x[1:])]
                if f['op'] == 'tfetch2D' and not f['point']:
                    nfetch.append(f['addr'])
    nfetch = sorted(set(nfetch))
    if nfetch:
        t['rm'] = str(nfetch[0])
    # Specular power
    for a, lst in sh.alu.items():
        for op, dreg, i, n, srcs in lst:
            if op != 'log' or 'pow' in t:
                continue
            for op2, d2, i2, n2, s2 in sh.users(n):
                if op2.startswith('mul'):
                    other = [x for x in n2.ins if x is not n]
                    if other and other[0].op == 'const' and other[0].comp[0] == 'c':
                        t['pow'] = other[0].comp
                    elif other:
                        notes.append('power from a register')
    # Albedo
    cube_t = {'t%d' % f['addr'] for f in cubes}
    shadow_t = {'t%d' % f['addr'] for f in sh.fetches.values() if f['point'] or f['op'] == 'getWeights2D'}
    colour_t = {'t%d' % a for a in sh.fetches} - cube_t - shadow_t - {'t%d' % a for a in nfetch}
    found = []
    seen = set()

    def walk(n, depth=0):
        if id(n) in seen or depth > 60:
            return
        seen.add(id(n))
        if n.op in ('mul', 'mad') and n.comp is not None:
            for x, y in ((n.ins[0], n.ins[1]), (n.ins[1], n.ins[0])):
                if 'c5' in x.tags() and 'c5' not in y.tags() and y.tags() & colour_t:
                    found.append((n, y))
        for i in n.ins:
            walk(i, depth + 1)
    for n in nodes_out or []:
        walk(n)
    if found:
        # The multiply per component; then follow the operand back past the
        # environment map (games blend reflections into the albedo).
        mul_addr = min(n.addr for n, y in found)
        per_comp = {}
        for n, y in found:
            if n.addr == mul_addr:
                per_comp[n.comp] = y
        t['_alb'] = (mul_addr, per_comp)
        alb = trace_albedo(sh, mul_addr, cube_t, colour_t)
        if alb:
            t['a'] = alb
    # Ambient occlusion: a one-channel (or grey) map on its own texture coordinates
    # (straight from an interpolator, unlike the albedo's), used by the output.
    out_tags = set()
    for n in nodes_out or []:
        out_tags |= n.tags()
    alb_coord = None
    if 'a' in t:
        reg = int(t['a'][1:t['a'].index('.')])
        at = t['a'].split('@')[1]
        src = sh.fetches.get(int(at)) if at.isdigit() else None
        if src:
            alb_coord = src['coord']
    for f in sorted(sh.fetches.values(), key=lambda f: f['addr']):
        written = [i for i, ch in enumerate(f['dsw']) if ch in XYZW]
        if (f['op'] != 'tfetch2D' or f['point'] or len(written) not in (1, 3) or not f['coord']
                or 't%d' % f['addr'] not in colour_t or 't%d' % f['addr'] not in out_tags):
            continue
        reg = int(re.search(r'\d+', f['coord']).group(0))
        st = sh.state_before(f['addr'])
        coord_nodes = [st.get((reg, XYZW.index(ch)), (None,))[0] for ch in f['coord'].split('.')[1]]
        if not all(n is None or n.op == 'in' for n in coord_nodes):  # None: not written yet
            continue
        if alb_coord and f['coord'].split('.')[0] == alb_coord.split('.')[0]:
            continue
        t['ao'] = 'r%d.%s@%d' % (f['dest'], XYZW[written[0]], f['addr'])
        break
    t['_notes'] = notes
    t['_colour'] = sorted(colour_t)
    t['_nfetch'] = nfetch
    return t


def trace_albedo(sh, mul_addr, cube_t, colour_t):
    """Register holding the albedo the light multiplies, before any env blend.

    Starts at the colour operand of the multiply and walks back through the
    instructions that wrote it, depth first, preferring operands that don't
    depend on the cube map, until it reaches a colour (three distinct
    components) that doesn't depend on it either. 1 - x is stepped through
    (screen blends with the reflection).
    """
    entries = [e for e in sh.alu.get(mul_addr, []) if e[0] in ('mul', 'mad') and e[2] is not None]
    if not entries:
        return None
    st = sh.state_before(mul_addr)
    for s in entries[0][4][:2]:
        if s['kind'] != 'r':
            continue
        comps = [XYZW.index(s['sw'][e[2]]) for e in entries[:3]]
        nodes = [st.get((s['idx'], c), (None,))[0] for c in comps]
        if all(nodes) and not any('c5' in n.tags() for n in nodes) and any(n.tags() & colour_t for n in nodes):
            r = albedo_dfs(sh, s['idx'], comps, mul_addr, cube_t, colour_t, 0)
            if r:
                return r
    return None


def albedo_dfs(sh, reg, comps, addr, cube_t, colour_t, depth):
    if depth > 16:
        return None
    st = sh.state_before(addr)
    if any((reg, c) not in st for c in comps):
        return None
    nodes = [st[(reg, c)] for c in comps]
    ns = [n for n, a in nodes]
    writer = max(a for n, a in nodes)
    cube = any(n.tags() & cube_t for n in ns)
    broadcast = len({id(n) for n in ns}) < len(ns)
    entries = {}
    if len({a for n, a in nodes}) == 1:
        entries = {e[2]: e for e in sh.alu.get(writer, []) if e[3] in ns}
    e0 = entries.get(comps[0]) if (len(entries) == len(set(comps)) and None not in entries
                                   and all(c in entries for c in comps)) else None
    inverted = bool(e0 and e0[0] == 'add' and len(e0[4]) == 2 and e0[4][0]['kind'] == 'r'
                    and e0[4][0]['neg'] and e0[4][1]['kind'] == 'c')
    if broadcast and not cube:
        return None
    if not cube and not inverted and all(n.op.startswith('tfetch') or n.op in ('mul', 'mad', 'add') for n in ns):
        order = [texel_channel(n, colour_t) for n in ns]
        if None not in order and len(set(order)) == 3:
            comps = [c for _, c in sorted(zip(order, comps))]
        return 'r%d.%s@%d' % (reg, ''.join(XYZW[c] for c in comps), writer)
    if not e0 or e0[0] not in ('add', 'mul', 'mad', 'max', 'min', 'cndeq', 'cndge', 'cndgt'):
        return None
    options = []
    for si, s in enumerate(e0[4]):
        if s['kind'] != 'r':
            continue
        sub = [entries[c][3].ins[si] for c in comps]
        if not any(x.tags() & colour_t for x in sub):
            continue
        has_cube = any(x.tags() & cube_t for x in sub)
        options.append((has_cube, si, s))
    for has_cube, si, s in sorted(options, key=lambda o: o[:2]):
        r = albedo_dfs(sh, s['idx'], [XYZW.index(s['sw'][c]) for c in comps], writer,
                       cube_t, colour_t, depth + 1)
        if r:
            return r
    return None


def texel_channel(n, colour_t, depth=0):
    """Channel of the colour texture a value comes from."""
    if n.op.startswith('tfetch'):
        return n.comp
    if depth > 8:
        return None
    for i in n.ins:
        if i.tags() & colour_t:
            return texel_channel(i, colour_t, depth + 1)
    return None


def fallback_rough(power):
    return (2.0 / (power + 2.0)) ** 0.25


def is_lit(path):
    text = open(path, errors='replace').read()
    return text.count('c1.') >= 2 and 'c5.' in text and re.search(r'\blog\b', text)


def main():
    folder = sys.argv[1]
    only = set(h.upper() for h in sys.argv[2:])
    for name in sorted(os.listdir(folder)):
        m = re.match(r'shader_([0-9A-F]{16})\.ucode\.frag$', name)
        if not m or (only and m.group(1) not in only):
            continue
        path = os.path.join(folder, name)
        if not is_lit(path):
            continue
        sh = Shader(path)
        t = find_taps(sh)
        need = ['n', 'v', 'a', 'out']
        keys = ['rm', 'n', 'v', 'a', 'lit', 'env', 'ao', 'out', 'pow']
        line = m.group(1) + ' ' + ' '.join('%s=%s' % (k, t[k]) for k in keys if k in t)
        line += ' rough_scale=0.66 bump=5'  # the pack's wood and stone are nearly matte; stronger relief
        missing = [k for k in need if k not in t]
        if missing:
            print('# %s  missing %s' % (line, ','.join(missing)))
        else:
            print(line)


if __name__ == '__main__':
    main()
