# Live over the timetable: the rules

Headway (the watch, `src/pkjs/index.js`) and Cache Rider (the web app, `app/rt.js`) each lay Connect's
GTFS-realtime over its timetable, and each does it in its own code, on purpose: one can't take the other down,
and a mistake in one shows up when the two are compared. This file is what the two agree the answer should be,
kept word for word in both repos. A rule changes here first, in both copies, and then in each app.

The feed comes to both through the relay (`cacherider/worker/src/index.js`), which passes it on and doesn't
interpret it. UTA's comes to Headway straight from UTA, in the same shape.

## The feed

1. **Stale is nothing.** An answer whose own time (the older of the feeds' headers) is more than 90 s old says
   nothing live: the timetable stands, unmarked. A bus position more than 180 s old is no bus.
2. **Match a stop and a place.** A prediction belongs to a departure by the stop and the trip's
   `stop_sequence`. Most of Connect's trips leave from and return to the same bay, so the stop alone would take
   the arrival back for the departure. A feed that names no stop (UTA) is matched by the sequence alone.
3. **No boarding at a trip's last stop.** The timetable has no departure there. A prediction for it is only an
   arrival.
4. **Skipped stops drop the departure.** This includes a skipped stop the feed gives no time for.
5. **Cancelled runs drop.** A trip whose `schedule_relationship` is CANCELED isn't coming.
6. **No word is the timetable.** A departure the feed says nothing about is shown as timetabled, unmarked.

## Holding and boarding

7. **Never early at the hub.** A departure from a Transit Center bay is never before its timetabled minute; the
   feed can only make it later.
8. **Never early at a timepoint.** At a stop the timetable marks as a timepoint (`stop_times.timepoint = 1`), a
   departure is never before its timetabled minute: an early bus waits there.
9. **Boarding.** A bus still listed at its hub bay after the feed's time for it is boarding and leaves now. The
   listing is trusted for 10 minutes past its time on a loop (G, B) and 30 on a route. Past that it's the feed's
   leftover.
10. **In before out** (Cache Rider). A departure from the hub is no earlier than the bus that runs it gets in from
    its trip before.
11. **Loops spacing** (Cache Rider). When the G or B run far off their timetable, they are spaced by where the
    buses are, not by the timetable. The hub and timepoint holds don't apply to them then.

## Where the buses are

12. **At the hub.** A bus is in at the Transit Center within 110 m of its centre (Cache Rider) or 60 m of a bay
    (Headway), by a fresh position on a trip of that route.
13. **Out from the yard.** A bus is out when it has a fresh position more than 250 m from the yard. The feed
    drops a bus when its last trip ends, not at the gate, so "none on a trip" is never "all in".

## The day

14. **After midnight.** A departure written past 24:00 belongs to the evening before's service. (Cache Rider
    doesn't do this yet. It doesn't matter for Connect today, whose latest trip ends at 9:03 PM.)
15. **Calendars.** Which services run on a day comes from `calendar.txt` and `calendar_dates.txt` both.
    Headway's hub countdown (the G and B chips) still goes by day types, so it doesn't know holidays.

## Where each app stands

| Rule | Headway | Cache Rider |
|---|---|---|
| 1 stale | ✓ | by its own fetch time, not the feed's |
| 2 stop and place | ✓ | ✓ (drops the end sequence) |
| 3 last stop | ✓ | ✓ |
| 4 skipped | ✓ | ✓ |
| 5 cancelled | ✓ | ✓ (branch `live-rules`) |
| 6 no word | ✓ | ✓, plus "on time" when the bus on the run before is due in |
| 7 hub hold | ✓ | ✓ |
| 8 timepoint hold | ✓ | ✓ (branch `live-rules`) |
| 9 boarding cap | ✓ | ✓ |
| 10 in before out | — | ✓ |
| 11 loops spacing | — | ✓ |
| 14 after midnight | ✓ | — |
| 15 calendars | flick ✓, hub countdown — | ✓ |
