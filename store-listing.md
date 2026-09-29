# Store listing

The text for the Pebble appstore page. Kept here so it changes with the
face. `pebble publish` carries the release notes; the title and description
go up with `tools/store.py push`.

## Title

Headway

## Short description

A watch that reads past your sleeve, and knows when you're near the bus.

## Description

A plain watch that reads past your sleeve: the time, heart rate, weather and
date hang from the outer edge, so the part a cuff uncovers first is the part
you need. Dark at night, light by day.

It knows two transit systems: Connect in Logan, Utah, and UTA from Ogden to
Provo. The half-hourly check that fetches the weather also tells it whether
you're near one. If you are, flick your wrist at a stop to see what leaves
next: each route as a coloured badge with its next times, live predictions
marked with a small arc where there are any. If you're not, a flick doesn't
check your location at all.

At Connect's transit centre it counts down to the next departure, with a
buzz at five minutes, and a flick shows which routes' buses are in. While buses run there, it checks every five minutes
nearby so the countdown is on when you arrive, and stops after the last bus.

Your location stays on your phone: the weather service gets it rounded to
about 100 m, and live times are asked for by stop number (UTA's come from
its own feed, whole, so nothing is asked). No accounts. Turn
transit off and it never checks your location for transit.

Settings on the phone for modules, theme, accent, wrist, clock and units.
Round watches aren't supported.

## Release notes (1.18.1)

A flick made while the phone is still answering an earlier one lights the
face again rather than staying dark until the answer comes. UTA's live feed
is asked for at the flick, alongside the timetable, so its times come
sooner. Small fixes at the yard.

## Release notes (1.18.0)

At Connect's yard the count of buses still out is big now: it takes the
countdown's place, 3 OUT, or 0 OUT when they're all in.

## Release notes (1.17.0)

A flick at Connect's transit centre now shows which buses are in: every
route that leaves there that day as a badge, solid where its bus is at a
bay and hollow where it isn't, with the G and B loops' next times under
them. At Connect's yard in North Logan a flick says how many buses are
still out, or that they're all in. A flick made while the phone is still
answering an earlier one shows its seconds at once rather than waiting.

## Release notes (1.16.0)

Live times for UTA. A flick at a TRAX, FrontRunner or bus stop reads UTA's
own live feed and marks predicted times with the arc; a run UTA has
cancelled drops out. At a timepoint, where the bus waits for its time, a
bus is never shown leaving early. Buses that start at the stop you're at
show the timetable until they set off. With no answer in three seconds
it's the timetable, as before.

## Release notes (1.15.2)

More memory to spare on the Pebble Classic.

## Release notes (1.15.1)

The hairline after a flick starts draining the moment you flick rather
than when the phone answers, and drains in even steps: a pixel at a time
while the light is on, then three at a time about every fifth of a second.
An answer that comes late still gets at least eight seconds on screen.

## Release notes (1.15.0)

Location, more lightly. One rough check every half hour now serves the
weather and transit both, and a flick only takes a precise fix where you
could be near a transit system; in another state it never does. Near
Connect's transit centre the watch's step count, not a timer, asks for a
look as you walk up, and nothing is checked there after the last bus.
"At the transit centre" now means within 100 m, not 300. A flick's seconds
stand in the colon, and the hairline counts the answer down. A bus due
this minute in a board's second column reads NOW.

## Release notes (1.14.0)

UTA: flick at any stop or station from Ogden to Provo and the face shows
what leaves next: TRAX by line (BLUE, RED, GRN, S), FrontRunner (FR) and the
buses, every bay of a station read as one. Timetables now follow each
agency's own calendar, so holidays and special service days show what really
runs, and late-night trips show after midnight.

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
