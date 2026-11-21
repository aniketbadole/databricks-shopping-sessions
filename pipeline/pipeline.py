"""Compatibility entry point for local use. The container executes C++ directly."""
import os
from pathlib import Path
import sys
core = Path(__file__).resolve().parents[1] / 'core'
data = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('DATA_DIR', '/data')
output = sys.argv[2] if len(sys.argv) > 2 else os.environ.get('OUTPUT_DIR', '/output')
os.execv(str(core), [str(core), data, output])
