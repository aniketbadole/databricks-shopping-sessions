#!/bin/sh
set -eu
exec /app/core "${DATA_DIR:-/data}" "${OUTPUT_DIR:-/output}"
