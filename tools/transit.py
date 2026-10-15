#!/usr/bin/env python3
"""Reduce a GTFS feed to the little the phone needs.

For any system: a file per stop with its scheduled departures and the
services they run on, and an index to find the nearest stop by, served from
GitHub Pages and fetched by the phone on a flick. A system with a hub — a
transit centre whose routes pulse together — also gets the hub's position,
its hours and the departures of the routes on their own timetable, for the
countdown. All of it goes in src/pkjs/transit.json's entry for the agency,
which the face reads none of; the phone does, and sends the watch what is
next.

  python3 tools/transit.py gtfs.zip --agency UTA --hints tools/hints/uta.json \
      --stops-out site/data/v2/uta --data-url https://.../data/v2/uta/
  python3 tools/transit.py gtfs.zip --agency CVTD --hub 41.7406,-111.8309 --routes G,B \
      --name "TRANSIT CTR" ...
"""
import argparse, collections, csv, datetime, hashlib, io, json, math, os, re, sys, zipfile

ap = argparse.ArgumentParser()
ap.add_argument('gtfs', help='path to a GTFS zip')
ap.add_argument('--agency', required=True)
ap.add_argument('--hub', help='lat,lon of the hub, for a system built around one')
ap.add_argument('--base', help='lat,lon,metres of the yard the buses sleep in: a flick there says how many are still out')
ap.add_argument('--routes', default='', help='short names of the routes that leave the hub on their own timetable, comma-separated')
ap.add_argument('--name', default='HUB')
ap.add_argument('--radius', type=int, default=150, help='metres around the hub that count as its stops')
ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', 'src', 'pkjs', 'transit.json'))
ap.add_argument('--hints', help='JSON of headsign and stop-name abbreviations and route labels for this agency')
ap.add_argument('--landmarks', action='store_true', help="name stops by the landmark in the feed's stop_desc where it has one that fits (Connect's: 'Fire Station'), not by the address")
ap.add_argument('--stops-out', help='directory for the per-stop departure files and the stop index (<site>/data/v2/<agency>)')
ap.add_argument('--legacy-out', help='also write the day-type stop files the phones of 1.13.0 and before read (<site>/data/<agency>)')
ap.add_argument('--data-url', default='', help='where the per-stop files are served from, for the phone')
ap.add_argument('--live-url', default='', help='where the phone gets predictions: a relay, or with --live-by feed the agency\'s own feed (optional; the schedule never needs it)')
ap.add_argument('--live-by', choices=['stop', 'feed'], default='stop', help='how live times are got: a relay asked by stop ids, or (feed) the agency\'s own GTFS-realtime TripUpdate, read by the phone and matched by trip and place in it')
ap.add_argument('--margin', type=float, default=3000, help='metres around the outermost stops that still count as the system\'s area')
ap.add_argument('--today', help='YYYYMMDD the data is built on (default: yesterday, UTC); services wholly before it are left out')
a = ap.parse_args()
hints = json.load(open(a.hints)) if a.hints else {}

z = zipfile.ZipFile(a.gtfs)
def table(name):
    if name not in z.namelist(): return []
    return list(csv.DictReader(io.TextIOWrapper(z.open(name), encoding='utf-8-sig')))

def mins(hms):
    h, m = hms.split(':')[:2]
    return int(h) * 60 + int(m)

stops_all = table('stops.txt')
lats = [float(s_['stop_lat']) for s_ in stops_all]
lons = [float(s_['stop_lon']) for s_ in stops_all]
if a.hub:
    lat, lon = (float(v) for v in a.hub.split(','))
else:
    lat, lon = sum(lats) / len(lats), sum(lons) / len(lons)
def dist(la, lo):
    return math.hypot((la - lat) * 111000, (lo - lon) * 111000 * math.cos(math.radians(lat)))

route_by_id = {r['route_id']: r for r in table('routes.txt')}
routes = {rid: r['route_short_name'] for rid, r in route_by_id.items()}
# What a badge says: the agency's short name, or the name riders use where
# the hints give one (UTA's 703 is the Red Line: RED).
labels = hints.get('labels', {})
def label(short_name):
    return labels.get(short_name, short_name)
trips = {t['trip_id']: t for t in table('trips.txt')}
stop_times = table('stop_times.txt')

# A trip's last stop is where it ends, not somewhere to board: the end of
# the line lists arrivals, and a rider at Preston must not be shown the bus
# that has just finished coming in as if it were leaving. The agency marks
# pickup_type on the rare stop it means to, so both are honoured.
terminus = {}
for st in stop_times:
    terminus[st['trip_id']] = max(terminus.get(st['trip_id'], -1), int(st['stop_sequence']))
def boards(st):
    # GTFS lets a stop between timepoints go without a time; nothing to count down to there.
    return int(st['stop_sequence']) < terminus[st['trip_id']] and (st.get('pickup_type') or '0') != '1' \
        and bool(st.get('departure_time'))

# ---- the services, as the agency defines them: a weekly pattern between two
# dates, and dates added or taken away. Many agencies run a third of their
# trips on services that exist only as added dates, and every one takes
# service away on its holidays, so the phone is given the whole calendar and
# works out which services run on the day it is asked about.
DAYS = ['monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday', 'sunday']
calendar = {c['service_id']: c for c in table('calendar.txt')}
added, removed = {}, {}
for c in table('calendar_dates.txt'):
    (added if c['exception_type'] == '1' else removed).setdefault(c['service_id'], []).append(c['date'])
# A timetable already over is left out, and so are exceptions already past:
# the data is rebuilt every night. Yesterday still counts, for the trips of
# its service that run past midnight.
today = a.today or (datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=1)).strftime('%Y%m%d')
# A service keeps its place in the list for as long as the feed does: the stop
# files name services by place, and the phone keeps a day-old index beside stop
# files fetched since, so a list that closed up as services ended would have
# the two disagree on what every place after the gap means. One that has ended
# stays as an empty slot, and its departures leave the stop files.
used = sorted({t['service_id'] for t in trips.values()})
services, svc_index, running = [], {}, set()
for sid in used:
    c = calendar.get(sid)
    mask = sum(1 << i for i, d in enumerate(DAYS) if c and c[d] == '1')
    if c and c['end_date'] < today: mask = 0
    plus = sorted(d for d in added.get(sid, []) if d >= today)
    svc_index[sid] = len(services)
    if not mask and not plus:   # a service that no longer runs
        services.append([0, '', '', [], []])
        continue
    running.add(sid)
    services.append([mask, c['start_date'] if c and mask else '', c['end_date'] if c and mask else '',
                     plus, sorted(d for d in removed.get(sid, []) if d >= today)])

# The day-type reduction the hub and the older phones use: one weekly
# service per day type, the one whose dates run the furthest out.
best = {}
for c in calendar.values():
    on = [d for d in DAYS if c[d] == '1']
    kind = 'weekday' if on and all(d in on for d in DAYS[:5]) and not any(d in on for d in DAYS[5:]) \
        else 'saturday' if on == ['saturday'] else 'sunday' if on == ['sunday'] else None
    if kind and (kind not in best or c['end_date'] > best[kind]['end_date']):
        best[kind] = c
kind_of = {c['service_id']: kind for kind, c in best.items()}

# ---- the hub, where there is one: the minutes its routes leave together
# (the pulse the countdown counts to), the departures of the routes that leave
# it on their own timetable, and its hours, from the first of those to the last.
# The phone works the same out for the day itself from the stop files and the
# calendar (hubDay in index.js); this day-type cut ships in the app for when it
# can't.
def pulse_of(by_route):
    """Every departure of a route that mostly leaves with another. A route that
    always leaves on its own (Connect's 16, at :05 and :50) keeps its own time
    and isn't the pulse; one that starts the day alone before the others join
    it (Saturday's 15 at 9:30) still is."""
    count = collections.Counter(m for ms in by_route.values() for m in ms)
    out = set()
    for ms in by_route.values():
        if 2 * sum(1 for m in ms if count[m] > 1) >= len(ms): out |= ms
    return sorted(out)

wanted = [r for r in a.routes.split(',') if r]
deps = {}; others = {}
if a.hub:
    hub_stops = {s['stop_id'] for s in stops_all if dist(float(s['stop_lat']), float(s['stop_lon'])) <= a.radius}
    if not hub_stops:
        sys.exit('no stops within %dm of the hub' % a.radius)
    for st in stop_times:
        t = trips[st['trip_id']]; kind = kind_of.get(t['service_id'])
        if not kind or not boards(st) or st['stop_id'] not in hub_stops: continue
        r = routes[t['route_id']]
        (deps if r in wanted else others).setdefault(kind, {}).setdefault(r, set()).add(mins(st['departure_time']))

# ---- per-stop departures, for the stop view on a flick: one small file a
# stop, fetched by the phone when the wearer asks, plus an index to find the
# nearest stop by. Headsigns and stop names are shortened by the hints, since
# a watch row has room for about a dozen letters.
def short(text, table):
    for k, v in table.items():
        text = text.replace(k, v)
    text = re.sub(r'\s*\([^)]*\)', '', text)   # a stop code or a bay in brackets says nothing on a watch
    return re.sub(r'\s+', ' ', text).strip().upper()

# A stop's name on the watch: the landmark the feed gives it in stop_desc, what
# the bus announces ('Across from Eccles Ice Center'), where the agency writes
# them and one fits; else its address. The cleaning is Cache Rider's, done
# here from the feed rather than taken from it, so neither build leans on the
# other. 'Across from' goes: twins across a road are read as one stop anyway.
NAME_MAX = 23
def landmark(desc, name):
    d = re.sub(r'\s*\((?:Timepoint|Detour)\)\s*', ' ', desc or '', flags=re.I)
    d = re.sub(r'\s*added \d+/\d+/\d+.*$', '', d, flags=re.I).strip(' -–·,')
    if d.count('(') > d.count(')'): d += ')'
    while d.count(')') > d.count('(') and ')' in d:
        i = d.rindex(')'); d = (d[:i] + d[i + 1:]).strip()
    if not d or re.fullmatch(r'(temp stop|timepoint|intermodal transit center)', d, re.I): return ''
    plain = lambda x: re.sub(r'\W', '', x).lower()
    if plain(d) in (plain(name), plain(name.rsplit(',', 1)[-1])): return ''
    d = re.sub(r'^across from\s+', '', d, flags=re.I)
    return d.split(' / ')[0].strip()
def stop_name(s_):
    address = short(s_['stop_name'], hints.get('stops', {}))
    if a.landmarks:
        by = short(landmark(s_.get('stop_desc', ''), s_['stop_name']), hints.get('stops', {}))
        if by and len(by) <= NAME_MAX: return by
    return address

# The trip a departure belongs to, as GTFS-realtime names it: the agency's
# trip_id without the -N its variants carry. Only used to lay live
# predictions over the schedule when there are any.
def base_trip(tid):
    return re.sub(r'-\d+$', '', tid)

if a.stops_out or a.legacy_out:
    per_stop, per_stop_kind = {}, {}
    for st in stop_times:
        t = trips[st['trip_id']]
        if not boards(st): continue
        r = route_by_id[t['route_id']]
        head = short(t.get('trip_headsign') or '', hints.get('headsigns', {}))
        if not head and r['route_short_name'] in hints.get('loops', []): head = 'LOOP'
        row = (mins(st['departure_time']), label(r['route_short_name']), head, base_trip(st['trip_id']))
        if t['service_id'] in running:
            # The departure's place in its trip, from the agency's own timetable:
            # a feed that names no stop in its predictions still names this.
            at_place = (svc_index[t['service_id']], int(st['stop_sequence']))
            per_stop.setdefault(st['stop_id'], set()).add(row + at_place)
        kind = kind_of.get(t['service_id'])
        if kind: per_stop_kind.setdefault(st['stop_id'], {}).setdefault(kind, set()).add(row)
    colours = {label(r['route_short_name']): [r.get('route_color') or '888888', r.get('route_text_color') or '000000']
               for r in route_by_id.values()}

    # A station is one place: stops that share a name, once the bay in
    # brackets is gone, within STATION metres of each other are marked with
    # the same group number in the index, so a flick at any bay answers for
    # the whole station without the phone fetching names first.
    STATION = 250
    named = {}
    for s_ in stops_all:
        named.setdefault(short(s_['stop_name'], hints.get('stops', {})), []).append(s_)
    group = {}
    for same in named.values():
        for i, s1 in enumerate(same):
            for s2 in same[i + 1:]:
                if math.hypot((float(s1['stop_lat']) - float(s2['stop_lat'])) * 111000,
                              (float(s1['stop_lon']) - float(s2['stop_lon'])) * 111000 * math.cos(math.radians(lat))) <= STATION:
                    g = group.get(s1['stop_id'], group.get(s2['stop_id'], len(set(group.values())) + 1))
                    group[s1['stop_id']] = group[s2['stop_id']] = g

    def write(out, rows_of, body, extra, grouped):
        os.makedirs(os.path.join(out, 'stops'), exist_ok=True)
        index, files = [], []
        # The version of this cut of the timetable, from the stop files alone: they change when the agency
        # publishes a new feed or a service ends, and not as the calendar's past dates are trimmed night by
        # night, so the phone keeps its stop files until they do. Each file carries it too, so the phone can
        # tell one newer than the index it holds.
        version = hashlib.sha1()
        for s_ in stops_all:
            sid = s_['stop_id']
            if sid not in rows_of: continue
            content = dict({'id': sid, 'name': stop_name(s_)}, **body(rows_of[sid]))
            files.append((sid, content))
            version.update(json.dumps(content, separators=(',', ':')).encode())
            entry = [sid, round(float(s_['stop_lat']), 5), round(float(s_['stop_lon']), 5)]
            if grouped and sid in group: entry.append(group[sid])
            index.append(entry)
        v = version.hexdigest()[:12]
        for sid, content in files:
            open(os.path.join(out, 'stops', sid + '.json'), 'w').write(
                json.dumps(dict(content, v=v), separators=(',', ':')))
        head = {'agency': a.agency}
        if a.hub: head['hub'] = {'lat': lat, 'lon': lon}
        json.dump(dict(head, v=v, routes=colours, stops=index, **extra),
                  open(os.path.join(out, 'stops.json'), 'w'), separators=(',', ':'))
        size = sum(os.path.getsize(os.path.join(out, 'stops', f)) for f in os.listdir(os.path.join(out, 'stops')))
        print('stops', len(index), 'files,', size, 'bytes, in', os.path.normpath(out))

    if a.stops_out:
        extra = {'services': services}
        if a.base:
            # When each service's day is done: the latest arrival of any of its
            # trips, so the phone knows when the buses start coming home to the yard.
            ends = [0] * len(services)
            for st in stop_times:
                i = svc_index.get(trips[st['trip_id']]['service_id'])
                hms = st.get('arrival_time') or st.get('departure_time')
                if i is not None and hms: ends[i] = max(ends[i], mins(hms))
            extra['ends'] = ends
        write(a.stops_out, per_stop, lambda rows: {'deps': [list(x) for x in sorted(rows)]}, extra, True)
    if a.legacy_out:
        write(a.legacy_out, per_stop_kind,
              lambda kinds: {'days': {k: [list(x) for x in sorted(v)] for k, v in kinds.items()}}, {}, False)

# The system's area: a box around every stop, with a margin, so the phone
# can tell which system it is in from a coarse fix.
dlat = a.margin / 111000.0
dlon = a.margin / (111000.0 * math.cos(math.radians(lat)))
system = {
    'agency': a.agency,
    'area': [round(min(lats) - dlat, 4), round(min(lons) - dlon, 4), round(max(lats) + dlat, 4), round(max(lons) + dlon, 4)],
    'data': a.data_url,
}
if a.live_url: system['live'] = a.live_url
if a.live_url and a.live_by != 'stop': system['liveBy'] = a.live_by
if a.hub:
    system['hub'] = {'name': a.name, 'lat': lat, 'lon': lon}
    system['routes'] = wanted
    system['days'] = {}
    for kind in sorted(set(deps) | set(others)):
        pulse = pulse_of(others.get(kind, {}))
        loops = {r: sorted(v) for r, v in deps.get(kind, {}).items()}
        every = pulse + [m for v in loops.values() for m in v]
        system['days'][kind] = {'hours': [min(every), max(every)], 'pulse': pulse, 'dep': loops}
if a.base:
    b_lat, b_lon, b_r = (float(v) for v in a.base.split(','))
    system['base'] = {'lat': b_lat, 'lon': b_lon, 'r': int(b_r)}
# One file, many systems: replace this agency's entry, keep the others.
try:
    existing = json.load(open(a.out))
    systems = existing.get('systems', [])
except (OSError, ValueError):
    systems = []
systems = [x for x in systems if x.get('agency') != a.agency] + [system]
json.dump({'systems': systems}, open(a.out, 'w'), separators=(',', ':'))
print('area', system['area'], 'services', len(services))
for kind, d in system.get('days', {}).items():
    print(kind, 'hours %02d:%02d-%02d:%02d' % (d['hours'][0] // 60, d['hours'][0] % 60, d['hours'][1] // 60, d['hours'][1] % 60), len(d['pulse']), 'pulse',
          {r: len(v) for r, v in d['dep'].items()})
print('wrote', os.path.normpath(a.out), os.path.getsize(a.out), 'bytes')
