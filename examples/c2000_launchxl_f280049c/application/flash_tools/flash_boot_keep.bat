@echo off
rem Writes the BOOTLOADER to flash; erases ONLY the bootloader sectors (0..2). The application, staging and journal (commit) STAY in place.
rem Use it to update the bootloader without disturbing a pending update. To erase all of flash use flash_boot.bat.
rem No debug session may be OPEN in CCS (if the XDS110 is in use by another program, the connection fails).
rem
rem Environment overrides (set before running; keep the quotes shown in the defaults):
rem   DSLITE : TI DSLite command line tool
rem   CCXML  : target configuration file of YOUR debug probe (not part of this repository; create it in CCS)
rem   OUT    : bootloader .out file (default: the CPU1_FLASH build output of a CCS project rooted in ..\..\bootloader)
if not defined DSLITE set DSLITE="C:\ti\ccs1281\ccs\ccs_base\DebugServer\bin\DSLite.exe"
if not defined CCXML set CCXML="%~dp0..\targetConfigs\TMS320F280049C_LaunchPad.ccxml"
if not defined OUT set OUT="%~dp0..\..\bootloader\CPU1_FLASH\fw_update_bootloader.out"
%DSLITE% flash --config=%CCXML% -s FlashEraseSelection=1 -f -v -r 1 -u -e %OUT%
echo.
echo Done. If an update is pending the bootloader copies it (a few seconds), then the application runs.
pause
