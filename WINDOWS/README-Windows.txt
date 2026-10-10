piHPSDR-IF for Windows
======================

piHPSDR-IF is piHPSDR built as a receive-only IF panadapter for a Kenwood
TS-570D: an ADALM-Pluto (via SoapySDR) receives the TS-570D 1st IF, and the
Kenwood's frequency is followed through Hamlib rigctld (CAT over TCP).

Start Menu entries (folder "piHPSDR-IF")
----------------------------------------
  piHPSDR-IF                          the program (also on the desktop)
  Start rigctld (TS-570D)             starts rigctld for the TS-570D; asks
                                      for the COM port
  piHPSDR-IF settings and logs folder %LOCALAPPDATA%\piHPSDR-IF
                                      (props files, pihpsdr.stdout/.stderr)

Order of starting
-----------------
1. Connect the TS-570D CAT cable (USB-serial adapter) and note its COM port
   (Device Manager -> Ports (COM & LPT), e.g. COM3).
   The TS-570D must be set to 57600 baud: menu 35 = "576-1", then switch
   the radio off and on again.
2. Start "Start rigctld (TS-570D)" and type the COM port, or from a
   command prompt in the program's bin folder:

     rigctld -m 2004 -r COM3 -s 57600

   (2004 = Hamlib model of the TS-570D. For COM10 and above use
   \\.\COM10.) Keep that window open while piHPSDR-IF runs; close it to
   stop rigctld.
3. Start piHPSDR-IF. It expects rigctld at 127.0.0.1, port 4532 (rigctld's
   default; props entries ifpan.rigctld_host / ifpan.rigctld_port in the
   radio's props file in the settings folder).

PlutoSDR
--------
* Network Pluto (USB "Ethernet" or a real network): piHPSDR-IF opens it
  with the libiio URI ip:<address> (default Pluto address 192.168.2.1).
  If the Pluto is connected by USB, Windows needs the Analog Devices
  PlutoSDR drivers (RNDIS network + USB) - "PlutoSDR-M2k-USB-Drivers"
  from Analog Devices (github.com/analogdevicesinc/plutosdr-m2k-drivers-win).
* Only ONE program may use the Pluto at a time.
* Diagnostics (command prompt in the bin folder):
     SoapySDRUtil --info                 lists the modules ("plutosdr")
     SoapySDRUtil --find="driver=plutosdr"
     iio_info -u ip:192.168.2.1

Audio
-----
Audio goes through PortAudio. Every Windows sound device is listed once per
Windows audio system, e.g. "MME: Speakers (...)", "Windows WASAPI: ...".
"MME" or "Windows DirectSound" devices work with any device sample rate;
"Windows WASAPI" devices only appear if the device is set to 48000 Hz in the
Windows sound settings.

Uninstall
---------
Settings -> Apps -> piHPSDR-IF. The settings folder
%LOCALAPPDATA%\piHPSDR-IF is kept; delete it by hand if no longer needed.
