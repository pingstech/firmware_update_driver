# Testing and validation

How to reproduce every test whose result is recorded in [`TEST_RESULTS.md`](TEST_RESULTS.md), and what each test does and does not prove.

## 1. Categories

| Category | Applies | How | Performed for this repository |
|---|---|---|---|
| Unit / module tests on the PC | yes | `tests/run_tests.sh` | yes (REPRODUCED) |
| Static checks | yes | `gcc -fanalyzer` and a policy `grep`, both inside `tests/run_tests.sh`; strict warnings with `-Werror` | yes |
| Configuration rule checks | yes | `src/config/check_config.sh` (run by `run_tests.sh`) | yes |
| Concurrency stress test | yes | `src/ring_buffer/stress_ringbuf.c` (needs POSIX threads) | yes |
| Host tool self-test | yes | `python tools/fwu_package.py selftest` | yes |
| Build / compile checks with the vendor toolchains | yes | `tests/target_builds/*.sh` | yes (compile only, not linked) |
| MCU-specific integration tests | partly | not automated; procedure in [`PORTING_GUIDE.md`](PORTING_GUIDE.md) section 7 | no |
| On-target hardware tests | yes | not reproducible from this repository (need your hardware and host tool) | reports exist, see `TEST_RESULTS.md` |
| Update success and failure cases | on the PC: yes (simulated flash/crypto, loss-injecting link simulator, power-loss injection); on hardware: only the recorded runs | `tests/run_tests.sh` | PC yes; hardware as recorded |
| Regression | yes | re-run `tests/run_tests.sh` after every change | yes |

## 2. Prerequisites

* `bash` (Git Bash or MSYS2 on Windows; any POSIX shell environment on Linux/macOS).
* `gcc` with C99 support and `-fanalyzer` (GCC 10 or newer provides the analyzer; **only GCC 15.2.0 from MSYS2 was used here**, other versions
  are UNVERIFIED). The strict warning set is in `tests/run_tests.sh`; a newer or older GCC may warn differently.
* `python3` for `tools/fwu_package.py` (standard library only).
* For the target compile checks: the vendor toolchains and SDK files named in the scripts' headers. They are not part of this repository.
* Paths containing spaces are a problem for some scripts (`tests/target_builds/*.sh`); work from a space-free copy of the repository.

## 3. Host test suite

```bash
bash tests/run_tests.sh
```

What it does, per module listed in the `MODULES` array (name | directory | sources | files for static analysis | extra flags):

1. builds the module's test program four times: {normal, `-DC2000`} x {`-O0`, `-O2`} with
   `-std=c99 -pedantic-errors -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef -Wcast-qual -Wcast-align
   -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wredundant-decls -Wold-style-definition -Wnull-dereference -Wvla
   -Wformat=2 -Wswitch-default -Wswitch-enum -Wdouble-promotion -Wfloat-equal -Werror`;
2. runs each program (a program exits non-zero if any check fails);
3. runs `gcc -fanalyzer -O1` on the module's sources (a warning is a failure);
4. runs `src/config/check_config.sh` (valid profile/layout combinations must compile with the expected values, forbidden ones must be
   rejected with the documented text);
5. runs the coding policy check (`fwu_ringbuf_init_raw` must only appear in the ring buffer module and its test).

Output markers: `PASS`, `FAIL`, `BUILD-FAILED`, `SKIPPED`, `WARNING`, `VIOLATION`; each passing line also shows the number of checks. The last line is
`ALL MODULES PASSED` or `FAILURES`, and the exit code is 0 only if everything passed. The `-DC2000` emulation makes `fwu_byte_t` a
16-bit container on the PC (it does **not** make `int` 16 bits). The STM32 port test is skipped in `-DC2000` mode because it is ST-specific.

Interpreting a failure: `BUILD-FAILED` shows the compiler output (a warning counts as an error here); `FAIL` lists up to five failing
`[FAIL]` lines of the test program with `got`/`expected` values; `WARNING` (analyzer) shows the analyzer message; `VIOLATION` names the file that uses
`fwu_ringbuf_init_raw`. The individual test descriptions are still partly in Turkish; the identifiers in each line (function names, status codes) are
English. To run a single module by hand, take its entry from the `MODULES` array of `tests/run_tests.sh` and compile exactly its source list with the script's `WARN` and `INC` flag sets from inside the module directory (the include flags are relative to `src/<module>`), then run the resulting program from that directory. Some entries need extra flags, for example the pull test is built with `-DFWU_PULL_INPUT_SIZE=256u`.

### What the host tests cover

The core is driven against a **simulated flash** (`fwu_flash_sim`, which enforces erase-before-write, alignment, and can cut power at any
operation), **simulated crypto** (real SHA-256 plus an insecure fake signature), and a **link simulator** (loss, duplication, corruption,
truncation, delay, garbage). Highlights stated in the tests' own headers: activation and journal are checked at every power-loss point and
every pair of points; the session is checked in every state against every message; the pull module is checked against a scripted HTTP
server (404, wrong sizes, truncated bodies, duplicates, link drops, overflow, stale responses); a 20,000,000-byte two-thread run exercises the
ring buffer.

### What they do not cover

Real flash timing, cell behaviour after a cut erase/write, ECC, interrupts, the vendor flash APIs themselves (only stubs), real
transports (UART, BLE, TCP, CAN), watchdog interaction, execution time and the 16-bit-`int` behaviour of the C28x. Those need the target
compile checks (partly) and hardware tests.

## 4. Ring buffer stress test

```bash
cd src/ring_buffer
gcc -O2 -pthread -std=gnu99 -I. -I.. -I../error fwu_ringbuf.c stress_ringbuf.c -o stress_ringbuf && ./stress_ringbuf
```

Expected: `Result: STRESS TEST PASSED` with 20,000,000 bytes and 0 data errors.

## 5. Host tool self-test

```bash
python tools/fwu_package.py selftest
```

Compares the frame encoder/decoder and the manifest builder with byte vectors in `tools/selftest_vectors.py`. Those vectors are generated
by the C code: to regenerate and compare, compile `tools/gen_vectors.c` with `src/frame/fwu_frame.c`, `src/session/fwu_proto.c`,
`src/crypto/fwu_crypto_sim.c`, `src/crypto/fwu_crypto.c` and the include directories `src`, `src/frame`, `src/session`, `src/bytes`, `src/crc`,
`src/error`, `src/crypto`, run it and diff its output against `tools/selftest_vectors.py`.

## 6. Target compile checks

These prove only that the sources compile with the vendor compiler against the vendor headers; they do not link or run anything.

```bash
# STM32G0 port + core + bootloader example (GNU Arm)
ARM_GCC_BIN=<dir with arm-none-eabi-gcc> STM32_PROJECT=<STM32CubeIDE project with Core/Inc and Drivers/> \
  bash tests/target_builds/arm_gcc_compile_check.sh

# C2000 ports + example (TI compiler)
TI_CGT=<ti-cgt-c2000 dir> C2000WARE=<C2000Ware dir> DEVICE_DIR=<TI 'device' folder> SYSCFG_DIR=<SysConfig output dir> \
FC41D_DIR=<FC41D driver root> FC41D_PARENT=<its parent for the driver's own includes> \
  bash tests/target_builds/c2000_compile_check.sh
```

The scripts list their required variables at the top. The C2000 application example also needs the FC41D module driver headers; without them
only the bootloader and the core can be checked (comment out the `app` line in the script).

## 7. On-target tests

The recorded hardware runs and what is missing are in [`TEST_RESULTS.md`](TEST_RESULTS.md). To repeat or extend them you need a host
that speaks the frame protocol (the C client in `src/client` implements it; `tools/fwu_package.py` builds manifests and decodes frames) and the
target setup in the example READMEs. The minimum set a new target should pass is in [`PORTING_GUIDE.md`](PORTING_GUIDE.md) section 7. Record
the tool versions, the procedure and the raw logs with the result; a result without them is only history.

## 8. Adding tests

See [`CONTRIBUTING.md`](../CONTRIBUTING.md). Every change to behaviour needs a test that fails without the change; register new test programs in
`tests/run_tests.sh`.
