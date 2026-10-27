# Review findings

Defects, risks and inconsistencies found by reading the code while this repository was prepared (2026-10-26). **Method:** static reading of every
module by several independent readers, each finding checked against the code it cites; nothing here was reproduced on hardware, and only the
items marked *fixed* have a test. Function names are authoritative; line numbers drift. Severity is the reviewer's judgement of the impact on an
integrator, not a certification statement. For the maintainer's older, numbered list of open issues see [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md).

Status: **open** (reported, not changed), **fixed** (changed in this repository, with a test), **doc** (comment/documentation corrected).

## 1. Fixed during preparation

| ID | Where | Problem | Test that fails on the old behaviour |
|---|---|---|---|
| F1 | `src/pull/fwu_pull.c` `fwu_pull_on_frame` | A `DONE` pull state survived a failed update; the next session's `PULL_START` to the same target was answered "done, offset 0" and no pull started | `test_new_session_after_done` |
| F2 | `src/pull/fwu_pull.c` `fwu_pull_feed` | after an input-ring overflow the bytes that arrived later were still accepted; they were parsed shifted and written to staging, and the retry resumed after them | `test_hole_rejects_following_bytes` |
| F3 | `src/pull/fwu_pull.c` | response bodies were written to flash while arriving, so a byte lost in transit shifted all later data of that response into staging (`HASH_MISMATCH` on hardware); now the whole response is buffered, its length checked, and only then committed | `test_silent_hole_never_reaches_flash` |
| F4 | `src/pull/fwu_pull.c` `pull_check_headers` | a stale or duplicate response (other range) consumed the retry budget; it is now skipped | `test_stale_response_is_skipped` |
| D1 | comments in `fwu_flash_stm32g0.h`, `layout_f280049c.h`, `KNOWN_ISSUES.md` | the STM32 header said "not tested on hardware" against a recorded device update; a stale path and a reference to an undocumented decision "D89" | (documentation) |

## 2. Core logic

| ID | Sev. | Location | Finding | Status |
|---|---|---|---|---|
| C1 | medium | `fwu_activate_confirm`, `fwu_boot.c` (`confirm_retry`) | `CONFIRMED` is written to the journal before the anti-rollback counter is raised; `confirm_retry` is RAM only. Power loss between the two leaves the counter low: a downgrade stays acceptable until the next confirmed update | open |
| C2 | medium | `fwu_journal_append`, `act_append` | after a failed journal write the in-RAM next-slot is not advanced and nothing rescans, so a retry may hit the same dirty slot (`FLASH_NOT_ERASED`) until reboot; the opposite case (record written but read-back failed) reports an error although the next boot will perform the activation | open |
| C3 | medium | `fwu_boot.h:116` vs `fwu_boot.c` `boot_validate` | the header requires `activate_work` to be a separate buffer from `verify_work`; the code does not check it. Sharing one buffer would corrupt a verification or copy in progress | open |
| C4 | medium | whole repository | nothing calls `fwu_boot_app_finish_trial`, `fwu_activate_confirm` or `fwu_boot_info_check` outside comments; a SWAP application built from this repository alone never confirms and rolls back after `max_trial_boots` | open (integrator duty, documented) |
| C5 | low-med. | `fwu_link.c` `link_flush` / `fwu_link_reboot_ready` | after a hard `send` error the response is dropped and `pending_length` becomes 0, so `reboot_ready` can be true although `ACTIVATE_OK` never reached the transport; the device then resets and the host only sees a timeout | open |
| C6 | low-med. | `fwu_pull.c` `pull_response_complete` | after a committed range with image left, the state is not reset to REQUESTING (stays DOWNLOADING); differs from the description of REQUESTING in `fwu_proto.h` | open |
| C7 | low | `fwu_boot.c` | `FWU_BOOT_REASON_CONFIG` is documented for any invalid configuration, but a bad `max_trial_boots` or scratch setting is only detected in `fwu_activate_init` and reported as `REASON_ACTIVATE_INIT` | open |
| C8 | low | `fwu_activate_init` | can return `FWU_ERR_BAD_STATE` from `act_refresh` without setting `phase`; with an un-zeroed caller object `fwu_activate_staging_writable` reads garbage (the bootloader does not use the object after a failed init) | open |
| C9 | low | version floor | the minimum image version copied into the verify configuration at init is not updated after `fwu_counters_raise`; an offer below the new minimum is accepted in the same run (rejected later at commit/boot) | open |
| C10 | low | `fwu_session_init` | does not cross-check the verify configuration against the activation object (`image_offset` vs `staging_offset`, `max_image_size` vs `region_length`); a mismatch makes the bootloader STAY on every boot | open |
| C11 | low | `handle_abort` | an unauthenticated `ABORT` is accepted in any state except COMMITTED (any peer on the link can cancel a download); also the recovery path from AUTHENTICATED | open (possibly intended) |
| C12 | low | `fwu_session_poll` | any verification failure, including a transient flash read error, resets the session and discards the download | open |
| C13 | low | `fwu_session_init` | out-of-flash regions return `FWU_ERR_OUT_OF_RANGE`; the header says `FWU_ERR_INVALID_ARG` | open (doc) |
| C14 | low | `fwu_session_on_frame` | `*out_length` is not set on the `INVALID_ARG` / `BUFFER_TOO_SMALL` returns although the header promises a value on every call (the link returns early, so no harm today) | open |
| C15 | low | session manifest write (`handle_request_download`) | a flash error while writing the manifest sends `ERROR` but does not reset the session (the chunk path does) | open |
| C16 | low | `fwu_pull_init` | sets `initialized = false` before validating `config`; a failed re-init silently disables a live controller | open |
| C17 | low | `fwu_pull.c` `pull_consume` | `last_rx_ms` is refreshed by stale-response bytes too; a server that keeps sending stale data can postpone the timeout indefinitely | open |
| C18 | low | `fwu_pull_poll` | a FAILED pull whose session has gone IDLE stays FAILED (only DONE is normalised); a new `PULL_START` clears it | open |
| C19 | low | `fwu_session.c` vs `fwu_activate.c` | both use `FWU_ERR_FLASH_WRITE` for "read-back mismatch"; a port returning it from `flash_read` is reported as `FWU_ERR_BAD_BLOCK` in the duplicate-chunk path | open (documented in the porting guide) |
| C20 | low | `fwu_error.h` | `FWU_ERR_FLASH_WRITE/ERASE` are described as "retry once"; no caller retries | open (doc) |
| C21 | low | `act_is_rejection` | the whole range 30..39 counts as "image rejected"; a future "could not verify" code in that range would wrongly reject | open |
| C22 | low | `fwu_client.c` | `pull_start_ms` is reset on every restart through HELLO, so `pull_timeout_ms` is not a total limit; `info.max_chunk` is ignored (chunk size comes from `DOWNLOAD_ACCEPTED`); a frame view with a NULL payload pointer is rejected | open |
| C23 | low | `fwu_isotp.c` | header says an unexpected CF/FC aborts a reassembly, the code only returns `BAD_STATE`; `fwu_isotp_transport_send` uses a stored clock (fresh if the link polls the transport first) | open |
| C24 | low | `fwu_auth.c` | `auth_check` returns `INVALID_ARG` for a wrong-length/wrong-device challenge (header lists other codes); the previous nonce is kept in RAM only | open |
| C25 | low | `fwu_parser.c` | the 500 ms frame timeout restarts after a resync, so a dribbling sender gets a fresh window each time (still bounded) | open |
| C26 | low | `fwu_journal.c` | scan stops at the first erased slot (a valid record after a gap is not seen; looks intentional) ; `fwu_journal_ab_init` can leave region 0 modified after a failure | open |
| C27 | low | `fwu_layout.h` | `#undef FWU_FLASH_SPARE_PAGES` makes a `-D` value ignored when a board is selected | open |
| C28 | info | misc | `FWU_ERR_BUFFER_EMPTY` is never produced; the INFO auth flag is set when `config.auth != NULL`, not when `require_auth`; `fwu_flash` wrappers divide by the geometry before validation (documented) | open |

## 3. Platform ports

| ID | Sev. | Location | Finding | Status |
|---|---|---|---|---|
| P1 | medium | `ports/c2000/f28004x/flash_port.c` `flash_write` | no erased check; a write into non-erased cells surfaces as a Flash API status mapped to `FWU_ERR_FLASH_WRITE`, never `FWU_ERR_FLASH_NOT_ERASED` (the STM32 and direct ports return it) | open |
| P2 | low | same, FSM wait loops | unbounded wait for the flash state machine (the direct port bounds it with `PORT_MAX_FSM_WAIT`) | open |
| P3 | medium | `ports/c2000/f28004x_direct/fwu_flash_f28004x.c` | `hw_init` returns `FWU_ERR_FLASH_GEOMETRY` for Flash API failures; calls `prefetch(true)` after a failed bank selection; no `.TI.ramfunc` pragma although the header says it must run from RAM. This port has only been run against a PC stub | open |
| P4 | low | `fwu_flash_adapter.c` `adapter_read` | alignment/hardware error cases map to `INVALID_ARG`; they cannot occur in `flash_read` | open |
| P5 | low | `fwu_flash_stm32g0.c` | an unlock failure in erase returns `FWU_ERR_FLASH_ERASE`; the header says `FWU_ERR_FLASH_WRITE` | open (doc) |
| P6 | medium | `src/config/layout_f2800157.h` | `FWU_BOARD_PAGE_SIZE` is `1024u` without `UL`; on a 16-bit-`int` target the offset arithmetic overflows (the F280049C header uses `8192UL`). The board is unverified and has no port | open |
| P7 | low | HAL stubs | unchecked indexing / unsigned underflow in test-only stubs | open (test code) |

## 4. C2000 example

| ID | Sev. | Location | Finding | Status |
|---|---|---|---|---|
| E1 | low-med. | `28004x_app_flash_lnk.cmd` | `APP_FLASH` covers all 10 application sectors; the updatable image capacity is 9; only `finalize_image.ps1` enforces the limit (hard-coded 0x12000 and 16) | open |
| E2 | low | `main.c` (disabled self-tests) | the self-tests call 0x08C000 "staging" (it is the last application sector; staging starts at 0x08D000); re-enabling them would be refused by `FLASH_PORT_WRITABLE` | open |
| E3 | low | `bootloader/flash_port/flash_port_config.h` | staging and the scratch sector are not writable, so SWAP activation could not run with this configuration (COPY is what the example uses) | open |
| E4 | low | `app_fwu.c` `app_fwu_reboot` | unbounded busy-wait on the SCI TX FIFO before the reset | open |
| E5 | low | `main.c` | `APP_FWU_OVER_BLE=0` with `FC41D_ON_FW_UART=1` compiles although the comment says UART update is unusable then; no `#error` | open |
| E6 | trivial | `main.c`, `flash_port_config.h` | stale LED comment; spare range written as `..0x09FFEF` (sector ends at 0x09FFFF); unused statics produce 7 compiler warnings | open |
| E7 | low | `flash_tools/*.bat`, `make_image.bat` | assumed the original CCS project layout; now take `DSLITE`, `CCXML`, `OUT`, `HEX2000` from the environment and the repository folder names, but still assume the CCS projects are rooted in `application/` and `bootloader/` | doc |

## 5. External dependency: FC41D module driver (not in this repository)

Found while reading the driver the example links against; reported only. The maintainer states the driver is shared with another project and is
not to be changed here.

| ID | Sev. | Finding |
|---|---|---|
| X1 | medium | its 256-character message queue is shared by BLE and TCP data and the TCP read length is sized only when a read starts; a BLE message arriving during a TCP read can make the driver drop a whole TCP piece (counted only in its `rx_dropped`). The header claim that nothing is lost holds only if the application drains continuously and no BLE traffic competes. Hypothesis for the lost response tails seen in pull mode; not confirmed (no counters were recorded) |
| X2 | medium | on a `closed` URC the driver clears its read marks without issuing a last read, so bytes still buffered in the module may never be fetched |
| X3 | low | a receive notification that arrives during a read can be erased when the read marks are cleared; recovery relies on a 1 s poll |
| X4 | medium | UART receive is polled with a 16-element FIFO (about 1.4 ms at 115200 baud): a main-loop stall during a read (flash erase/write) can overrun it and desynchronise the raw-data parser |

## 6. What this list does not contain

Anything about the host PC applications, the vendors' SDKs, or hardware behaviour that was not observed in a recorded run.
