#!/usr/bin/env python3
"""Exercise packed-ID boundaries and the exact-string fallback against the oracle."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp:
    base=Path(temp); data=base/'data';data.mkdir()
    for d in ('events','clicks','links'):(data/d).mkdir()
    (data/'manifest.json').write_text(json.dumps({'window_start':'2026-10-01T00:00:00Z','window_end':'2026-10-15T00:00:00Z'}))
    (data/'products.csv').write_text('product_id,category\np,category\n')
    (data/'campaigns.csv').write_text('campaign_id,channel\nc,channel\n')
    identities=['0','00','A','a','_','0000000000','00000000000','abcdefghij','abcdefghijk',' A','A ','用户','é','é','a\rb','x'*100]
    def line(fields):return ','.join('"'+str(s)+'"' for s in fields)+'\n'
    (data/'links'/'part.csv').write_text('user_id_a,user_id_b\n'+line(['unused-minimum-id','x'*100]))
    for part in range(8):
        with (data/'events'/f'{part}.csv').open('w',newline='') as e, (data/'clicks'/f'{part}.csv').open('w',newline='') as c:
            e.write('event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents\n')
            c.write('click_id,user_id,campaign_id,ts\n')
            for i,user in enumerate(identities):
                # These names deliberately share packed prefixes and differ by case/length.
                eid=f'{part}:{user}'
                e.write(line([eid,user,'2026-10-01T01:00:00Z','2026-10-01T01:01:00Z','purchase','p','0250']))
                e.write(line([eid,user,'2026-10-02T01:00:00Z','2026-10-02T01:01:00Z','purchase','p','999']))
                c.write(line([f'click:{part}:{i}',user,'c','2026-10-01T00:00:00Z']))
    subprocess.run([str(root/'core'),str(data),str(base/'actual')],check=True)
    subprocess.run([sys.executable,str(root/'scripts/reference.py'),str(data),str(base/'expected')],check=True)
    subprocess.run([sys.executable,str(root/'scripts/compare.py'),str(base/'actual'),str(base/'expected')],check=True)

    # A very wide reporting window exercises sparse campaign/day counters.
    (data/'manifest.json').write_text(json.dumps({'window_start':'2026-10-01T00:00:00Z','window_end':'3000-10-15T00:00:00Z'}))
    subprocess.run([str(root/'core'),str(data),str(base/'wide-actual')],check=True)
    subprocess.run([sys.executable,str(root/'scripts/reference.py'),str(data),str(base/'wide-expected')],check=True)
    subprocess.run([sys.executable,str(root/'scripts/compare.py'),str(base/'wide-actual'),str(base/'wide-expected')],check=True)
