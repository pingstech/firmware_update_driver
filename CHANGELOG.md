# Changelog

Format: grouped by release; this repository was assembled from an existing private development tree whose earlier history is not reproduced
here. Entries below are limited to changes whose effect was verified by the host tests (`bash tests/run_tests.sh`) or by the checks named in
`docs/TEST_RESULTS.md`.

## [Unreleased] - 2026-10-26: first public repository layout

### Added
- Repository structure: portable core in `src/`, platform ports in `ports/`, reference integrations in `examples/`, host tools in `tools/`, tests in
  `tests/`, documentation in `docs/`.
- Documentation: architecture (with Mermaid diagrams), integration guide, porting guide, API reference per module group, testing guide,
  recorded test results with evidence files, troubleshooting, review findings, translated known-issues table and design-decision log.
- `tests/target_builds/arm_gcc_compile_check.sh` and `c2000_compile_check.sh`: compile-only checks with the vendor toolchains.
- `tools/fwu_package.py` (manifest/frame tool), `gen_vectors.c` and `selftest_vectors.py`; the vectors are reproduced from the C code.
- Evidence: sanitised STM32 update session log, verbatim excerpt of a C2000 pull-mode update log, outputs of the host suite and of the two compile checks.
- MIT `LICENSE` and `THIRD_PARTY_NOTICES.md`; `SECURITY.md`; `CONTRIBUTING.md`.
- C2000 example: `HEX2000`, `DSLITE`, `CCXML`, `OUT` can be set from the environment in the helper scripts.

### Changed
- All source comments of the core, ports and examples (except test programs) were translated to English; an equivalence checker confirmed that no
  code token, macro value or string literal changed (only comments and the text of `#error` messages and a few test-stub trace strings).
- The text of every `#error` message and the output of `src/config/check_config.sh` are English (the script's expected texts were updated together).
- Test programs and `tests/run_tests.sh` print `PASS` / `FAIL` and a `Result:` line instead of Turkish markers; `run_tests.sh` also prints the number of
  checks per module. Individual check descriptions inside the test programs were not translated.
- `tests/run_tests.sh` was relocated for the new layout; its results were compared with the pre-move run (same pass/fail set; the coding policy check
  now passes because a PC-only host test of the old tree is not included).
- The STM32 flash port header now states the maintainer-reported hardware use instead of "not tested on hardware"; stale paths in comments were corrected.
- Example Wi-Fi settings in `examples/c2000_launchxl_f280049c/application/main.c` are placeholders; the FC41D driver is included by plain header name.

### Fixed (pull mode, `src/pull/`; each has a host test that fails on the old behaviour)
- A pull state `DONE` left over from a failed session no longer blocks the next session's `PULL_START` (`test_new_session_after_done`).
- After an input-ring overflow all later bytes of that response are rejected until the range is requested again, so shifted data cannot reach staging
  (`test_hole_rejects_following_bytes`).
- A response is buffered completely and its length verified before anything is written to staging; a byte lost in transit can no longer corrupt
  the staged image (`test_silent_hole_never_reaches_flash`). `chunk_buffer_length` must now be at least `range_bytes`.
- A stale or duplicate response (another range) is skipped without consuming the retry budget (`test_stale_response_is_skipped`).
- The example's pull range size is 4,096 octets and the range buffer lives in `RAMGS2` (`ramgs2` section in the linker file).

### Removed / not included
- Vendor SDK and HAL files, IDE project files, firmware binaries, the FC41D module driver, the product-specific JSON/TCP integration, and the PC GUI tool.
- An earlier C28x integration tree that was never run on hardware and whose host test violated the coding policy; the hardware-exercised C2000 path
  is `ports/c2000/f28004x` plus the example.
