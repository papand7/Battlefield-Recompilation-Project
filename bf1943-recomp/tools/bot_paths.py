"""Draws the bots' tracks from a BF_BOT_LOG file (seen from above).

  python tools/bot_paths.py bots.csv [out.png]

Flags are squares (white: nobody's, blue: US, red: Japan) with their number,
vehicles grey dots with their kind, each bot a coloured line from a ring
(start) to a dot (end); thicker where it sat in a vehicle. The scenario's
result, if any, is written at the top.
"""
import csv
import sys

from PIL import Image, ImageDraw

SIZE = 1000
BOT_COLOURS = [(255, 200, 40), (60, 220, 120), (240, 90, 220), (80, 200, 255), (255, 120, 60), (180, 255, 80),
               (255, 255, 255), (150, 120, 255)]
OWNER = {0: (235, 235, 235), 1: (70, 130, 255), 2: (230, 60, 60)}


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else src.rsplit('.', 1)[0] + '.png'
    flags, vehicles, tracks, result = {}, {}, {}, None
    with open(src, newline='') as f:
        for row in csv.reader(f):
            if not row or row[0] == 't':
                continue
            if row[0] == '#flag':
                flags[int(row[1])] = (float(row[2]), float(row[3]), int(row[4]))
            elif row[0] == '#vehicle':
                vehicles.setdefault(row[1], set()).add((round(float(row[2])), round(float(row[3]))))
            elif row[0] == '#result':
                result = ','.join(row[1:])
            else:
                t, bot, team, x, y, z, veh, state = row[:8]
                tracks.setdefault(int(bot), []).append((float(t), float(x), float(z), veh, state))

    xs = [p[0] for p in flags.values()] + [p[1] for tr in tracks.values() for p in tr]
    zs = [p[1] for p in flags.values()] + [p[2] for tr in tracks.values() for p in tr]
    if not xs:
        sys.exit('nothing to draw')
    pad = 40
    span = max(max(xs) - min(xs), max(zs) - min(zs), 50)
    x0, z0 = (max(xs) + min(xs) - span) / 2, (max(zs) + min(zs) - span) / 2
    scale = (SIZE - 2 * pad) / span

    def at(x, z):  # +x right, +z up
        return pad + (x - x0) * scale, SIZE - pad - (z - z0) * scale

    img = Image.new('RGB', (SIZE, SIZE), (24, 30, 38))
    d = ImageDraw.Draw(img)
    for kind, spots in vehicles.items():
        for x, z in spots:
            px, pz = at(x, z)
            if 0 <= px < SIZE and 0 <= pz < SIZE:
                d.ellipse([px - 3, pz - 3, px + 3, pz + 3], fill=(110, 110, 110))
                d.text((px + 5, pz - 6), kind, fill=(120, 120, 120))
    for i, (x, z, owner) in flags.items():
        px, pz = at(x, z)
        d.rectangle([px - 9, pz - 9, px + 9, pz + 9], outline=OWNER.get(owner, (200, 200, 200)), width=3)
        d.text((px - 3, pz - 6), str(i), fill=(255, 255, 255))
    for bot, tr in sorted(tracks.items()):
        c = BOT_COLOURS[(bot - 1) % len(BOT_COLOURS)]
        for a, b in zip(tr, tr[1:]):
            if b[0] - a[0] > 5:  # died and respawned: no line across
                continue
            d.line([at(a[1], a[2]), at(b[1], b[2])], fill=c, width=4 if b[3] else 2)
        sx, sz = at(tr[0][1], tr[0][2])
        ex, ez = at(tr[-1][1], tr[-1][2])
        d.ellipse([sx - 6, sz - 6, sx + 6, sz + 6], outline=c, width=2)
        d.ellipse([ex - 5, ez - 5, ex + 5, ez + 5], fill=c)
        d.text((ex + 8, ez - 6), f'bot{bot}: {tr[-1][4]}', fill=c)
    d.text((10, 8), result or 'no result', fill=(120, 255, 120) if result and result.startswith('PASS') else
           (255, 120, 120) if result else (200, 200, 200))
    span_m = 100 * round(span / 500) or 50
    d.line([(10, SIZE - 15), (10 + span_m * scale, SIZE - 15)], fill=(200, 200, 200), width=2)
    d.text((14, SIZE - 30), f'{span_m} m', fill=(200, 200, 200))
    img.save(out)
    print(out)


if __name__ == '__main__':
    main()
