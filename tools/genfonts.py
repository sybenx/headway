#!/usr/bin/env python
"""Build Headway's font resources with the auto-hinter on.

The SDK's own generator rasterises a TrueType font through its native hints,
which turn Barlow Condensed's even strokes into a mix of one- and two-pixel
stems at label sizes. This runs the same generator (tools/fontgen.py, vendored
from the SDK) with FreeType's auto-hinter forced, and writes the result as
.pfo blobs that package.json ships as raw resources; fonts_load_custom_font
takes them as it would any font resource.

Run with the pebble tool's Python, which has freetype-py:
  ~/.local/share/uv/tools/pebble-tool/bin/python tools/genfonts.py
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from fontgen import Font, MAX_GLYPHS

TTF = os.path.join(os.path.dirname(__file__), '..', 'resources', 'fonts', 'BarlowCondensed-SemiBold.ttf')
OUT = os.path.join(os.path.dirname(__file__), '..', 'resources', 'fonts')
DIGITS = '[0-9:]'
# (name, pixel height, glyph regex, MAX_FONT_GLYPH_SIZE of the platform: 256 on
# the 144px watches, 512 on emery)
FONTS = [
    ('time_60', 60, DIGITS), ('time_83', 83, DIGITS),
    ('count_44', 44, '[0-9NOW]'), ('count_61', 61, '[0-9NOW]'),
    ('time_48', 48, DIGITS), ('time_67', 67, DIGITS),         # the flick view's smaller time
    ('count_30', 30, '[0-9NOW]'), ('count_42', 42, '[0-9NOW]'),  # and its smaller countdown
    ('mod_15', 15, '[0-9.%°KA-Z: ]'), ('mod_21', 21, '[0-9.%°KA-Z: ]'),   # letters and a colon for the stop view's badges and clock times
    ('label_11', 11, '[A-Z0-9:. ]'), ('label_15', 15, '[A-Z0-9:. ]'),   # the point in a distance
    ('date_12', 12, '[A-Z0-9 ]'), ('date_17', 17, '[A-Z0-9 ]'),
    ('date_18', 18, '[A-Z0-9 ]'), ('date_25', 25, '[A-Z0-9 ]'),   # the idle face's larger date
    ('cap_9', 9, '[A-Z%°]'), ('cap_12', 12, '[A-Z%°]'),
]
for name, height, regex in FONTS:
    f = Font(TTF, height, MAX_GLYPHS, 512 if name.endswith(('_83', '_61', '_21', '_15', '_17', '_12', '_25', '_67', '_42')) and name not in ('mod_15',) else 256, False)
    f.set_regex_filter(regex)
    f.build_tables()
    data = f.bitstring()
    with open(os.path.join(OUT, name + '.pfo'), 'wb') as out:
        out.write(data)
    print(f'{name}.pfo {len(data)} bytes')
