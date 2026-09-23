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

Up to three, in the band the design leaves open, drawn as a caption over a
value — `BPM 72`, `STEPS 4.8K`, `BATT 64%`. Each slot takes heart rate, steps,
battery, weather or nothing, and the captions can be swapped for icons.

![modules with icons](screenshots/modules-icons.png)

They sit on the wrist side, below the date, so a sleeve covers them before the
countdown or the rail — the design's zone ranking is preserved.

Weather always shows its sky condition as an icon rather than a caption: sun,
partly cloudy, cloud, fog, rain, snow or thunder, drawn from the WMO code. A
module whose sensor is unavailable — heart rate on a watch without the sensor,
weather before the first fetch — simply does not appear.

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
- **Icons instead of captions** — weather always uses its condition icon
- **Units** — °C or °F

Weather comes from [Open-Meteo](https://open-meteo.com), which needs no API key
and no account.

## Platforms

Rectangular displays only: `aplite`, `basalt`, `diorite`, `flint` (144×168) and
`emery` (200×228). The layout is an edge-flush rail on a rectangle, which a
round screen cannot carry without a different design, so `chalk` and `gabbro`
are not targeted.

| aplite | diorite | flint | emery |
|---|---|---|---|
| ![aplite](screenshots/aplite.png) | ![diorite](screenshots/diorite.png) | ![flint](screenshots/flint.png) | ![emery](screenshots/emery.png) |

On one-bit watches the accent becomes ink and the quarter-hour ticks dither,
as the design's mono note asks. Modules whose sensor the watch lacks remove
themselves — which is why aplite shows only battery and weather.

Light mode, with the accent set to red:

![light](screenshots/light.png)

## Building

```bash
npm install
pebble build
pebble install --emulator basalt
```

## Type

Numerals are [Barlow Condensed](https://github.com/jpt/barlow) SemiBold, bundled
as a resource at 60px (144-wide) and 83px (emery) so they scale with the
display — Pebble's system fonts are a fixed pixel size and its largest numeric
face, `LECO_42`, renders at half the scale the design calls for. Labels use the
system Gothic Bold the design specifies, and module captions use Gothic 09,
where a hand-tuned bitmap face beats anything a TTF rasterises at 7px.

Module icons are drawn as vectors rather than bundled bitmaps, so they invert
with the theme and scale with the display at no resource cost.

## Licence

Code under the MIT licence, see `LICENSE`. Barlow Condensed is under the SIL
Open Font License, see `resources/fonts/OFL.txt`.
