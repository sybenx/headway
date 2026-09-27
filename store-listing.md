# Store listing

The text for the Pebble appstore page. Kept here so it changes with the
face. `pebble publish` carries the release notes; the title and description
go up with `tools/store.py push`.

## Title

Headway

## Short description

A watch that reads past your sleeve, and knows when you're near the bus.

## Description

Headway is a plain watch most of the day: big time, your heart rate and the
weather as small coloured icons, and the date. Everything hangs from the
outer edge of the screen, so when your cuff slides over the watch the part
you can still see is the part you need. Dark at night, light by day.

Near a transit centre it knows, it changes. A countdown to the next
departure takes the outer edge, minutes and the clock time, and goes solid
with a buzz at five minutes. Routes with their own timetable get a second
countdown as colour chips. Away from the centre, it's the plain watch again.

Flick your wrist at a bus stop and for twelve seconds the face shows what
leaves from there next: the stop, each route as a coloured badge, and its
next times. One route sits beside the icons and the rest of the face stays;
two or three routes take the screen. After the last bus it shows the next
one with its day. Stops across a road from each other read as one. At the
transit centre, where a dozen routes leave together, it answers by time
instead: each departure minute with every route leaving then as a badge.

It knows one system so far: Connect, the Cache Valley Transit District in
Logan, Utah. A flick also asks for Connect's live predictions: a predicted
time carries a small arc, a late bus shows when it will really leave, and
without a live answer the face shows the published schedule, refreshed
nightly. Elsewhere it's the plain watch, and it asks the phone very little.

Settings on the phone: three module slots (heart rate, steps, battery,
weather) as captions or icons; theme and night hours; accent colour; left or
right wrist; 12 or 24 hour; units. Or turn the transit part off, or turn the
countdown on everywhere for any service that runs on a fixed headway.

Colour watches show the badges and icons in colour; Pebble Classic and Pebble
2 draw them in black and white. Round watches aren't supported.

## Release notes (1.13.0)

Live times: a flick asks for Connect's live predictions, and a predicted
time carries a small arc. A late bus shows when it will really leave; one
that will skip the stop drops out. Without a live answer the face shows the
schedule as before. At the transit centre a flick now answers by time: each
departure minute with every route leaving then as a badge, in route order,
instead of three rows that mostly repeated the countdown; a hollow badge is
a bus that isn't in yet. Long stop names now shorten by whole words, and
the badges' 5, 6 and 8 are redrawn. On the charger the battery takes the
heart rate's place, with a small bolt beside it.

## Release notes (1.12.2)

At the transit centre the watch now buzzes once when the next departure is
five minutes out, on by default. The setting can move the buzz to wherever
the countdown runs, or turn it off. The battery icon no longer turns amber
at 20%: on the Time 2 it goes amber at 10% and red at 5%, and elsewhere red
at 10%.

## Release notes (1.12.1)

A different watch from 1.0.5. Out of the box it's the time, heart rate and
the weather as small coloured icons, and the date, everything hung from the
outer edge so a cuff uncovers what matters first. Near Connect's transit
centre in Logan the countdown takes over, with the G and B loops as colour
chips. A flick of the wrist at any Connect stop shows what leaves from it
next. Every flick shows the seconds, and the last minute counts down in
them. The small captions are drawn by hand, a pixel at a time. The settings
page is reordered and reworded to match.
