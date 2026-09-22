# Headway

A Pebble watchface for riding a fixed-headway service — a train, a ferry, a
shuttle — where what you actually need to know is *how much of this half hour
is left*.

Built from a [Claude Design](https://claude.ai/design) spec.

![waiting](screenshots/waiting.png)
![boarding](screenshots/boarding.png)
![departing](screenshots/departing.png)

## Read it from the cuff in

A sleeve slides in from the wrist side and uncovers the far edge first, so the
face ranks its information by edge:

| Zone | | |
|---|---|---|
| **01 Rail** | outer edge | A 7px column that drains as the headway runs out. Ticks at ¼, ½ and ¾. Readable as a shape under any cuff, and at arm's length while driving. |
| **02 Countdown** | | Minutes to the next departure, with its clock time. At five minutes the block fills solid; at zero it reads `NOW` for one minute, then rolls to the next run. |
| **03 Minute** | | The time sits flush to the outer edge, so the minute digits survive a cuff that hides the hour. |
| **04 Hour & date** | wrist side | First to disappear. |

Optional extras (heart rate, steps, weather) sit on the wrist side too, below
the date — so a sleeve covers them before anything that matters.

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
- **Extras** — heart rate, steps, weather, in °C or °F

Weather comes from [Open-Meteo](https://open-meteo.com), which needs no API key
and no account.

## Platforms

Rectangular displays only: `aplite`, `basalt`, `diorite`, `flint` (144×168) and
`emery` (200×228). The layout is an edge-flush rail on a rectangle, which a
round screen cannot carry without a different design, so `chalk` and `gabbro`
are not targeted.

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
system Gothic Bold the design specifies.

## Licence

Code under the MIT licence, see `LICENSE`. Barlow Condensed is under the SIL
Open Font License, see `resources/fonts/OFL.txt`.
