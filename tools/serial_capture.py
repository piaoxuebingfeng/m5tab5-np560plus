#!/usr/bin/env python3
"""Capture M5Stack Tab5 (ESP32-P4) serial log for a fixed number of seconds.

This is the tool used during the NP560+ port bring-up. It must run with the
*system* Python interpreter (/usr/bin/python3), which has pyserial installed;
the managed WorkBuddy Python does not.

Usage:
    /usr/bin/python3 tools/serial_capture.py 90
    /usr/bin/python3 tools/serial_capture.py 25 --no-reset
    /usr/bin/python3 tools/serial_capture.py 60 --port /dev/ttyACM0 \
        > /tmp/boot.log 2>&1

Options:
    seconds        How long to capture (default 60).
    --no-reset     Do not toggle DTR/RTS before capturing (attach to a board
                   that is already running, e.g. idf.py flash just finished).
    --port PATH    Serial device (default /dev/ttyACM0).
    --baud N       Baud rate (default 115200; USB-Serial-JTAG ignores it).
"""
import argparse
import sys
import time

import serial


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("seconds", nargs="?", type=float, default=60.0)
    p.add_argument("--no-reset", action="store_true")
    p.add_argument("--port", default="/dev/ttyACM0")
    p.add_argument("--baud", type=int, default=115200)
    return p.parse_args()


def main():
    args = parse_args()
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as exc:
        print("open %s failed: %s" % (args.port, exc), file=sys.stderr)
        return 2

    # ESP32 USB-Serial-JTAG / UART bridge: a short DTR/RTS pulse resets the
    # chip. Skip this when the board is already in the middle of a boot.
    if not args.no_reset:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.05)
        ser.dtr = True
        ser.rts = False
        time.sleep(0.05)

    start = time.time()
    buf = bytearray()
    while time.time() - start < args.seconds:
        try:
            chunk = ser.read(4096)
        except serial.SerialException:
            break
        if chunk:
            buf.extend(chunk)
            # Flush complete lines immediately so redirected logs are readable
            # while the capture is still running.
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                sys.stdout.write(line.decode("utf-8", "replace") + "\n")
            sys.stdout.flush()
    if buf:
        sys.stdout.write(buf.decode("utf-8", "replace"))
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
