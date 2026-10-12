' piHPSDR-IF: TS-570D CAT bridge at logon (hidden).
' 1) keeps the WSL distro Ubuntu-24.04 running, whose systemd service
'    ts570-cat runs socat TCP 127.0.0.1:7373 <-> /dev/ttyUSB0;
' 2) attaches the FTDI USB-RS232 adapter (0403:6001) to WSL and re-attaches
'    it whenever it is replugged.
Set sh = CreateObject("WScript.Shell")
sh.Run "wsl.exe -d Ubuntu-24.04 -e sleep infinity", 0, False
sh.Run "usbipd attach --wsl --hardware-id 0403:6001 --auto-attach", 0, False
