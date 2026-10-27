# Troubleshooting

Only failure modes that the source code defines or that appear in the recorded evidence are listed. Each entry says whether a cause is
**confirmed** (shown by code or by a reproducing test) or a **hypothesis** (plausible, not established). Status-code meanings:
[`api/session-and-basics.md`](api/session-and-basics.md) section 2 (all `fwu_return_e` values). Decode captured frames with
`python tools/fwu_package.py decode <hex-or-base64>`.

How to read device-side state in the C2000 example (debugger watch, not part of any API): `g_boot_result` (bootloader),
`app_fwu_status()` / `g_app_pull_init_status` / `g_app_ble_stats` / `g_app_pull_stats` (application), `fwu_link_get_stats()`,
`fwu_pull_get_stats()`.

## Quick index

| Symptom | Entry |
|---|---|
| Build stops with `#error` | [1](#1-build-stops-with-error) |
| No answer to `HELLO` / host timeouts | [2](#2-no-answer-to-hello-or-host-timeouts) |
| `ERROR` frame `BAD_STATE` on `REQUEST_DOWNLOAD` | [3](#3-error-bad_state-on-request_download) |
| `ERROR` frame `BAD_BLOCK` | [4](#4-error-bad_block) |
| `VERIFY_RESULT` is not 0 | [5](#5-verify_result-is-not-0) |
| `VERIFY_RESULT` = `HASH_MISMATCH` (35) | [6](#6-verify_result--hash_mismatch-35) |
| Signed profile always fails (`CRYPTO_FAILURE`) | [7](#7-signed-profile-always-fails) |
| Board does not start after the update / bootloader stays | [8](#8-board-does-not-start-after-the-update-bootloader-stays) |
| Host sees a timeout although the device rebooted | [9](#9-host-timeout-although-the-device-rebooted) |
| Pull mode: `PULL_STARTED` already `DONE` with offset 0 | [10](#10-pull-pull_started-reports-done-with-offset-0) |
| Pull mode: `PULL_PROTOCOL` (80), `PULL_HTTP_STATUS` (81), `PULL_SIZE_MISMATCH` (83) | [11](#11-pull-pull_protocol-http-status-size-mismatch) |
| Pull mode: stalls of about 8 s, slow transfer | [12](#12-pull-stalls-and-slow-transfer) |
| Initialisation returns `INVALID_ARG` on the C2000 only | [13](#13-init-fails-on-the-c2000-but-passes-on-the-pc) |
| BLE frames lost | [14](#14-ble-frames-lost-or-unanswered) |
| `CONTRACT_MISMATCH` (70) | [15](#15-contract_mismatch-70) |

## 1. Build stops with `#error`

* **Symptom:** the compiler stops in `fwu_config.h`, `fwu_layout.h`, a `layout_*.h`, `fwu_transport.h`, `fwu_crypto_sim.h` or
  `app_fwu_config.h` with a message starting `FWU` (messages are in English).
* **Cause (confirmed, each message names it):** not exactly one profile (`-DFWU_PROFILE_*`) or board (`-DFWU_BOARD_*`); DEV profile with
  `FWU_PRODUCTION`; fewer than 2 (journal) or 3 (swap) spare pages; region page counts that do not add up, staging not equal to the
  application region, counter region not 2 pages; a board whose numbers are unverified in a production build
  (`-DFWU_BOARD_NUMBERS_VERIFIED` is required); the simulated crypto in a production build; the CAN identifiers still placeholders
  (`FWU_TRANSPORT_CAN_IDS_VERIFIED`); `APP_FWU_PULL` without `APP_FWU_OVER_BLE` in the C2000 example.
* **Evidence / procedure:** read the message; `bash src/config/check_config.sh` reproduces each rejected combination on the PC.
* **Action:** fix the define or the layout header. **Expected:** the build proceeds.
* **Source:** `src/config/fwu_config.h`, `src/config/fwu_layout.h`.

## 2. No answer to `HELLO` or host timeouts

* **Possible causes:**
  * *Confirmed by code:* the driver did not initialise (the C2000 example then only drains its receive ring, so the host times out);
    frames not complete within 500 ms are discarded by the parser (`FWU_PARSER_FRAME_TIMEOUT_MS`); the response could not be sent
    (`tx_errors` in `fwu_link_get_stats`); the receive ring overflowed.
  * *Hypotheses:* transport pieces sent too fast for the receiver (see entry 14), wrong baud rate or wiring.
* **Evidence to collect:** `app_fwu_status()` (expect `FWU_OK`, 0); `fwu_link_get_stats()` (`frames_in`, `frames_out`, `tx_busy`, `tx_errors`);
  the raw bytes on the wire.
* **Procedure:** send a known-good `HELLO`: `A5 01 01 01 00 01 01 A6 FD 5A` (type 0x01, seq 1; produced by `tools/fwu_package.py`, matching the C
  encoder - see `tools/selftest_vectors.py`). If `frames_in` does not increase, the problem is below the parser (wiring, ring buffer, ISR).
* **Action:** correct the initialisation error, feed complete frames, enlarge the ring if it overflows.
* **Source:** `src/transport/fwu_link.c`, `src/frame/fwu_parser.c`, `examples/.../app_fwu.c` (`app_fwu_poll`).

## 3. `ERROR` BAD_STATE on `REQUEST_DOWNLOAD`

* **Cause (confirmed, `fwu_session.c`):** (a) a different manifest is sent while a download is already in progress; (b) an activation or
  trial is still pending (`fwu_activate_staging_writable` is false, so staging must not be touched); (c) an authorization hook is
  configured and the session is not yet authenticated.
* **Evidence:** the `ERROR` frame's request-type byte is 0x10; `INFO` shows the session state.
* **Action:** (a) send `ABORT`, then start again; (b) let the bootloader finish (reset), or confirm/revert the trial; (c) authenticate.
  Note `ABORT` is refused in the COMMITTED state: reset the device.
* **Expected:** `DOWNLOAD_ACCEPTED`. **Source:** `src/session/fwu_session.c` (`handle_request_download`).

## 4. `ERROR` BAD_BLOCK

* **Cause (confirmed):** `TRANSFER_DATA` offset is not the device's next expected offset, an exact repeat of the previous chunk differs from what is
  in flash, or the size in `TRANSFER_EXIT` / `ACTIVATE` does not match or the image is incomplete.
* **Action:** continue from `DOWNLOAD_ACCEPTED.next_offset` (re-send the same manifest in `REQUEST_DOWNLOAD` to resume; the host client
  in `src/client` does this automatically). **Expected:** the device re-acknowledges and continues.
* **Source:** `src/session/fwu_session.c` (`session_store_chunk`).

## 5. `VERIFY_RESULT` is not 0

`VERIFY_RESULT` carries the code of the verification pipeline; the session returns to IDLE. The pipeline checks, in this order:
manifest structure (33), product id (52), version against the minimum (53), size against the staging capacity (34), key id/version (54),
signature (50 or 51), image SHA-256 (35). The code therefore tells you which check failed:

| Code | Name | Meaning | Action |
|---|---|---|---|
| 33 | `BAD_MANIFEST` | magic, format version, reserved field or size invalid | rebuild the package with `tools/fwu_package.py` |
| 52 | `PRODUCT_MISMATCH` | manifest `product_id` differs from the device's `product_id` | use the product id configured in `fwu_boot_config_t` |
| 53 | `VERSION_REJECTED` | version below the minimum (minimum is 0 unless anti-rollback counters are used) | raise the image version |
| 34 | `IMAGE_TOO_LARGE` | image larger than the staging capacity | shrink the application or enlarge the layout |
| 54 | `KEY_REJECTED` | unknown key id or key version too low | signed profiles only |
| 50 / 51 | `BAD_SIGNATURE` / `CRYPTO_FAILURE` | signature invalid / could not be checked | see entry 7 |
| 35 | `HASH_MISMATCH` | digest of the staged image differs from the manifest | see entry 6 |

Flash read errors during verification are returned as flash codes (not as "invalid image"). **Source:** `src/verify/fwu_verify.c`.

## 6. `VERIFY_RESULT` = `HASH_MISMATCH` (35)

* **Confirmed causes:**
  * The bytes in staging differ from the file the manifest was built from. In pull mode, before the controller buffered whole responses,
    a byte lost in transit made every later byte of that response land shifted in flash; a host test that injects such a loss reproduces
    it, and with the current code nothing from an incomplete response is written (`src/pull/test_pull.c`, `test_silent_hole_never_reaches_flash`).
  * The manifest and the image come from different builds (rebuilding `app.bin` after the manifest was made).
* **Hypotheses (not established):** the origin of lost bytes on the evidenced C2000/FC41D hardware (shared 256-character FC41D message queue,
  UART overrun during flash work, AT-command contention) - see entry 12.
* **Evidence to collect:** SHA-256 of the file that is served (`sha256sum`) versus the manifest digest
  (`python -c` with `tools/fwu_package.py`'s `parse_manifest`, field `sha256`); in pull mode `fwu_pull_get_stats()`: `retries`,
  `rx_overflows`, and the FC41D diagnostic counters of your module driver.
* **Action:** regenerate the package from the exact `.bin`; in pull mode fix the byte loss; the device retries cleanly because the session
  returns to IDLE.
* **Expected:** `VERIFY_RESULT` 0.

## 7. Signed profile always fails

* **Cause (confirmed):** the Ed25519 hook of the software crypto port always returns `FWU_ERR_CRYPTO_FAILURE`
  (`src/crypto/fwu_crypto_sw.c`), so no signed image and no authorization response can ever be accepted with that port.
* **Action:** provide a real crypto port (`fwu_crypto_callback_t.ed25519_verify`), or use the DEV profile for development only
  (integrity check, no authenticity; not buildable with `FWU_PRODUCTION`).

## 8. Board does not start after the update (bootloader stays)

* **Symptom:** after `ACTIVATE_OK` and the reset the application does not run; in the C2000 example LED5 toggles forever (the STAY loop).
* **Cause by `g_boot_result.reason` (confirmed, `fwu_boot_start`):**

| Reason | Meaning |
|---|---|
| `FWU_BOOT_REASON_CONFIG` | invalid policy or configuration (the STM32 example also reports it when flash or crypto initialisation failed; the C2000 example records that case in `g_boot_setup_status` and leaves `g_boot_result` untouched) |
| `FWU_BOOT_REASON_COUNTERS` | anti-rollback counters could not be read |
| `FWU_BOOT_REASON_ACTIVATE_INIT` | activation setup failed (invalid trial count, journal regions, uninterpretable journal) |
| `FWU_BOOT_REASON_ACTIVATE_RUN` | verification could not run or a flash operation failed while installing; the application may be half written |
| `FWU_BOOT_REASON_NO_APP` | the `app_valid` hook said there is no startable application (verification rejection leaves the old application in place and is not an error) |

* **Also observed (confirmed from the repository's own comments):** a fixed-length image tool once truncated the image silently when the
  application grew, so `.cinit` and the RAM-function tail were missing and the board did not start after a successful-looking update.
  `finalize_image.ps1` now trims padding and refuses images larger than the staging capacity.
* **Evidence:** `g_boot_result` (`decision`, `reason`, `status`, `outcome`, `phase`); the size and tail of the staged image; the journal.
* **Action:** per reason; for `ACTIVATE_RUN` investigate the flash error (`status`) - the next boot repeats the interrupted step. A
  power-loss test of this phase on your hardware is not recorded in this repository.

## 9. Host timeout although the device rebooted

* **Cause (confirmed in code):** after `ACTIVATE` the device resets as soon as `fwu_link_reboot_ready()` is true. If the transmitter was not
  drained, `ACTIVATE_OK` is lost. In addition a hard transport send error drops the response yet `reboot_ready` can still become true.
* **Action:** drain the transmitter (the C2000 example flushes BLE and the SCI FIFO and waits 20 ms) before resetting; after a timeout the host
  should read `INFO` (running version) on reconnect rather than assume failure.

## 10. Pull: `PULL_STARTED` reports DONE with offset 0

* **Symptom:** a new update after a failed one ends immediately with a "protocol violation" and nothing is downloaded (older builds).
* **Cause (confirmed, fixed):** the pull controller kept state DONE across sessions and treated the same-target `PULL_START` as a repeat. Current
  code answers the repeat only if the image really is complete in this session; test `test_new_session_after_done`.
* **Action:** use a current `fwu_pull.c`; as a workaround on old builds reset the device.

## 11. Pull: PULL_PROTOCOL, HTTP status, size mismatch

* **`FWU_ERR_PULL_HTTP_STATUS` (81):** the server did not answer `206 Partial Content` (404, 200 because it ignored `Range`, 401/403, 5xx). Permanent;
  fix the URL path/server; nothing was written.
* **`FWU_ERR_PULL_SIZE_MISMATCH` (83):** the `Content-Range` total differs from the manifest size: the server serves another file. Nothing was written.
* **`FWU_ERR_PULL_PROTOCOL` (80) after retries:** unparsable or inconsistent responses (chunked encoding, missing `Content-Length`/`Content-Range`,
  header block over 2048 bytes, ring overflow). Confirmed from a recorded run: duplicate responses to repeated requests used to consume the
  retry budget; stale (other-range) responses are now skipped without a retry.
* **Evidence:** `PULL_STATUS` (state, flash offset, HTTP status, error), `fwu_pull_get_stats()`, the server's request log.
* **Source:** `src/pull/fwu_pull.c`, `src/pull/fwu_http.c`.

## 12. Pull: stalls and slow transfer

* **Symptom (recorded):** on the evidenced C2000 + FC41D setup the flash offset stopped advancing for about 8 s several times, then the
  same range was requested again; overall about 1 KB/s (see [`TEST_RESULTS.md`](TEST_RESULTS.md)). Lost `PULL_QUERY` replies coincided with some stalls.
* **Confirmed:** the 8 s is `APP_PULL_RESPONSE_TIMEOUT_MS`: no byte for that long renews the request; the resumed download is correct because
  nothing from an incomplete response is written.
* **Hypotheses (not established):** a lost tail of the response below the pull layer. Candidates are the FC41D driver's shared 256-character message
  queue (a BLE message arriving during a TCP read), UART receive overrun while flash is erased/written, or AT-command contention. The maintainer's
  host tool (not part of this repository) lengthened the `PULL_QUERY` interval to reduce BLE traffic as an experiment; its effect is not recorded here.
* **Evidence to collect:** your FC41D driver's drop/overflow/timeout counters (`rx_dropped`, `rx_overflow`, `cmd_timeouts` in the maintainer's driver),
  `fwu_pull_get_stats()` (`retries`, `rx_overflows`), counts of repeated GET lines in the server log.
* **Action:** reduce concurrent BLE traffic, keep flash work away from receive windows, fix the lower layer once the counters identify it.

## 13. Init fails on the C2000 but passes on the PC

* **Cause (confirmed, a real incident):** `int` is 16 bits on the C28x; layout macros such as `page * 8192` wrapped silently above 65535, so
  `fwu_session_init`/`fwu_boot_init` returned `FWU_ERR_INVALID_ARG` on the target. `layout_f280049c.h` uses `8192UL`.
* **Action:** give size macros a `UL` suffix; check with the TI compiler, not only on the PC.

## 14. BLE frames lost or unanswered

* **Confirmed constraints in the code:** the parser drops frames older than 500 ms, so pieces must be reassembled (the example's staging buffer
  flushes a complete frame or after 250 ms idle); the FC41D receive limit is 95 characters per message (`APP_BLE_PIECE_BYTES` = 47 octets).
* **Maintainer-reported measurements (not recorded in the repository):** a write gap of 50 ms lost frames, 100 ms or more did not; 150-octet
  pieces got no reply because of the module driver's 256-character queue.
* **Action:** keep pieces within the limit and the inter-piece gap at the example's values (device side 150 ms; the host-tool log excerpt shows 80 ms between its 126-byte writes).

## 15. CONTRACT_MISMATCH (70)

* **Cause (confirmed):** the application's configuration differs from the bootloader's published contract (policy, product id, trial count, flash
  geometry or any layout offset). Only reachable if the application calls `fwu_boot_info_check`, which no code here does.
* **Action:** build both binaries from the same shared configuration header and compare `fwu_boot_fingerprint` values.
