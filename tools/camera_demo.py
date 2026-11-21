"""Serve the camera comparison and relay measurements from ESP32-S3 over UART."""

import argparse
from dataclasses import dataclass, field
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import socket
import threading
import time
from urllib.parse import parse_qs, urlsplit
import zlib

import serial
from serial.tools import list_ports


WIDTH, HEIGHT, TILE = 160, 96, 16
FRAME_BYTES = WIDTH * HEIGHT
TILE_COUNT = FRAME_BYTES // (TILE * TILE)
PROTOCOL_VERSION = 3
MODE_ORDER = ("SHARED", "ADAPTIVE")
HEX_TILE = re.compile(r"[0-9a-f]{512}")
WEB = Path(__file__).resolve().with_name("camera_web")
ASSETS = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
}
DEVICE_ERRORS = {
    "IMAGE_TIMEOUT": "Bo nhận thiếu dữ liệu ảnh qua UART.",
    "IMAGE_CRC": "Ảnh nhận trên bo không khớp mã kiểm tra CRC32.",
    "HEAP_START": "Bộ cấp phát không sẵn sàng ở đầu lượt.",
    "HEAP_BATCH": "Bo không tạo được lô ảnh tiếp theo.",
    "HEAP_FINISH": "Bộ cấp phát không qua được kiểm tra sau lượt.",
    "TELEMETRY_START": "Không tạo được bản tin kiểm tra đầu lượt.",
    "TELEMETRY_FINISH": "Không qua được kiểm tra bản tin sau lượt.",
    "BATCH_ACK": "Bo chưa nhận được xác nhận đã nhận đủ lô ảnh.",
}


def available_ports():
    return [{"device": port.device, "label": port.description}
            for port in sorted(list_ports.comports())
            if not (port.hwid or "").upper().startswith("BTHENUM")]


class ProtocolError(RuntimeError):
    """The device did not complete a valid camera-demo exchange."""


@dataclass
class LaneResult:
    tiles: list[str] = field(default_factory=list)
    core: int | None = None
    offset_us: int = 0
    batch: int = 0
    batch_end: int | None = None
    before: bool = False
    after: bool = False
    sample_count: int = 0
    frame_us: int | None = None

    def accept(self, event):
        """Check one record in this lane and return the batch to acknowledge, if any."""
        kind = event["event"]
        if self.after:
            raise ProtocolError("Bo gửi thêm dữ liệu sau bản tin kết thúc.")
        if kind == "sample":
            self.accept_sample(event)
            return None
        if kind == "lane_start":
            core, offset = event.get("core"), event.get("offset_us")
            if (not self.before or self.core is not None or type(core) is not int or core not in (0, 1)
                    or type(offset) is not int or offset < 0):
                raise ProtocolError("Bo trả thông tin khởi động tác vụ không hợp lệ.")
            self.core, self.offset_us = core, offset
            return None
        if self.core is None or self.frame_us is not None:
            raise ProtocolError("Bo gửi dữ liệu ảnh ngoài thời gian chạy của tác vụ.")
        if kind == "batch":
            first = len(self.tiles)
            count = event.get("count")
            if (self.batch_end is not None or event.get("batch") != self.batch + 1 or event.get("first") != first
                    or type(count) is not int or not 1 <= count <= TILE_COUNT - first
                    or any(type(event.get(key)) is not int or event[key] < 0 for key in ("borrowed", "charged"))):
                raise ProtocolError("Bo trả phạm vi hoặc dung lượng lô ảnh không hợp lệ.")
            self.batch += 1
            self.batch_end = first + count
        elif kind == "tile":
            gray = event.get("gray")
            if (self.batch_end is None or len(self.tiles) >= self.batch_end or event.get("index") != len(self.tiles)
                    or not isinstance(gray, str) or not HEX_TILE.fullmatch(gray)):
                raise ProtocolError("Bo trả ô ảnh thiếu dữ liệu hoặc sai vị trí.")
            self.tiles.append(gray)
            if len(self.tiles) == self.batch_end:
                return self.batch
        elif kind == "released":
            if self.batch_end != len(self.tiles) or event.get("batch") != self.batch:
                raise ProtocolError("Bo xác nhận trả nhầm lô bộ nhớ.")
            self.batch_end = None
        elif kind == "done":
            if (len(self.tiles) != TILE_COUNT or self.batch_end is not None or event.get("batches") != self.batch
                    or any(type(event.get(key)) is not int or event[key] < 0 for key in ("frame_us", "filter_us"))
                    or event["filter_us"] > event["frame_us"]):
                raise ProtocolError("Bo trả kết quả đo của tác vụ không hợp lệ.")
            self.frame_us = event["frame_us"]
        return None

    def accept_sample(self, event):
        phase, index, status = (event.get(key) for key in ("phase", "index", "status"))
        temp, elapsed = event.get("temperature_mc"), event.get("time_ms")
        if (status not in ("OK", "SENSOR_ERROR", "QUOTA", "NO_MEMORY")
                or (status == "OK" and type(temp) is not int) or (status != "OK" and temp is not None)
                or type(elapsed) is not int or elapsed < 0):
            raise ProtocolError("Bản tin nhiệt độ thiếu dữ liệu.")
        if phase == "before" and index == 0 and not self.before:
            self.before = True
        elif phase == "during" and self.core is not None and index == self.sample_count + 1:
            self.sample_count += 1
        elif phase == "after" and index == 0 and self.frame_us is not None:
            self.after = True
        else:
            raise ProtocolError("Bản tin nhiệt độ sai thứ tự.")


class CameraSerial:
    def __init__(self, port, baud):
        self.connection = serial.Serial(port=None, baudrate=baud, timeout=0.15, write_timeout=4)
        self.connection.dtr = False
        self.connection.rts = False
        self.connection.port = port
        self.buffer = bytearray()
        self.sequence = secrets.randbelow(800_000_000)

    def __enter__(self):
        try:
            self.connection.open()
        except serial.SerialException as error:
            raise ProtocolError(f"Không mở được {self.connection.port}. "
                                "Thoát IDF Monitor hoặc Heap Lab trước khi chạy.") from error
        return self

    def __exit__(self, *_exc):
        self.connection.close()

    def command(self, command):
        self.sequence += 1
        self.connection.write(f"\nCAMERA {self.sequence} {command}\n".encode("ascii"))
        return f"CAMERA,{self.sequence},".encode("ascii")

    def read_event(self, prefix, expected, deadline):
        kinds = (expected,) if isinstance(expected, str) else expected
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, _, self.buffer = self.buffer.partition(b"\n")
                if not line.startswith(prefix):
                    continue
                try:
                    event = json.loads(line[len(prefix):])
                    kind = event["event"]
                except (KeyError, TypeError, ValueError) as error:
                    raise ProtocolError("Bản tin UART của bo không hợp lệ.") from error
                if kind == "error":
                    reason = event.get("reason")
                    message = DEVICE_ERRORS.get(reason, f"Bo báo lỗi {reason}.")
                    mode = event.get("mode")
                    if mode in MODE_ORDER:
                        message = f"{mode} · {message}"
                    raise ProtocolError(message)
                if kind not in kinds:
                    raise ProtocolError(f"Bo gửi {kind} trong khi đang chờ {' hoặc '.join(kinds)}.")
                return event
            self.buffer.extend(self.connection.read(min(4096, self.connection.in_waiting or 1)))
            if len(self.buffer) > 8192 and b"\n" not in self.buffer:
                self.buffer.clear()
        raise TimeoutError(" hoặc ".join(kinds))

    def probe(self):
        deadline = time.monotonic() + 9
        while time.monotonic() < deadline:
            prefix = self.command("PING")
            try:
                event = self.read_event(prefix, "hello", min(deadline, time.monotonic() + 2))
            except TimeoutError:
                continue
            if event.get("version") == PROTOCOL_VERSION:
                return
            raise ProtocolError(f"Bo đang chạy phiên bản demo ảnh khác. Nạp bản có CAMERA_DEMO_READY,{PROTOCOL_VERSION}.")
        raise ProtocolError("Cổng nối tiếp không trả lời giao thức camera. "
                            f"Kiểm tra firmware có dòng CAMERA_DEMO_READY,{PROTOCOL_VERSION} và chọn đúng cổng.")

    def compare(self, image, send):
        self.probe()
        crc = f"{zlib.crc32(image):08x}"
        prefix = self.command(f"RUN {crc}")
        self.read_event(prefix, "ready", time.monotonic() + 3)
        self.connection.write(image)
        deadline = time.monotonic() + 35
        event = self.read_event(prefix, "start", deadline)
        if event.get("crc32") != crc or (event.get("width"), event.get("height")) != (WIDTH, HEIGHT):
            raise ProtocolError("Mã CRC32 do bo trả về không khớp ảnh đầu vào.")
        send(event)

        lanes = {mode: LaneResult() for mode in MODE_ORDER}
        kinds = ("lane_start", "sample", "batch", "tile", "released", "done", "complete")
        while True:
            event = self.read_event(prefix, kinds, deadline)
            if event["event"] == "complete":
                break
            mode = event.get("mode")
            if mode not in lanes:
                raise ProtocolError("Bo gửi bản tin thiếu tên chính sách bộ nhớ.")
            acknowledge = lanes[mode].accept(event)
            if acknowledge is not None:
                self.connection.write(f"CAMERA {self.sequence} ACK {mode} {acknowledge}\n".encode("ascii"))
            send(event)

        shared, adaptive = lanes.values()
        if any(not lane.after or lane.frame_us is None for lane in lanes.values()) or {shared.core, adaptive.core} != {0, 1}:
            raise ProtocolError("Bo chưa gửi đủ kết quả của hai tác vụ chạy trên hai lõi.")
        if shared.tiles != adaptive.tiles:
            raise ProtocolError("Hai ảnh đầu ra không trùng nhau ở mọi ô.")
        gap = shared.frame_us - adaptive.frame_us
        first_mode = "ADAPTIVE" if gap > 0 else "SHARED" if gap < 0 else None
        send({"event": "complete", "crc32": crc, "matching_tiles": len(shared.tiles),
              "first_mode": first_mode, "finish_gap_us": abs(gap),
              "start_skew_us": abs(shared.offset_us - adaptive.offset_us)})


class DemoServer(ThreadingHTTPServer):
    def __init__(self, address, preferred_port, baud, sample_path):
        super().__init__(address, DemoHandler)
        self.preferred_port = preferred_port
        self.baud = baud
        self.sample_path = sample_path
        self.uart_lock = threading.Lock()


class DemoHandler(BaseHTTPRequestHandler):
    def valid_origin(self):
        port = self.server.server_port
        hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}
        origin = self.headers.get("Origin")
        return (self.headers.get("Host") in hosts and
                (origin is None or origin in {f"http://{host}" for host in hosts}))

    def send_bytes(self, data, content_type):
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if not self.valid_origin():
            self.send_error(HTTPStatus.FORBIDDEN)
            return
        path = urlsplit(self.path).path
        if path in ASSETS:
            name, content_type = ASSETS[path]
            self.send_bytes((WEB / name).read_bytes(), content_type)
        elif path == "/sample.jpg" and self.server.sample_path is not None:
            self.send_bytes(self.server.sample_path.read_bytes(), "image/jpeg")
        elif path == "/api/ports":
            data = json.dumps({"ports": available_ports(), "preferred": self.server.preferred_port}).encode("utf-8")
            self.send_bytes(data, "application/json; charset=utf-8")
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        if not self.valid_origin():
            self.send_error(HTTPStatus.FORBIDDEN)
            return
        url = urlsplit(self.path)
        if url.path != "/api/compare":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        if (self.headers.get("Content-Length") != str(FRAME_BYTES) or
                self.headers.get_content_type() != "application/octet-stream"):
            self.send_error(HTTPStatus.BAD_REQUEST, f"Expected a {WIDTH} x {HEIGHT} grayscale frame")
            return
        port = parse_qs(url.query).get("port", [""])[0]
        if port not in {item["device"] for item in available_ports()}:
            self.send_error(HTTPStatus.BAD_REQUEST, "Serial port is not connected")
            return
        if not self.server.uart_lock.acquire(blocking=False):
            self.send_error(HTTPStatus.CONFLICT, "A comparison is already running")
            return
        try:
            self.connection.settimeout(15)
            image = self.rfile.read(FRAME_BYTES)
            if len(image) != FRAME_BYTES:
                self.send_error(HTTPStatus.BAD_REQUEST, "Incomplete image")
                return
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "application/x-ndjson; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                with CameraSerial(port, self.server.baud) as link:
                    link.compare(image, self.send_event)
            except (ProtocolError, serial.SerialException, TimeoutError) as error:
                message = f"Hết thời gian chờ bản tin {error} từ bo." if isinstance(error, TimeoutError) else str(error)
                self.send_event({"event": "error", "message": message})
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            pass
        finally:
            self.server.uart_lock.release()

    def send_event(self, event):
        self.wfile.write((json.dumps(event, ensure_ascii=False) + "\n").encode("utf-8"))
        self.wfile.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--com", default="COM7", help="preferred UART port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--http-port", type=int, default=8765)
    args = parser.parse_args()
    sdk = os.environ.get("IDF_PATH")
    sample = Path(sdk) / "examples/peripherals/lcd/tjpgd/main/image.jpg" if sdk else None
    sample = sample if sample is not None and sample.is_file() else None
    with DemoServer(("127.0.0.1", args.http_port), args.com, args.baud, sample) as server:
        print(f"Heap Lab Camera · http://127.0.0.1:{server.server_port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
