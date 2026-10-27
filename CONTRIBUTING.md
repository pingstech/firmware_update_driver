# Contributing

Thank you for considering a contribution. This is an embedded firmware project: correctness on resource-limited MCUs and
honest reporting of what has and has not been tested matter more than feature count.

## Before you start

1. Read `docs/ARCHITECTURE.md` (module boundaries) and `docs/PORTING_GUIDE.md` (what a platform port must provide).
2. Check `docs/KNOWN_ISSUES.md` and `docs/DESIGN_DECISIONS.md`; many questions are already answered there.
3. Open an issue before large changes (new module, protocol change, new target).

## Coding rules (derived from the existing code base)

- C99, no dynamic memory (`malloc`/`free`): callers provide all storage.
- Portable core code (`src/`) must not depend on a specific MCU. Anything device-specific lives behind the flash, crypto,
  transport or hook interfaces and in `ports/` or `examples/`.
- Do not assume `char` is 8 bits, byte order, or `sizeof` of fundamental types. The C2000 `char` is 16 bits; use `fwu_byte_t`
  and mask with `FWU_BYTE_MASK` as the existing code does (`src/fwu_types.h`).
- Do not assume `int` is wider than 16 bits. Use explicit `uint32_t` / `UL` suffixes in arithmetic that can exceed 65535.
- Functions that can fail report it with `fwu_return_e` (see `src/error/fwu_error.h`; pure queries and getters may return other
  types). When you add an error code, update the header,
  `src/error/test_error.c`, and the module that returns it.
- Production code must create ring buffers with `FWU_RINGBUF_DEFINE`; `fwu_ringbuf_init_raw` is reserved for the ring buffer module
  and its test (enforced by `tests/run_tests.sh`).
- Source comments and documentation are written in English.

## Tests

Every module has a self-checking test next to its sources (`test_*.c`). The whole suite is run with:

```bash
bash tests/run_tests.sh
```

It compiles every module in four combinations (`-O0`/`-O2`, with and without the `-DC2000` 16-bit-char emulation) using a strict warning set with
`-Werror`, runs the tests, runs `gcc -fanalyzer` over the module sources, checks the protection-profile combinations
(`src/config/check_config.sh`) and enforces the coding policy above. Requirements: `bash`, `gcc` (MinGW/MSYS2 on Windows is
sufficient). See `docs/TESTING.md` for details and for what the host tests do **not** cover.

A change is not ready for review unless `tests/run_tests.sh` passes. Include the relevant part of its output in the pull request.

### Adding or changing tests

- Put the test file in the module directory and register it in the `MODULES` list of `tests/run_tests.sh` (name | directory |
  sources | files for static analysis | extra flags).
- New behaviour needs a test that fails without the change. For bug fixes, reproduce the bug first.
- The tests print `[PASS]` / `[FAIL]` markers and a final `Result:` line that `tests/run_tests.sh` parses; keep that output format
  unless you update the script at the same time.

## Adding a platform port

Follow `docs/PORTING_GUIDE.md`. In short:

1. Add `ports/<vendor>/<device>/` with the flash port (`fwu_flash_t` callbacks) and, if needed, a board description header in
   `src/config/layout_<board>.h`.
2. Provide a host test with a vendor-API stub (see `ports/stm32g0/flash/hal_stub/`) so the port logic runs in `tests/run_tests.sh`.
3. State in your pull request exactly what was verified: compiled with the vendor toolchain? run on real hardware? which device,
   which tool versions? Do not mark a target as supported on the basis of compilation alone. Add the evidence under
   `docs/evidence/` and update the support matrix in `README.md` and `docs/TEST_RESULTS.md`.

## Commit and pull-request conventions

- One logical change per commit; explain the reason in the message.
- Do not commit build outputs, firmware images, IDE workspace files, credentials, or absolute local paths (see `.gitignore`).
- Never commit Wi-Fi credentials or keys; the examples use placeholders on purpose.

## License

The repository is licensed under the MIT License (`LICENSE`). By submitting a contribution you agree that it is provided under
the same license. Third-party material (vendor SDKs, HALs, tool-generated headers) keeps its own terms; see
`THIRD_PARTY_NOTICES.md` and do not add vendor code to the repository.
