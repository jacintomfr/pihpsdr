#!/bin/sh
# TS-570D CAT bridge: TCP 127.0.0.1:7373 <-> FTDI USB-RS232 (57600 8N1)
while true; do
  DEV=$(ls /dev/serial/by-id/*FTDI* 2>/dev/null | head -1)
  if [ -n "$DEV" ]; then
    stty -F "$DEV" 57600 raw -echo 2>/dev/null
    socat TCP-LISTEN:7373,bind=127.0.0.1,reuseaddr,fork FILE:"$DEV",b57600,raw,echo=0
  fi
  sleep 2
done
