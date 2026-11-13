"""Capture ESP32 UART logs until suite completion or timeout."""
import argparse
import datetime as dt
from pathlib import Path
import sys
import time

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM7")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=90)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    try:
        import serial
    except ImportError:
        parser.error("pyserial is required; run this with the ESP-IDF Python environment")
    output = args.out or Path("results") / ("uart_" + dt.datetime.now().strftime("%Y%m%d_%H%M%S") + ".log")
    output.parent.mkdir(parents=True, exist_ok=True)
    connection = serial.Serial(port=None, baudrate=args.baud, timeout=1)
    connection.dtr = False
    connection.rts = False
    connection.port = args.port
    try:
        connection.open()
        with output.open("xb") as file:
            print(f"Recording {args.port} to {output}. Press EN once now. Close other monitors first.", flush=True)
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                data = connection.readline()
                if not data:
                    continue
                file.write(data)
                file.flush()
                print(data.decode("utf-8", errors="replace"), end="", flush=True)
                if b"SUITE_PASS," in data:
                    return 0
                if b"SUITE_FAIL," in data:
                    return 2
            print("Capture timed out without a complete suite; log retained.", file=sys.stderr)
            return 2
    except (serial.SerialException, OSError) as error:
        print(f"Capture failed: {error}. Log files are never overwritten.", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\nCapture stopped; partial log retained.")
        return 2
    finally:
        connection.close()

if __name__ == "__main__":
    raise SystemExit(main())
