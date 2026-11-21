#!/usr/bin/env python3
"""Deliberately simple independent oracle for small development datasets.
Not used by the submission. It materializes rows and scans clicks per session.
"""
from collections import defaultdict
from datetime import datetime, timezone, timedelta, date
from pathlib import Path
import json
import re
import sys
import polars as pl

DIGITS = re.compile(r'[0-9]+\Z')
ISO = re.compile(r'[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(Z|[+-][0-9]{2}:[0-9]{2})\Z')
DAY = 86400000
WEEK = 7 * DAY

def rows(path, width):
    paths = sorted(path.glob('*.csv')) if path.is_dir() else [path]
    for p in paths:
        # Split bytes on LF to preserve CR as a field character.
        raw = p.read_bytes().split(b'\n')[1:]
        for line in raw:
            parts = line.decode('utf-8').split(',')
            if len(parts) != width:
                continue
            clean = []
            for part in parts:
                if part.startswith('"') and part.endswith('"') and len(part) >= 2:
                    part = part[1:-1]
                if '"' in part:
                    break
                clean.append(part)
            else:
                yield clean

def stamp(s):
    if DIGITS.fullmatch(s):
        return int(s)
    if not ISO.fullmatch(s):
        return None
    if len(s) == 25 and (int(s[20:22]) > 23 or int(s[23:]) > 59):
        return None
    try:
        dt = datetime.fromisoformat(s.replace('Z', '+00:00'))
        return int((dt - datetime(1970, 1, 1, tzinfo=timezone.utc)).total_seconds()) * 1000
    except ValueError:
        return None

def main(data, output):
    data, output = Path(data), Path(output)
    manifest = json.loads((data / 'manifest.json').read_text())
    start, end = map(stamp, [manifest['window_start'], manifest['window_end']])
    graph = defaultdict(set)
    for a, b in rows(data / 'links', 2):
        if a and b:
            graph[a].add(b)
            graph[b].add(a)
    person = {}
    for a in graph:
        if a in person:
            continue
        component, todo = {a}, [a]
        while todo:
            for b in graph[todo.pop()]:
                if b not in component:
                    component.add(b)
                    todo.append(b)
        name = min(component, key=lambda s: s.encode('utf-8'))
        person.update(dict.fromkeys(component, name))
    products, campaigns = dict(rows(data / 'products.csv', 2)), dict(rows(data / 'campaigns.csv', 2))
    dedup = {}
    for row in rows(data / 'events', 7):
        if row[0] and (row[0] not in dedup or row[3] < dedup[row[0]][3]):
            dedup[row[0]] = row
    events = defaultdict(list)
    for _, user, ts, ingest, kind, product, price in dedup.values():
        t = stamp(ts)
        if not user or not product or t is None or not start <= t < end or kind not in ('view', 'cart', 'purchase'):
            continue
        if kind == 'purchase':
            if not DIGITS.fullmatch(price) or not 1 <= int(price) <= 10000000:
                continue
            price = int(price)
        else:
            price = 0
        if stamp(ingest) - t > DAY:
            continue
        events[person.get(user, user)].append((t, price, products.get(product, 'unknown')))
    clicks = defaultdict(list)
    seen = set()
    for cid, user, campaign, ts in rows(data / 'clicks', 4):
        if not cid or cid in seen:
            continue
        seen.add(cid)
        t = stamp(ts)
        if not user or campaign not in campaigns or t is None or not start - WEEK <= t < end:
            continue
        clicks[person.get(user, user)].append((t, cid, campaign))
    ud = {}
    cd = defaultdict(lambda: [0, 0, 0, 0])
    for user, es in events.items():
        es.sort()
        sessions = []
        for e in es:
            if not sessions or e[0] - sessions[-1][-1][0] > 1800000:
                sessions.append([])
            sessions[-1].append(e)
        for session in sessions:
            t = session[0][0]
            day = date(1970, 1, 1) + timedelta(days=t // DAY)
            eligible = [c for c in clicks[user] if t - WEEK <= c[0] <= t]
            credit = min(eligible, key=lambda c: (-c[0], c[1].encode('utf-8')))[2] if eligible else 'organic'
            revenue = sum(e[1] for e in session)
            if (user, day) not in ud:
                ud[user, day] = [0, 0, 0, defaultdict(int)]
            row = ud[user, day]
            row[0] += 1
            row[1] += len(session)
            row[2] += revenue
            for _, price, category in session:
                if price:
                    row[3][category] += price
            row = cd[credit, day]
            row[0] += 1
            row[1] += int(revenue > 0)
            row[2] += revenue
            for campaign in {c[2] for c in eligible}:
                cd[campaign, day][3] += 1
    user_rows = [(u, d, n, e, r, min(cat, key=lambda c: (-cat[c], c.encode('utf-8'))) if cat else None) for (u, d), (n, e, r, cat) in ud.items()]
    campaign_rows = [(c, campaigns.get(c, 'organic'), d, *v) for (c, d), v in cd.items()]
    from run_fixtures import SCHEMAS
    # Counts fit in Int64; use this Parquet-compatible width instead of Int128.
    for table, values in [('user_daily', user_rows), ('campaign_daily', campaign_rows)]:
        schema = {k: pl.Int64 if v == pl.Int128 else v for k, v in SCHEMAS[table].items()}
        dest = output / table
        dest.mkdir(parents=True, exist_ok=True)
        pl.DataFrame(values, schema=schema, orient='row').write_parquet(dest / 'part.parquet')

if __name__ == '__main__':
    main(*sys.argv[1:])
