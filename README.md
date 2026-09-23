# Headway

A Pebble watchface for riding a fixed-headway service — a train, a ferry, a
shuttle — where what you actually need to know is *how much of this half hour
is left*.

Built from a [Claude Design](https://claude.ai/design) spec.

| Waiting | Boarding | Departing |
|---|---|---|
| ![waiting](screenshots/waiting.png) | ![boarding](screenshots/boarding.png) | ![departing](screenshots/departing.png) |

## Read it from the cuff in

A sleeve slides in from the wrist side and uncovers the far edge first, so the
face ranks its information by edge:

| Zone | | |
|---|---|---|
| **01 Rail** | outer edge | A 7px column that drains as the headway runs out. Ticks at ¼, ½ and ¾. Readable as a shape under any cuff, and at arm's length while driving. |
| **02 Countdown** | | Minutes to the next departure, with its clock time. At five minutes the block fills solid; at zero it reads `NOW` for one minute, then rolls to the next run. |
| **03 Minute** | | The time sits flush to the outer edge, so the minute digits survive a cuff that hides the hour. |
| **04 Hour & date** | wrist side | First to disappear. |

## Modules

Off by default: out of the box the face is the plain digital watch the design
describes. Up to three can be switched on from the settings page, in the band
the design leaves open, drawn as a caption over a value — `BPM 72`,
`STEPS 4.8K`, `BATT 64%`. Each slot takes heart rate, steps, battery, weather
or nothing, and the captions can be swapped for icons.

| Captions | Icons |
|---|---|
| ![modules](screenshots/modules.png) | ![modules with icons](screenshots/modules-icons.png) |

They sit on the wrist side, below the date, so a sleeve covers them before the
countdown or the rail — the design's zone ranking is preserved.

Weather captions with the sky itself — `CLOUDY 12°` — rather than the unit,
and as an icon draws it: sun, partly cloudy, cloud, fog, rain, snow or
thunder, from the WMO code. A module whose sensor is unavailable — heart rate
on a watch without the sensor, weather before the first fetch — simply does
not appear.

## States

- **Waiting** — more than five minutes out. Quiet.
- **Boarding** — five minutes out, the block goes solid. Optional single buzz.
- **Departing** — rail empty, `NOW` for one minute.

## Settings

Reachable from the Pebble app.

- **Headway** — 30 / 20 / 15 minutes between runs
- **Departure offset** — minutes past the hour of the first run
- **Boarding buzz** — a single pulse at T-5, off by default
- **Theme** — dark (default), light, or dark at night only, with a configurable night window
- **Accent** — any colour; drives the rail, the colon and the boarding block
- **Wrist** — mirrors the whole layout so the sleeve comes from the other side
- **Time format** — system, 12h or 24h
- **Modules** — three slots: heart rate, steps, battery, weather or nothing
- **Icons instead of captions**
- **Units** — °C or °F

Weather comes from [Open-Meteo](https://open-meteo.com), which needs no API key
and no account.

## Platforms

Rectangular displays only: `aplite`, `basalt`, `diorite` (144×168) and `emery`
(200×228). The layout is an edge-flush rail on a rectangle, which a round
screen cannot carry without a different design, so `chalk` and `gabbro` are
not targeted.

`flint` (Core 2 Duo) is out of this release. Its firmware gives a watchface a
smaller stack than the older watches do, and the text renderer runs out of it
on this face; 1.0.2 was the last build that ran there, and it comes back once
the face fits.

| aplite | diorite | emery |
|---|---|---|
| ![aplite](screenshots/aplite.png) | ![diorite](screenshots/diorite.png) | ![emery](screenshots/emery.png) |

On one-bit watches the accent becomes ink and the quarter-hour ticks dither,
as the design's mono note asks. A module whose sensor the watch lacks — heart
rate on aplite, say — removes itself rather than showing a blank.

Light mode with the accent set to red, and the right-wrist layout, mirrored
so the sleeve comes from the other side:

| Light | Right wrist |
|---|---|
| ![light](screenshots/light.png) | ![right wrist](screenshots/wrist-right.png) |

## Building

```bash
npm install
pebble build
pebble install --emulator basalt
```

The fonts are prebuilt. To regenerate them after changing a size or a
character set in `tools/genfonts.py`:

```bash
~/.local/share/uv/tools/pebble-tool/bin/python tools/genfonts.py
```

## Type

Everything is [Barlow Condensed](https://github.com/jpt/barlow) SemiBold, as
the design draws it, at the design's sizes: 60px numerals (83px on emery),
44px countdown, 11px labels, 12px date and 9px module captions. Digits are
set tabular and the labels tracked, as the design's CSS has them, by drawing
a glyph at a time.

The SDK's font generator rasterises through the font's own hints, which
scatter one- and two-pixel stems across the small sizes. `tools/fontgen.py`
is that generator, vendored from the SDK with FreeType's auto-hinter forced,
and the `.pfo` blobs it writes ship as raw resources, which
`fonts_load_custom_font` takes as it would any font.

Module icons are hand-placed pixel patterns, scaled by nearest neighbour, so
they invert with the theme and hold their shape on emery at no resource cost.

## Licence

Code under the MIT licence, see `LICENSE`. Barlow Condensed is under the SIL
Open Font License, see `resources/fonts/OFL.txt`.
