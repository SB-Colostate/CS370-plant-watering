#!/usr/bin/env python3
"""
CS370 Milestone 2 bring-up: prove both sensors are electrically alive.
Pi 3 reads the Uno R3 over USB serial; the Uno handles both sensors
(soil probe on A0, HC-SR04 on D9/D10). See soil_uno.ino.

Setup:  sudo apt install python3-serial
Run:    python3 plantpi.py | tee transcript_sensors.txt
Port:   /dev/ttyACM0 on the Pi (override: python3 plantpi.py COM3 on Windows)
"""
import sys
import time
from datetime import datetime

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"


def parse(line):
    """'SOIL,412,DIST,23.4' -> (412, 23.4); anything else -> None."""
    parts = line.strip().split(",")
    if len(parts) == 4 and parts[0] == "SOIL" and parts[2] == "DIST":
        try:
            return int(parts[1]), float(parts[3])
        except ValueError:
            return None
    return None


def main():
    uno = serial.Serial(PORT, 9600, timeout=3)
    time.sleep(2)              # Uno auto-resets when the port opens
    uno.reset_input_buffer()
    print(f"# plantpi start {datetime.now().isoformat(timespec='seconds')}  port={PORT}")
    try:
        while True:
            raw = uno.readline().decode("ascii", errors="ignore")
            ts = datetime.now().isoformat(timespec="seconds")
            r = parse(raw)
            if r is None:
                print(f"{ts}  NO DATA / bad line: {raw.strip()!r}", flush=True)
                continue
            soil, dist = r
            d = "NO ECHO" if dist < 0 else f"{dist:6.1f} cm"
            print(f"{ts}  soil_raw={soil:4d}  ultrasonic={d}", flush=True)
    except KeyboardInterrupt:
        print(f"# stopped {datetime.now().isoformat(timespec='seconds')}")
    finally:
        uno.close()


if __name__ == "__main__":
    main()
