#!/usr/bin/env python3
"""Reduce a GTFS feed to the little the watch needs for its hub.

Writes src/pkjs/transit.json: the hub stop's position, service hours per day
type, and the departures of the routes that leave the hub together on their
own timetable, as minutes past midnight. The face reads none of this; the
phone side does, and sends the watch what is next.

  python3 tools/transit.py gtfs.zip --hub 41.7406,-111.8309 --routes G,B \
      --agency CVTD --name ITC
"""
import argparse, csv, io, json, math, os, sys, zipfile

ap = argparse.ArgumentParser()
ap.add_argument('gtfs', help='path to a GTFS zip')
ap.add_argument('--hub', required=True, help='lat,lon of the hub')
ap.add_argument('--routes', required=True, help='short names of the routes that share a timetable, comma-separated')
ap.add_argument('--agency', required=True)
ap.add_argument('--name', default='HUB')
ap.add_argument('--radius', type=int, default=150, help='metres around the hub that count as its stops')
ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', 'src', 'pkjs', 'transit.json'))
ap.add_argument('--hints', help='JSON of headsign and stop-name abbreviations for this agency')
ap.add_argument('--stops-out', help='directory for the per-stop departure files and the stop index (docs/data/<agency>)')
ap.add_argument('--data-url', default='', help='where the per-stop files are served from, for the phone')
ap.add_argument('--margin', type=float, default=3000, help='metres around the outermost stops that still count as the system\'s area')
a = ap.parse_args()
hints = json.load(open(a.hints)) if a.hints else {}

z = zipfile.ZipFile(a.gtfs)
def table(name):
    return list(csv.DictReader(io.TextIOWrapper(z.open(name), encoding='utf-8-sig')))

lat, lon = (float(v) for v in a.hub.split(','))
def dist(la, lo):
    return math.hypot((la - lat) * 111000, (lo - lon) * 111000 * math.cos(math.radians(lat)))
def mins(hms):
    h, m = hms.split(':')[:2]
    return int(h) * 60 + int(m)

routes = {r['route_id']: r['route_short_name'] for r in table('routes.txt')}
wanted = a.routes.split(',')
trips = {t['trip_id']: t for t in table('trips.txt')}
hub_stops = {s['stop_id'] for s in table('stops.txt') if dist(float(s['stop_lat']), float(s['stop_lon'])) <= a.radius}
if not hub_stops:
    sys.exit('no stops within %dm of the hub' % a.radius)

# One service per day type: the one whose dates run the furthest out.
DAYS = ['monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday', 'sunday']
best = {}
for c in table('calendar.txt'):
    on = [d for d in DAYS if c[d] == '1']
    kind = 'weekday' if on and all(d in on for d in DAYS[:5]) and not any(d in on for d in DAYS[5:]) \
        else 'saturday' if on == ['saturday'] else 'sunday' if on == ['sunday'] else None
    if kind and (kind not in best or c['end_date'] > best[kind]['end_date']):
        best[kind] = c
svc = {c['service_id']: kind for kind, c in best.items()}

first = {}; last = {}; deps = {}
for st in table('stop_times.txt'):
    t = trips[st['trip_id']]; kind = svc.get(t['service_id'])
    if not kind: continue
    m = mins(st['departure_time'])
    first[kind] = min(first.get(kind, 9999), m); last[kind] = max(last.get(kind, 0), m)
    if st['stop_id'] in hub_stops and routes[t['route_id']] in wanted:
        deps.setdefault(kind, {}).setdefault(routes[t['route_id']], set()).add(m)

# ---- per-stop departures, for the stop view on a flick: one small file a
# stop, fetched by the phone when the wearer asks, plus an index to find the
# nearest stop by. Headsigns and stop names are shortened by the hints, since
# a watch row has room for about a dozen letters.
def short(text, table):
    for k, v in table.items():
        text = text.replace(k, v)
    return text.strip().upper()

if a.stops_out:
    os.makedirs(os.path.join(a.stops_out, 'stops'), exist_ok=True)
    route_by_id = {r['route_id']: r for r in table('routes.txt')}
    stops_all = table('stops.txt')
    per_stop = {}
    for st in table('stop_times.txt'):
        t = trips[st['trip_id']]; kind = svc.get(t['service_id'])
        if not kind: continue
        r = route_by_id[t['route_id']]
        head = short(t.get('trip_headsign') or '', hints.get('headsigns', {}))
        if not head and r['route_short_name'] in hints.get('loops', []): head = 'LOOP'
        per_stop.setdefault(st['stop_id'], {}).setdefault(kind, set()).add((mins(st['departure_time']), r['route_short_name'], head))
    index = []
    for s_ in stops_all:
        sid = s_['stop_id']
        if sid not in per_stop: continue
        days = {kind: [list(x) for x in sorted(v)] for kind, v in per_stop[sid].items()}
        json.dump({'name': short(s_['stop_name'], hints.get('stops', {})), 'days': days},
                  open(os.path.join(a.stops_out, 'stops', sid + '.json'), 'w'), separators=(',', ':'))
        index.append([sid, round(float(s_['stop_lat']), 5), round(float(s_['stop_lon']), 5)])
    colours = {r['route_short_name']: [r.get('route_color') or '888888', r.get('route_text_color') or '000000']
               for r in route_by_id.values()}
    json.dump({'agency': a.agency, 'hub': {'lat': lat, 'lon': lon}, 'routes': colours, 'stops': index},
              open(os.path.join(a.stops_out, 'stops.json'), 'w'), separators=(',', ':'))
    print('stops', len(index), 'files in', os.path.normpath(a.stops_out))

# The system's area: a box around every stop, with a margin, so the phone
# can tell which system it is in from a coarse fix.
lats = [float(s_['stop_lat']) for s_ in table('stops.txt')]
lons = [float(s_['stop_lon']) for s_ in table('stops.txt')]
dlat = a.margin / 111000.0
dlon = a.margin / (111000.0 * math.cos(math.radians(lat)))
system = {
    'agency': a.agency, 'hub': {'name': a.name, 'lat': lat, 'lon': lon}, 'routes': wanted,
    'area': [round(min(lats) - dlat, 4), round(min(lons) - dlon, 4), round(max(lats) + dlat, 4), round(max(lons) + dlon, 4)],
    'data': a.data_url,
    'days': {kind: {'hours': [first[kind], last[kind]],
                    'dep': {r: sorted(v) for r, v in deps.get(kind, {}).items()}}
             for kind in sorted(first)},
}
# One file, many systems: replace this agency's entry, keep the others.
try:
    existing = json.load(open(a.out))
    systems = existing.get('systems', [])
except (OSError, ValueError):
    systems = []
systems = [x for x in systems if x.get('agency') != a.agency] + [system]
json.dump({'systems': systems}, open(a.out, 'w'), separators=(',', ':'))
print('area', system['area'])
for kind, d in system['days'].items():
    print(kind, 'hours %02d:%02d-%02d:%02d' % (d['hours'][0] // 60, d['hours'][0] % 60, d['hours'][1] // 60, d['hours'][1] % 60),
          {r: len(v) for r, v in d['dep'].items()})
print('wrote', os.path.normpath(a.out), os.path.getsize(a.out), 'bytes')
