#!/usr/bin/env python3
"""Write key-down evdev events (no key-up, so the keys stay held) to a file."""
import struct
import sys

with open(sys.argv[1], "wb") as f:
    for code in sys.argv[2:]:
        # struct input_event on 64-bit Linux: timeval (2 x long), type, code, value
        f.write(struct.pack("qqHHi", 0, 0, 1, int(code), 1))
