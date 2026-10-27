@echo off
rem Builds the raw image (app_test.bin) that the host tool will send, from the application .out file.
rem hex2000 counts BYTE addresses: word address 0x083000 -> byte address 0x106000 (x2).
rem The length is NOT hard-coded: app_bin.hexcfg produces an image LARGER than the capacity (0x20000), finalize_image.ps1 trims the trailing 0xFF padding
rem and STOPS the build if the application exceeds the capacity (0x12000 = 9 sectors). (Previously a fixed 0xE040 was used; when the application grew the image
rem was silently truncated, .cinit and the ramfunc tail were missing and the board did not start after the update.)
rem Location of hex2000: override by setting the HEX2000 environment variable before running this script.
if not defined HEX2000 set "HEX2000=C:\ti\ccs1281\ccs\tools\compiler\ti-cgt-c2000_22.6.3.LTS\bin\hex2000.exe"
cd /d "%~dp0"
del /q "app_test.bin" 2>nul
copy /y "..\CPU1_FLASH\fw_update_application.out" "fw_update_application.out" >nul
"%HEX2000%" "app_bin.hexcfg"
del /q "fw_update_application.out"
powershell -NoProfile -ExecutionPolicy Bypass -File "finalize_image.ps1"
if errorlevel 1 (
    echo IMAGE NOT PRODUCED.
    exit /b 1
)
