#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
import os
import re
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import urllib.error
import webbrowser
from http.server import BaseHTTPRequestHandler


MAGIC = b"AVC1"
MAX_PACKET = 8 * 1024 * 1024
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
INDEX_HTML_PATH = os.path.join(BASE_DIR, "win_multi_console.html")
SNAPSHOT_KEEP_COUNT = 5
AUTOMATION_SNAPSHOT_HTTP_PORT = 18091
RK_RECOVER_HOST = "192.168.110.86"
RK_RECOVER_PORT = 18110
AUTOMATION_DEFAULT_SETTINGS = {
    "view_seconds": 6.0,
    "swipe_percent": 25.0,
    "page_ocr_passes": 2,
    "action_delay": 1.0,
    "tap_y_offset": -45,
    "ignore_author_hits": True,
    "detail_action": "wait",
}


def clamp_float(value, default, min_value, max_value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return default
    return max(min_value, min(max_value, number))


def clamp_int(value, default, min_value, max_value):
    try:
        number = int(value)
    except (TypeError, ValueError):
        return default
    return max(min_value, min(max_value, number))


def normalize_automation_settings(values):
    values = values or {}
    defaults = AUTOMATION_DEFAULT_SETTINGS
    settings = {
        "view_seconds": clamp_float(values.get("view_seconds"), defaults["view_seconds"], 1.0, 60.0),
        "swipe_percent": clamp_float(values.get("swipe_percent"), defaults["swipe_percent"], 5.0, 80.0),
        "page_ocr_passes": clamp_int(values.get("page_ocr_passes"), defaults["page_ocr_passes"], 1, 5),
        "action_delay": clamp_float(values.get("action_delay"), defaults["action_delay"], 0.2, 5.0),
        "tap_y_offset": clamp_int(values.get("tap_y_offset"), defaults["tap_y_offset"], -200, 120),
        "ignore_author_hits": bool(values.get("ignore_author_hits", defaults["ignore_author_hits"])),
        "detail_action": str(values.get("detail_action", defaults["detail_action"])),
    }
    if settings["detail_action"] not in ("wait", "swipe_once"):
        settings["detail_action"] = defaults["detail_action"]
    return settings


automation_settings_lock = threading.Lock()
automation_settings = normalize_automation_settings({})


def close_quietly(conn):
    try:
        conn.close()
    except OSError:
        pass


def safe_send_all(conn, data):
    try:
        conn.sendall(data)
        return True
    except OSError:
        return False


def read_exact(conn, n):
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = conn.recv(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_avc1_packet(conn):
    window = bytearray()
    while True:
        b = conn.recv(1)
        if not b:
            return None
        window += b
        if len(window) > 4:
            del window[0]
        if len(window) == 4 and bytes(window) == MAGIC:
            rest = read_exact(conn, 24)
            if rest is None:
                return None
            header = MAGIC + rest
            data_len = struct.unpack(">I", header[24:28])[0]
            if data_len == 0 or data_len > MAX_PACKET:
                continue
            data = read_exact(conn, data_len)
            if data is None:
                return None
            return header + data


def load_index_html():
    try:
        with open(INDEX_HTML_PATH, "r", encoding="utf-8") as f:
            return f.read()
    except OSError:
        return INDEX_HTML


def prune_device_snapshots(device_dir, device_id, keep_count=SNAPSHOT_KEEP_COUNT):
    prefix = f"device_{device_id}_"
    snapshots = []
    try:
        names = os.listdir(device_dir)
    except OSError:
        return 0

    for name in names:
        if not (name.startswith(prefix) and name.endswith(".png")):
            continue
        path = os.path.join(device_dir, name)
        if os.path.isfile(path):
            snapshots.append((os.path.getmtime(path), name, path))

    snapshots.sort(reverse=True)
    removed = 0
    for _, _, path in snapshots[keep_count:]:
        try:
            os.remove(path)
            removed += 1
        except OSError:
            pass
    return removed


class DeviceHub:
    def __init__(self, device_id, video_port, control_host, control_port):
        self.device_id = device_id
        self.video_port = video_port
        self.control_host = control_host
        self.control_port = control_port
        self.lock = threading.Lock()
        self.ws_clients = set()
        self.latest_width = 0
        self.latest_height = 0
        self.latest_flags = 0
        self.latest_pts_us = 0
        self.packet_count = 0
        self.byte_count = 0
        self.last_packet_ms = 0
        self.connected = False
        self.config_cache = bytearray()
        self.latest_key_payload = None
        self.last_log_ms = 0

    def status(self):
        with self.lock:
            now_ms = int(time.time() * 1000)
            video_age_ms = now_ms - self.last_packet_ms if self.last_packet_ms else None
            return {
                "id": self.device_id,
                "video_port": self.video_port,
                "control_host": self.control_host,
                "control_port": self.control_port,
                "connected": self.connected,
                "width": self.latest_width,
                "height": self.latest_height,
                "flags": self.latest_flags,
                "pts_us": self.latest_pts_us,
                "packets": self.packet_count,
                "bytes": self.byte_count,
                "last_packet_ms": self.last_packet_ms,
                "video_age_ms": video_age_ms,
                "browser_clients": len(self.ws_clients),
            }

    def set_connected(self, value):
        with self.lock:
            self.connected = value

    def publish_avc1(self, payload):
        if len(payload) < 28 or payload[:4] != MAGIC:
            return

        width, height, flags = struct.unpack(">III", payload[4:16])
        pts_us = struct.unpack(">Q", payload[16:24])[0]
        data_len = struct.unpack(">I", payload[24:28])[0]
        if data_len == 0 or data_len > MAX_PACKET or 28 + data_len > len(payload):
            return

        h264 = payload[28:28 + data_len]
        with self.lock:
            self.latest_width = width
            self.latest_height = height
            self.latest_flags = flags
            self.latest_pts_us = pts_us
            self.packet_count += 1
            self.byte_count += data_len
            self.last_packet_ms = int(time.time() * 1000)
            clients = list(self.ws_clients)
            if flags & 2:
                self.config_cache = bytearray(h264)
            config = bytes(self.config_cache)

        if flags & 1 and config:
            h264 = config + h264
            payload = MAGIC + struct.pack(">IIIQI", width, height, flags, pts_us, len(h264)) + h264

        should_broadcast = not (flags & 2 and not flags & 1)
        if should_broadcast and (flags & 1):
            with self.lock:
                self.latest_key_payload = bytes(payload)

        if should_broadcast:
            self._broadcast_ws(payload, clients)

        now_ms = int(time.time() * 1000)
        if now_ms - self.last_log_ms >= 1000:
            self.last_log_ms = now_ms
            print(
                f"device {self.device_id} packets={self.packet_count} bytes={self.byte_count} latest={width}x{height} flags={flags} size={data_len}",
                flush=True
            )

    def add_ws(self, conn):
        with self.lock:
            self.ws_clients.add(conn)
            latest_key_payload = self.latest_key_payload
        if latest_key_payload:
            websocket_send_binary(conn, latest_key_payload)

    def remove_ws(self, conn):
        with self.lock:
            self.ws_clients.discard(conn)

    def _broadcast_ws(self, payload, clients):
        dead = []
        for conn in clients:
            if not websocket_send_binary(conn, payload):
                dead.append(conn)
        if dead:
            with self.lock:
                for conn in dead:
                    self.ws_clients.discard(conn)
                    close_quietly(conn)

    def send_control(self, text):
        if not self.control_host:
            return False, "control_host_not_configured"
        try:
            with socket.create_connection((self.control_host, self.control_port), timeout=1.5) as s:
                s.sendall(text.encode("utf-8") + b"\n")
            return True, "ok"
        except OSError as exc:
            return False, str(exc)


class MultiHub:
    def __init__(self, devices):
        self.devices = devices

    def get(self, device_id):
        if device_id < 0 or device_id >= len(self.devices):
            return None
        return self.devices[device_id]

    def status(self):
        return {
            "role": "windows_multi_video_receiver",
            "device_count": len(self.devices),
            "devices": [d.status() for d in self.devices],
        }


multi_hub = None


def get_automation_settings():
    with automation_settings_lock:
        return dict(automation_settings)


def set_automation_settings(values):
    global automation_settings
    settings = normalize_automation_settings(values)
    with automation_settings_lock:
        automation_settings = settings
    return dict(settings)


class AutomationManager:
    def __init__(self):
        self.lock = threading.Lock()
        self.processes = {}

    def status(self):
        with self.lock:
            result = {}
            for device_id, proc in list(self.processes.items()):
                running = proc.poll() is None
                if not running:
                    self.processes.pop(device_id, None)
                result[str(device_id)] = {"running": running, "pid": proc.pid}
            return result

    def start(self, device_ids, keywords, scan_interval=1.0):
        keywords = self.normalize_keywords(keywords)
        if not keywords:
            return {"started": [], "already_running": [], "errors": {"keywords": "missing"}}
        settings = get_automation_settings()
        swipe_delta = settings["swipe_percent"] / 100.0
        swipe_start = 0.68
        swipe_end = max(0.05, swipe_start - swipe_delta)

        started = []
        already_running = []
        errors = {}

        with self.lock:
            for device_id in device_ids:
                hub = multi_hub.get(device_id) if multi_hub else None
                if not hub:
                    errors[str(device_id)] = "unknown device"
                    continue

                existing = self.processes.get(device_id)
                if existing and existing.poll() is None:
                    already_running.append(device_id)
                    continue

                snapshot_path = os.path.join(BASE_DIR, "snapshots", f"device_{device_id}", "latest.png")
                log_dir = os.path.join(BASE_DIR, "logs")
                os.makedirs(log_dir, exist_ok=True)
                log_path = os.path.join(log_dir, f"automation_device_{device_id}.log")
                log_file = open(log_path, "a", encoding="utf-8", buffering=1)

                args = [
                    sys.executable,
                    os.path.join(BASE_DIR, "ocr_screen_bot.py"),
                    "--device-id", str(device_id),
                    "--snapshot", snapshot_path,
                    "--automation-http-port", str(AUTOMATION_SNAPSHOT_HTTP_PORT),
                    "--rk-host", hub.control_host,
                    "--control-port", str(hub.control_port),
                    "--scan-interval", str(scan_interval),
                    "--settings-url", f"http://127.0.0.1:{AUTOMATION_SNAPSHOT_HTTP_PORT}/automation/settings",
                    "--view-seconds", str(settings["view_seconds"]),
                    "--page-ocr-passes", str(settings["page_ocr_passes"]),
                    "--swipe-start-ratio", f"{swipe_start:.3f}",
                    "--swipe-end-ratio", f"{swipe_end:.3f}",
                    "--action-delay", str(settings["action_delay"]),
                    "--tap-y-offset", str(settings["tap_y_offset"]),
                    "--detail-action", settings["detail_action"],
                ]
                args.append("--ignore-author-hits" if settings["ignore_author_hits"] else "--no-ignore-author-hits")
                for keyword in keywords:
                    args += ["--keyword", keyword]

                try:
                    proc = subprocess.Popen(
                        args,
                        cwd=BASE_DIR,
                        stdout=log_file,
                        stderr=subprocess.STDOUT,
                    )
                except OSError as exc:
                    errors[str(device_id)] = str(exc)
                    log_file.close()
                    continue

                self.processes[device_id] = proc
                started.append(device_id)

        return {"started": started, "already_running": already_running, "errors": errors, "settings": settings}
    def normalize_keywords(self, keywords):
        result = []
        seen = set()
        for value in keywords or []:
            for part in re.split(r"[\s,，、;；]+", str(value)):
                text = part.strip()
                if text and text not in seen:
                    seen.add(text)
                    result.append(text)
        return result

    def stop(self, device_ids=None):
        stopped = []
        with self.lock:
            ids = list(self.processes.keys()) if device_ids is None else device_ids
            for device_id in ids:
                proc = self.processes.get(device_id)
                if not proc:
                    continue
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                self.processes.pop(device_id, None)
                stopped.append(device_id)
        return {"stopped": stopped}


automation_manager = AutomationManager()


class SnapshotRequestManager:
    def __init__(self):
        self.lock = threading.Lock()
        self.pending = {}

    def request(self, device_ids, reason):
        with self.lock:
            for device_id in device_ids:
                self.pending[int(device_id)] = str(reason or "automation")

    def take(self):
        with self.lock:
            payload = [{"id": device_id, "reason": reason} for device_id, reason in self.pending.items()]
            self.pending.clear()
            return payload


snapshot_request_manager = SnapshotRequestManager()


def recover_device_on_rk(device_id):
    url = f"http://{RK_RECOVER_HOST}:{RK_RECOVER_PORT}/recover"
    data = json.dumps({"device": int(device_id)}).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=8) as response:
            body = response.read(1024 * 1024)
            try:
                payload = json.loads(body.decode("utf-8"))
            except json.JSONDecodeError:
                payload = {"raw": body.decode("utf-8", errors="replace")}
            return {"status": "ok", "rk": payload}
    except urllib.error.HTTPError as exc:
        return {"status": "failed", "message": f"RK recover HTTP {exc.code}"}
    except OSError as exc:
        return {"status": "failed", "message": str(exc)}


class VideoInHandler(socketserver.BaseRequestHandler):
    def handle(self):
        hub = self.server.device_hub
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        print(f"device {hub.device_id} video bridge connected: {peer}", flush=True)
        hub.set_connected(True)
        try:
            while True:
                packet = read_avc1_packet(self.request)
                if packet is None:
                    break
                hub.publish_avc1(packet)
        finally:
            hub.set_connected(False)
            print(f"device {hub.device_id} video bridge disconnected: {peer}", flush=True)


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class DeviceTCPServer(ThreadedTCPServer):
    def __init__(self, server_address, handler_class, device_hub):
        self.device_hub = device_hub
        super().__init__(server_address, handler_class)


class HttpHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/status":
            body = json.dumps(multi_hub.status(), ensure_ascii=False).encode("utf-8") + b"\n"
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/automation/status":
            body = json.dumps(
                {"devices": automation_manager.status(), "settings": get_automation_settings()},
                ensure_ascii=False,
            ).encode("utf-8") + b"\n"
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/automation/settings":
            self.send_json({"settings": get_automation_settings()})
        elif path == "/automation/snapshot_requests":
            body = json.dumps({"requests": snapshot_request_manager.take()}, ensure_ascii=False).encode("utf-8") + b"\n"
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path.startswith("/video/"):
            self.handle_video_ws(path)
        elif path.startswith("/ws/"):
            self.handle_control_ws(path)
        else:
            body = load_index_html().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        if path == "/automation/start":
            payload = self.read_json_body()
            device_ids = [int(x) for x in payload.get("devices", [])]
            keywords = [str(x).strip() for x in payload.get("keywords", []) if str(x).strip()]
            if not keywords:
                self.send_json({"started": [], "already_running": [], "errors": {"keywords": "missing"}})
                return
            scan_interval = float(payload.get("scan_interval", 1.0))
            result = automation_manager.start(device_ids, keywords, scan_interval)
            self.send_json(result)
            return

        if path == "/automation/settings":
            payload = self.read_json_body()
            settings = set_automation_settings(payload.get("settings", payload))
            self.send_json({"settings": settings})
            return

        if path == "/automation/stop":
            payload = self.read_json_body()
            raw_ids = payload.get("devices")
            device_ids = None if raw_ids is None else [int(x) for x in raw_ids]
            result = automation_manager.stop(device_ids)
            self.send_json(result)
            return

        if path == "/automation/request_snapshot":
            payload = self.read_json_body()
            device_ids = [int(x) for x in payload.get("devices", [])]
            reason = str(payload.get("reason", "automation"))
            snapshot_request_manager.request(device_ids, reason)
            self.send_json({"status": "ok", "devices": device_ids, "reason": reason})
            return

        if path == "/device/recover":
            payload = self.read_json_body()
            try:
                device_id = int(payload.get("device"))
            except (TypeError, ValueError):
                self.send_error(400, "invalid device")
                return
            hub = multi_hub.get(device_id)
            if not hub:
                self.send_error(404, "unknown device")
                return
            result = recover_device_on_rk(device_id)
            self.send_json({"device": device_id, **result})
            return

        if not path.startswith("/snapshot/"):
            self.send_error(404, "unknown endpoint")
            return

        device_id = self.device_from_path(path)
        hub = multi_hub.get(device_id)
        if not hub:
            self.send_error(404, "unknown device")
            return

        try:
            content_length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            content_length = 0
        if content_length <= 0 or content_length > 20 * 1024 * 1024:
            self.send_error(400, "invalid snapshot size")
            return

        data = self.rfile.read(content_length)
        if not data:
            self.send_error(400, "empty snapshot")
            return

        snapshot_root = os.path.join(BASE_DIR, "snapshots")
        device_dir = os.path.join(snapshot_root, f"device_{device_id}")
        os.makedirs(device_dir, exist_ok=True)
        now = time.time()
        timestamp = time.strftime("%Y%m%d_%H%M%S", time.localtime(now)) + f"_{int((now % 1) * 1000):03d}"
        latest_path = os.path.join(device_dir, "latest.png")
        dated_path = os.path.join(device_dir, f"device_{device_id}_{timestamp}.png")
        with open(latest_path, "wb") as f:
            f.write(data)
        with open(dated_path, "wb") as f:
            f.write(data)
        removed_count = prune_device_snapshots(device_dir, device_id)

        body = json.dumps({
            "status": "ok",
            "path": os.path.relpath(dated_path, BASE_DIR),
            "latest": os.path.relpath(latest_path, BASE_DIR),
            "kept": SNAPSHOT_KEEP_COUNT,
            "removed": removed_count,
        }).encode("utf-8") + b"\n"
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        return

    def read_json_body(self):
        try:
            content_length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            content_length = 0
        if content_length <= 0:
            return {}
        data = self.rfile.read(min(content_length, 1024 * 1024))
        try:
            return json.loads(data.decode("utf-8"))
        except json.JSONDecodeError:
            return {}

    def send_json(self, value):
        body = json.dumps(value, ensure_ascii=False).encode("utf-8") + b"\n"
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def device_from_path(self, path):
        try:
            return int(path.strip("/").split("/")[1])
        except (IndexError, ValueError):
            return -1

    def handle_video_ws(self, path):
        device_id = self.device_from_path(path)
        hub = multi_hub.get(device_id)
        if not hub:
            self.send_error(404, "unknown device")
            return
        if not websocket_handshake(self):
            return
        hub.add_ws(self.connection)
        try:
            while True:
                time.sleep(1)
        finally:
            hub.remove_ws(self.connection)

    def handle_control_ws(self, path):
        device_id = self.device_from_path(path)
        hub = multi_hub.get(device_id)
        if not hub:
            self.send_error(404, "unknown device")
            return
        if not websocket_handshake(self):
            return
        websocket_send_text(self.connection, '{"type":"hello","status":"ok"}')
        while True:
            try:
                text = websocket_read_text(self.connection)
            except OSError:
                break
            if text is None:
                break
            ok, msg = hub.send_control(text)
            if not ('"type":"move"' in text or '"type":"cursor"' in text or '"type":"stream"' in text):
                print(f"device {device_id} control {text} -> {'ok' if ok else msg}", flush=True)
            if not ('"type":"move"' in text or '"type":"cursor"' in text or '"type":"stream"' in text):
                websocket_send_text(
                    self.connection,
                    json.dumps({"status": "ok" if ok else "failed", "message": msg})
                )


class ThreadedHTTPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


def websocket_handshake(handler):
    key = handler.headers.get("Sec-WebSocket-Key")
    if not key:
        handler.send_error(400, "missing websocket key")
        return False
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    handler.send_response(101)
    handler.send_header("Upgrade", "websocket")
    handler.send_header("Connection", "Upgrade")
    handler.send_header("Sec-WebSocket-Accept", accept)
    handler.end_headers()
    return True


def websocket_send_binary(conn, payload):
    return websocket_send_frame(conn, 0x82, payload)


def websocket_send_text(conn, text):
    return websocket_send_frame(conn, 0x81, text.encode("utf-8"))


def websocket_send_frame(conn, opcode, payload):
    try:
        n = len(payload)
        if n < 126:
            hdr = bytes([opcode, n])
        elif n <= 65535:
            hdr = bytes([opcode, 126]) + struct.pack(">H", n)
        else:
            hdr = bytes([opcode, 127]) + struct.pack(">Q", n)
        conn.sendall(hdr + payload)
        return True
    except OSError:
        return False


def websocket_read_text(conn):
    hdr = read_exact(conn, 2)
    if not hdr:
        return None
    opcode = hdr[0] & 0x0F
    masked = hdr[1] & 0x80
    n = hdr[1] & 0x7F
    if opcode == 0x8:
        return None
    if n == 126:
        n = struct.unpack(">H", read_exact(conn, 2))[0]
    elif n == 127:
        n = struct.unpack(">Q", read_exact(conn, 8))[0]
    mask = read_exact(conn, 4) if masked else b"\x00\x00\x00\x00"
    data = read_exact(conn, n)
    if data is None:
        return None
    if masked:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return data.decode("utf-8", "replace")


INDEX_HTML = r"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RK AOA Multi Device Console</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#292929;color:#e5e7eb;font-family:Arial,sans-serif;overflow:hidden}
header{height:46px;display:flex;align-items:center;justify-content:space-between;padding:0 16px;border-bottom:1px solid #3a3a3a;background:#1f1f1f}
h1{font-size:16px;margin:0;font-weight:700}.summary{font-size:12px;color:#aab0bb}
.layout{height:calc(100vh - 46px);display:grid;grid-template-columns:180px 1fr;gap:28px;padding:8px 28px}
.sidebar{display:flex;flex-direction:column;gap:16px;overflow:auto;padding:4px 4px 16px}
.device-item{height:130px;border:2px solid #ef4444;border-radius:4px;background:#2f2f2f;color:#e5e7eb;display:grid;grid-template-rows:1fr auto;cursor:pointer;user-select:none;outline:none}
.device-item.active{background:#383838;box-shadow:0 0 0 2px rgba(239,68,68,.25)}
.device-title{position:relative;display:flex;align-items:center;justify-content:center;font-weight:700;font-size:15px}.sync-check{position:absolute;left:8px;top:8px;width:18px;height:18px;accent-color:#0ea5e9}.device-meta{padding:8px;border-top:1px solid rgba(239,68,68,.45);font-size:11px;color:#b8bec8;line-height:1.35}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#f6c85f;margin-right:5px}.dot.ok{background:#68d391}.dot.bad{background:#f87171}
.stage-wrap{min-width:0;display:grid;grid-template-rows:30px minmax(0,1fr) 40px;align-self:center;justify-self:center;width:min(440px,100%);height:calc(100vh - 62px)}
.stage-bar{display:flex;align-items:center;justify-content:space-between;padding:0 10px;border:2px solid #0ea5e9;border-bottom:0;border-radius:4px 4px 0 0;background:#2f2f2f;font-size:12px}
.stage{position:relative;min-height:0;border:2px solid #0ea5e9;background:#111;display:grid;place-items:center;touch-action:none;user-select:none;cursor:none;overflow:hidden}
.stage.empty:after{content:"Select a device";color:#8b949e;font-size:15px}
.stage canvas{display:block;max-width:100%;max-height:100%;width:auto;height:auto;background:#000;touch-action:none;object-fit:contain}
.parking{display:none}.cursor{position:absolute;left:0;top:0;width:14px;height:14px;border:2px solid #fff;border-radius:50%;box-shadow:0 0 0 1px #111,0 1px 6px #000;pointer-events:none;transform:translate(-50%,-50%);display:none;z-index:5}
.cursor:after{content:"";position:absolute;left:5px;top:5px;width:3px;height:3px;background:#fff;border-radius:50%}
.log{font-size:12px;color:#aab0bb;padding:5px 10px;border:2px solid #0ea5e9;border-top:0;border-radius:0 0 4px 4px;background:#2f2f2f;white-space:pre-wrap;overflow:hidden}
@media(max-width:760px){body{overflow:auto}.layout{height:auto;grid-template-columns:1fr;padding:12px}.sidebar{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));max-height:none}.device-item{height:110px}.stage-wrap{width:100%;height:min(760px,82vh)}}
</style></head><body>
<header><h1>RK AOA Multi Device Console</h1><div class="summary" id="summary">Starting...</div></header>
<main class="layout"><aside class="sidebar" id="deviceList"></aside><section class="stage-wrap"><div class="stage-bar"><span id="stageTitle">No device selected</span><span id="stageState">waiting</span></div><div class="stage empty" id="stage"><div class="cursor" id="cursor"></div></div><div class="log" id="mainLog">Select a device from the left list.</div></section></main>
<div class="parking" id="parking"></div>
<script>
let devices=[],views=[],activeView=null,activeId=-1;
const list=document.getElementById('deviceList'),summary=document.getElementById('summary'),stage=document.getElementById('stage'),stageTitle=document.getElementById('stageTitle'),stageState=document.getElementById('stageState'),mainLog=document.getElementById('mainLog'),parking=document.getElementById('parking'),cursor=document.getElementById('cursor');
function u32(dv,o){return dv.getUint32(o,false)}function u64(dv,o){return dv.getUint32(o,false)*4294967296+dv.getUint32(o+4,false)}
async function loadStatus(){const r=await fetch('/status');const s=await r.json();devices=s.devices||[];summary.textContent='devices='+devices.length;devices.forEach(createDevice);if(devices.length>0)selectDevice(devices[0].id);}
function createDevice(cfg){if(views[cfg.id])return;const item=document.createElement('div');item.className='device-item';item.id='devItem'+cfg.id;item.tabIndex=0;item.innerHTML='<div class="device-title"><input class="sync-check" type="checkbox" id="sync'+cfg.id+'">Device '+cfg.id+'</div><div class="device-meta"><span class="dot bad" id="dot'+cfg.id+'"></span><span id="miniState'+cfg.id+'">waiting</span><br>video '+cfg.video_port+'<br>control '+cfg.control_port+'</div>';item.onclick=e=>{if(e.target&&e.target.classList.contains('sync-check'))return;selectDevice(cfg.id);};item.onkeydown=e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();selectDevice(cfg.id);}};list.appendChild(item);views[cfg.id]=new DeviceView(cfg.id,cfg);}
function selectDevice(id){const view=views[id];if(!view)return;if(activeView&&activeView.id!==id){activeView.setStreamMode('idle');activeView.closeVideo();if(activeView.canvas.parentNode===stage){parking.appendChild(activeView.canvas)}}activeView=view;activeId=id;document.querySelectorAll('.device-item').forEach(el=>el.classList.remove('active'));const item=document.getElementById('devItem'+id);if(item)item.classList.add('active');stage.classList.remove('empty');stage.appendChild(view.canvas);stage.appendChild(cursor);view.fitCanvas();cursor.style.display='none';stageTitle.textContent='Device '+id;stageState.textContent=view.online?'online':'waiting';mainLog.textContent=view.lastLog||'video '+view.cfg.video_port+' | control '+view.cfg.control_port;view.setStreamMode('active');view.connectVideo();}
class DeviceView{
 constructor(id,cfg){this.id=id;this.cfg=cfg;this.W=720;this.H=1568;this.decoder=null;this.decoderReady=false;this.pending=[];this.frames=0;this.fps=0;this.lastFps=performance.now();this.lastDecoderReset=0;this.wsReady=false;this.videoReady=false;this.online=false;this.active=false;this.pointerId=null;this.down=null;this.drag=false;this.downSent=false;this.lastMove=0;this.timer=null;this.lastLog='';this.vws=null;this.videoReconnect=null;this.snapshotPending=false;this.snapshotBusy=false;this.canvas=document.createElement('canvas');this.canvas.width=this.W;this.canvas.height=this.H;this.canvas.id='canvas'+id;this.ctx=this.canvas.getContext('2d');parking.appendChild(this.canvas);this.connectControl();}
 log(s){this.lastLog=s;if(activeId===this.id)mainLog.textContent=s;}state(s,ok){this.online=ok;const mini=document.getElementById('miniState'+this.id),dot=document.getElementById('dot'+this.id);if(mini)mini.textContent=s;if(dot)dot.className='dot '+(ok?'ok':'bad');if(activeId===this.id){stageState.textContent=s;}}
 setSize(w,h){if(w&&h&&(w!==this.W||h!==this.H)){this.W=w;this.H=h;this.canvas.width=w;this.canvas.height=h;}this.fitCanvas();}
 fitCanvas(){const sw=stage.clientWidth||420,sh=stage.clientHeight||800;const scale=Math.min(sw/this.W,sh/this.H);this.canvas.style.width=Math.max(1,Math.floor(this.W*scale))+'px';this.canvas.style.height=Math.max(1,Math.floor(this.H*scale))+'px';}
 async makeDecoder(){if(!('VideoDecoder'in window)){this.log('WebCodecs required: use Chrome or Edge');return null;}const codecs=['avc1.640028','avc1.640032','avc1.64001f','avc1.4D4028','avc1.42E01E'];for(const codec of codecs){for(const cfg of [{codec,codedWidth:this.W,codedHeight:this.H,optimizeForLatency:true,avc:{format:'annexb'}},{codec,codedWidth:this.W,codedHeight:this.H,optimizeForLatency:true}]){try{if(VideoDecoder.isConfigSupported){const r=await VideoDecoder.isConfigSupported(cfg);if(!r.supported)continue;}const d=new VideoDecoder({output:f=>{try{this.ctx.drawImage(f,0,0,this.W,this.H);}finally{f.close();}if(this.snapshotPending)this.finishSnapshot();this.frames++;const now=performance.now();if(now-this.lastFps>=1000){this.fps=this.frames;this.frames=0;this.lastFps=now;}},error:e=>this.log('decoder error: '+e.message)});d.configure(cfg);return d;}catch(e){}}}this.log('H.264 decoder config failed');return null;}
 async ensureDecoder(){if(this.decoderReady&&this.decoder)return this.decoder;this.decoder=await this.makeDecoder();this.decoderReady=!!this.decoder;if(this.decoderReady){const q=this.pending;this.pending=[];q.forEach(b=>this.decodePacket(b));}return this.decoder;}
 decodePacket(buf){if(buf.byteLength<28)return;const dv=new DataView(buf);if(String.fromCharCode(dv.getUint8(0),dv.getUint8(1),dv.getUint8(2),dv.getUint8(3))!=='AVC1')return;const w=u32(dv,4),h=u32(dv,8),flags=u32(dv,12);let pts=u64(dv,16);const size=u32(dv,24);if(28+size>buf.byteLength)return;if((flags&2)&&!(flags&1))return;this.setSize(w,h);if(!this.decoderReady||!this.decoder){this.pending.push(buf);this.ensureDecoder();return;}const data=new Uint8Array(buf,28,size);const type=(flags&1)?'key':'delta';if(flags&2)pts=performance.now()*1000;try{if(this.decoder.decodeQueueSize>8){const now=performance.now();if(now-this.lastDecoderReset>1000){this.lastDecoderReset=now;try{this.decoder.close()}catch(_){}this.decoder=null;this.decoderReady=false;this.pending=[];this.log('decoder reset: queue stuck');}return;}this.decoder.decode(new EncodedVideoChunk({type,timestamp:pts||Math.round(performance.now()*1000),data}));this.state('online',true);this.log('video '+this.W+'x'+this.H+' fps='+this.fps+' queue='+this.decoder.decodeQueueSize+' control='+(this.wsReady?'ok':'wait'));}catch(e){this.log('decode error: '+e.message);}}
 requestSnapshot(){if(activeId===this.id||this.snapshotBusy)return;this.snapshotBusy=true;this.snapshotPending=true;this.setStreamMode('active');this.connectVideo();setTimeout(()=>{if(this.snapshotPending){this.snapshotPending=false;this.snapshotBusy=false;this.closeVideo();this.setStreamMode('idle');}},12000);}
 finishSnapshot(){if(!this.snapshotPending)return;this.snapshotPending=false;this.canvas.toBlob(blob=>{if(!blob){this.snapshotBusy=false;this.closeVideo();this.setStreamMode('idle');return;}fetch('/snapshot/'+this.id,{method:'POST',headers:{'Content-Type':'image/png'},body:blob}).catch(()=>{}).finally(()=>{this.snapshotBusy=false;if(activeId!==this.id){this.closeVideo();this.setStreamMode('idle');}});},'image/png');}
 connectVideo(){if(this.videoReady||this.vws)return;const vws=new WebSocket('ws://'+location.host+'/video/'+this.id);this.vws=vws;vws.binaryType='arraybuffer';vws.onopen=()=>{this.videoReady=true;this.log('video ws connected, waiting RK stream...')};vws.onmessage=e=>{if(e.data instanceof ArrayBuffer)this.decodePacket(e.data);};vws.onclose=()=>{this.videoReady=false;this.vws=null;this.state('waiting',false);this.decoderReady=false;try{this.decoder&&this.decoder.close();}catch(_){}this.decoder=null;if(activeId===this.id){clearTimeout(this.videoReconnect);this.videoReconnect=setTimeout(()=>this.connectVideo(),1000)}};vws.onerror=()=>this.log('video ws error');}
 closeVideo(){clearTimeout(this.videoReconnect);this.videoReconnect=null;if(this.vws){const ws=this.vws;this.vws=null;try{ws.onclose=null;ws.close()}catch(_){}}this.videoReady=false;this.decoderReady=false;try{this.decoder&&this.decoder.close()}catch(_){}this.decoder=null;this.pending=[];}
 connectControl(){this.ws=new WebSocket('ws://'+location.host+'/ws/'+this.id);this.ws.onopen=()=>{this.wsReady=true;this.log('control connected');this.setStreamMode(activeId===this.id?'active':'idle');};this.ws.onmessage=e=>{try{const r=JSON.parse(e.data);if(r.status==='failed')this.log('control failed: '+r.message);}catch(_){}};this.ws.onclose=()=>{this.wsReady=false;this.log('control disconnected');setTimeout(()=>this.connectControl(),1000);};this.ws.onerror=()=>{this.wsReady=false;this.log('control ws error');};}
 pt(e){const r=this.canvas.getBoundingClientRect();return{x:Math.max(0,Math.min(this.W-1,Math.round((e.clientX-r.left)*this.W/r.width))),y:Math.max(0,Math.min(this.H-1,Math.round((e.clientY-r.top)*this.H/r.height)))}}send(a){if(!this.wsReady||!this.ws||this.ws.readyState!==1){this.log('control not ready');return false;}this.ws.send(JSON.stringify(a));return true;}setStreamMode(mode){this.send({type:'stream',mode});}dist(a,b){const x=a.x-b.x,y=a.y-b.y;return Math.sqrt(x*x+y*y)}startDrag(){if(!this.active||!this.down||this.downSent)return;this.drag=true;this.downSent=true;this.send({type:'down',x:this.down.x,y:this.down.y,width:this.W,height:this.H});}}
function route(){return activeView}
function controlTargets(){const ids=new Set();if(activeView)ids.add(activeView.id);views.forEach(v=>{const cb=document.getElementById('sync'+v.id);if(cb&&cb.checked)ids.add(v.id);});return Array.from(ids).map(id=>views[id]).filter(Boolean)}
function sendToTargets(action){controlTargets().forEach(v=>v.send(Object.assign({},action)))}
function startDragTargets(){controlTargets().forEach(v=>{if(v.active&&v.down&&!v.downSent){v.drag=true;v.downSent=true;v.send({type:'down',x:v.down.x,y:v.down.y,width:v.W,height:v.H});}})}
function targetPoint(v,base,source){if(!source||source.W<=1||source.H<=1)return{x:base.x,y:base.y};return{x:Math.max(0,Math.min(v.W-1,Math.round(base.x*(v.W-1)/(source.W-1)))),y:Math.max(0,Math.min(v.H-1,Math.round(base.y*(v.H-1)/(source.H-1))))}}
function keyUsage(e){const c=e.code;if(/^Key[A-Z]$/.test(c))return 4+c.charCodeAt(3)-65;if(/^Digit[1-9]$/.test(c))return 0x1e+Number(c[5])-1;if(c==='Digit0')return 0x27;const m={Enter:0x28,Escape:0x29,Backspace:0x2a,Tab:0x2b,Space:0x2c,Minus:0x2d,Equal:0x2e,BracketLeft:0x2f,BracketRight:0x30,Backslash:0x31,Semicolon:0x33,Quote:0x34,Backquote:0x35,Comma:0x36,Period:0x37,Slash:0x38,CapsLock:0x39,PrintScreen:0x46,ScrollLock:0x47,Pause:0x48,Insert:0x49,Home:0x4a,PageUp:0x4b,Delete:0x4c,End:0x4d,PageDown:0x4e,ArrowRight:0x4f,ArrowLeft:0x50,ArrowDown:0x51,ArrowUp:0x52};if(/^F([1-9]|1[0-2])$/.test(c))return 0x3a+Number(c.slice(1))-1;return m[c]||0}
function keyModifier(e){let m=0;if(e.ctrlKey)m|=1;if(e.shiftKey)m|=2;if(e.altKey)m|=4;if(e.metaKey)m|=8;return m}
function sendKey(e,action){const v=route();if(!v||e.ctrlKey||e.altKey||e.metaKey)return;const usage=keyUsage(e);if(!usage)return;if(action==='down'&&e.repeat)return;e.preventDefault();sendToTargets({type:'key',action,usage,modifier:keyModifier(e)})}
function moveCursor(e){const r=stage.getBoundingClientRect();cursor.style.left=(e.clientX-r.left)+'px';cursor.style.top=(e.clientY-r.top)+'px';cursor.style.display='block'}
stage.oncontextmenu=e=>e.preventDefault();stage.onpointerenter=e=>{if(route())moveCursor(e)};stage.onpointerleave=e=>{const v=route();if(v&&!v.active)cursor.style.display='none'};
stage.onpointerdown=e=>{const v=route();if(!v||e.button!==0)return;e.preventDefault();moveCursor(e);stage.setPointerCapture(e.pointerId);const p=v.pt(e);const targets=controlTargets();targets.forEach(t=>{const tp=targetPoint(t,p,v);t.send({type:'cursor',x:tp.x,y:tp.y,width:t.W,height:t.H});t.active=true;t.pointerId=e.pointerId;t.down=tp;t.drag=false;t.downSent=false;});v.timer=setTimeout(()=>startDragTargets(),450)};
stage.onpointermove=e=>{const v=route();if(!v)return;moveCursor(e);const p=v.pt(e);if(!v.active)return;if(v.pointerId!==e.pointerId)return;e.preventDefault();if(!v.drag&&v.dist(p,v.down)>8){clearTimeout(v.timer);startDragTargets()}if(v.drag&&Date.now()-v.lastMove>16){v.lastMove=Date.now();controlTargets().forEach(t=>{if(!t.active||t.pointerId!==e.pointerId)return;const tp=targetPoint(t,p,v);if(t.drag)t.send({type:'move',x:tp.x,y:tp.y,width:t.W,height:t.H});})}};
stage.onpointerup=e=>{const v=route();if(!v||!v.active||v.pointerId!==e.pointerId)return;e.preventDefault();clearTimeout(v.timer);const p=v.pt(e);controlTargets().forEach(t=>{if(!t.active||t.pointerId!==e.pointerId)return;const tp=targetPoint(t,p,v);if(t.drag||t.downSent)t.send({type:'up',x:tp.x,y:tp.y,width:t.W,height:t.H});else t.send({type:'tap',x:tp.x,y:tp.y,width:t.W,height:t.H});t.active=false;t.pointerId=null;t.down=null;t.drag=false;t.downSent=false;});};
window.addEventListener('keydown',e=>sendKey(e,'down'));
window.addEventListener('keyup',e=>sendKey(e,'up'));
window.addEventListener('beforeunload',()=>{views.forEach(v=>{if(v)v.setStreamMode('idle');});});
window.onresize=()=>{if(activeView)activeView.fitCanvas()};
setInterval(()=>{views.forEach(v=>{if(v&&v.id!==activeId)v.requestSnapshot();});},60000);
setInterval(()=>{if(activeView){activeView.setStreamMode('active');activeView.connectVideo();}},2000);
loadStatus().catch(e=>{summary.textContent='status error: '+e.message});
</script></body></html>"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--http-host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=18091)
    parser.add_argument("--video-host", default="0.0.0.0")
    parser.add_argument("--control-host", default="192.168.110.86")
    parser.add_argument("--rk-recover-host", default="")
    parser.add_argument("--rk-recover-port", type=int, default=18110)
    parser.add_argument("--count", type=int, default=2)
    parser.add_argument("--base-video-port", type=int, default=9001)
    parser.add_argument("--base-control-port", type=int, default=9002)
    parser.add_argument("--port-step", type=int, default=10)
    parser.add_argument("--open-browser", action="store_true")
    args = parser.parse_args()

    global AUTOMATION_SNAPSHOT_HTTP_PORT
    AUTOMATION_SNAPSHOT_HTTP_PORT = args.http_port
    global RK_RECOVER_HOST, RK_RECOVER_PORT
    RK_RECOVER_HOST = args.rk_recover_host or args.control_host
    RK_RECOVER_PORT = args.rk_recover_port

    if args.count < 1:
        raise SystemExit("--count must be >= 1")

    devices = []
    servers = []
    for i in range(args.count):
        video_port = args.base_video_port + i * args.port_step
        control_port = args.base_control_port + i * args.port_step
        hub = DeviceHub(i, video_port, args.control_host, control_port)
        devices.append(hub)
        server = DeviceTCPServer((args.video_host, video_port), VideoInHandler, hub)
        servers.append(server)
        threading.Thread(target=server.serve_forever, daemon=True).start()

    global multi_hub
    multi_hub = MultiHub(devices)

    http_server = ThreadedHTTPServer((args.http_host, args.http_port), HttpHandler)
    threading.Thread(target=http_server.serve_forever, daemon=True).start()

    url = f"http://{args.http_host}:{args.http_port}/"
    print(f"browser UI: {url}", flush=True)
    for d in devices:
        print(f"device {d.device_id}: video tcp://{args.video_host}:{d.video_port} control {d.control_host}:{d.control_port}", flush=True)
    if args.open_browser:
        webbrowser.open(url)

    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()


