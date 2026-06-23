#!/bin/bash
# WEAK_NET Dashboard startup script

set -e
cd "$(dirname "$0")"

HOST="${WEAKNET_DASHBOARD_HOST:-127.0.0.1}"
PORT="${WEAKNET_DASHBOARD_PORT:-8080}"
LOG_PATH="${WEAKNET_LOG_PATH:-logs/server/runtime.log}"
DB_PATH="${WEAKNET_DASHBOARD_DB:-dashboard/weaknet_dashboard.sqlite3}"

mkdir -p logs/server dashboard
touch "$LOG_PATH"

echo "Starting WeakNet Dashboard..."
echo "URL: http://${HOST}:${PORT}"
echo "Log: ${LOG_PATH}"

exec python3 dashboard/server.py \
  --host "$HOST" \
  --port "$PORT" \
  --log "$LOG_PATH" \
  --db "$DB_PATH"
