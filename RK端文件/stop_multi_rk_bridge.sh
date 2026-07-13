#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${1:-"$SCRIPT_DIR/rk_bridge_config.sh"}"

if [[ -f "$CONFIG_FILE" ]]; then
  # shellcheck source=/dev/null
  source "$CONFIG_FILE"
else
  LOG_DIR="./logs"
fi

LOG_DIR="${LOG_DIR:-./logs}"

if [[ ! -d "$LOG_DIR" ]]; then
  echo "Log dir not found: $LOG_DIR"
  exit 0
fi

found=0

recover_pid_file="$LOG_DIR/rk_recover_service.pid"
if [[ -f "$recover_pid_file" ]]; then
  recover_pid="$(cat "$recover_pid_file" 2>/dev/null || true)"
  if [[ -n "$recover_pid" ]] && kill -0 "$recover_pid" 2>/dev/null; then
    echo "Stopping RK recover service pid $recover_pid"
    kill "$recover_pid" 2>/dev/null || true
    sleep 0.5
    if kill -0 "$recover_pid" 2>/dev/null; then
      echo "RK recover service still running, force kill pid $recover_pid"
      kill -9 "$recover_pid" 2>/dev/null || true
    fi
  fi
  rm -f "$recover_pid_file"
fi

for pid_file in "$LOG_DIR"/rk_bridge_instance_*.pid; do
  [[ -f "$pid_file" ]] || continue
  found=1
  pid="$(cat "$pid_file" 2>/dev/null || true)"

  if [[ -z "$pid" ]]; then
    rm -f "$pid_file"
    continue
  fi

  if kill -0 "$pid" 2>/dev/null; then
    echo "Stopping pid $pid from $pid_file"
    kill "$pid" 2>/dev/null || true
  else
    echo "Pid $pid is not running"
  fi

  rm -f "$pid_file"
done

if [[ "$found" == "0" ]]; then
  echo "No RK bridge pid files found in $LOG_DIR"
fi

bridge_name="$(basename "${RK_BRIDGE_BIN:-rk_aoa_bridge}")"
for pid in $(pgrep -f "$bridge_name" 2>/dev/null || true); do
  if [[ "$pid" == "$$" ]]; then
    continue
  fi
  cmd="$(ps -p "$pid" -o comm= 2>/dev/null || true)"
  if [[ "$cmd" == "$bridge_name" ]]; then
    echo "Stopping leftover $bridge_name pid $pid"
    kill "$pid" 2>/dev/null || true
  fi
done

for pid in $(pgrep -f "rk_recover_service" 2>/dev/null || true); do
  if [[ "$pid" == "$$" ]]; then
    continue
  fi
  echo "Stopping leftover rk_recover_service pid $pid"
  kill "$pid" 2>/dev/null || true
  sleep 0.2
  if kill -0 "$pid" 2>/dev/null; then
    echo "Force stopping leftover rk_recover_service pid $pid"
    kill -9 "$pid" 2>/dev/null || true
  fi
done

sleep 0.5
