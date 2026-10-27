# API reference: transport, link, ISO-TP, link simulator, host client

Scope: `src/transport/fwu_transport.h`, `src/transport/fwu_link.[ch]`, `src/transport/fwu_isotp.[ch]`,
`src/transport/fwu_linksim.[ch]`, `src/client/fwu_client.[ch]`.
Everything below was taken from the headers and confirmed against the `.c` files. Items that could not be confirmed are
marked `UNVERIFIED`. Line numbers refer to the repository state at the time of writing.

Error codes referenced (from `src/error/fwu_error.h`): `FWU_OK`, `FWU_ERR_INVALID_ARG` (10), `FWU_ERR_BUFFER_TOO_SMALL` (11),
`FWU_ERR_BAD_STATE` (12), `FWU_ERR_BUFFER_FULL` (21), `FWU_ERR_NOT_ENOUGH_DATA` (23), `FWU_ERR_BAD_LENGTH` (31),
`FWU_ERR_BAD_FRAME` (32), `FWU_ERR_BAD_BLOCK` (61), `FWU_ERR_PULL_PROTOCOL` (80).

---------------------------------------------------------------------------------------------------------------------

## 1. Transport contract (`fwu_transport.h`)

**Purpose.** Defines (1) the interface the device side uses to talk to a transport, `fwu_transport_t`, and (2) the profile
(constants) of each supported transport (UART, TCP, BLE, CAN) so that host and device share one source of truth. The frame
(`fwu_frame.h`) and message (`fwu_proto.h`) layers are identical on every transport; a transport only carries bytes and is
never trusted (decision D16 in the header). The frame layer resynchronizes and checks the CRC, so the transport need not be
contiguous, ordered or complete: loss, duplication and corruption are recovered by the host retransmission.

Used by / depends on: includes `fwu_error.h`, `fwu_types.h`. Included by `fwu_link.h`, `fwu_isotp.h`, `src/pull/fwu_pull.h`;
used in `examples/c2000_launchxl_f280049c/application/port/app_fwu.c` (fills a `fwu_transport_t`).

### 1.1 Types

```c
typedef fwu_return_e (*fwu_transport_send_fn)(void *context, const fwu_byte_t *data, size_t length);
typedef void (*fwu_transport_poll_fn)(void *context, uint32_t now_ms);

typedef struct {
    fwu_transport_send_fn send;     /* Mandatory. */
    fwu_transport_poll_fn poll;     /* Optional (NULL = none). The link calls it at the start of every poll. */
    void                 *context;  /* Passed to both functions. */
} fwu_transport_t;
```
Source: `src/transport/fwu_transport.h:54`, `:57`, `:60`.

**`send` semantics** (`fwu_transport.h:44-53`; the only caller in this scope is `link_flush`, `fwu_link.c:10`):

| Return value | Meaning | Caller reaction (in `fwu_link`) |
|---|---|---|
| `FWU_OK` | The transport accepted the COMPLETE frame (copied it into its own buffer); the caller may reuse its buffer. | `frames_out++`, pending response cleared. |
| `FWU_ERR_BUFFER_FULL` | The transport is busy now (TX buffer full / previous message in progress). NOTHING was accepted. | `tx_busy++`; the same frame is retried on the next poll. |
| any other code | Send failed (no connection, driver error). | `tx_errors++`; the frame is dropped; the host recovers by timeout and retransmission. |

`send` takes the whole frame or nothing: there is no partial acceptance. The data pointer is only valid during the call.
Whether `send` may block is not specified by the contract; `fwu_link_poll` itself adds no waiting.

**`poll` semantics:** optional, called with the current `now_ms` at the start of every `fwu_link_poll`, for the transport's
time-dependent work (the header names ISO-TP timers and consecutive-frame transmission as examples). The RX direction is
NOT part of this structure: the transport driver pushes received bytes into a ring buffer.

### 1.2 Macros (transport profiles)

| Macro | Value | Meaning |
|---|---|---|
| `FWU_TRANSPORT_UART_BAUD_DEFAULT` | `115200u` | Default baud (configurable). |
| `FWU_TRANSPORT_UART_DATA_BITS` | `8u` | |
| `FWU_TRANSPORT_UART_PARITY_NONE` | `1` | No parity (the value is as written in the header; UNVERIFIED: how a driver maps it). |
| `FWU_TRANSPORT_UART_STOP_BITS` | `1u` | |
| `FWU_TRANSPORT_BLE_SERVICE_UUID` | `"795c2cc5-f830-41e4-b8db-3f18978bb05f"` | GATT service (randomly generated for this project). |
| `FWU_TRANSPORT_BLE_RX_CHAR_UUID` | `"f956ae27-636f-4988-aa5a-670216e418ba"` | Host -> device, "write without response". |
| `FWU_TRANSPORT_BLE_TX_CHAR_UUID` | `"7dd31ef7-1bb3-46c5-a253-47fd51e0e5bd"` | Device -> host, "notify" (host must enable the subscription). |
| `FWU_TRANSPORT_BLE_DEFAULT_CHUNK` | `20u` | ATT_MTU 23 - 3; grows with MTU negotiation (at most MTU - 3). |
| `FWU_TRANSPORT_CAN_REQUEST_ID` | `0x5F0u` | Host -> device, 11-bit. **PLACEHOLDER**, overridable with `-D` (guarded by `#ifndef`). |
| `FWU_TRANSPORT_CAN_RESPONSE_ID` | `0x5F1u` | Device -> host, 11-bit. **PLACEHOLDER**, overridable with `-D`. |
| `FWU_TRANSPORT_CAN_DLC` | `8u` | Every CAN frame is 8 bytes (short content padded). |
| `FWU_TRANSPORT_CAN_PAD_BYTE` | `0xCCu` | Padding byte. |
| `FWU_TRANSPORT_CAN_BLOCK_SIZE` | `8u` | Block size announced by the receiver in each flow control (0 = unlimited). |
| `FWU_TRANSPORT_CAN_ST_MIN_MS` | `2u` | Consecutive-frame spacing requested by the receiver (ms, 0..127). |
| `FWU_TRANSPORT_CAN_TIMEOUT_MS` | `1000u` | N_Bs / N_Cr: wait for flow control or for a consecutive frame. |

Source: `fwu_transport.h:65-68`, `:77-83`, `:89-99`.

**The CAN identifiers are placeholders.** The header states they are PLACEHOLDERS marked unverified: only the owner of the
real network knows which identifiers are unused; OBD/UDS diagnostic identifiers such as 0x7E0/0x7E8 were deliberately not used.
If `FWU_TRANSPORT_USE_CAN` and `FWU_PRODUCTION` are defined and `FWU_TRANSPORT_CAN_IDS_VERIFIED` is not, the header stops the
build with `#error` (`fwu_transport.h:101-103`). Choose free identifiers and build with `-DFWU_TRANSPORT_CAN_IDS_VERIFIED`.

Profile notes taken from the header comments (not enforced by code in this scope): UART/BLE share the UART profile on the MCU side
(BLE reaches the MCU through a bridge that forwards bytes unchanged and does not preserve frame boundaries); TCP is a byte stream
with a single client and NO default port; UART has no flow control because the host works stop-and-wait.

---------------------------------------------------------------------------------------------------------------------

## 2. Device link layer (`fwu_link.h`, `fwu_link.c`)

**Purpose.** Runs the loop ring buffer -> parser -> session -> transport in one place so that MCU integration reduces to
"the ISR pushes bytes into the ring buffer, call `fwu_link_poll` from the main loop". The module never reads bytes itself;
the driver pushes into the ring buffer the parser is attached to (single-producer / single-consumer rule of `fwu_ringbuf.h`).

Used by / depends on: includes `fwu_error.h`, `fwu_types.h`, `fwu_frame.h`, `fwu_parser.h`, `fwu_session.h`, `fwu_transport.h`.
Used by `src/transport/test_world.h`, `examples/c2000_launchxl_f280049c/application/port/app_fwu.[ch]` and `app_pull.[ch]`
(`fwu_link_set_extension`).

### 2.1 How the link couples the layers

`fwu_link_poll` (`fwu_link.c:65-120`), executed on every call, non-blocking:

1. If `transport->poll != NULL`, call it with `now_ms`.
2. `link_flush`: if a response is pending, call `transport->send`. On `FWU_ERR_BUFFER_FULL` the response is held (same bytes
   retried next poll); on any other error it is dropped.
3. If a response is still pending after step 2, return `FWU_OK` immediately: NO new request is processed while a response is
   pending (stop-and-wait order is preserved).
4. Up to `FWU_LINK_MAX_FRAMES_PER_POLL` times: `fwu_parser_poll`. `FWU_ERR_NOT_ENOUGH_DATA` ends the loop; any other non-OK
   status is returned to the caller. Each frame increments `frames_in`, is routed to the extension (if `ext_handles(view.type)`)
   or to `fwu_session_on_frame`, writes its response directly into `link->pending`, and is flushed immediately. If the flush leaves
   the response pending, `fwu_link_poll` returns `FWU_OK` and leaves remaining frames for the next poll.
5. When the loop ends without a pending response, `fwu_session_poll` is called (verification result, timeouts) and its output is flushed.

Error propagation: a non-OK status from the parser, the extension or the session is returned unchanged by `fwu_link_poll`
(the header: usage errors; protocol errors are ERROR response frames, not return codes). A response that was already generated and
could not be sent because of a hard transport error is dropped, never retried.

### 2.2 Reboot-ready rule

`fwu_link_reboot_ready(link)` is true when all of: `link != NULL`, `link->initialized`, `fwu_session_reboot_requested(session)`,
and `pending_length == 0` (`fwu_link.c:122-126`). "Delivered" means the transport accepted the frame, NOT that it has left the
wire; waiting for the hardware TX to drain (UART FIFO, BLE bridge) is the integrator's job (the C2000 example does this in
`app_fwu_reboot`, `app_fwu.c:130`). See also 2.5 for the dropped-response edge case.

### 2.3 Extension hook (used by the pull module)

`fwu_link_set_extension` installs a pair of callbacks that take over a set of frame types from the session. `ext_handles` is a
pure query on the frame type; `ext_on_frame` has the same contract as `fwu_session_on_frame` and writes its response into the link's
pending buffer (capacity `FWU_FRAME_MAX_SIZE`), so the response uses the same pending/BUFFER_FULL retry path. The pull module
supplies `fwu_pull_handles` and `fwu_pull_on_frame` (`src/pull/fwu_pull.h`); the example wires them in `app_pull.c` (`pull_handles`,
`pull_on_frame`, `fwu_link_set_extension(link, pull_handles, pull_on_frame, NULL)`). With no extension (default after init) behaviour
is unchanged. Passing both handlers `NULL` removes the extension.

### 2.4 Macros and types

```c
#define FWU_LINK_MAX_FRAMES_PER_POLL    4u

typedef struct {
    uint32_t frames_in;     /* frames taken from the parser and given to the session (or extension) */
    uint32_t frames_out;    /* responses accepted by the transport */
    uint32_t tx_busy;       /* attempts for which the transport returned BUFFER_FULL */
    uint32_t tx_errors;     /* responses dropped because of a send error */
} fwu_link_stats_t;

typedef bool (*fwu_link_ext_handles_fn)(fwu_byte_t type);
typedef fwu_return_e (*fwu_link_ext_on_frame_fn)(void *context, const fwu_frame_view_t *view, uint32_t now_ms,
                                                 fwu_byte_t *out, size_t out_capacity, size_t *out_length);

typedef struct {
    fwu_session_t         *session;     /* initialized session */
    fwu_parser_t          *parser;      /* initialized parser (attached to the ring buffer) */
    const fwu_transport_t *transport;   /* send is mandatory */
} fwu_link_config_t;

typedef struct {
    fwu_link_config_t config;
    fwu_byte_t        pending[FWU_FRAME_MAX_SIZE];
    size_t            pending_length;               /* 0 = nothing pending */
    fwu_link_stats_t  stats;
    bool              initialized;
    fwu_link_ext_handles_fn  ext_handles;
    fwu_link_ext_on_frame_fn ext_on_frame;
    void                    *ext_context;
} fwu_link_t;
```
Source: `fwu_link.h:46`, `:49-54`, `:57`, `:60-61`, `:64-68`, `:71-80`. Counters are 32-bit and wrap. `init` stores a COPY of
`fwu_link_config_t`; the objects it points to (session, parser, transport) must outlive the link. Do not touch `fwu_link_t` fields
except `stats` (read-only).

### 2.5 Functions

```c
fwu_return_e fwu_link_init(fwu_link_t *link, const fwu_link_config_t *config);
```
`fwu_link.h:88`, `fwu_link.c:31`. Initializes the link, clears pending and counters and any extension.
Returns: `FWU_OK`; `FWU_ERR_INVALID_ARG` if `link`/`config` is NULL, or `session`, `parser`, `transport` or `transport->send` is NULL.
Note: `initialized` is set to false first, so a failed re-init leaves the link unusable.

```c
fwu_return_e fwu_link_set_extension(fwu_link_t *link, fwu_link_ext_handles_fn handles, fwu_link_ext_on_frame_fn on_frame, void *context);
```
`fwu_link.h:95`, `fwu_link.c:54`. Call AFTER init. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` if `link` is NULL, not initialized, or exactly one of
`handles`/`on_frame` is NULL. `context` is stored only if `handles != NULL`.

```c
fwu_return_e fwu_link_poll(fwu_link_t *link, uint32_t now_ms);
```
`fwu_link.h:103`, `fwu_link.c:65`. See 2.1. `now_ms` supports 32-bit wrap. Non-blocking itself (it calls the transport `send`/`poll`, the parser,
the extension and the session; their blocking behaviour is outside this scope; `fwu_session_on_frame` may do flash work, UNVERIFIED here).
Returns: `FWU_OK`; `FWU_ERR_INVALID_ARG` for NULL or uninitialized link; otherwise any non-OK status of `fwu_parser_poll` (except
`FWU_ERR_NOT_ENOUGH_DATA`, which is absorbed), of the extension `on_frame`, or of `fwu_session_on_frame` / `fwu_session_poll` (the exact set
is defined by those modules; not verified here).

```c
bool fwu_link_reboot_ready(const fwu_link_t *link);
```
`fwu_link.h:106`, `fwu_link.c:122`. A query. See 2.2. Returns false for NULL.

```c
bool fwu_link_tx_pending(const fwu_link_t *link);
```
`fwu_link.h:109`, `fwu_link.c:128`. A query: true if a response is waiting to be sent. Returns false for NULL (note: unlike `reboot_ready` it does not check `initialized`).

```c
const fwu_link_stats_t *fwu_link_get_stats(const fwu_link_t *link);
```
`fwu_link.h:112`, `fwu_link.c:133`. Returns a pointer to the counters inside the link, NULL if `link` is NULL.

**Edge case (see report):** after a hard `send` error the response is dropped and `pending_length` becomes 0, so
`fwu_link_reboot_ready` can become true although the last response never reached the transport. The header says "delivered".

---------------------------------------------------------------------------------------------------------------------

## 3. ISO-TP subset for CAN (`fwu_isotp.h`, `fwu_isotp.c`)

**Purpose.** A SUBSET of ISO 15765-2 for classic CAN: splits an fwu frame (at most 521 bytes) into 8-byte CAN frames for sending, and
reassembles incoming CAN frames into complete messages. Bidirectional: the same module is used on the device and on the host (the
DLL of the host application). Hardware independent: it sends through a "send 8 bytes" hook and receives through `fwu_isotp_on_can_frame`.
The header states the subset is NOT verified against the official text of ISO 15765-2; the format follows the commonly known implementation.
The CAN identifiers are NOT handled here (see the placeholder note in section 1.2).

Used by / depends on: includes `fwu_error.h`, `fwu_types.h`, `fwu_frame.h`, `fwu_transport.h`. Used by `src/client/test_client.c` and
`src/transport/test_isotp.c`. No production call site found in `examples/` or `ports/` (grep).

### 3.1 Supported subset and limits

| Item | Value (from header/code) |
|---|---|
| CAN frame | classic CAN, normal addressing, always 8 bytes; short content padded with `FWU_TRANSPORT_CAN_PAD_BYTE` |
| SF (single frame) | `[0x0 \| length(1..7)] [data...]`; send uses SF when `tx_length <= 7` |
| FF (first frame) | `[0x1 \| len>>8] [len & 0xFF] [6 data bytes]`; length 8..4095 in ISO, here `<= FWU_ISOTP_MAX_MESSAGE` (521); FF with total `<= 7` rejected (`FWU_ERR_BAD_LENGTH`) |
| CF (consecutive frame) | `[0x2 \| SN] [7 data bytes]`; SN starts at 1 and wraps `15 -> 0` |
| FC (flow control) | `[0x3 \| FS] [BS] [STmin]`; FS 0 = CTS, 1 = WAIT, 2 = OVFLW; any other FS: `FWU_ERR_BAD_FRAME` |
| STmin decoding (sender side) | 0..127 = ms; 0xF1..0xF9 rounded up to 1 ms; any other value = 127 ms |
| Concurrent transmissions | ONE (stop-and-wait); `fwu_isotp_send` returns `FWU_ERR_BUFFER_FULL` while busy |
| Buffers | two 521-byte buffers (tx, rx); no dynamic memory |
| Not supported / not present | extended or mixed addressing, CAN FD, 12-bit-escape FF (length > 4095), N_As/N_Ar timers, multiple concurrent messages (UNVERIFIED as "not supported by design"; simply absent from the code) |

Receiver rules (`fwu_isotp.c:249-395`): a new SF delivers its message and aborts any reassembly in progress; a new FF aborts the old reassembly and
starts a new one (FF > 521 additionally sends FC(OVFLW) and returns `FWU_ERR_BAD_LENGTH`); wrong SN or short CF aborts the reassembly; a CF
without an active reassembly returns `FWU_ERR_BAD_STATE`; a reassembly with no CF for `timeout_ms` is aborted in `fwu_isotp_poll`. FC is sent
on FF (CTS), and again every `block_size` CFs when `block_size > 0`. A FC that cannot be sent (`FWU_ERR_BUFFER_FULL`) is held and retried in
`fwu_isotp_poll`; on another error it is dropped (the sender gives up by timeout).
Sender rules (`isotp_tx_step`, `fwu_isotp.c:94-184`): after FF, wait for FC up to `timeout_ms` (counted as `tx_timeouts`, then abort); FC(WAIT)
accepted up to `wft_max` times (each restarts the timer), then abort; FC(OVFLW) aborts; CFs are sent with STmin spacing, at most
`FWU_ISOTP_MAX_CF_PER_POLL` per step; when `BS` is exhausted the sender waits for a new FC.

### 3.2 Macros

| Macro | Value | Meaning |
|---|---|---|
| `FWU_ISOTP_MAX_MESSAGE` | `FWU_FRAME_MAX_SIZE` (521) | Largest message. |
| `FWU_ISOTP_CAN_DLC` | `8u` | Data length of every CAN frame. |
| `FWU_ISOTP_MAX_CF_PER_POLL` | `8u` | Maximum CFs sent in one step. |
| `FWU_ISOTP_DEFAULT_WFT_MAX` | `10u` | Suggested maximum consecutive FC(WAIT) (it is only a constant; `wft_max` comes from the config). |

Source: `fwu_isotp.h:51-54`.

### 3.3 Types

```c
typedef fwu_return_e (*fwu_isotp_can_send_fn)(void *context, const fwu_byte_t *data);
typedef void (*fwu_isotp_deliver_fn)(void *context, const fwu_byte_t *message, size_t length);

typedef struct {
    uint32_t rx_messages, tx_messages, rx_errors, rx_aborts, rx_timeouts,
             tx_timeouts, tx_aborts, cf_sent, fc_sent;      /* each a separate uint32_t member */
} fwu_isotp_stats_t;

typedef struct {
    fwu_isotp_can_send_fn can_send;     /* mandatory */
    fwu_isotp_deliver_fn  deliver;      /* mandatory */
    void                 *context;      /* passed to both hooks */
    uint32_t              block_size;   /* BS announced as receiver (0..255; 0 = unlimited) */
    uint32_t              st_min_ms;    /* STmin announced as receiver (0..127 ms) */
    uint32_t              timeout_ms;   /* N_Bs / N_Cr timeout (>= 1) */
    uint32_t              wft_max;      /* max consecutive FC(WAIT) accepted */
} fwu_isotp_config_t;

typedef struct { /* internal state; see header lines 85-112 for all members */ } fwu_isotp_t;
```
Source: `fwu_isotp.h:62`, `:64`, `:67-77` (stats), `:79-87` (config), `:90-117` (state). `fwu_isotp_stats_t` members, in declaration order:
`rx_messages`, `tx_messages`, `rx_errors`, `rx_aborts`, `rx_timeouts`, `tx_timeouts`, `tx_aborts`, `cf_sent`, `fc_sent`. `fwu_isotp_t` fields must not be
touched directly except `stats`.

Hook contracts:
- `can_send(context, data)`: send ONE 8-byte frame with the TX identifier. `FWU_OK` = accepted; `FWU_ERR_BUFFER_FULL` = mailbox full, retried later
  (in the implementation: the first frame/CF is retried on the next step, an FC is retried in poll); any other code = hard error (tx aborted for SF/FF/CF; FC dropped).
- `deliver(context, message, length)`: called when a complete message is available. `message` is valid only during the call (copy it). For an SF it
  points to a local array; for an FF-started message it points into the internal rx buffer. It is called from `fwu_isotp_on_can_frame`, i.e. from the
  caller's context (ISR if `on_can_frame` is called from an ISR).

### 3.4 Functions

```c
fwu_return_e fwu_isotp_init(fwu_isotp_t *iso, const fwu_isotp_config_t *config);
```
`fwu_isotp.h:123`, `fwu_isotp.c:188`. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` for NULL `iso`/`config`, NULL `can_send` or `deliver`, `block_size > 255`,
`st_min_ms > 127`, `timeout_ms == 0`. A copy of the config is stored. `wft_max` is not range-checked.

```c
fwu_return_e fwu_isotp_send(fwu_isotp_t *iso, const fwu_byte_t *data, size_t length, uint32_t now_ms);
```
`fwu_isotp.h:129`, `fwu_isotp.c:231`. Copies the message into the tx buffer and tries to send the first frame immediately. Returns `FWU_OK` (including when the first
frame hit `FWU_ERR_BUFFER_FULL`: the message is accepted and the frame is retried in `fwu_isotp_poll`; for a multi-frame message `FWU_OK` only means the transmission
started, not that it completed); `FWU_ERR_INVALID_ARG` for NULL, uninitialized, `length == 0` or `length > 521`; `FWU_ERR_BUFFER_FULL` if a transmission is in progress; or the
hard error code returned by `can_send` for the first frame (the transmission is then aborted and counted in `tx_aborts`).

```c
fwu_return_e fwu_isotp_on_can_frame(fwu_isotp_t *iso, const fwu_byte_t *data, size_t length, uint32_t now_ms);
```
`fwu_isotp.h:136`, `fwu_isotp.c:249`. Processes one received CAN frame (`length` 1..8). Must be called from the same context as `fwu_isotp_poll` (header). Returns:

| Code | When |
|---|---|
| `FWU_OK` | Frame processed (also for FC(WAIT), FC(OVFLW) and an accepted CTS). |
| `FWU_ERR_INVALID_ARG` | NULL pointer, uninitialized, `length == 0` or `length > 8`. |
| `FWU_ERR_BAD_LENGTH` | SF length field 0 or > 7; FF total <= 7; FF total > 521. |
| `FWU_ERR_BAD_FRAME` | SF/CF content longer than the frame; FF shorter than 8 bytes; FC shorter than 3 bytes; reserved FS value; unknown PCI type. |
| `FWU_ERR_BAD_STATE` | CF with no reassembly active; CF with wrong SN (also aborts the reassembly); FC when not waiting for one. |

Protocol violations are dropped and counted in `rx_errors`; the header says an ISR may ignore these returns. May invoke `deliver`, may call `can_send`.

```c
fwu_return_e fwu_isotp_poll(fwu_isotp_t *iso, uint32_t now_ms);
```
`fwu_isotp.h:139`, `fwu_isotp.c:397`. Time-dependent work: rx timeout, retry of a held FC, one `isotp_tx_step` (first frame retry, FC wait timeout, up to 8 CFs). Returns
`FWU_OK`, or `FWU_ERR_INVALID_ARG` for NULL/uninitialized. Hard `can_send` errors inside the step are not returned (they abort the tx and count in `tx_aborts`).

```c
bool fwu_isotp_tx_busy(const fwu_isotp_t *iso);
const fwu_isotp_stats_t *fwu_isotp_get_stats(const fwu_isotp_t *iso);
```
`fwu_isotp.h:142`, `:145`; `fwu_isotp.c:419`, `:424`. `tx_busy` is true while a transmission is in progress (false for NULL/uninitialized). `get_stats` returns NULL for NULL.

```c
fwu_return_e fwu_isotp_transport_send(void *context, const fwu_byte_t *data, size_t length);
void fwu_isotp_transport_poll(void *context, uint32_t now_ms);
```
`fwu_isotp.h:148`, `:149`; `fwu_isotp.c:429`, `:439`. Adapters matching `fwu_transport_send_fn` / `fwu_transport_poll_fn`, with `context` = `fwu_isotp_t *`.
`transport_send` returns `FWU_ERR_INVALID_ARG` if `context` is NULL, otherwise the result of `fwu_isotp_send(iso, data, length, iso->now_ms)`, i.e. it uses the last known
time stored in the state (updated by `poll`, `on_can_frame` and `send`), so `fwu_link_poll` (which calls the transport `poll` first) keeps it current. `FWU_ERR_BUFFER_FULL`
maps directly onto the transport contract of section 1.1.

---------------------------------------------------------------------------------------------------------------------

## 4. Lossy link simulator (`fwu_linksim.h`, `fwu_linksim.c`)

**Purpose.** TEST/PC only. Imitates a one-directional channel and applies deterministic, seed-repeatable faults to every chunk of bytes sent (usually one
frame), so that the client + transport + session chain can be exercised under bad conditions. Not secure; the random generator is a linear congruential
generator (`fwu_linksim.c:10`). No dynamic memory.

Used by / depends on: includes `fwu_error.h`, `fwu_types.h`. Used by `src/transport/test_linksim.c`, `src/client/test_client.c`, `src/auth/test_auth.c`.

Faults (per-mille, 0..1000 each, independent per send): drop, duplicate (second copy with its own delay), corrupt (flip 1..3 random bits), truncate (cut to a
random length >= 1), delay (1..`delay_max_ms`; different delays reorder chunks), garbage (1..8 random bytes prepended). Every delivered chunk gets at
least 1 ms of delay (`fwu_linksim.c:67`).

### 4.1 Macros and types

```c
#define FWU_LINKSIM_QUEUE       128u    /* maximum chunks in flight */
#define FWU_LINKSIM_CHUNK_MAX   640u    /* maximum chunk size */

typedef struct {
    uint32_t drop_pm;  uint32_t duplicate_pm;  uint32_t corrupt_pm;  uint32_t truncate_pm;
    uint32_t delay_pm; uint32_t garbage_pm;    uint32_t delay_max_ms;   /* delay_max_ms >= 1 if delay_pm > 0 */
} fwu_linksim_profile_t;   /* each member is a separate uint32_t */

typedef struct {
    uint32_t sent, dropped, duplicated, corrupted, truncated, delayed, garbage, delivered, queue_overflow;
} fwu_linksim_stats_t;     /* each member is a separate uint32_t */

typedef struct { uint32_t deliver_ms; size_t length; fwu_byte_t data[FWU_LINKSIM_CHUNK_MAX]; } fwu_linksim_chunk_t;
typedef struct { /* profile, rng, queue[], count, order[], next_order, stats */ } fwu_linksim_t;
```
Source: `fwu_linksim.h:32-33`, `:36-44`, `:46-56`, `:58-62`, `:64-72`.

### 4.2 Functions

```c
fwu_return_e fwu_linksim_init(fwu_linksim_t *sim, const fwu_linksim_profile_t *profile, uint32_t seed);
fwu_return_e fwu_linksim_set_profile(fwu_linksim_t *sim, const fwu_linksim_profile_t *profile);
```
`fwu_linksim.h:75`, `:78`; `fwu_linksim.c:34`, `:55`. Return `FWU_OK`; `FWU_ERR_INVALID_ARG` for NULL, any probability > 1000, or `delay_pm > 0` with
`delay_max_ms == 0`. `init` also seeds the generator, empties the queue and zeroes stats; `set_profile` only replaces the profile.

```c
fwu_return_e fwu_linksim_send(fwu_linksim_t *sim, const fwu_byte_t *data, size_t length, uint32_t now_ms);
```
`fwu_linksim.h:84`, `fwu_linksim.c:88`. Puts a chunk on the line. `length` 1..`FWU_LINKSIM_CHUNK_MAX - 8` (632). Returns `FWU_OK` even if the chunk was dropped (loss is silent,
including a full queue, which counts `queue_overflow` and `dropped`); `FWU_ERR_INVALID_ARG` for NULL `sim`/`data` or an invalid length. Does not block.

```c
size_t fwu_linksim_receive(fwu_linksim_t *sim, uint32_t now_ms, fwu_byte_t *out, size_t capacity);
```
`fwu_linksim.h:90`, `fwu_linksim.c:133`. Returns the oldest chunk whose delivery time has arrived (ties broken by insertion order; wrap-safe comparison). Returns the number of
bytes copied; 0 means nothing due, or the chunk did not fit in `capacity` (then the chunk is DROPPED and removed; the two cases are indistinguishable from the return value).
Returns 0 for NULL `sim`/`out`.

```c
uint32_t fwu_linksim_in_flight(const fwu_linksim_t *sim);
```
`fwu_linksim.h:93`, `fwu_linksim.c:170`. Number of chunks currently queued; 0 for NULL.

---------------------------------------------------------------------------------------------------------------------

## 5. Host update client (`fwu_client.h`, `fwu_client.c`)

**Purpose.** Host-side client that talks to the device session (`fwu_session`) and uploads an image end to end. Transport independent. Handles timeouts, retransmission,
resume after a disconnect and restart after the device session is lost. Written in C so the host application can wrap it as a DLL. It is a pull-style API (mirror of
`fwu_session`): the caller sends whatever `fwu_client_poll` returns and feeds every received frame to `fwu_client_on_frame`. The client does NOT send or receive bytes itself.

Used by / depends on: includes `fwu_error.h`, `fwu_types.h`, `fwu_frame.h`, `fwu_decode.h`, `fwu_manifest.h`, `fwu_proto.h`. Used by `src/client/test_client.c`, `src/auth/test_auth.c`,
`src/pull/test_pull.c`. The auth hook type is implemented by `fwu_auth` (`src/auth/fwu_auth.h` mentions `fwu_client_auth_fn`).

### 5.1 Usage sequence

```
fwu_client_init(&c, &config);   fwu_client_start(&c, now);
loop:  fwu_client_poll(&c, now, out, sizeof(out), &n)  -> if n > 0 send out[0..n)   (out_capacity >= FWU_FRAME_MAX_SIZE)
       for each received frame (from the parser):  fwu_client_on_frame(&c, &view, now)
       until fwu_client_is_finished(&c);  then fwu_client_get_result(&c, &result)
```
Single session, single outstanding request, no windowed streaming.

### 5.2 State machine

Every request carries a new SEQ (`next_seq`, starts at 1, wraps at 256) and is stored in `req_frame`. A response is accepted only if its SEQ equals `req_seq` AND its type is `ERROR`, or
`req_type | FWU_PROTO_RESPONSE_FLAG`, or (while `req_type` is `FWU_MSG_TRANSFER_EXIT`) `FWU_MSG_EXIT_PENDING` / `FWU_MSG_VERIFY_RESULT` (`client_type_ok`, `fwu_client.c:134`). Anything else
(or any frame while not awaiting) increments `ignored` and is dropped.

States (`fwu_client_state_e`, values in declaration order starting at 0):

| State | Value | Waiting for / activity |
|---|---|---|
| `FWU_CLIENT_IDLE` | 0 | Not started. |
| `FWU_CLIENT_HELLO` | 1 | HELLO response (INFO). |
| `FWU_CLIENT_AUTH_REQ` | 2 | AUTH_CHALLENGE. |
| `FWU_CLIENT_AUTH_RESP` | 3 | AUTH_OK. |
| `FWU_CLIENT_REQUEST` | 4 | DOWNLOAD_ACCEPTED (chunk size, next offset). |
| `FWU_CLIENT_TRANSFER` | 5 | DATA_ACK for the chunk just sent. |
| `FWU_CLIENT_EXIT` | 6 | EXIT_PENDING (or VERIFY_RESULT). |
| `FWU_CLIENT_VERIFY_WAIT` | 7 | VERIFY_RESULT. |
| `FWU_CLIENT_ACTIVATE` | 8 | ACTIVATE_OK. |
| `FWU_CLIENT_ABORTING` | 9 | ABORT_OK. |
| `FWU_CLIENT_DONE` | 10 | Finished successfully. |
| `FWU_CLIENT_FAILED` | 11 | Finished with a failure (`failure` field). |
| `FWU_CLIENT_PULL_START` | 12 | (pull mode) PULL_STARTED. |
| `FWU_CLIENT_PULL` | 13 | (pull mode) device is pulling; a PULL_QUERY is issued every `pull_poll_ms`. |

Transitions (`client_handle_response`, `fwu_client.c:209-335`):

```
IDLE --start--> HELLO
HELLO --INFO ok--> [AUTH_REQUIRED flag] ? AUTH_REQ : REQUEST       (INFO bad version/product/format -> FAILED(PROTOCOL))
AUTH_REQ --challenge--> AUTH_RESP   (no auth hook / hook failure / bad challenge length -> FAILED(AUTH_UNAVAILABLE or PROTOCOL))
AUTH_RESP --AUTH_OK--> REQUEST
REQUEST --DOWNLOAD_ACCEPTED--> acked := device next_offset;
        pull mode and acked < image_size ? PULL_START : (TRANSFER, or EXIT if acked >= image_size)
TRANSFER --DATA_ACK(next)--> acked := next; TRANSFER (next chunk) or EXIT when acked >= image_size   [next != last_offset+last_length => resyncs++]
EXIT --EXIT_PENDING--> VERIFY_WAIT ; EXIT/VERIFY_WAIT --VERIFY_RESULT(0)--> ACTIVATE ; --VERIFY_RESULT(!=0)--> FAILED(REJECTED)
ACTIVATE --ACTIVATE_OK--> DONE
PULL_START --PULL_STARTED/PULL_STATUS--> per device pull state (below)
PULL --PULL_STATUS--> per device pull state (below)
ABORTING --ABORT_OK or ERROR--> FAILED(CANCELLED)
```
Pull-state handling (`client_handle_pull_status`, `fwu_client.c:146`): status unparsable or `next_offset > image_size` -> FAILED(PROTOCOL); otherwise `acked :=
next_offset` and: `FWU_PROTO_PULL_DONE` -> offset must equal `image_size` (else FAILED(PROTOCOL)), then the EXIT step; `FWU_PROTO_PULL_FAILED` -> FAILED(DEVICE_ERROR, device error
or `FWU_ERR_PULL_PROTOCOL` if the error byte is 0); `FWU_PROTO_PULL_IDLE` -> restart; `FWU_PROTO_PULL_REQUESTING` / `FWU_PROTO_PULL_DOWNLOADING` / anything else -> state `FWU_CLIENT_PULL`, wait
`pull_poll_ms` for the next PULL_QUERY.

### 5.3 Retry, resync and restart rules

| Situation | Rule | Source |
|---|---|---|
| No response within `response_timeout_ms` | Resend the SAME frame with the SAME SEQ, `retries++`, `retransmits++`; when `retries >= max_retries`: FAILED(TIMEOUT) (FAILED(CANCELLED) while ABORTING). | `fwu_client.c:453-471` |
| Timeout in VERIFY_WAIT | Resend TRANSFER_EXIT (`retransmits++`, no retry limit); give up when `now - verify_start >= verify_timeout_ms` -> FAILED(TIMEOUT). The clock starts when EXIT_PENDING is received. | `:454-461` |
| DATA_ACK offset differs from expected | `resyncs++`; continue from the device-reported offset. | `:289-292` |
| ERROR(BAD_BLOCK) in TRANSFER or EXIT | `resyncs++`; reissue REQUEST_DOWNLOAD (state REQUEST); device reports where it left off. | `:196-200` |
| ERROR(BAD_STATE), state != HELLO | Restart: if `restarts >= max_restarts` -> FAILED(DEVICE_ERROR, `FWU_ERR_BAD_STATE`); else `restarts++`, `acked := 0`, back to HELLO. | `:92-101`, `:201-204` |
| ERROR(BAD_STATE) in HELLO, and any other ERROR code | FAILED(DEVICE_ERROR, code), final. | `:205` |
| ERROR while ABORTING | FAILED(CANCELLED). | `:192-195` |
| Unparsable ERROR payload | FAILED(PROTOCOL). | `:188-191` |
| Pull: device state IDLE | Restart (same limit as above). | `:168-170` |
| Pull: total time since PULL_START >= `pull_timeout_ms` (non-zero) | FAILED(TIMEOUT). Checked in poll in states PULL_START and PULL. Note `pull_start_ms` is reset after a restart. | `:442-447` |
| Cancel | `fwu_client_cancel` only sets a flag; the next poll moves to ABORTING and issues ABORT (which replaces any request in flight). | `:436-440` |

The ERROR payload carries a code byte and a request-type byte; the client compares the code byte with `FWU_ERR_BAD_BLOCK` / `FWU_ERR_BAD_STATE` cast to `fwu_byte_t`.

### 5.4 Types

```c
typedef enum {
    FWU_CLIENT_FAIL_NONE = 0,
    FWU_CLIENT_FAIL_TIMEOUT,            /* retransmissions exhausted or verification wait exceeded */
    FWU_CLIENT_FAIL_DEVICE_ERROR,       /* device returned a final ERROR (result.device_error) */
    FWU_CLIENT_FAIL_REJECTED,           /* image failed verification (result.verify_code) */
    FWU_CLIENT_FAIL_PROTOCOL,           /* invalid device response (version, product, format, offset) */
    FWU_CLIENT_FAIL_AUTH_UNAVAILABLE,   /* authorization required but no hook / hook failed */
    FWU_CLIENT_FAIL_CANCELLED           /* user cancelled */
} fwu_client_failure_e;      /* values 0..6 in this order */

typedef fwu_return_e (*fwu_client_auth_fn)(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                                           fwu_byte_t *response, uint32_t capacity, uint32_t *response_length);

typedef struct {
    const fwu_byte_t  *manifest;            /* signed manifest, FWU_MANIFEST_SIZE (138) bytes */
    const fwu_byte_t  *image;               /* image_size elements; must stay valid; may be NULL in pull mode */
    uint32_t           image_size;          /* >= 1; must equal the size in the manifest */
    const fwu_byte_t  *pull_target;         /* pull mode if != NULL: HTTP request target (path + query) */
    size_t             pull_target_length;  /* 1..FWU_PROTO_PULL_TARGET_MAX */
    fwu_byte_t         pull_ip[4];          /* server IPv4 address */
    uint16_t           pull_port;           /* server TCP port (1..65535) */
    uint32_t           pull_poll_ms;        /* PULL_QUERY interval (>= 1) */
    uint32_t           pull_timeout_ms;     /* total pull time limit since PULL_START (0 = none) */
    fwu_client_auth_fn auth;                /* authorization hook (NULL = none) */
    void              *auth_context;
    uint32_t           expected_product;    /* if != 0 the product id in INFO must match */
    uint32_t           response_timeout_ms; /* >= 1 */
    uint32_t           max_retries;         /* retransmissions per request */
    uint32_t           verify_timeout_ms;   /* total wait for the verification result (>= response_timeout_ms) */
    uint32_t           max_restarts;        /* restarts from the beginning allowed after a session loss */
} fwu_client_config_t;

typedef struct {
    fwu_client_state_e   state;
    fwu_client_failure_e failure;           /* reason if FAILED */
    fwu_return_e         device_error;      /* device code if FAIL_DEVICE_ERROR, else FWU_OK */
    fwu_byte_t           verify_code;       /* VERIFY_RESULT code if FAIL_REJECTED */
    uint32_t             acked_bytes;       /* image bytes acknowledged by the device (progress) */
    uint32_t             total_bytes;       /* image size */
    uint32_t             frames_sent;       /* including retransmissions */
    uint32_t             retransmits;       /* resent because of a timeout */
    uint32_t             restarts;
    uint32_t             resyncs;
    uint32_t             ignored;           /* responses ignored (SEQ/type mismatch) */
    uint16_t             pull_http_status;  /* pull mode: last HTTP status reported by the device (0 = none) */
} fwu_client_result_t;

typedef struct { /* internal state; do not touch (use fwu_client_get_result) */ } fwu_client_t;
```
Source: `fwu_client.h:65-80` (states), `:83-91`, `:94-95`, `:97-115`, `:118-131`, `:134-165`. `manifest`, `image`, `pull_target` and `auth_context` are borrowed (the client copies the config struct, not the data they point to);
they must remain valid for the whole update. `fwu_client_config_t` fields are stored as given; `fwu_client_init` does not check `max_retries`/`max_restarts` ranges.

### 5.5 Functions

```c
fwu_return_e fwu_client_init(fwu_client_t *client, const fwu_client_config_t *config);
```
`fwu_client.h:174`, `fwu_client.c:339`. Validates the configuration and resets the state to IDLE. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` for: NULL pointers; NULL `manifest`; `image == NULL` while `pull_target == NULL`; `image_size == 0`;
`response_timeout_ms == 0`; `verify_timeout_ms < response_timeout_ms`; in pull mode `pull_poll_ms == 0` or a target/port that `fwu_proto_encode_pull_start` rejects; a manifest that `fwu_manifest_parse` rejects or whose
`image_size` differs from `config->image_size`. Misconfiguration therefore never reaches the device. First SEQ used is 1.

```c
fwu_return_e fwu_client_start(fwu_client_t *client, uint32_t now_ms);
```
`fwu_client.h:177`, `fwu_client.c:398`. Queues HELLO. `now_ms` is unused. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` for NULL/uninitialized; `FWU_ERR_BAD_STATE` if not IDLE. The frame is emitted by the next `fwu_client_poll`.

```c
fwu_return_e fwu_client_cancel(fwu_client_t *client);
```
`fwu_client.h:180`, `fwu_client.c:411`. Requests cancellation. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` for NULL/uninitialized; `FWU_ERR_BAD_STATE` if IDLE, DONE, FAILED, in ACTIVATE (commit phase) or already ABORTING.

```c
fwu_return_e fwu_client_poll(fwu_client_t *client, uint32_t now_ms, fwu_byte_t *out, size_t out_capacity, size_t *out_length);
```
`fwu_client.h:187`, `fwu_client.c:423`. Returns the next frame to transmit (first send or timeout retransmission) in `out`, or `*out_length == 0`. Handles cancel request, pull timeout, PULL_QUERY scheduling, response timeouts and retry limits (section 5.3).
Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` for NULL `client`/`out`/`out_length` or uninitialized; `FWU_ERR_BUFFER_TOO_SMALL` if `out_capacity < FWU_FRAME_MAX_SIZE`. Failures are reported through the FAILED state, never through the return code. Non-blocking; no waiting inside.
The frame is copied into `out`; the caller transmits it. If the transmission itself fails the client is not told: it recovers by the response timeout.

```c
fwu_return_e fwu_client_on_frame(fwu_client_t *client, const fwu_frame_view_t *response, uint32_t now_ms);
```
`fwu_client.h:193`, `fwu_client.c:485`. Feeds one received frame. Stale, duplicate or unexpected responses are ignored and counted. Protocol errors become the FAILED state, not a return code.
Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` for NULL `client`/`response`, uninitialized, or `response->payload == NULL`.

```c
bool fwu_client_is_finished(const fwu_client_t *client);
fwu_client_state_e fwu_client_get_state(const fwu_client_t *client);
fwu_return_e fwu_client_get_result(const fwu_client_t *client, fwu_client_result_t *result);
```
`fwu_client.h:197`, `:200`, `:203`; `fwu_client.c:505`, `:510`, `:515`. `is_finished`: true in DONE or FAILED, false for NULL. `get_state`: current state, `FWU_CLIENT_IDLE` for NULL. `get_result`: fills the result struct; returns `FWU_OK`, or
`FWU_ERR_INVALID_ARG` for a NULL argument (it does not check `initialized`).

### 5.6 Hooks

`fwu_client_auth_fn`: called synchronously from inside `fwu_client_on_frame` while in AUTH_REQ, with the challenge (1..`FWU_PROTO_AUTH_CHALLENGE_MAX` bytes) and a response buffer of `FWU_PROTO_AUTH_RESPONSE_MAX` bytes. It must return `FWU_OK`
and a `response_length` in 1..`FWU_PROTO_AUTH_RESPONSE_MAX`; otherwise the client fails with `FWU_CLIENT_FAIL_AUTH_UNAVAILABLE`. How long the hook may take (it blocks `on_frame`) is up to the implementer.
