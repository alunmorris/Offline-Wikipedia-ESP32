#!/usr/bin/env python3
"""Read ESP32 serial output. Ctrl-C to stop."""
import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

s = serial.Serial(port, baud, timeout=0.1)
print(f"Listening on {port} at {baud} baud — Ctrl-C to stop\n", flush=True)
try:
    while True:
        d = s.read(512)
        if d:
            print(d.decode("utf-8", "replace"), end="", flush=True)
        else:
            time.sleep(0.05)
except KeyboardInterrupt:
    pass
finally:
    s.close()
