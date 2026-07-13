#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${2:-"$SCRIPT_DIR/rk_bridge_config.sh"}"
DEVICE_ID="${1:-}"

if [[ -z "$DEVICE_ID" || ! "$DEVICE_ID" =~ ^[0-9]+$ ]]; then
  echo "Usage: $0 DEVICE_ID [config_file]" >&2
  exit 2
fi

if [[ ! -f "$CONFIG_FILE" ]]; then
  echo "Config file not found: $CONFIG_FILE" >&2
  exit 1
fi

# shellcheck source=/dev/null
source "$CONFIG_FILE"

mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/rk_bridge.log"
PID_FILE="$LOG_DIR/rk_bridge_instance_${DEVICE_ID}.pid"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] recover device $DEVICE_ID: $*" | tee -a "$LOG_FILE"
}

stop_old_instance() {
  if [[ ! -f "$PID_FILE" ]]; then
    return
  fi

  local old_pid
  old_pid="$(cat "$PID_FILE" 2>/dev/null || true)"
  rm -f "$PID_FILE"

  if [[ -n "$old_pid" ]] && kill -0 "$old_pid" 2>/dev/null; then
    log "stopping old pid $old_pid"
    kill "$old_pid" 2>/dev/null || true
    sleep 0.5
    if kill -0 "$old_pid" 2>/dev/null; then
      log "old pid still running, force kill $old_pid"
      kill -9 "$old_pid" 2>/dev/null || true
    fi
  fi
}

aoa_count() {
  "$RK_BRIDGE_BIN" --list 2>/dev/null |
    awk '/^AOA devices:/{in_aoa=1;next}/^Starter devices:/{in_aoa=0}in_aoa&&/index=/{count++}END{print count+0}'
}

starter_count() {
  "$RK_BRIDGE_BIN" --list 2>/dev/null |
    awk '/^Starter devices:/{in_starter=1;next}in_starter&&/index=/{count++}END{print count+0}'
}

wait_for_aoa() {
  local deadline=$((SECONDS + ${RECOVER_AOA_WAIT_SECONDS:-20}))
  local next_start_try=0

  while (( SECONDS <= deadline )); do
    local count
    count="$(aoa_count)"

    if [[ "$DEVICE_BIND_MODE" == "index" ]]; then
      log "waiting for AOA index $DEVICE_ID, current AOA count=$count"
      if (( count > DEVICE_ID )); then
        return 0
      fi
    else
      log "waiting for any AOA device, current AOA count=$count"
      if (( count > 0 )); then
        return 0
      fi
    fi

    if (( SECONDS >= next_start_try )); then
      local starters
      starters="$(starter_count)"
      if (( starters > 0 )); then
        log "starter device count=$starters, retry AOA start"
        "$RK_BRIDGE_BIN" --start-all 2>&1 | tee -a "$LOG_FILE" || true
      elif [[ "${RECOVER_FORCE_AOA_START:-1}" == "1" ]]; then
        log "target AOA not found, refresh accessory mode"
        "$RK_BRIDGE_BIN" --start-all 2>&1 | tee -a "$LOG_FILE" || true
      fi
      next_start_try=$((SECONDS + ${RECOVER_AOA_START_RETRY_SECONDS:-3}))
    fi

    sleep 1
  done

  return 1
}

build_selector() {
  case "$DEVICE_BIND_MODE" in
    index)
      selector=(--device-index "$DEVICE_ID")
      ;;
    usb)
      usb_value="${USB_SELECTORS[$DEVICE_ID]:-}"
      if [[ -z "$usb_value" ]]; then
        log "USB_SELECTORS[$DEVICE_ID] is empty"
        exit 1
      fi
      selector=(--usb "$usb_value")
      ;;
    *)
      log "invalid DEVICE_BIND_MODE: $DEVICE_BIND_MODE"
      exit 1
      ;;
  esac
}

stop_old_instance

log "checking USB devices"
"$RK_BRIDGE_BIN" --list 2>&1 | tee -a "$LOG_FILE"

if [[ "$(starter_count)" != "0" ]]; then
  log "starter device found, starting into AOA"
  "$RK_BRIDGE_BIN" --start-all 2>&1 | tee -a "$LOG_FILE" || true
elif [[ "${RECOVER_FORCE_AOA_START:-1}" == "1" ]]; then
  log "forcing AOA start request to refresh accessory/HID state"
  "$RK_BRIDGE_BIN" --start-all 2>&1 | tee -a "$LOG_FILE" || true
fi

if ! wait_for_aoa; then
  log "AOA device is not ready, ask user to reconnect cable and tap Start screen stream"
  exit 3
fi

video_port=$((BASE_VIDEO_PORT + DEVICE_ID * PORT_STEP))
control_port=$((BASE_CONTROL_PORT + DEVICE_ID * PORT_STEP))
selector=()
build_selector

log "starting bridge: $RK_BRIDGE_BIN $WINDOWS_IP $video_port $control_port ${selector[*]}"
nohup "$RK_BRIDGE_BIN" "$WINDOWS_IP" "$video_port" "$control_port" "${selector[@]}" \
  >>"$LOG_FILE" 2>&1 &

pid=$!
echo "$pid" > "$PID_FILE"
log "started pid $pid"
log "waiting for phone companion app Start screen stream if video is not visible yet"
