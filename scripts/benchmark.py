#!/usr/bin/env python3
"""Native end-to-end timing. Run from repository root with DATA OUTPUT args."""
import os
import resource
import subprocess
import sys
import time
from pathlib import Path

os.environ.setdefault('SCRATCH_DIR', str(Path('scratch').resolve()))
os.environ.setdefault('NCPU', '8')
start = time.perf_counter()
result = subprocess.run([str(Path('core').resolve()), *sys.argv[1:]])
print(f'elapsed_seconds={time.perf_counter() - start:.3f} '
      f'peak_child_rss_mib={resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss / 1024:.1f}')
sys.exit(result.returncode)
