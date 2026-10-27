# Test results

A factual record. Every row is classified by the strength of its evidence:

* **REPRODUCED** - run while this repository was prepared (2026-10-26); the command and its output are stored under
  [`evidence/`](evidence/) and you can repeat it ([`TESTING.md`](TESTING.md)).
* **RECORDED** - a log of a hardware run is stored in this repository; the log was produced by the maintainer's host tool (not part of this
  repository), so the run cannot be repeated from here, but the record can be inspected.
* **MAINTAINER-REPORTED** - the maintainer states the test was done, but no log, photo or measurement is stored here. This is history,
  not independently reproducible evidence.
* **NOT TESTED** - no evidence of any kind.

Status values: PASS, FAIL, PARTIAL, NOT TESTED. Nothing on this page was invented: where a detail is unknown it says so.

## 1. Summary

| ID | What | Target / environment | Status | Evidence class |
|---|---|---|---|---|
| H1 | Host test suite: 34 module test programs, 4 build variants each | PC, MSYS2 gcc 15.2.0 | PASS | REPRODUCED |
| H2 | Protection profile / flash layout rule checks | PC | PASS (70 combinations) | REPRODUCED |
| H3 | `gcc -fanalyzer` over the module sources | PC | PASS (no warnings) | REPRODUCED |
| H4 | Coding policy check (`fwu_ringbuf_init_raw`) | PC | PASS | REPRODUCED |
| H5 | Ring buffer two-thread stress test (20,000,000 bytes) | PC | PASS (0 data errors) | REPRODUCED (run, output not stored) |
| H6 | `tools/fwu_package.py` self-test and C-generated vectors | PC, Python 3.13 | PASS | REPRODUCED |
| B1 | Compile STM32G0 port, core pieces and bootloader example | GNU Arm 13.3.1, real ST HAL headers | PASS, 0 warnings | REPRODUCED |
| B2 | Compile C2000 ports and example (application + bootloader) | TI C2000 compiler 22.6.3 | PASS (7 warnings, all unused statics in `main.c`) | REPRODUCED |
| D1 | Firmware update, pull mode (BLE control + HTTP over FC41D), 67,936 B | LAUNCHXL-F280049C (TMS320F280049C) | PASS | RECORDED (excerpt) |
| D2 | Firmware update over BLE (push) | LAUNCHXL-F280049C | PASS | MAINTAINER-REPORTED |
| D3 | Firmware update over UART (push) | LAUNCHXL-F280049C | PASS | MAINTAINER-REPORTED |
| D4 | Transfer interruption / link-drop tests on D1-D3 | LAUNCHXL-F280049C | PASS | MAINTAINER-REPORTED |
| D5 | Firmware update over TCP (JSON/base64 framing), 120,960 B, new version confirmed after reboot | STM32G0B1-based device | PASS | RECORDED (sanitised full session log) |
| D6 | Power-loss tests on the STM32G0B1 device | STM32G0B1-based device | PARTIAL: reported, no detail | MAINTAINER-REPORTED |
| D7 | Power loss during the bootloader copy | LAUNCHXL-F280049C | NOT TESTED | - |
| D8 | SWAP + ROLLBACK activation on hardware | any | NOT TESTED | - |
| D9 | Signed profiles (Ed25519) | any | NOT TESTED; not possible with the shipped crypto port | - |
| D10 | CAN / ISO-TP on hardware | any | NOT TESTED (host-only tests) | - |
| D11 | Flash ECC / NMI behaviour of the STM32 port, flash erase duration vs watchdog | STM32G0B1 | NOT TESTED | - |

## 2. Environment of the REPRODUCED records

| Item | Value |
|---|---|
| Date | 2026-10-26 |
| Host OS | Windows 11 Pro (build 10.0.26200) |
| Shell and compiler for H1-H5 | MSYS2 `bash`; `gcc.exe (Rev8, Built by MSYS2 project) 15.2.0` |
| Python for H6 | Python 3.13 (Windows Store build) |
| GNU Arm toolchain for B1 | `arm-none-eabi-gcc (GNU Tools for STM32 13.3.rel1.20250523-0900) 13.3.1 20240614`, taken from an STM32CubeIDE 2.0.0 installation |
| ST headers for B1 | the HAL/CMSIS files of the maintainer's STM32CubeIDE project (STM32G0xx HAL driver, CMSIS); the exact STM32CubeG0 firmware package version was **not recorded** |
| TI compiler for B2 | `cl2000` revision `22.6.3` (`ti-cgt-c2000_22.6.3.LTS`) from a CCS installation in directory `ccs1281` (the project metadata names CCS 12.8.0), C2000Ware 26.01.00.00 (`C2000ware` directory `C2000Ware_26_01_00_00`) |
| Other TI inputs for B2 | the maintainer's CCS project supplied `device.h`/`device.c` and the SysConfig-generated `board.h`; the FC41D module driver headers came from the maintainer's separate driver tree |

## 3. Detailed records

### H1 - Host test suite

| Field | Value |
|---|---|
| Target | PC (the tests run against a simulated flash and simulated crypto; no MCU) |
| Hardware | not applicable |
| Software environment | see section 2; `tests/run_tests.sh` compiles each module with `-std=c99 -pedantic-errors -Wall -Wextra -Wpedantic -Wconversion ... -Werror` |
| Test case | one self-checking test program per module (34 programs, listed in `tests/run_tests.sh`), each built as {normal, `-DC2000`} x {`-O0`, `-O2`}; `-DC2000` emulates the 16-bit `char` of the C28x. The STM32 port test is built only in the normal mode (reported as SKIPPED for `-DC2000`, by design) |
| Procedure | `bash tests/run_tests.sh` |
| Expected result | every program compiles without warnings and exits 0; the script's last line is `ALL MODULES PASSED` |
| Actual result | `ALL MODULES PASSED`, exit code 0. Checks counted at the normal `-O0` variant: **3,586** (per module: crc 9, bytes 23, error 13, ring_buffer 112, flash 75, flash_sim 69, frame 56, frame_decode 62, frame_parser 82, journal 79, journal_ab 93, activate 329, manifest 69, crypto 78, sha256 50, crypto_sim 37, flash_stm32g0 128, flash_f28004x 91, verify 234, session_proto 160, session 378, pull_http 101, pull 176, counters 154, linksim 53, isotp 185, link 89, client 213, auth 164, boot 184, four boot_profile programs 10 each) |
| Status | PASS |
| Evidence | [`evidence/host_test_run_2026-10-26.txt`](evidence/host_test_run_2026-10-26.txt) |
| Limitations | The tests exercise the logic against **simulations**; they cannot show real flash behaviour, timing, ECC, interrupt interplay or transport behaviour. They run on a 32-bit-`int` PC; `int`-width problems on a 16-bit-`int` target are only caught by compiling with the target compiler (B2) and on the device. Test descriptions inside the test programs are still partly in Turkish (the PASS/FAIL markers and the summary are English). MISRA conformance was **not** checked with a MISRA tool. |

### H2, H3, H4 - Configuration rules, static analysis, policy

| Field | Value |
|---|---|
| Procedure | part of `bash tests/run_tests.sh`: `src/config/check_config.sh`; `gcc -fanalyzer -O1 -std=c99` over each module's source files; a `grep`-based policy check |
| Expected / actual | valid profile+layout combinations compile with the expected values and forbidden ones are rejected with the documented `#error` text: 70 combinations PASS. `-fanalyzer`: 29 module-source checks, no warnings. Policy: `fwu_ringbuf_init_raw` only in the ring buffer module and its test: PASS |
| Status / evidence | PASS / [`evidence/host_test_run_2026-10-26.txt`](evidence/host_test_run_2026-10-26.txt) |
| Limitations | `-fanalyzer` of GCC 15.2.0 only; other analysers were not run |

### H5 - Ring buffer stress test

| Field | Value |
|---|---|
| Procedure | `gcc -O2 -pthread -std=gnu99 -I. -I.. -I../error fwu_ringbuf.c stress_ringbuf.c` in `src/ring_buffer`, then run (a producer thread and a consumer thread, 20,000,000 bytes) |
| Actual result | consumer received 20,000,000 / 20,000,000 bytes, 0 data errors, final line `Result: STRESS TEST PASSED` |
| Status | PASS |
| Limitations | PC threads are not an ISR on a single-core MCU; the guarantee documented in `fwu_ringbuf.h` is limited to single-core MCUs without weak memory ordering. The terminal output was not stored |

### H6 - Host tool self-test

| Field | Value |
|---|---|
| Procedure | `python tools/fwu_package.py selftest`; additionally `gen_vectors.c` was compiled against the C encoder and its output compared with `tools/selftest_vectors.py` |
| Actual result | all checks PASS; the stored vectors are byte-identical to the output of the C code |
| Status | PASS |
| Limitations | covers frame encoding/decoding and manifest building only |

### B1 - STM32 compile check (GNU Arm)

| Field | Value |
|---|---|
| Target | STM32G0B1 (Cortex-M0+), compile only |
| Software environment | section 2; flags as in the maintainer's STM32CubeIDE project: `-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft -std=gnu11 -Os -ffunction-sections -fdata-sections -Wall -Wextra -DUSE_HAL_DRIVER -DSTM32G0B1xx` |
| Procedure | `tests/target_builds/arm_gcc_compile_check.sh` (13 files: core pieces used by the bootloader, `ports/stm32g0/flash/fwu_flash_stm32g0.c`, `examples/stm32g0b1/bootloader/boot_app.c`) |
| Actual result | 13 of 13 compiled, 0 warnings. Object sizes (not a linked image): text 8,048 B total, bss 1,350 B total (the bss is in `boot_app.o`) |
| Status | PASS |
| Evidence | [`evidence/arm_gcc_compile_check.txt`](evidence/arm_gcc_compile_check.txt) |
| Limitations | not linked, not run; sizes are the sum of object files at `-Os`, not a final map; the application-side STM32 integration is not part of the repository so it was not compiled |

### B2 - C2000 compile check (TI compiler)

| Field | Value |
|---|---|
| Target | TMS320F280049C, compile only |
| Software environment | section 2; options of the maintainer's CCS configuration `CPU1_FLASH`: `-v28 -ml -mt --cla_support=cla2 --float_support=fpu32 --tmu_support=tmu0 --vcu_support=vcu0 --c99 --abi=eabi ...`, application `-Ooff`, bootloader `-O2`, defines `C2000 FWU_BOARD_F280049C FWU_PROFILE_DEV DEBUG _FLASH` |
| Procedure | `tests/target_builds/c2000_compile_check.sh` (43 files: core, both C2000 flash ports, the example port files, `main.c` of application and bootloader) |
| Actual result | 43 of 43 compiled. 7 warnings, all `#179-D ... declared but never referenced` in the application's `main.c` (disabled self-test functions and an unused buffer) |
| Status | PASS |
| Evidence | [`evidence/ti_cl2000_compile_check.txt`](evidence/ti_cl2000_compile_check.txt) |
| Limitations | not linked; no map file produced by this check. The `ver_1` define of the maintainer's project (selects the LED/version used in the original tests) was not used |

### D1 - C2000 pull-mode update (hardware)

| Field | Value |
|---|---|
| Target | TMS320F280049C |
| Hardware | LAUNCHXL-F280049C LaunchPad with an FC41D Wi-Fi/BLE module (module revision and wiring not recorded) |
| Software environment | application built with the options in the example README (CCS 12.8.x / `cl2000` 22.6.3, C2000Ware 26.01.00.00, `-Ooff`); host tool: the maintainer's PC application (not part of this repository) acting as BLE central and as a local HTTP server; Wi-Fi access point of the maintainer's network |
| Test case | update of a 67,936-byte application image in pull mode: `HELLO`, `REQUEST_DOWNLOAD`, `PULL_START`; the device downloads 4,096-byte ranges over HTTP; host polls `PULL_QUERY`; then `TRANSFER_EXIT`, `ACTIVATE` |
| Procedure | maintainer's run on the date shown only as time of day in the log (13:16-13:18); not repeatable from this repository |
| Expected result | `PULL_STATUS` reaches DONE with flash offset = image size; `VERIFY_RESULT` 0; `ACTIVATE_OK` |
| Actual result | as expected: `PULL_STATUS durum=TAMAM flash_ofset=67936`, `VERIFY_RESULT kod=0 (GECERLI)`, `ACTIVATE_OK` (the tool's messages are Turkish: TAMAM = DONE, GECERLI = VALID). The same excerpt shows some ranges requested twice after about 8 s without data |
| Status | PASS |
| Evidence | [`evidence/c2000_f280049c_pull_update_host_log_excerpt.txt`](evidence/c2000_f280049c_pull_update_host_log_excerpt.txt) (verbatim lines, redacted addresses; the polling lines are omitted) |
| Limitations | An **excerpt**: the full log, the post-reboot run of the new application, and the repeated-range stalls' cause are not recorded. The log does not show the new application starting. Earlier pull runs on the same hardware ended with `HASH_MISMATCH` before the range-buffer change in `src/pull/fwu_pull.c`; they are not recorded here. The tool's label "TI F2800157-Q1" in the excerpt is the selected host profile name, not the hardware |

### D2-D4 - C2000 BLE and UART updates, interruption tests

| Field | Value |
|---|---|
| Target / hardware | TMS320F280049C on a LAUNCHXL-F280049C (BLE through the FC41D module) |
| Statement | the maintainer reports successful full updates over BLE and over UART, and that tests with a transfer interrupted by a lost connection were performed with all three transports (D1-D3), all passing |
| Status | PASS, **MAINTAINER-REPORTED** |
| Missing detail | logs, exact procedure, number of repetitions, interruption points, tool versions. The only quantitative statement available, also maintainer-reported: a BLE update of a 57,408-byte image took about 1.13 minutes (about 789 B/s) with no retransmissions. No file in this repository confirms it |

### D5 - STM32G0B1 device update over TCP (hardware)

| Field | Value |
|---|---|
| Target | STM32G0B1-based device; the exact part `STM32G0B1KEU6` (512 KB flash) is taken from the repository's layout header and the maintainer's project, **not from the log** |
| Hardware | a production-type board with an ESP32-class Wi-Fi module as the network path (revision not recorded); the host connected over TCP; the device is identified in the log only by an id that was redacted |
| Software environment | bootloader and application built in the maintainer's STM32CubeIDE projects; the original compiler version is not recorded (the installed CubeIDE 2.0.0 has GNU Arm 13.3.1: **UNVERIFIED** that the original build used it); host tool: the maintainer's PC application with the device vendor's JSON/base64 command framing (not part of this repository) |
| Test case | update of an image of 120,960 bytes (`TRANSFER_EXIT` payload `0001d480`); protocol frames identical to the push protocol, carried base64-encoded in JSON messages |
| Procedure | maintainer's run; not repeatable from this repository |
| Expected result | `VERIFY_RESULT` valid, `ACTIVATE_OK`, device restarts and reports the new running version |
| Actual result | `VERIFY_RESULT kod=0 (GECERLI)`, `ACTIVATE_OK`, then after the restart the device reported `Result: Confirmed, Version: 3` (the previous running version in the same log was 2). A second record in the maintainer's files reports a success with running version 4. Computed from the log timestamps: download phase 69.45 s for 120,960 bytes, about 1.74 KB/s |
| Status | PASS |
| Evidence | [`evidence/stm32g0b1_tcp_update_log.csv`](evidence/stm32g0b1_tcp_update_log.csv) (complete session up to the result line; device id and host address redacted; image payload bytes removed; messages in Turkish) |
| Limitations | The log is a host-side view; it does not contain flash-level data, power events, or the bootloader's journal. It proves the update sequence including the post-reboot version change, not any power-loss property. The tool's message text, timestamps without date, and the first line (`FwuRefused Busy` followed by a successful retry) are part of the log |

### D6 - STM32 power-loss tests

| Field | Value |
|---|---|
| Statement | a maintainer commit message in the product repository (not included here) says the application build was "verified on the device after the power-loss tests" (2026-10-15). This is the only trace |
| Status | PARTIAL / MAINTAINER-REPORTED |
| Missing detail | interruption points, number of cycles, how power was cut, observed journal states, results. Without them this repository does **not** claim power-loss safety on hardware |

### D7-D11 - Not tested

| ID | Not tested | Consequence |
|---|---|---|
| D7 | power loss at several points of the bootloader's copy on the C2000 | host tests inject power loss into the simulated flash at every operation of the activation, journal and session code, but real flash behaviour on this device is unrecorded |
| D8 | SWAP + ROLLBACK with trial boots on hardware; confirm function use | `fwu_boot_app_finish_trial` / `fwu_activate_confirm` are not called by any example |
| D9 | signed profiles | the shipped Ed25519 hook always fails closed |
| D10 | CAN / ISO-TP | implemented and host-tested against itself only; identifiers are placeholders; not checked against ISO 15765-2 |
| D11 | STM32 ECC/NMI path; page erase time vs watchdog; verification time of the largest image on either device | open issues 33, 40, 41, 44, 49 |

## 4. Resource usage (measured where an artifact exists)

| Item | Value | Source and caveat |
|---|---|---|
| STM32 bootloader object code (13 files, `-Os`) | text 8,048 B, bss 1,350 B (sum of object files, not linked) | REPRODUCED, B1 |
| C2000 bootloader, flash | 0x2 + 0x296E words used (`BEGIN` plus `BOOT_FLASH`; `BOOT_FLASH` is 0x2FFE words long); built `-O2` | from the maintainer's linker map dated 2026-10-19; not rebuilt here |
| C2000 application, flash | 0x2 + 0x84A7 words (= 67,922 octets) of 0xA000-word application region; built `-Ooff` | from the maintainer's linker map dated 2026-10-21; not rebuilt here |
| C2000 application image file | 67,936 octets = 92.1 % of the 73,728-octet staging image capacity; margin 5,792 octets | file size of the image used in D1 (the file itself is not in the repository) |
| C2000 application RAM | `.bss` in RAMGS0 0x1CA8 words of 0x2000; driver object for the FC41D 0x774 words in RAMGS1; pull range buffer 0x1000 words in RAMGS2; `.TI.ramfunc` 0xB5C words in RAMLS03 | same map; see the example README section 4 |
| C2000 bootloader RAM | `.bss` 0x3BC + `.data` 0xC words in RAMLS5; `.TI.ramfunc` 0xB2C words in RAMLS03 | map dated 2026-10-19 |
| Execution times (erase, verification, copy) | **not measured** | - |

Throughput (computed from the stored logs, not from a benchmark): D1 about 1.0 KB/s end to end (67,936 B in about 67 s, including
repeated ranges after response timeouts); D5 about 1.74 KB/s (120,960 B in 69.45 s). The maintainer's separate BLE figure is in D2-D4.

## 5. How the history of failures relates to these results

For transparency: before the pull module buffered whole HTTP responses, pull runs on the C2000 hardware ended with `HASH_MISMATCH`, and a
second update attempt after such a failure was refused by a stale `DONE` state. Both defects are fixed in this repository and each has a
host test that fails on the old behaviour (`test_hole_rejects_following_bytes`, `test_silent_hole_never_reaches_flash`,
`test_new_session_after_done`, `test_stale_response_is_skipped` in `src/pull/test_pull.c`). The hardware run D1 was recorded after those
changes. The reason bytes were being lost on that hardware is still **not established** (see [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md),
entry 12).
