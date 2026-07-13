#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${1:-"$SCRIPT_DIR/rk_bridge_config.sh"}"

if [[ ! -f "$CONFIG_FILE" ]]; then
  echo "Config file not found: $CONFIG_FILE" >&2
  exit 1
fi

# shellcheck source=/dev/null
source "$CONFIG_FILE"

mkdir -p "$LOG_DIR"

LOG_FILE="$LOG_DIR/rk_bridge.log"

: > "$LOG_FILE"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

require_file() {
  if [[ ! -x "$1" ]]; then
    echo "Executable not found or not executable: $1" >&2
    echo "Try: chmod +x $1" >&2
    exit 1
  fi
}

require_file "$RK_BRIDGE_BIN"
if [[ -n "${RK_RECOVER_SERVICE_BIN:-}" ]]; then
  require_file "$RK_RECOVER_SERVICE_BIN"
fi

aoa_count() {
  "$RK_BRIDGE_BIN" --list 2>/dev/null |
    awk '/^AOA devices:/{in_aoa=1;next}/^Starter devices:/{in_aoa=0}in_aoa&&/index=/{count++}END{print count+0}'
}

resolve_phone_count() {
  if [[ "${PHONE_COUNT}" == "auto" ]]; then
    case "$DEVICE_BIND_MODE" in
      index)
        aoa_count
        ;;
      usb)
        echo "${#USB_SELECTORS[@]}"
        ;;
      *)
        log "ERROR: invalid DEVICE_BIND_MODE: $DEVICE_BIND_MODE"
        exit 1
        ;;
    esac
    return
  fi

  if [[ ! "${PHONE_COUNT}" =~ ^[0-9]+$ ]]; then
    log "ERROR: PHONE_COUNT must be a number or auto, got: $PHONE_COUNT"
    exit 1
  fi

  echo "$PHONE_COUNT"
}

if [[ -x "$SCRIPT_DIR/stop_multi_rk_bridge.sh" ]]; then
  "$SCRIPT_DIR/stop_multi_rk_bridge.sh" "$CONFIG_FILE" >/dev/null 2>&1 || true
  sleep 0.5
fi

log "RK multi bridge start"
log "config: $CONFIG_FILE"
log "windows_ip: $WINDOWS_IP"
log "phone_count: $PHONE_COUNT"
log "base_video_port: $BASE_VIDEO_PORT"
log "base_control_port: $BASE_CONTROL_PORT"
log "port_step: $PORT_STEP"
log "device_bind_mode: $DEVICE_BIND_MODE"
log "log_dir: $LOG_DIR"
log "recover_service: ${RK_RECOVER_HOST:-0.0.0.0}:${RK_RECOVER_PORT:-18110}"

log "Current USB device list:"
"$RK_BRIDGE_BIN" --list 2>&1 | tee -a "$LOG_FILE"

if [[ -n "${RK_RECOVER_SERVICE_BIN:-}" ]]; then
  recover_log="$LOG_DIR/rk_recover_service.log"
  recover_pid_file="$LOG_DIR/rk_recover_service.pid"
  for old_recover_pid in $(pgrep -f "rk_recover_service" 2>/dev/null || true); do
    if [[ "$old_recover_pid" != "$$" ]]; then
      log "Stopping old RK recover service pid $old_recover_pid"
      kill "$old_recover_pid" 2>/dev/null || true
      sleep 0.2
      kill -9 "$old_recover_pid" 2>/dev/null || true
    fi
  done
  log "Starting RK recover service"
  log "  command: $RK_RECOVER_SERVICE_BIN --host ${RK_RECOVER_HOST:-0.0.0.0} --port ${RK_RECOVER_PORT:-18110}"
  nohup "$RK_RECOVER_SERVICE_BIN" \
    --host "${RK_RECOVER_HOST:-0.0.0.0}" \
    --port "${RK_RECOVER_PORT:-18110}" \
    --script "$SCRIPT_DIR/restart_one_rk_bridge.sh" \
    --config "$CONFIG_FILE" \
    >>"$recover_log" 2>&1 &
  echo "$!" > "$recover_pid_file"
  log "  recover pid: $(cat "$recover_pid_file")"
fi

if [[ "${START_AOA_ALL:-0}" == "1" ]]; then
  log "Starting all known starter devices into AOA mode"
  "$RK_BRIDGE_BIN" --start-all 2>&1 | tee -a "$LOG_FILE" || true
  log "Waiting ${AOA_WAIT_SECONDS}s for USB re-enumeration"
  sleep "$AOA_WAIT_SECONDS"
  log "USB device list after AOA start:"
  "$RK_BRIDGE_BIN" --list 2>&1 | tee -a "$LOG_FILE"
fi

RESOLVED_PHONE_COUNT="$(resolve_phone_count)"
log "resolved_phone_count: $RESOLVED_PHONE_COUNT"

if (( RESOLVED_PHONE_COUNT <= 0 )); then
  log "No AOA phone bridge instances to start"
  log "If phones are connected as Starter devices, set START_AOA_ALL=1 or run: $RK_BRIDGE_BIN --start-all"
  exit 0
fi

for ((i = 0; i < RESOLVED_PHONE_COUNT; i++)); do
  video_port=$((BASE_VIDEO_PORT + i * PORT_STEP))
  control_port=$((BASE_CONTROL_PORT + i * PORT_STEP))
  pid_file="$LOG_DIR/rk_bridge_instance_${i}.pid"

  selector=()
  case "$DEVICE_BIND_MODE" in
    index)
      selector=(--device-index "$i")
      ;;
    usb)
      usb_value="${USB_SELECTORS[$i]:-}"
      if [[ -z "$usb_value" ]]; then
        log "ERROR: USB_SELECTORS[$i] is empty"
        exit 1
      fi
      selector=(--usb "$usb_value")
      ;;
    *)
      log "ERROR: invalid DEVICE_BIND_MODE: $DEVICE_BIND_MODE"
      exit 1
      ;;
  esac

  log "Starting instance $i"
  log "  command: $RK_BRIDGE_BIN $WINDOWS_IP $video_port $control_port ${selector[*]}"
  log "  log: $LOG_FILE"

  nohup "$RK_BRIDGE_BIN" "$WINDOWS_IP" "$video_port" "$control_port" "${selector[@]}" \
    >>"$LOG_FILE" 2>&1 &

  pid=$!
  echo "$pid" > "$pid_file"
  log "  pid: $pid"

  sleep 0.3
done

log "All bridge instances launched"
log "Check log:"
log "  tail -f $LOG_FILE"
log "Stop one instance:"
log "  kill \$(cat $LOG_DIR/rk_bridge_instance_0.pid)"
