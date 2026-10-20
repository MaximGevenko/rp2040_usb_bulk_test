RP2040 raw USB Bulk-IN throughput test
======================================

Purpose
-------
This project bypasses TinyUSB and drives the RP2040 USB device controller
at register level. It exposes one vendor-specific Bulk-IN endpoint (0x81)
and continuously sends 64-byte packets containing a repeating 0..255 pattern.

Build
-----
Place pico_sdk_import.cmake next to CMakeLists.txt, or otherwise use your
normal Pico SDK project setup / PICO_SDK_PATH.

mkdir build
cd build
cmake ..
cmake --build .

Flash rp2040_usb_bulk_test.uf2 using your normal RP2040 flashing method.
Set the board USB mux switch to the RP2040 side.

Host / WSL
----------
The test device uses VID:PID cafe:4002 and a vendor-specific interface.
For WSL 2, attach the device with usbipd from Windows, then run PyUSB in WSL.

Windows PowerShell (Administrator once):
  usbipd list
  usbipd bind --busid <BUSID>

Windows PowerShell (normal, while WSL is running):
  usbipd attach --wsl --busid <BUSID>

WSL:
  lsusb
  sudo apt update
  sudo apt install python3-usb
  sudo python3 speed_test.py

Expected output:
  900.0 kB/s    7.20 Mbit/s
  ...

Notes
-----
- No TinyUSB and no CDC are involved.
- The endpoint is single-buffered in this first benchmark implementation.
- If this already gives enough margin over ~600 kB/s, it is sufficient to
  prove the board's RP2040 -> FSUSB42 -> USB-C path can carry the CIR stream.
- A later version can add USB endpoint double buffering if more throughput is
  required.
