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
a = ap.parse_args()

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

out = {
    'agency': a.agency, 'hub': {'name': a.name, 'lat': lat, 'lon': lon}, 'routes': wanted,
    'days': {kind: {'hours': [first[kind], last[kind]],
                    'dep': {r: sorted(v) for r, v in deps.get(kind, {}).items()}}
             for kind in sorted(first)},
}
json.dump(out, open(a.out, 'w'), separators=(',', ':'))
for kind, d in out['days'].items():
    print(kind, 'hours %02d:%02d-%02d:%02d' % (d['hours'][0] // 60, d['hours'][0] % 60, d['hours'][1] // 60, d['hours'][1] % 60),
          {r: len(v) for r, v in d['dep'].items()})
print('wrote', os.path.normpath(a.out), os.path.getsize(a.out), 'bytes')
