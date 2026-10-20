#!/usr/bin/env python3
import time
import usb.core
import usb.util

VID = 0xCAFE
PID = 0x4002
EP_IN = 0x81
READ_SIZE = 64 * 256      # Ask libusb for 16 KiB per read
TIMEOUT_MS = 1000


def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        raise SystemExit(f"Device {VID:04x}:{PID:04x} not found")

    dev.set_configuration()

    total = 0
    interval = 0
    start = time.perf_counter()
    last = start

    print(f"Reading bulk IN endpoint 0x{EP_IN:02x} ...")

    try:
        while True:
            data = dev.read(EP_IN, READ_SIZE, timeout=TIMEOUT_MS)
            n = len(data)
            total += n
            interval += n

            now = time.perf_counter()
            if now - last >= 1.0:
                dt = now - last
                bps = interval / dt
                print(f"{bps / 1000:8.1f} kB/s   {bps * 8 / 1_000_000:6.2f} Mbit/s")
                interval = 0
                last = now

    except KeyboardInterrupt:
        elapsed = time.perf_counter() - start
        if elapsed > 0:
            avg = total / elapsed
            print(f"\nAverage: {avg / 1000:.1f} kB/s ({avg * 8 / 1_000_000:.2f} Mbit/s)")


if __name__ == "__main__":
    main()
