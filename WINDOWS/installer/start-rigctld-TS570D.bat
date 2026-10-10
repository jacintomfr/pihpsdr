@echo off
rem Start Hamlib rigctld for the Kenwood TS-570D (Hamlib model 2004) at 57600 baud.
rem piHPSDR-IF connects to it at 127.0.0.1:4532 (Radio menu, IF panadapter).
rem Usage: start-rigctld-TS570D.bat [COMx]   (asks for the port if none given)
rem The TS-570D must be set to 57600 baud as well (menu 35 = "576-1", then power-cycle the radio).
setlocal
set PORT=%1
if "%PORT%"=="" set /p PORT=COM port of the TS-570D CAT cable (e.g. COM3): 
if "%PORT%"=="" goto :eof
echo Starting rigctld -m 2004 -r %PORT% -s 57600  (close this window to stop it)
"%~dp0rigctld.exe" -m 2004 -r %PORT% -s 57600
echo rigctld ended (exit code %ERRORLEVEL%).
pause
