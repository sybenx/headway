"""Read and write Pebble .pfo bitmap fonts as plain glyph tables, so a font can be
drawn by hand — a JSON of rows of '#' and '.' — and still ship through the same
raw-resource path as the rasterised ones.

    python tools/pfo.py decode resources/fonts/cap_9.pfo > cap_9.json
    python tools/pfo.py encode cap_9.json resources/fonts/cap_9.pfo

The table maps a character to {"l": left bearing, "t": rows from the top of the
line box to the glyph's top, "a": advance, "rows": ["..#..", ...]}, plus
"height": the line box (the font's pixel size). The format is fontgen's
version 3 with 16-bit offsets: a 10-byte header, a 255-bucket hash table, the
offset tables, then the glyphs, each a 5-byte header and its bits packed
LSB-first into 32-bit words.
"""
import json, struct, sys

WILDCARD = 0x25AF
HASH = 255


def decode(data):
    ver, height, n, wild, hsize, cpb, hdr, feat = struct.unpack_from('<BBHHBBBB', data, 0)
    assert ver == 3 and hsize == HASH and cpb == 2 and hdr == 10 and feat & 1, 'not a v3 font with 16-bit offsets'
    if feat & 2: raise SystemExit('RLE4-compressed fonts are not handled')
    pos = hdr
    buckets = [struct.unpack_from('<BBH', data, pos + 4 * i) for i in range(HASH)]
    pos += 4 * HASH
    entries = []
    for _, size, _ in buckets:
        for _ in range(size):
            entries.append(struct.unpack_from('<HH', data, pos)); pos += 4
    gt = pos   # the glyph table; offsets count from here (the first 4 bytes are padding)
    glyphs = {}
    for cp, off in entries:
        w, h, l, t, a = struct.unpack_from('<BBbbb', data, gt + off)
        bits = data[gt + off + 5: gt + off + 5 + max(4, ((w * h + 31) // 32) * 4)]
        rows = []
        for y in range(h):
            rows.append(''.join('#' if bits[(y * w + x) >> 3] >> ((y * w + x) & 7) & 1 else '.' for x in range(w)))
        glyphs[chr(cp)] = {'l': l, 't': t, 'a': a, 'rows': rows}
    return {'height': height, 'glyphs': glyphs}


def encode(font):
    height = font['height']; glyphs = font['glyphs']
    cps = sorted(ord(c) for c in glyphs)
    assert WILDCARD in cps, 'the font needs a wildcard glyph (U+25AF)'
    order = [WILDCARD] + [c for c in cps if c != WILDCARD]
    table = bytearray(b'\0\0\0\0'); off = {}
    for cp in order:
        g = glyphs[chr(cp)]; rows = g['rows']; w = len(rows[0]) if rows and rows[0] else 0; h = len(rows) if w else 0
        off[cp] = len(table)
        table += struct.pack('<BBbbb', w, h, g['l'], g['t'], g['a'])
        bits = bytearray(max(4, ((w * h + 31) // 32) * 4))
        for y, r in enumerate(rows):
            for x, px in enumerate(r):
                if px == '#': k = y * w + x; bits[k >> 3] |= 1 << (k & 7)
        table += bits
    buckets = [[] for _ in range(HASH)]
    for cp in cps: buckets[cp % HASH].append(cp)
    ht = bytearray(); ot = bytearray(); run = 0
    for i, b in enumerate(buckets):
        ht += struct.pack('<BBH', i, len(b), run)
        for cp in sorted(b): ot += struct.pack('<HH', cp, off[cp]); run += 4
    head = struct.pack('<BBHHBBBB', 3, height, len(cps), WILDCARD, HASH, 2, 10, 1)
    return bytes(head + ht + ot + table)


if __name__ == '__main__':
    if sys.argv[1] == 'decode':
        json.dump(decode(open(sys.argv[2], 'rb').read()), sys.stdout, indent=1, ensure_ascii=False)
    elif sys.argv[1] == 'encode':
        open(sys.argv[3], 'wb').write(encode(json.load(open(sys.argv[2]))))
