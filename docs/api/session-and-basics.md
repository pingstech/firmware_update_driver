# API reference: session and basics

Modules covered (all paths relative to the repository root):

| Module | Files |
|---|---|
| Base types | `src/fwu_types.h` |
| Error codes | `src/error/fwu_error.h` |
| Byte order helpers | `src/bytes/fwu_bytes.h` |
| CRC-16/MODBUS | `src/crc/fwu_crc.h` |
| Session protocol (wire format) | `src/session/fwu_proto.h`, `src/session/fwu_proto.c` |
| Session state machine (device side) | `src/session/fwu_session.h`, `src/session/fwu_session.c` |

Line numbers refer to the files as they are in the repository after the comment translation (code lines were not changed).

---

## 1. `fwu_types.h` - base types

**Purpose.** Collects the platform-dependent type decision in one place. Core modules use `fwu_byte_t` instead of `uint8_t`. On TI C2000 (C28x, 16-bit `char`) the build must define `C2000` (`-DC2000`); otherwise the ARM/ST/PC definition is used.

**Used by / depends on.** Depends on `<stdint.h>`, `<stddef.h>`. Included by every other module in this document and by almost every header in `src/`, `ports/`, `examples/`.

### Macros

| Macro | Value | Meaning | Location |
|---|---|---|---|
| `FWU_BYTE_MASK` | `0xFFu` | Mask applied when reading a `fwu_byte_t` (on C28x the upper 8 bits of the 16-bit container may be garbage). | `src/fwu_types.h:53` |
| `C2000` | (user-defined, not defined by the header) | Selects the C28x type definition. | `src/fwu_types.h:33` (`#if defined(C2000)`) |

### Types

| Type | Definition | Location |
|---|---|---|
| `fwu_byte_t` | `uint16_t` if `C2000` is defined (value kept in 0..255, one element = one byte value, unpacked); otherwise `unsigned char` | `src/fwu_types.h:36`, `:41` |

---

## 2. `fwu_error.h` - status codes

**Purpose.** Common return type of every fallible function. One success value (`FWU_OK = 0`); every other value is an error. Numeric values are permanent (they travel on the wire in the ERROR and VERIFY_RESULT messages as a single byte, so they must not exceed 255). Check with `status != FWU_OK`.

**Used by / depends on.** No dependencies. Included by every module that returns a status.

### Type

```c
typedef enum { /* ... see table ... */ } fwu_return_e;
```

Location: `src/error/fwu_error.h:36` (start of the enum).

### Groups

| Group | Meaning |
|---|---|
| 0 | success |
| 1x | usage errors |
| 2x | stream / resource |
| 3x | integrity |
| 4x | storage |
| 5x | security |
| 6x | session / protocol |
| 7x | bootloader contract |
| 8x | pull from network |

"Transient" (header wording): the same operation may succeed when retried. "Permanent": retrying does not help.

### Complete table of `fwu_return_e`

"Produced by" lists the non-test `.c` files (under `src/`, `ports/`, `examples/`) that reference the code as a return/assigned value, found by grep; a code that a module only forwards from a lower layer is not listed for that module. "Declared class" is the transient/permanent wording given in the header comment. Where the table says "referenced in" the code is passed through `pull_fail()` / `pull_retry()` helpers or otherwise not a direct `return`; where it says "no producer" no `.c` file outside tests uses it.

| Name | Value | Meaning (from the header) | Declared class | Layer / produced by |
|---|---|---|---|---|
| `FWU_OK` | 0 | Success. | - | all |
| `FWU_ERR_INVALID_ARG` | 10 | Invalid parameter (e.g. NULL pointer). | permanent | all layers (argument checks), e.g. `fwu_proto.c`, `fwu_session.c`, `fwu_frame.c`, `fwu_decode.c`, `fwu_parser.c`, `fwu_flash*.c`, `fwu_verify.c`, `fwu_pull.c`, `fwu_isotp.c`, ports |
| `FWU_ERR_BUFFER_TOO_SMALL` | 11 | Supplied buffer too small for the result. | permanent | `fwu_proto.c`, `fwu_session.c`, `fwu_frame.c`, `fwu_auth.c`, `fwu_boot.c`, `fwu_client.c`, `fwu_http.c`, `fwu_pull.c`, example ports (`app_ble.c`, `app_pull.c`) |
| `FWU_ERR_BAD_STATE` | 12 | Operation not valid in the current state. Also used by the session for messages not valid in the current session state. | permanent | `fwu_session.c`, `fwu_pull.c`, `fwu_client.c`, `fwu_activate.c`, `fwu_boot.c`, `fwu_counters.c`, `fwu_crypto.c`, `fwu_isotp.c`, examples (`app_fwu.c`, `app_pull.c`, `main.c`, `boot_app.c`) |
| `FWU_ERR_BUFFER_FULL` | 21 | Buffer full, new data does not fit. | transient | `fwu_ringbuf.c`, `fwu_journal.c`, `fwu_isotp.c`, `fwu_pull.c`, example ports (`app_ble.c`, `app_pull.c`); also referenced in `fwu_link.c` |
| `FWU_ERR_BUFFER_EMPTY` | 22 | Buffer empty, nothing to read. | transient | no producer found (only the header and `src/error/test_error.c` reference it) |
| `FWU_ERR_NOT_ENOUGH_DATA` | 23 | Not enough data available (not an integrity error; wait, do not request retransmission). | transient | `fwu_ringbuf.c`, `fwu_decode.c`, `fwu_parser.c`, `fwu_manifest.c`, `fwu_boot.c`; also referenced in `fwu_link.c` |
| `FWU_ERR_CRC_MISMATCH` | 30 | CRC mismatch. | transient | `fwu_decode.c` |
| `FWU_ERR_BAD_LENGTH` | 31 | Frame length does not match the expected one. Also used for wrong payload length in session/proto messages. | transient | `fwu_decode.c`, `fwu_isotp.c`, `fwu_proto.c`, `fwu_session.c`, `fwu_pull.c`, `fwu_auth.c` |
| `FWU_ERR_BAD_FRAME` | 32 | Invalid frame format (SOF, VER or EOF). The frame is discarded; the parser looks for the next SOF. | permanent | `fwu_decode.c`, `fwu_isotp.c`; handled in `fwu_parser.c` |
| `FWU_ERR_BAD_MANIFEST` | 33 | Invalid manifest structure (magic, format version, reserved field, image size), checked before the signature. Treated by the activation verification hook as "image invalid". | permanent | `fwu_manifest.c` |
| `FWU_ERR_IMAGE_TOO_LARGE` | 34 | Image size exceeds the allocated area. Treated as "image invalid". | permanent | `fwu_verify.c` |
| `FWU_ERR_HASH_MISMATCH` | 35 | SHA-256 of the image differs from the manifest digest. Treated as "image invalid". | permanent | `fwu_verify.c` |
| `FWU_ERR_FLASH_WRITE` | 40 | Flash write failed. Also returned by `fwu_session.c` when read-back after a write does not match. | retry once, then permanent | `fwu_session.c`, `fwu_activate.c`, `fwu_flash_stm32g0.c`, `fwu_flash_f28004x.c`; `fwu_flash_adapter.c` (c2000 port) also references it |
| `FWU_ERR_OUT_OF_RANGE` | 41 | Offset/length runs outside the flash region. | permanent | `fwu_flash.c`, `fwu_flash_sim.c`, `fwu_flash_adapter.c`, `fwu_journal.c`, `fwu_activate.c`, `fwu_session.c` (init) |
| `FWU_ERR_WRITE_ALIGNMENT` | 42 | Write offset/length not a multiple of the write granule. | permanent | `fwu_flash.c`, `fwu_flash_sim.c`, `fwu_flash_adapter.c` |
| `FWU_ERR_ERASE_ALIGNMENT` | 43 | Erase offset/length not a multiple of the page size. | permanent | `fwu_flash.c`, `fwu_flash_sim.c`, `fwu_flash_adapter.c` |
| `FWU_ERR_FLASH_GEOMETRY` | 44 | Flash object geometry inconsistent. | permanent | `fwu_flash.c`, `fwu_flash_sim.c`, `fwu_flash_stm32g0.c`, `fwu_flash_f28004x.c`, `fwu_journal.c`, `fwu_activate.c`, `fwu_session.c` (init) |
| `FWU_ERR_FLASH_NOT_ERASED` | 45 | Target area not erased. Checked before writing by the simulated flash and the STM32G0 port. | permanent | `fwu_flash_sim.c`, `fwu_flash_stm32g0.c`, `fwu_flash_f28004x.c` |
| `FWU_ERR_FLASH_POWER_LOSS` | 46 | Power lost; returned ONLY by the simulated flash (tests). | n/a | `fwu_flash_sim.c` |
| `FWU_ERR_FLASH_ERASE` | 47 | Flash erase failed. | retry once, then permanent | `fwu_flash_stm32g0.c`, `fwu_flash_f28004x.c` (c2000 `fwu_flash_adapter.c` also references it) |
| `FWU_ERR_BAD_SIGNATURE` | 50 | Signature verified and found INVALID. Different from `FWU_ERR_CRYPTO_FAILURE`. | permanent | `fwu_crypto.c`, `fwu_crypto_sim.c`; classified in `fwu_activate.c` |
| `FWU_ERR_CRYPTO_FAILURE` | 51 | Cryptographic operation could not be performed (does not mean the signature is invalid). `fwu_session.c` also substitutes it when a hook returns a code outside 1..255 (`session_code`, `src/session/fwu_session.c:23`). | may be transient | `fwu_crypto.c`, `fwu_crypto_sw.c`, `fwu_auth.c`, `fwu_session.c`; classified in `fwu_activate.c` |
| `FWU_ERR_PRODUCT_MISMATCH` | 52 | Manifest belongs to another product. | permanent | `fwu_verify.c`; referenced in `fwu_activate.c` |
| `FWU_ERR_VERSION_REJECTED` | 53 | Image version below the minimum accepted version (anti-rollback). | permanent | `fwu_verify.c`; referenced in `fwu_activate.c` |
| `FWU_ERR_KEY_REJECTED` | 54 | Key id not on the device, or key version below the minimum. | permanent | `fwu_verify.c`, `fwu_auth.c`; referenced in `fwu_activate.c` |
| `FWU_ERR_UNKNOWN_MESSAGE` | 60 | Unknown message type (or a RESPONSE type arriving at the device). Session state unchanged. | permanent | `fwu_session.c` (default branch of the dispatcher, line 609-610), `fwu_pull.c` |
| `FWU_ERR_BAD_BLOCK` | 61 | Data chunk not in expected order (offset mismatch), or TRANSFER_EXIT / ACTIVATE size claim wrong or image not fully received. Session stays open. | - | `fwu_session.c` (lines 431, 441, 509, 523) |
| `FWU_ERR_LOCKED_OUT` | 62 | Too many consecutive authorization failures; new AUTH_REQ refused for `auth_lockout_ms`. | retry after timeout | `fwu_session.c` (line 275) |
| `FWU_ERR_CONTRACT_MISMATCH` | 70 | Bootloader/application build contracts mismatch, or the contract info cannot be read. | permanent | `fwu_boot.c` |
| `FWU_ERR_PULL_PROTOCOL` | 80 | HTTP response from the server could not be parsed or is inconsistent with the requested range. `fwu_pull` retries a limited number of times. | may be transient | `fwu_http.c`; referenced in `fwu_pull.c`, `fwu_client.c` |
| `FWU_ERR_PULL_HTTP_STATUS` | 81 | Server returned a status other than 206 (404, 401/403, 5xx, or 200). Status code kept in `fwu_pull` detail field. | permanent | `fwu_pull.c` (`pull_fail`, line 145) |
| `FWU_ERR_PULL_TIMEOUT` | 82 | Expected data did not arrive in time and the retry budget is exhausted. Session stays open. | - | `fwu_pull.c` (`pull_retry`, lines 127, 588) |
| `FWU_ERR_PULL_SIZE_MISMATCH` | 83 | Total file size on the server (Content-Range) differs from the manifest image size; nothing is written to flash. | permanent | `fwu_pull.c` (`pull_fail`, line 153) |

Notes (from the header and code):
- Codes 9x and above are reserved. Number 20 is unused.
- `FWU_ERR_BAD_LENGTH` is in the "integrity" group but is also the response code for a payload of the wrong length in session messages.

---

## 3. `fwu_bytes.h` - number <-> byte array conversion

**Purpose.** Header-only (`static inline`) helpers that convert 16/32-bit numbers to and from `fwu_byte_t` arrays in big-endian or little-endian order, independent of the CPU byte order (shifts and masks only, no casts/unions). On read, each element is first masked with `FWU_BYTE_MASK`, then widened to the target type, then shifted. No argument checking is done (the caller guarantees the array has enough elements).

**Used by / depends on.** Depends on `fwu_types.h`. Included by `fwu_proto.c`, `fwu_session.c`, `fwu_auth.c`, `fwu_boot.c`, `fwu_decode.c`, `fwu_frame.c`, `fwu_journal.c`, `fwu_manifest.c`.

All functions: no return status, no blocking, no side effect other than the written bytes.

| Declaration (exact) | Behaviour | Location |
|---|---|---|
| `static inline void fwu_write_u16_big_endian(fwu_byte_t *buffer, uint16_t value)` | Writes `buffer[0]` = high byte, `buffer[1]` = low byte. Needs >= 2 elements. Only those two elements are modified. | `src/bytes/fwu_bytes.h:41` |
| `static inline void fwu_write_u16_little_endian(fwu_byte_t *buffer, uint16_t value)` | `buffer[0]` = low byte, `buffer[1]` = high byte. | `src/bytes/fwu_bytes.h:55` |
| `static inline uint16_t fwu_read_u16_big_endian(const fwu_byte_t *buffer)` | Returns `buffer[0]` as high byte, `buffer[1]` as low byte. Needs >= 2 elements. | `src/bytes/fwu_bytes.h:70` |
| `static inline uint16_t fwu_read_u16_little_endian(const fwu_byte_t *buffer)` | `buffer[1]` high byte, `buffer[0]` low byte. | `src/bytes/fwu_bytes.h:90` |
| `static inline void fwu_write_u32_big_endian(fwu_byte_t *buffer, uint32_t value)` | `buffer[0]` = most significant byte ... `buffer[3]` = least significant. Needs >= 4 elements. | `src/bytes/fwu_bytes.h:109` |
| `static inline void fwu_write_u32_little_endian(fwu_byte_t *buffer, uint32_t value)` | `buffer[0]` = least significant ... `buffer[3]` = most significant. | `src/bytes/fwu_bytes.h:125` |
| `static inline uint32_t fwu_read_u32_big_endian(const fwu_byte_t *buffer)` | Inverse of the big-endian write. Needs >= 4 elements. | `src/bytes/fwu_bytes.h:146` |
| `static inline uint32_t fwu_read_u32_little_endian(const fwu_byte_t *buffer)` | Inverse of the little-endian write. | `src/bytes/fwu_bytes.h:165` |

Examples given in the header: `0x1234` big-endian -> `{0x12, 0x34}`; `0x89ABCDEF` little-endian -> `{0xEF, 0xCD, 0xAB, 0x89}`.

---

## 4. `fwu_crc.h` - CRC-16/MODBUS

**Purpose.** Header-only (`static inline`) CRC-16/MODBUS: reflected polynomial `0xA001`, initial value `0xFFFF`, no final XOR; check vector `"123456789"` -> `0x4B37` (as stated in the header). A CRC detects corruption only; it is neither encryption nor authentication. Bit loop is 8 steps per byte (not `CHAR_BIT`, because it is 16 on C28x). Appending the CRC low byte first to the data and recomputing gives `0x0000` (header hint).

**Used by / depends on.** Depends on `<stddef.h>`, `fwu_types.h`. Included by `fwu_decode.c`, `fwu_frame.c`, `fwu_boot.c`, `fwu_journal.c`.

### Macros

| Macro | Value | Meaning | Location |
|---|---|---|---|
| `FWU_CRC16_INIT` | `0xFFFF` | Standard initial value. | `src/crc/fwu_crc.h:42` |

### Functions

```c
static inline uint16_t fwu_crc16_calculate_w_start_value(const uint16_t start_value, const fwu_byte_t *data, size_t length)
```
- Continues a CRC from `start_value` (use `FWU_CRC16_INIT` for the first chunk, the previous result afterwards). Chunk order matters.
- `data`: must not be NULL if `length > 0` (not checked). `length == 0` returns `start_value` unchanged. Each element is masked with `FWU_BYTE_MASK`.
- Returns the updated 16-bit CRC. No error codes. Runs in time proportional to `length`; no blocking calls.
- `src/crc/fwu_crc.h:69`

```c
static inline uint16_t fwu_crc16_classic_calculate(const fwu_byte_t *data, size_t length)
```
- Same as `fwu_crc16_calculate_w_start_value(FWU_CRC16_INIT, data, length)`; returns `0xFFFF` for `length == 0`.
- `src/crc/fwu_crc.h:104`

---

## 5. `fwu_proto.h` / `fwu_proto.c` - session protocol (wire format)

**Purpose.** Single source of truth for the update-session protocol: message types, payload layouts, and stateless encode/parse functions. A message is the PAYLOAD of a `fwu_frame.h` frame; the message type is the frame's TYPE byte. A response carries the SEQ of the request. The frame format does not accept an empty payload, so messages without data carry a 1-byte placeholder (`FWU_PROTO_PLACEHOLDER`, value 0). All multi-byte numbers are big-endian. Offsets and sizes are counts of `fwu_byte_t`. The numbers are the project's own; alignment with UDS (ISO 14229) is stated in the header as UNVERIFIED. Response type = request type `| 0x80` (`FWU_PROTO_RESPONSE_FLAG`), except `FWU_MSG_ERROR` (0x7F) and the pairs that do not follow the rule below (see table).

**Used by / depends on.** Depends on `fwu_error.h`, `fwu_types.h`, `fwu_frame.h`; implementation also on `fwu_bytes.h`. Used by `fwu_session.[ch]`, `fwu_pull.[ch]`, `fwu_http.[ch]`, `fwu_client.[ch]`, `examples/c2000_launchxl_f280049c/application/port/app_pull.c`.

### 5.1 Message types and payload layouts

Payload sizes in the "Payload" columns are numbers of `fwu_byte_t`. All integers big-endian. "Handled by" shows who processes the request on the device (confirmed in the code).

#### Requests (host -> device)

| Name | Type | Payload layout | Valid payload length | Handled by (device) | Success response |
|---|---|---|---|---|---|
| `FWU_MSG_HELLO` | 0x01 | `[version(1)]` | 1 exactly, otherwise ERROR/`FWU_ERR_BAD_LENGTH` (`fwu_session.c:235`). The version byte is not interpreted. | `fwu_session.c:227` | `FWU_MSG_INFO` |
| `FWU_MSG_AUTH_REQ` | 0x02 | `[0]` (placeholder) | 1 exactly (`fwu_session.c:270`) | `fwu_session.c:261` | `FWU_MSG_AUTH_CHALLENGE` |
| `FWU_MSG_AUTH_RESP` | 0x03 | `[response(1..128)]` | 1..`FWU_PROTO_AUTH_RESPONSE_MAX` (`fwu_session.c:306`) | `fwu_session.c:299` | `FWU_MSG_AUTH_OK` |
| `FWU_MSG_REQUEST_DOWNLOAD` | 0x10 | `[manifest(138)]` (`FWU_MANIFEST_SIZE`) | exactly `FWU_MANIFEST_SIZE` (`fwu_session.c:347`) | `fwu_session.c:334` | `FWU_MSG_DOWNLOAD_ACCEPTED` |
| `FWU_MSG_TRANSFER_DATA` | 0x11 | `[offset(4)][data(1..508)]` | 5..512 (parse), data length also <= configured `chunk_size` | `fwu_session.c:479` | `FWU_MSG_DATA_ACK` |
| `FWU_MSG_TRANSFER_EXIT` | 0x12 | `[image_size(4)]` | 4 exactly (`fwu_proto_parse_u32`) | `fwu_session.c:512` | `FWU_MSG_EXIT_PENDING` (then later `FWU_MSG_VERIFY_RESULT`); or `FWU_MSG_VERIFY_RESULT` directly in READY |
| `FWU_MSG_ACTIVATE` | 0x13 | `[image_size(4)]` | 4 exactly | `fwu_session.c:537` | `FWU_MSG_ACTIVATE_OK` |
| `FWU_MSG_PULL_START` | 0x15 | `[ip(4)][port(2)][length(1)][target(1..96)]` | `7 + length`, length 1..96 | NOT by `fwu_session.c` (it returns `FWU_ERR_UNKNOWN_MESSAGE`); handled by `fwu_pull_on_frame` (`src/pull/fwu_pull.c:416`), which the transport must route to via `fwu_pull_handles` (`fwu_pull.c:390`) | `FWU_MSG_PULL_STARTED` |
| `FWU_MSG_PULL_QUERY` | 0x16 | `[0]` (placeholder) | 1 exactly (`fwu_pull.c:435`) | `fwu_pull_on_frame` (same routing) | `FWU_MSG_PULL_STATUS` |
| `FWU_MSG_ABORT` | 0x1F | `[reason(1)]` | 1 exactly (`fwu_session.c:569`); the reason value is not read | `fwu_session.c:563` | `FWU_MSG_ABORT_OK` |
| `FWU_MSG_KEEPALIVE` | 0x3E | `[0]` (placeholder) | 1 exactly (`fwu_session.c:255`) | `fwu_session.c:252` | `FWU_MSG_KEEPALIVE_OK` |

#### Responses (device -> host)

| Name | Type | Payload layout | Sent when |
|---|---|---|---|
| `FWU_MSG_ERROR` | 0x7F | `[code(1)][request_type(1)]` (`FWU_PROTO_ERROR_SIZE` = 2). `code` is the `fwu_return_e` value (1..255); `request_type` echoes the TYPE of the request that failed. Frame SEQ = request SEQ. | Any request that fails at protocol level (wrong state, wrong length, unknown type, hook error, flash error...). |
| `FWU_MSG_INFO` | 0x81 | 13 bytes: `version(1) flags(1) product_id(4) running_image_version(4) max_chunk(2) state(1)` (`FWU_PROTO_INFO_SIZE`). `flags` bit0 = `FWU_PROTO_FLAG_AUTH_REQUIRED`, bit1 = `FWU_PROTO_FLAG_SIGNED`; `state` = `FWU_PROTO_STATE_*`. | Reply to HELLO. In the code bit0 is set when an auth hook is configured (`config.auth != NULL`), bit1 when `verify->require_signature` (`fwu_session.c:239-240`). |
| `FWU_MSG_AUTH_CHALLENGE` | 0x82 | `[challenge(1..64)]` | Reply to AUTH_REQ. |
| `FWU_MSG_AUTH_OK` | 0x83 | `[0]` (placeholder) | Reply to a correct AUTH_RESP. |
| `FWU_MSG_DOWNLOAD_ACCEPTED` | 0x90 | 9 bytes: `chunk_size(2) mode(1) timeout_s(2) next_offset(4)` (`FWU_PROTO_ACCEPTED_SIZE`). `mode` = `FWU_PROTO_MODE_HANDSHAKE` (0, one acknowledgement per chunk). `timeout_s` = idle timeout in seconds, saturated at 0xFFFF (`fwu_session.c:138`). `next_offset` = 0 for a new download, the resume point otherwise. | Reply to REQUEST_DOWNLOAD (new or resumed). |
| `FWU_MSG_DATA_ACK` | 0x91 | `[next_offset(4)]` | Reply to TRANSFER_DATA: chunk written to flash AND read back and compared. |
| `FWU_MSG_EXIT_PENDING` | 0x92 | `[1]` | Immediate reply to TRANSFER_EXIT; verification runs later in `fwu_session_poll`. |
| `FWU_MSG_ACTIVATE_OK` | 0x93 | `[0]` (placeholder) | Reply to ACTIVATE after the commit is written (or repeated in COMMITTED). |
| `FWU_MSG_VERIFY_RESULT` | 0x94 | `[code(1)]`, 0 = image valid, otherwise the `fwu_return_e` value. Frame SEQ = SEQ of the latest TRANSFER_EXIT. | Separate, LATER message produced by `fwu_session_poll` after verification; or immediate reply to TRANSFER_EXIT in READY (code 0). |
| `FWU_MSG_PULL_STARTED` | 0x95 | 8 bytes, PULL_STATUS layout | Reply to PULL_START. |
| `FWU_MSG_PULL_STATUS` | 0x96 | 8 bytes: `state(1) next_offset(4) error(1) http_status(2)` (`FWU_PROTO_PULL_STATUS_SIZE`). `state` = `FWU_PROTO_PULL_*`; `next_offset` = image bytes pulled and written to flash; `error` = `fwu_return_e` value when `FWU_PROTO_PULL_FAILED`, else 0; `http_status` = last HTTP status (0 = none). | Reply to PULL_QUERY. |
| `FWU_MSG_ABORT_OK` | 0x9F | `[0]` (placeholder) | Reply to ABORT. |
| `FWU_MSG_KEEPALIVE_OK` | 0xBE | `[state(1)]` (`FWU_PROTO_STATE_*`) | Reply to KEEPALIVE. |

Note: TRANSFER_EXIT (0x12) has two responses: `FWU_MSG_EXIT_PENDING` (0x92 = 0x12 | 0x80) and the later `FWU_MSG_VERIFY_RESULT` (0x94, there is no request type 0x14). `FWU_MSG_ERROR` (0x7F) does not follow the `| 0x80` rule.

### 5.2 Macros and constants

| Macro | Value | Meaning | Location |
|---|---|---|---|
| `FWU_PROTO_VERSION` | `1u` | Protocol version reported in INFO. | `fwu_proto.h:68` |
| `FWU_PROTO_RESPONSE_FLAG` | `0x80u` | Response type = request type OR this flag. | `fwu_proto.h:70` |
| `FWU_MSG_*` | see tables above | Message types. | `fwu_proto.h:73-98` |
| `FWU_PROTO_STATE_IDLE` | `0u` | Session states carried by INFO and KEEPALIVE_OK (permanent numbers). | `fwu_proto.h:101` |
| `FWU_PROTO_STATE_AUTH_PENDING` | `1u` | | `fwu_proto.h:102` |
| `FWU_PROTO_STATE_AUTHENTICATED` | `2u` | | `fwu_proto.h:103` |
| `FWU_PROTO_STATE_DOWNLOADING` | `3u` | | `fwu_proto.h:104` |
| `FWU_PROTO_STATE_VERIFYING` | `4u` | | `fwu_proto.h:105` |
| `FWU_PROTO_STATE_READY` | `5u` | | `fwu_proto.h:106` |
| `FWU_PROTO_STATE_COMMITTED` | `6u` | | `fwu_proto.h:107` |
| `FWU_PROTO_PULL_IDLE` | `0u` | Pull states carried by PULL_STATUS (permanent numbers). | `fwu_proto.h:110` |
| `FWU_PROTO_PULL_REQUESTING` | `1u` | Request sent / waiting for header / retrying. | `fwu_proto.h:111` |
| `FWU_PROTO_PULL_DOWNLOADING` | `2u` | Body streaming, chunks being written. | `fwu_proto.h:112` |
| `FWU_PROTO_PULL_DONE` | `3u` | Whole image pulled and written. | `fwu_proto.h:113` |
| `FWU_PROTO_PULL_FAILED` | `4u` | Failed (see the error field). | `fwu_proto.h:114` |
| `FWU_PROTO_FLAG_AUTH_REQUIRED` | `0x01u` | INFO flag bit0. | `fwu_proto.h:117` |
| `FWU_PROTO_FLAG_SIGNED` | `0x02u` | INFO flag bit1. | `fwu_proto.h:118` |
| `FWU_PROTO_MODE_HANDSHAKE` | `0u` | Flow mode: acknowledgement per chunk (stop-and-wait). The only mode defined. | `fwu_proto.h:121` |
| `FWU_PROTO_INFO_SIZE` | `13u` | INFO payload size. | `fwu_proto.h:124` |
| `FWU_PROTO_ACCEPTED_SIZE` | `9u` | DOWNLOAD_ACCEPTED payload size. | `fwu_proto.h:125` |
| `FWU_PROTO_U32_SIZE` | `4u` | Single 32-bit value payload. | `fwu_proto.h:126` |
| `FWU_PROTO_DATA_HEADER_SIZE` | `4u` | Offset field in TRANSFER_DATA. | `fwu_proto.h:127` |
| `FWU_PROTO_ERROR_SIZE` | `2u` | ERROR payload size. | `fwu_proto.h:128` |
| `FWU_PROTO_MAX_CHUNK` | `(FWU_FRAME_MAX_PAYLOAD - FWU_PROTO_DATA_HEADER_SIZE)` = 508 (`FWU_FRAME_MAX_PAYLOAD` is 512, defined in `src/frame/fwu_frame.h`) | Largest data length in one TRANSFER_DATA. | `fwu_proto.h:129` |
| `FWU_PROTO_PULL_START_FIXED` | `7u` | Bytes before the target in PULL_START (`ip(4) port(2) length(1)`). | `fwu_proto.h:130` |
| `FWU_PROTO_PULL_TARGET_MAX` | `96u` | Maximum HTTP request target length. | `fwu_proto.h:131` |
| `FWU_PROTO_PULL_STATUS_SIZE` | `8u` | PULL_STATUS payload size. | `fwu_proto.h:132` |
| `FWU_PROTO_AUTH_CHALLENGE_MAX` | `64u` | Maximum challenge length. | `fwu_proto.h:133` |
| `FWU_PROTO_AUTH_RESPONSE_MAX` | `128u` | Maximum authorization response length. | `fwu_proto.h:134` |
| `FWU_PROTO_PLACEHOLDER` | `0u` | Value of the 1-byte placeholder field. | `fwu_proto.h:135` |

### 5.3 Types

```c
typedef struct {
    fwu_byte_t proto_version;           /* Protocol version of the device. */
    fwu_byte_t flags;                   /* FWU_PROTO_FLAG_*. */
    uint32_t   product_id;              /* Product id of the device. */
    uint32_t   running_image_version;   /* Version of the running image (informational). */
    uint16_t   max_chunk;               /* Data chunk size the device accepts. */
    fwu_byte_t state;                   /* FWU_PROTO_STATE_*. */
} fwu_proto_info_t;                     /* fwu_proto.h:140 */

typedef struct {
    uint16_t   chunk_size;              /* Data length in each TRANSFER_DATA (except the last chunk). */
    fwu_byte_t mode;                    /* FWU_PROTO_MODE_*. */
    uint16_t   timeout_s;               /* Idle timeout of the device (seconds). */
    uint32_t   next_offset;             /* First offset the device expects. */
} fwu_proto_accepted_t;                 /* fwu_proto.h:150 */

typedef struct {
    uint32_t          offset;           /* Offset of the chunk within the image. */
    const fwu_byte_t *data;             /* Points inside the payload, no copy. */
    size_t            data_length;      /* 1..FWU_PROTO_MAX_CHUNK. */
} fwu_proto_data_t;                     /* fwu_proto.h:158 */

typedef struct {
    fwu_byte_t        ip[4];            /* IPv4 address in a.b.c.d order. */
    uint16_t          port;             /* 1..65535. */
    const fwu_byte_t *target;           /* Points inside the payload, no copy; not NUL-terminated. */
    size_t            target_length;    /* 1..FWU_PROTO_PULL_TARGET_MAX. */
} fwu_proto_pull_start_t;               /* fwu_proto.h:165 */

typedef struct {
    fwu_byte_t state;                   /* FWU_PROTO_PULL_*. */
    uint32_t   next_offset;             /* Image bytes written to flash. */
    fwu_byte_t error;                   /* fwu_return_e value if FAILED, otherwise 0. */
    uint16_t   http_status;             /* Last HTTP status code (0 = no response at all). */
} fwu_proto_pull_status_t;              /* fwu_proto.h:173 */
```

Lifetime: `fwu_proto_data_t.data` and `fwu_proto_pull_start_t.target` alias the payload buffer passed to the parse function; they are valid only while that buffer is unchanged.

### 5.4 Functions

General contract (header comment and `proto_check_out` / `proto_check_in`, `fwu_proto.c:14`, `:26`): all functions are pure (no state, no blocking, no dynamic memory). `encode_*` writes `out` only if ALL validation passes; `FWU_ERR_BUFFER_TOO_SMALL` if `out_capacity` is too small, `FWU_ERR_INVALID_ARG` for NULL `out` / `out_length` (or other arguments as listed), `*out_length` is set to the payload length on success only. `parse_*` returns `FWU_ERR_BAD_LENGTH` if the payload length differs from the expected one (checked after the NULL-payload check), `FWU_ERR_INVALID_ARG` for NULL arguments; output is written only on `FWU_OK`. Every `encode_*` / `parse_*` also returns only `FWU_OK`, `FWU_ERR_INVALID_ARG`, `FWU_ERR_BUFFER_TOO_SMALL` (encode) or `FWU_ERR_BAD_LENGTH` (parse), as confirmed in `fwu_proto.c`, with the extra cases noted below.

| Declaration (exact) | Layout | Specific behaviour | Location |
|---|---|---|---|
| `fwu_return_e fwu_proto_encode_info(const fwu_proto_info_t *info, fwu_byte_t *out, size_t out_capacity, size_t *out_length)` | `out[0]=proto_version, [1]=flags, [2..5]=product_id, [6..9]=running_image_version, [10..11]=max_chunk, [12]=state` | needs 13 bytes; `*out_length = FWU_PROTO_INFO_SIZE` | `fwu_proto.c:44` |
| `fwu_return_e fwu_proto_parse_info(const fwu_byte_t *payload, size_t payload_length, fwu_proto_info_t *info)` | inverse | length must be 13 | `fwu_proto.c:65` |
| `fwu_return_e fwu_proto_encode_accepted(const fwu_proto_accepted_t *accepted, fwu_byte_t *out, size_t out_capacity, size_t *out_length)` | `[0..1]=chunk_size, [2]=mode, [3..4]=timeout_s, [5..8]=next_offset` | needs 9 bytes | `fwu_proto.c:86` |
| `fwu_return_e fwu_proto_parse_accepted(const fwu_byte_t *payload, size_t payload_length, fwu_proto_accepted_t *accepted)` | inverse | length must be 9 | `fwu_proto.c:105` |
| `fwu_return_e fwu_proto_encode_u32(uint32_t value, fwu_byte_t *out, size_t out_capacity, size_t *out_length)` | 4 bytes big-endian | used for DATA_ACK next offset, TRANSFER_EXIT and ACTIVATE image size | `fwu_proto.c:124` |
| `fwu_return_e fwu_proto_parse_u32(const fwu_byte_t *payload, size_t payload_length, uint32_t *value)` | inverse | length must be 4 | `fwu_proto.c:136` |
| `fwu_return_e fwu_proto_encode_data(uint32_t offset, const fwu_byte_t *data, size_t data_length, fwu_byte_t *out, size_t out_capacity, size_t *out_length)` | `[0..3]=offset, [4..]=data` | host side. `FWU_ERR_INVALID_ARG` if `data == NULL`, `data_length == 0` or `> FWU_PROTO_MAX_CHUNK`; copies each element masked with `FWU_BYTE_MASK` | `fwu_proto.c:152` |
| `fwu_return_e fwu_proto_parse_data(const fwu_byte_t *payload, size_t payload_length, fwu_proto_data_t *data)` | inverse, no copy | `FWU_ERR_BAD_LENGTH` if `payload_length <= 4` or `> FWU_FRAME_MAX_PAYLOAD`; checks `payload == NULL` first, then the length, then `data == NULL` | `fwu_proto.c:172` |
| `fwu_return_e fwu_proto_encode_result(fwu_return_e code, fwu_byte_t *out, size_t out_capacity, size_t *out_length)` | 1 byte | `FWU_ERR_INVALID_ARG` if `code < 0` or `> 255`; 0 is allowed (valid image) | `fwu_proto.c:191` |
| `fwu_return_e fwu_proto_parse_result(const fwu_byte_t *payload, size_t payload_length, fwu_byte_t *code)` | inverse | length must be 1 | `fwu_proto.c:207` |
| `fwu_return_e fwu_proto_encode_error(fwu_return_e code, fwu_byte_t request_type, fwu_byte_t *out, size_t out_capacity, size_t *out_length)` | `[0]=code, [1]=request_type` | `FWU_ERR_INVALID_ARG` if `code < 1` or `> 255` | `fwu_proto.c:221` |
| `fwu_return_e fwu_proto_parse_error(const fwu_byte_t *payload, size_t payload_length, fwu_byte_t *code, fwu_byte_t *request_type)` | inverse | length must be 2; both output pointers must be non-NULL | `fwu_proto.c:239` |
| `fwu_return_e fwu_proto_encode_pull_start(const fwu_byte_t ip[4], uint16_t port, const fwu_byte_t *target, size_t target_length, fwu_byte_t *out, size_t out_capacity, size_t *out_length)` | `[0..3]=ip, [4..5]=port, [6]=target_length, [7..]=target` | host side. `FWU_ERR_INVALID_ARG` if `ip == NULL`, `port == 0`, or the target is invalid. Valid target: non-NULL, length 1..96, first character `'/'`, every character in 0x21..0x7E (`proto_target_valid`, `fwu_proto.c:257`) | `fwu_proto.c:275` |
| `fwu_return_e fwu_proto_parse_pull_start(const fwu_byte_t *payload, size_t payload_length, fwu_proto_pull_start_t *start)` | inverse, `target` aliases the payload | `FWU_ERR_BAD_LENGTH` if `payload_length <= 7`, target length (byte 6) is 0 or > 96, or the total length is not `7 + target_length`; `FWU_ERR_INVALID_ARG` if port is 0 or the target is invalid or `start == NULL` | `fwu_proto.c:299` |
| `fwu_return_e fwu_proto_encode_pull_status(const fwu_proto_pull_status_t *status, fwu_byte_t *out, size_t out_capacity, size_t *out_length)` | `[0]=state, [1..4]=next_offset, [5]=error, [6..7]=http_status` | `FWU_ERR_INVALID_ARG` if `status->state > FWU_PROTO_PULL_FAILED` (the `error` byte is not validated) | `fwu_proto.c:331` |
| `fwu_return_e fwu_proto_parse_pull_status(const fwu_byte_t *payload, size_t payload_length, fwu_proto_pull_status_t *status)` | inverse | length must be 8; `FWU_ERR_INVALID_ARG` if state byte `> FWU_PROTO_PULL_FAILED` | `fwu_proto.c:350` |

The declarations are in `src/session/fwu_proto.h:186-221`.

---

## 6. `fwu_session.h` / `fwu_session.c` - update session (device side)

**Purpose.** Device-side state machine. Processes the frames handed over by the transport (parsed with the frame layer; `fwu_proto.h` messages), downloads the image into the staging area, verifies it with `fwu_verify_image` and takes it up to the commit written through `fwu_activate_commit`. The module knows nothing about UART/BLE/CAN/TCP, the MCU or a timer: time is passed in as `now_ms` (32-bit wraparound handled by unsigned subtraction), response frames are written into a caller buffer, and restarting the MCU is the caller's job (`fwu_session_reboot_requested`). No dynamic memory. One session, one client. No windowed streaming and no resume across a power cycle (resume works only from RAM progress within the same power-up; header "LIMITS").

**Used by / depends on.**
- Depends on (`#include` in `fwu_session.h`): `fwu_error.h`, `fwu_types.h`, `fwu_frame.h`, `fwu_decode.h`, `fwu_flash.h`, `fwu_manifest.h`, `fwu_verify.h`, `fwu_activate.h`, `fwu_proto.h`; `fwu_session.c` also includes `fwu_bytes.h`. Calls: `fwu_frame_encode`, `fwu_flash_read/write/erase/validate`, `fwu_verify_offer`, `fwu_verify_image`, `fwu_activate_staging_writable`, `fwu_activate_commit`, `fwu_proto_encode_*`.
- Used by: `src/transport/fwu_link.c` (calls `fwu_session_on_frame` at line 97 and `fwu_session_poll` at line 112), `src/pull/fwu_pull.[ch]` (uses `fwu_session_touch`, `fwu_session_push_chunk`, `fwu_session_get_state`, `fwu_session_get_next_offset`, `fwu_session_get_image_size`), `src/auth/fwu_auth.h`, `src/transport/fwu_link.h`, `examples/c2000_launchxl_f280049c/application/port/app_fwu.c`, `app_pull.[ch]`.

### 6.1 Macros

| Macro | Value | Meaning | Location |
|---|---|---|---|
| `FWU_SESSION_MANIFEST_BUFFER` | `192u` | Size of the internal manifest buffer (`manifest_raw`); the 138-byte manifest rounded up to the granule must fit (compile-time check `session_manifest_buffer_fits`, `fwu_session.c:12`). | `fwu_session.h:100` |
| `FWU_SESSION_MAX_GRANULE` | `64u` | Largest supported write granule (`fwu_byte_t`). A larger granule gives `FWU_ERR_FLASH_GEOMETRY` in init. | `fwu_session.h:102` |
| `FWU_SESSION_MIN_OUT` | `(FWU_FRAME_OVERHEAD + FWU_PROTO_AUTH_CHALLENGE_MAX)` (= 9 + 64 = 73 with the current frame layout) | Minimum output buffer size for `fwu_session_on_frame` / `fwu_session_poll` (largest response = the challenge). | `fwu_session.h:104` |

### 6.2 Types

```c
typedef enum {
    FWU_SESSION_IDLE          = (int)FWU_PROTO_STATE_IDLE,
    FWU_SESSION_AUTH_PENDING  = (int)FWU_PROTO_STATE_AUTH_PENDING,
    FWU_SESSION_AUTHENTICATED = (int)FWU_PROTO_STATE_AUTHENTICATED,
    FWU_SESSION_DOWNLOADING   = (int)FWU_PROTO_STATE_DOWNLOADING,
    FWU_SESSION_VERIFYING     = (int)FWU_PROTO_STATE_VERIFYING,
    FWU_SESSION_READY         = (int)FWU_PROTO_STATE_READY,
    FWU_SESSION_COMMITTED     = (int)FWU_PROTO_STATE_COMMITTED
} fwu_session_state_e;     /* fwu_session.h:107; values 0..6 */
```

```c
typedef fwu_return_e (*fwu_session_auth_begin_fn)(void *context, fwu_byte_t *challenge, uint32_t capacity, uint32_t *length);
typedef fwu_return_e (*fwu_session_auth_check_fn)(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                                                  const fwu_byte_t *response, uint32_t response_length);
```
Authorization hooks (`fwu_session.h:121-123`). `begin` must write 1..`capacity` elements (the session passes `FWU_PROTO_AUTH_CHALLENGE_MAX` = 64) and set `*length`; the challenge must be single-use. A returned length of 0 or above 64 is turned into `FWU_ERR_INVALID_ARG` by the session (`fwu_session.c:286-288`). `check` returns `FWU_OK` for an authorized response; any other code is forwarded in the ERROR response (a code outside 1..255 becomes `FWU_ERR_CRYPTO_FAILURE`).

```c
typedef struct {
    fwu_session_auth_begin_fn begin;
    fwu_session_auth_check_fn check;
    void                     *context;   /* passed to both hooks */
} fwu_session_auth_t;                    /* fwu_session.h:125 */
```

```c
typedef struct {
    const fwu_flash_t         *flash;               /* flash that holds staging */
    fwu_activate_t            *activate;            /* initialized activation object (commit written through it) */
    const fwu_verify_config_t *verify;              /* offer + image verification; manifest_offset, image_offset (page aligned), max_image_size (multiple of a page) */
    const fwu_session_auth_t  *auth;                /* NULL = no authorization */
    bool                       require_auth;        /* true: auth must not be NULL */
    uint32_t                   running_image_version; /* reported in INFO */
    uint32_t                   chunk_size;          /* multiple of the granule, divides the page size, 1..508 */
    uint32_t                   idle_timeout_ms;     /* idle timeout of a started session, >= 1 */
    uint32_t                   auth_max_failures;   /* >= 1 if auth is present */
    uint32_t                   auth_lockout_ms;     /* >= 1 if auth is present */
    fwu_byte_t                *work;                /* read-back buffer, >= chunk_size elements */
    uint32_t                   work_length;         /* number of work elements */
} fwu_session_config_t;                             /* fwu_session.h:132 */
```
`fwu_session_init` stores a COPY of the structure; the objects it points to (`flash`, `activate`, `verify`, `auth`, `work`) must stay valid for the lifetime of the session.

```c
typedef struct { /* fields: config, initialized, state, page_length, granule, last_activity_ms, image_size,
                    next_offset, last_chunk_offset, last_chunk_length, manifest, manifest_raw, pad,
                    challenge, challenge_length, auth_seq, auth_failures, locked, lock_start_ms,
                    pending_seq, reboot_requested */ } fwu_session_t;   /* fwu_session.h:148 */
```
Caller-allocated; the fields must not be accessed directly (use the getters). The full definition is in the header.

### 6.3 Functions

#### `fwu_session_init`

```c
fwu_return_e fwu_session_init(fwu_session_t *session, const fwu_session_config_t *config);
```
- Responsibility: validates the configuration, copies it, resets the session to `FWU_SESSION_IDLE`, clears the lockout counters.
- Return values (confirmed in `fwu_session.c:150-221`):
  - `FWU_ERR_INVALID_ARG`: `session` or `config` NULL; `flash`, `activate`, `verify` or `work` NULL; `auth` non-NULL with a NULL `begin`/`check` or `auth_max_failures == 0` or `auth_lockout_ms == 0`; `auth == NULL` with `require_auth`; `chunk_size` 0, > 508, not a multiple of the granule, or not dividing `page_length`; `work_length < chunk_size`; `idle_timeout_ms == 0`; `manifest_offset` or `image_offset` not page aligned; `max_image_size` 0 or not a page multiple; one granule-rounded manifest larger than a page; manifest page overlapping the image region.
  - `FWU_ERR_FLASH_GEOMETRY`: `fwu_flash_validate` fails, or granule > `FWU_SESSION_MAX_GRANULE`.
  - `FWU_ERR_OUT_OF_RANGE`: manifest page or image region outside the flash (`fwu_session.c:198-201`). (The header `@return` text lists "out-of-flash regions" under `FWU_ERR_INVALID_ARG`; the code returns `FWU_ERR_OUT_OF_RANGE`.)
  - `FWU_OK`.
- On any error the `initialized` flag is false (it is cleared first, `fwu_session.c:160`), but `session->config` etc. are not populated.
- Precondition: call once before any other function. Does not touch flash contents (only `fwu_flash_validate`).
- `src/session/fwu_session.c:150`

#### `fwu_session_on_frame`

```c
fwu_return_e fwu_session_on_frame(fwu_session_t *session, const fwu_frame_view_t *request, uint32_t now_ms,
                                  fwu_byte_t *out, size_t out_capacity, size_t *out_length);
```
- Responsibility: handles one valid request frame and writes the response frame (`fwu_frame_encode`, same SEQ as the request) into `out`. Protocol errors are not error returns: the return is `FWU_OK` and `*out_length > 0` with a `FWU_MSG_ERROR` frame.
- Parameters: `request` is a frame view from the frame decoder (type, seq, payload pointer, payload length); `now_ms` current time; `out` buffer of at least `FWU_SESSION_MIN_OUT`; `out_length` receives the response length.
- Return values (confirmed): `FWU_OK`; `FWU_ERR_INVALID_ARG` if any pointer is NULL, the session is not initialized, `request->payload` is NULL, `payload_length` is 0 or > `FWU_FRAME_MAX_PAYLOAD`; `FWU_ERR_BUFFER_TOO_SMALL` if `out_capacity < FWU_SESSION_MIN_OUT` (checked before the state is touched, so no state change and no flash operation). If a response frame cannot be built the status of the encoder is returned (not further verified here). `*out_length` is NOT set on the two error returns above (see report).
- Side effects: `last_activity_ms` is set to `now_ms` for every accepted frame, before dispatch (`fwu_session.c:588`), so even HELLO / KEEPALIVE / frames answered with ERROR extend the idle timeout.
- Dispatch (`fwu_session.c:590-611`): HELLO, KEEPALIVE, AUTH_REQ, AUTH_RESP, REQUEST_DOWNLOAD, TRANSFER_DATA, TRANSFER_EXIT, ACTIVATE, ABORT; every other type (including PULL_START / PULL_QUERY and any response type) yields ERROR `FWU_ERR_UNKNOWN_MESSAGE` with no state change.
- Blocking: REQUEST_DOWNLOAD erases the manifest page and writes it (`fwu_flash_erase`, `fwu_flash_write`); TRANSFER_DATA may erase a page and writes + reads back the chunk; AUTH_* call the hooks; ACTIVATE calls `fwu_activate_commit`. All these are synchronous calls; how long they take depends on the flash port and hooks.
- Precondition: `fwu_session_init` returned `FWU_OK`.
- `src/session/fwu_session.c:578`

#### `fwu_session_poll`

```c
fwu_return_e fwu_session_poll(fwu_session_t *session, uint32_t now_ms, fwu_byte_t *out, size_t out_capacity, size_t *out_length);
```
- Responsibility: (1) if the state is VERIFYING, runs `fwu_verify_image(config.verify, config.flash, NULL)` synchronously and writes a `FWU_MSG_VERIFY_RESULT` frame (SEQ = `pending_seq`, the SEQ of the latest TRANSFER_EXIT); on `FWU_OK` the state becomes READY, otherwise the session is reset to IDLE and the result byte is the error code; (2) otherwise applies the idle timeout: in the states AUTH_PENDING, AUTHENTICATED, DOWNLOADING, READY, if `now_ms - last_activity_ms >= idle_timeout_ms` the session is reset to IDLE.
- Return values: `FWU_OK` (`*out_length == 0` means nothing to send); `FWU_ERR_INVALID_ARG` for NULL arguments or an uninitialized session; `FWU_ERR_BUFFER_TOO_SMALL` if `out_capacity < FWU_SESSION_MIN_OUT` (state unchanged). When a VERIFY_RESULT frame is built, the return value is the frame encoder's status.
- Blocking: verification is blocking and can be long (the header says feeding the watchdog is the integration's job). With no pending work the function is fast. No timeout is applied in VERIFYING; COMMITTED and IDLE have none.
- The VERIFY_RESULT error code is passed through `session_code` (`fwu_session.c:23`), so codes outside 1..255 become `FWU_ERR_CRYPTO_FAILURE`.
- `src/session/fwu_session.c:614`

#### `fwu_session_push_chunk`

```c
fwu_return_e fwu_session_push_chunk(fwu_session_t *session, uint32_t offset, const fwu_byte_t *data, size_t length,
                                    uint32_t now_ms, uint32_t *next_offset);
```
- Responsibility: frameless variant of TRANSFER_DATA used by `fwu_pull`; shares `session_store_chunk` (`fwu_session.c:410`) with the TRANSFER_DATA handler, so the rules and error codes are identical. Also sets `last_activity_ms = now_ms`.
- Return values (confirmed in `session_store_chunk`): `FWU_OK` with `*next_offset` updated; `FWU_ERR_INVALID_ARG` for NULL `session` / `data` / `next_offset` or an uninitialized session; `FWU_ERR_BAD_STATE` if the state is not DOWNLOADING; `FWU_ERR_BAD_LENGTH` if `length == 0` or `> chunk_size`, or the end-of-image rule is violated (offset + length beyond the image size, or a short chunk that is not the last one); `FWU_ERR_BAD_BLOCK` if `offset` is not the expected next offset, or an exact repeat of the previous chunk (same offset and length) has different content than flash; flash codes (`FWU_ERR_FLASH_WRITE` for a read-back mismatch, other flash port codes) with the session reset to IDLE. BAD_STATE / BAD_LENGTH / BAD_BLOCK do not change the session.
- Behaviour: an exact repeat of the previous chunk (same offset and length) is re-acknowledged without writing. On the first chunk of a page (`offset % page_length == 0`) the page is erased. Whole granules are written directly; a short tail is padded to the granule with the flash's erased value (`geometry.erased_value`) and written. Every write is read back and compared (`session_write_checked`, `fwu_session.c:111`).
- Blocking: synchronous flash erase/write/read.
- `src/session/fwu_session.c:645`

#### `fwu_session_touch`

```c
void fwu_session_touch(fwu_session_t *session, uint32_t now_ms);
```
- Sets `last_activity_ms = now_ms` (postpones the idle timeout). Does nothing for a NULL or uninitialized session. Used by `fwu_pull` while the device pulls the image and the host sends no frames.
- `src/session/fwu_session.c:655`

#### Query functions

```c
uint32_t fwu_session_get_image_size(const fwu_session_t *session);
uint32_t fwu_session_get_next_offset(const fwu_session_t *session);
uint32_t fwu_session_get_chunk_size(const fwu_session_t *session);
fwu_session_state_e fwu_session_get_state(const fwu_session_t *session);
bool fwu_session_reboot_requested(const fwu_session_t *session);
```

| Function | Returns | NULL session | Location |
|---|---|---|---|
| `fwu_session_get_image_size` | image size of the accepted manifest; 0 when no download (`session_reset` clears it) | 0 | `fwu_session.c:662` |
| `fwu_session_get_next_offset` | number of image bytes written = the expected next offset | 0 | `fwu_session.c:667` |
| `fwu_session_get_chunk_size` | configured `chunk_size` | 0 | `fwu_session.c:672` |
| `fwu_session_get_state` | current state | `FWU_SESSION_IDLE` | `fwu_session.c:677` |
| `fwu_session_reboot_requested` | true after a successful commit (ACTIVATE in READY) | false | `fwu_session.c:682` |

Note: once set by a successful commit, `reboot_requested` is cleared only by `fwu_session_init`; no other code path clears it.

---

## 7. Session state machine

All facts below were read from `src/session/fwu_session.c`. Numbers in parentheses are the permanent state numbers (`FWU_PROTO_STATE_*`, carried in INFO and KEEPALIVE_OK).

### 7.1 States

| State | Number | Meaning |
|---|---|---|
| `FWU_SESSION_IDLE` | 0 | No session. Initial state, and the state reached after abort, failure, or timeout. |
| `FWU_SESSION_AUTH_PENDING` | 1 | Challenge sent, waiting for AUTH_RESP. |
| `FWU_SESSION_AUTHENTICATED` | 2 | Authorization succeeded; REQUEST_DOWNLOAD may follow. |
| `FWU_SESSION_DOWNLOADING` | 3 | Manifest accepted and written; image chunks are being received (also used while `fwu_pull` supplies chunks). |
| `FWU_SESSION_VERIFYING` | 4 | TRANSFER_EXIT accepted; verification is run by the next `fwu_session_poll`. |
| `FWU_SESSION_READY` | 5 | Verification passed; waiting for ACTIVATE. |
| `FWU_SESSION_COMMITTED` | 6 | Commit written (`reboot_requested` = true); waiting for the caller to restart the MCU. In this state ABORT is refused (BAD_STATE); a repeated ACTIVATE is answered with ACTIVATE_OK again, and HELLO / KEEPALIVE are still answered. |

### 7.2 Messages accepted in each state

Cells show the resulting state on success. `-` = ERROR response `FWU_ERR_BAD_STATE`. A failed request leaves the state unchanged unless the note says otherwise. Common length errors (`FWU_ERR_BAD_LENGTH`) are noted per message.

| Message \ State | IDLE | AUTH_PENDING | AUTHENTICATED | DOWNLOADING | VERIFYING | READY | COMMITTED |
|---|---|---|---|---|---|---|---|
| HELLO | INFO, same | INFO, same | INFO, same | INFO, same | INFO, same | INFO, same | INFO, same |
| KEEPALIVE | KEEPALIVE_OK, same | same | same | same | same | same | same |
| AUTH_REQ | AUTH_PENDING (only if `config.auth != NULL`; else `-`) | AUTH_PENDING (same SEQ: same challenge re-sent; different SEQ: new challenge) | - | - | - | - | - |
| AUTH_RESP | - | AUTHENTICATED if hook returns OK; otherwise IDLE | AUTHENTICATED (re-ack if hook returns OK; else `-`) | - | - | - | - |
| REQUEST_DOWNLOAD | DOWNLOADING (only if `config.auth == NULL`; else `-`) | - | DOWNLOADING | DOWNLOADING (same manifest = resume; different manifest = `-`) | - | - | - |
| TRANSFER_DATA | - | - | - | DOWNLOADING (or IDLE on flash error) | - | - | - |
| TRANSFER_EXIT | - | - | - | VERIFYING (needs all bytes received) | VERIFYING (repeat; no re-verify) | READY (VERIFY_RESULT(0) immediately) | - |
| ACTIVATE | - | - | - | - | - | COMMITTED (or IDLE if commit fails) | COMMITTED (repeat; ACTIVATE_OK again) |
| ABORT | IDLE | IDLE | IDLE | IDLE | IDLE | IDLE | - |
| PULL_START / PULL_QUERY and any unknown or response type | `FWU_ERR_UNKNOWN_MESSAGE` in every state (no state change) | | | | | | |

Per-message rules, in the order the code checks them:

- **HELLO** (`fwu_session.c:227`): payload length must be 1 (`FWU_ERR_BAD_LENGTH`); no state check; no state change; answers INFO with the current state.
- **KEEPALIVE** (`:252`): payload length must be 1; answers `FWU_MSG_KEEPALIVE_OK` with `[state]`. No state change (but `last_activity_ms` was refreshed by `fwu_session_on_frame`).
- **AUTH_REQ** (`:261`): (1) `config.auth == NULL` or state not IDLE/AUTH_PENDING -> `FWU_ERR_BAD_STATE`; (2) payload length != 1 -> `FWU_ERR_BAD_LENGTH`; (3) lockout: if `locked` and `now_ms - lock_start_ms < auth_lockout_ms` -> `FWU_ERR_LOCKED_OUT`, otherwise the lock and failure counter are cleared; (4) if state is AUTH_PENDING and the request SEQ equals `auth_seq` -> the same challenge is returned and `begin` is not called; (5) otherwise `begin` is called; a hook error, or a returned length 0 / > 64, resets the session to IDLE and returns the ERROR response; on success the challenge is stored, `auth_seq` = request SEQ, state = AUTH_PENDING, AUTH_CHALLENGE is returned.
- **AUTH_RESP** (`:299`): (1) state not AUTH_PENDING/AUTHENTICATED -> `FWU_ERR_BAD_STATE`; (2) payload length not in 1..128 -> `FWU_ERR_BAD_LENGTH`; (3) `check` hook is called in both states; (4) in AUTHENTICATED: hook OK -> AUTH_OK again (state and counters unchanged), else `FWU_ERR_BAD_STATE` (not counted as a failure); (5) in AUTH_PENDING: hook OK -> failure counter cleared, state AUTHENTICATED, AUTH_OK; hook failure -> `auth_failures++`, if `auth_failures >= auth_max_failures` the lockout starts (`locked`, `lock_start_ms = now_ms`), the session is reset to IDLE (challenge forgotten), ERROR response carries the hook's code.
- **REQUEST_DOWNLOAD** (`:334`): "fresh" = state AUTHENTICATED, or state IDLE with `config.auth == NULL`. (1) not fresh and state != DOWNLOADING -> `FWU_ERR_BAD_STATE`; (2) payload length != `FWU_MANIFEST_SIZE` -> `FWU_ERR_BAD_LENGTH`; (3) if state is DOWNLOADING (resume): payload must equal the stored manifest byte for byte, else `FWU_ERR_BAD_STATE`; on equality DOWNLOAD_ACCEPTED is sent with the current `next_offset`, flash is not touched; (4) if fresh: `fwu_activate_staging_writable` false -> `FWU_ERR_BAD_STATE`; `fwu_verify_offer` error -> that code in ERROR, nothing is written to flash; then the manifest page is erased and the manifest padded to the granule is written and read back; a flash error -> ERROR with the flash code and the state does NOT change (no reset on this path); on success the manifest is stored, `image_size` set, `next_offset`/chunk tracking cleared, `challenge_length` cleared, state = DOWNLOADING and DOWNLOAD_ACCEPTED is sent.
- **TRANSFER_DATA** (`:479`): state must be DOWNLOADING (`FWU_ERR_BAD_STATE`); `fwu_proto_parse_data` failure or data length > `chunk_size` -> `FWU_ERR_BAD_LENGTH`; then `session_store_chunk` (see `fwu_session_push_chunk` above for the codes); success -> DATA_ACK with the new next offset.
- **TRANSFER_EXIT** (`:512`): (1) state not DOWNLOADING/VERIFYING/READY -> `FWU_ERR_BAD_STATE`; (2) payload must be 4 bytes (`FWU_ERR_BAD_LENGTH`) and equal `image_size` (`FWU_ERR_BAD_BLOCK`); (3) in DOWNLOADING, `next_offset` must equal `image_size` (`FWU_ERR_BAD_BLOCK`); (4) READY -> VERIFY_RESULT with code 0 immediately; (5) otherwise `pending_seq` = request SEQ, state = VERIFYING, EXIT_PENDING `[1]` is returned (a repeated EXIT in VERIFYING only refreshes `pending_seq`).
- **ACTIVATE** (`:537`): (1) state not READY/COMMITTED -> `FWU_ERR_BAD_STATE`; (2) payload 4 bytes and equal to `image_size` (`FWU_ERR_BAD_LENGTH` / `FWU_ERR_BAD_BLOCK`); (3) COMMITTED -> ACTIVATE_OK again, commit not repeated; (4) READY -> `fwu_activate_commit(activate, ceil(image_size / page_length))`; failure -> session reset to IDLE and ERROR with the commit's code; success -> state COMMITTED, `reboot_requested` = true, ACTIVATE_OK.
- **ABORT** (`:563`): COMMITTED -> `FWU_ERR_BAD_STATE`; payload length != 1 -> `FWU_ERR_BAD_LENGTH`; otherwise `session_reset` (state IDLE, image size / offsets / challenge cleared) and ABORT_OK. The lockout counters and `reboot_requested` are not cleared. The reason byte is not read. The manifest page and any chunks already written to flash are left as they are.

`session_reset` (`fwu_session.c:74`) sets state IDLE and clears `image_size`, `next_offset`, `last_chunk_offset`, `last_chunk_length`, `challenge_length`. It does not touch `locked`, `auth_failures`, `lock_start_ms`, `reboot_requested`, or flash.

### 7.3 Transitions confirmed in `fwu_session.c`

| From | Trigger | To | Line |
|---|---|---|---|
| IDLE | AUTH_REQ, hook `begin` OK | AUTH_PENDING | 295 |
| AUTH_PENDING | AUTH_REQ (different SEQ), `begin` OK | AUTH_PENDING (new challenge) | 295 |
| AUTH_PENDING | AUTH_REQ, `begin` fails | IDLE | 290 |
| AUTH_PENDING | AUTH_RESP, `check` OK | AUTHENTICATED | 320 |
| AUTH_PENDING | AUTH_RESP, `check` fails | IDLE (failure counter +1, maybe locked) | 330 |
| AUTHENTICATED | REQUEST_DOWNLOAD accepted | DOWNLOADING | 389 |
| IDLE (no auth hook) | REQUEST_DOWNLOAD accepted | DOWNLOADING | 389 |
| DOWNLOADING | TRANSFER_DATA / `fwu_session_push_chunk` flash error (erase, write, read-back I/O error) | IDLE | 468, 434 |
| DOWNLOADING | TRANSFER_EXIT with complete image | VERIFYING | 533 |
| VERIFYING | `fwu_session_poll`, `fwu_verify_image` OK | READY (VERIFY_RESULT code 0) | 629 |
| VERIFYING | `fwu_session_poll`, `fwu_verify_image` fails | IDLE (VERIFY_RESULT code = error) | 632 |
| READY | ACTIVATE, `fwu_activate_commit` OK | COMMITTED | 558 |
| READY | ACTIVATE, `fwu_activate_commit` fails | IDLE | 555 |
| AUTH_PENDING, AUTHENTICATED, DOWNLOADING, READY | `fwu_session_poll`: `now_ms - last_activity_ms >= idle_timeout_ms` | IDLE | 639-640 |
| any except COMMITTED | ABORT | IDLE | 572 |
| (init) | `fwu_session_init` OK | IDLE | 218 |

There is no code path out of COMMITTED other than a reset of the whole device (no function sets the state back; `fwu_session_init` would reinitialize).

### 7.4 Timeouts and lockout

- Idle timeout (`idle_timeout_ms`): applied only by `fwu_session_poll`, only in AUTH_PENDING, AUTHENTICATED, DOWNLOADING, READY. Not applied in IDLE, VERIFYING (verification takes time), COMMITTED. Every frame accepted by `fwu_session_on_frame`, `fwu_session_push_chunk`, `fwu_session_touch`, and the completion of a verification refresh `last_activity_ms`.
- Authorization lockout: `auth_max_failures` consecutive wrong AUTH_RESP (counted only while the state is AUTH_PENDING) start a lock for `auth_lockout_ms`, enforced in AUTH_REQ. The counter is cleared by a correct AUTH_RESP or when AUTH_REQ finds the lock expired. `fwu_session_init` is the only place that resets the lock besides expiry.

### 7.5 Error propagation to the host

- Every protocol-level failure is an `FWU_MSG_ERROR` frame `[code(1)][request_type(1)]` with the request SEQ; the C function returns `FWU_OK`.
- `code` is the `fwu_return_e` value, forced into 1..255 (`session_code`); a hook that returns a value outside 1..255 appears as `FWU_ERR_CRYPTO_FAILURE`.
- VERIFY_RESULT carries the code returned by `fwu_verify_image` (passed through `session_code`), never an ERROR frame. Which codes `fwu_verify_image` can return is documented with the verify module.
- Transport errors (bad frame, CRC) are handled below the session by the parser and never reach `fwu_session_on_frame`.
