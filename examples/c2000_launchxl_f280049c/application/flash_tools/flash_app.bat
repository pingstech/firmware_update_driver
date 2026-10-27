@echo off
rem Writes the APPLICATION to flash. Only the required sectors are erased, the BOOTLOADER stays in place.
rem The bootloader must have been written with flash_boot.bat first. No debug session may be OPEN in CCS.
rem
rem Environment overrides (set before running; keep the quotes shown in the defaults):
rem   DSLITE : TI DSLite command line tool
rem   CCXML  : target configuration file of YOUR debug probe (not part of this repository; create it in CCS)
rem   OUT    : application .out file (default: the CPU1_FLASH build output of a CCS project rooted in ..\ )
if not defined DSLITE set DSLITE="C:\ti\ccs1281\ccs\ccs_base\DebugServer\bin\DSLite.exe"
if not defined CCXML set CCXML="%~dp0..\targetConfigs\TMS320F280049C_LaunchPad.ccxml"
if not defined OUT set OUT="%~dp0..\CPU1_FLASH\fw_update_application.out"
%DSLITE% flash --config=%CCXML% -s FlashEraseSelection=1 -f -v -r 1 -u -e %OUT%
echo.
echo Done. If there was no error, LED4 should blink at about 1 Hz.
pause
