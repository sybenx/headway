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
marked with a small arc where there are any. Away from a stop, a flick shows
the stops near you, each with its next buses. If you're not, a flick doesn't
check your location at all.

At Connect's transit centre it counts down to the next departure, with a
buzz at five minutes, and a flick shows which routes' buses are in. While buses run there, it checks every five minutes
nearby so the countdown is on when you arrive, and stops after the last bus.

Your location stays on your phone, and it never asks for a high-accuracy
fix: the weather service gets it rounded to about 100 m, and live times are
asked for by stop number (UTA's come from its own feed, whole, so nothing is
asked). No accounts. Turn transit off and it never checks your location for
transit.

Settings on the phone for modules, theme, accent, wrist, clock and units.
Round watches aren't supported.

## Release notes (1.21.0)

At Connect's yard the watch itself asks for the count through the hour after
the last trip, so it keeps coming when the phone would sleep. A flick there
says how many buses are pulling in and how far the nearest is, and after the
last trip the minutes count to 25, then the face is itself again.

## Release notes (1.20.0)

A flick answers more of what you're asking. At a stop it shows every route
there, taking the screen as it needs, with the time as a line at the top. Away
from a stop it shows the four nearest stops, how far each is, and the next
two buses at each.

## Release notes (1.19.4)

At the transit centre, for ten minutes after a G or B departure, its minutes
show before the next one's (:25 10:41), so a late bus still at its bay can
be told from the one after it. Where it would cost a module its place, it
stays out.

## Release notes (1.19.3)

A one-row answer beside the modules reads as a list: its lines start
together. A bus running early shows early at a timepoint too: most wait
there, but a time held to the timetable could cost you the bus when one
doesn't. At the transit centre buses are still never shown leaving early.

## Release notes (1.19.2)

At the transit centre a bus still listed at its bay after its time reads NOW
for ten minutes on a loop and thirty on a route, not indefinitely. Connect's
stops go by their landmarks, like FIRE STATION or KEY BANK, where the feed
gives one.

## Release notes (1.19.1)

Live times at stops a trip both leaves from and comes back to, like the
transit centre's bays and the loops' ends, are the departure's, not the
arrival back half an hour later.

## Release notes (1.19.0)

At Connect's yard in North Logan, for an hour after the day's last trip, the
face shows the buses coming home without a flick: how many are still on
trips, then how long ago the last trip ended, counting up. The feed drops a
bus when its last trip ends, not at the gate, so the minutes are for you to
judge by. Only a phone near the yard checks, and only in that hour. Detoured
buses are counted too.

## Release notes (1.18.9)

At Connect's yard a count of none reads NONE ON TRIPS rather than ALL BUSES
IN: the feed drops a bus as its last trip ends, not at the gate. A rough
location never pushes the stop you're at off the board; its routes come
first. With no signal and a timetable kept from days ago, the stop reads
OLD, since the schedule may have changed since.

## Release notes (1.18.8)

A flick after the last bus takes a fresh location again, so the next day's
first buses are for the stop you're at, not one you passed half an hour ago.

## Release notes (1.18.7)

The flick's hairline runs nearer the edge, clear of the centred time and
the weather at any minute. After Connect's last bus a flick doesn't ask for
a new location: where the phone last knew you were, half an hour ago at
most, does for the next day's first buses.

## Release notes (1.18.6)

The time sits centred as it looks, even at 10:11. At the transit centre a
flick's seconds are in the colon, as everywhere else. The weather's degree
sign hangs past the edge, so its number lines up with the date.

## Release notes (1.18.5)

The face never asks the phone for a high-accuracy fix. A flick takes the
phone's everyday one at its word: every stop within its accuracy of the
nearest answers, so a rough fix shows the stop you're at among them rather
than the wrong one alone.

## Release notes (1.18.4)

A flick with no signal still has its timetable. Every stop you flick at, and
the stops around it, stays on the phone until the agency publishes a new
timetable, and answers when the network doesn't. Only a flick ever asks for
stop timetables; the background checks never do.

## Release notes (1.18.3)

A bus due next minute reads 1 MIN on a flick's answer, at every stop and
at the transit centre, as the countdown says it. NOW keeps meaning the
minute it's due.

## Release notes (1.18.2)

A flick shows the timetable at once, and live times redraw it when they
come, rather than waiting on them. Away from a countdown the outer edge is
bare now, and the face sits centred: the hairline appears with a flick and
drains for as long as its answer stays. At the transit centre a flick's
times read NOW from the minute before, while the bus boards.
Units are Automatic by default: °F and feet in the United States, °C and
metres elsewhere. A choice already saved stays as it was.

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
