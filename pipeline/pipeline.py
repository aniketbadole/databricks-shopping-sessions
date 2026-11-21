"""Native aggregation, followed by typed Parquet serialization."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from datetime import datetime

def main():
    data = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('DATA_DIR', '/data')).resolve()
    output = Path(sys.argv[2] if len(sys.argv) > 2 else os.environ.get('OUTPUT_DIR', '/output')).resolve()
    scratch = Path(os.environ.get('SCRATCH_DIR', '/scratch'))
    scratch.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((data / 'manifest.json').read_text())
    epoch = lambda s: int(datetime.fromisoformat(s.replace('Z', '+00:00')).timestamp()) * 1000
    core = Path(__file__).resolve().parents[1] / 'core'
    with tempfile.TemporaryDirectory(prefix='shopping-', dir=scratch) as temp:
        subprocess.run([str(core), str(data), temp, str(epoch(manifest['window_start'])), str(epoch(manifest['window_end']))], check=True)
        os.environ.setdefault('POLARS_MAX_THREADS', os.environ.get('NCPU', '8'))
        import polars as pl
        schemas = {
            'user_daily': {'user_id': pl.String, 'day': pl.Date, 'sessions': pl.Int64, 'events': pl.Int64, 'revenue_cents': pl.Int64, 'top_category': pl.String},
            'campaign_daily': {'campaign_id': pl.String, 'channel': pl.String, 'day': pl.Date, 'sessions': pl.Int64, 'purchasing_sessions': pl.Int64, 'revenue_cents': pl.Int64, 'assisted_sessions': pl.Int64},
        }
        for table, pattern in [('user_daily', 'user-*.csv'), ('campaign_daily', 'campaign.csv')]:
            folder = output / table
            folder.mkdir(parents=True, exist_ok=True)
            pl.scan_csv(str(Path(temp) / pattern), schema=schemas[table], try_parse_dates=True).sink_parquet(folder / 'part-00000.parquet', compression='snappy')

if __name__ == '__main__':
    main()
