#!/bin/sh
# TS-570D CAT bridge: TCP 127.0.0.1:7373 <-> FTDI USB-RS232 (57600 8N1)
#
# Only the CAT adapter, by its serial number (FTCSEGX1). Never a pattern
# like *FTDI*: the FT232R adapter (A50285BI) that keys PTT on ACC2 pin 9 is
# also FTDI, and opening it raises RTS = the TS-570 goes into TX
# (2026-10-14: the Pi booted with both plugged in, the pattern picked the PTT
# adapter, the radio stayed in TX).
#
DEV=/dev/serial/by-id/usb-FTDI_usb_serial_converter_FTCSEGX1-if00-port0
while true; do
  if [ -e "$DEV" ]; then
    stty -F "$DEV" 57600 raw -echo 2>/dev/null
    socat TCP-LISTEN:7373,bind=127.0.0.1,reuseaddr,fork FILE:"$DEV",b57600,raw,echo=0
  fi
  sleep 2
done
