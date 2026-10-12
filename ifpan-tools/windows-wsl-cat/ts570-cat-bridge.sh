#!/bin/bash
# TS-570D CAT bridge for piHPSDR-IF (stands in for the ESP32 WiFi bridge):
# TCP 7373 (IPv4, so Windows reaches it as 127.0.0.1:7373) <-> /dev/ttyUSB0 at 57600.
# Waits for the USB-RS232 adapter (usbipd attach from Windows).
DEV=/dev/ttyUSB0
until [ -e "$DEV" ]; do sleep 2; done
exec socat TCP4-LISTEN:7373,reuseaddr,fork FILE:$DEV,b57600,raw,echo=0
