#!/usr/bin/env bash

# RK3568 multi-phone bridge config.
# Copy this file and start_multi_rk_bridge.sh to RK3568, then edit values here.

WINDOWS_IP="192.168.110.69"
# Number or auto. With auto + DEVICE_BIND_MODE=index, the script counts current AOA devices.
PHONE_COUNT=auto

BASE_VIDEO_PORT=9001
BASE_CONTROL_PORT=9002
PORT_STEP=10

RK_BRIDGE_BIN="./rk_aoa_bridge"
LOG_DIR="./logs"

RK_RECOVER_SERVICE_BIN="./rk_recover_service"
RK_RECOVER_HOST="0.0.0.0"
RK_RECOVER_PORT=18110
RECOVER_AOA_WAIT_SECONDS=20
RECOVER_FORCE_AOA_START=1
RECOVER_AOA_START_RETRY_SECONDS=3

# 1: run "$RK_BRIDGE_BIN --start-all" before starting bridge workers.
# If phones are already in AOA mode, set to 0.
START_AOA_ALL=0
AOA_WAIT_SECONDS=2

# Device binding mode:
#   index: use --device-index 0, --device-index 1, ...
#   usb:   use USB_SELECTORS array below, for example "005:007"
DEVICE_BIND_MODE="index"

# Used only when DEVICE_BIND_MODE="usb".
# Keep the order aligned with instance number.
USB_SELECTORS=(
  "005:007"
  "005:008"
)
