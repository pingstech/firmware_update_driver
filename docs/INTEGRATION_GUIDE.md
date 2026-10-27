# Integration guide

How to add the driver to an existing firmware project, in the order you have to do it. Everything here is taken from the code in this
repository and from the two reference integrations ([`examples/c2000_launchxl_f280049c`](../examples/c2000_launchxl_f280049c),
[`examples/stm32g0b1`](../examples/stm32g0b1)). Code excerpts are copied from those files. There is **no complete buildable
project in this repository**: the examples omit IDE project files, vendor SDK files and the FC41D module driver, and say so.

Legend: **[mandatory]** every integration, **[optional]** only for the feature named, **[target]** depends on your MCU/board.

## 0. Decide what you are integrating

| You need | Parts to use |
|---|---|
| A bootloader that installs a staged image | `fwu_boot` + counters/verify/activation/journal/flash/crypto (see step 2) and a flash port |
| An application that receives an image and stages it | `fwu_link`, `fwu_parser`, ring buffer, `fwu_session`, `fwu_boot` (only for `fwu_boot_init` and its configuration objects), flash port, crypto port |
| A host tool | `tools/fwu_package.py` for the manifest; `src/client` if the host is C code |
| Pull mode (device downloads the image) | additionally `src/pull/*` and a TCP client you provide |

The application and the bootloader are separate binaries. They **must** share: protection profile, board layout, product id,
chunk size and the journal/staging layout. Put these values in one header that both projects include (the STM32 example does this in
`fwu_platform.h`; the C2000 example currently duplicates them in `app_fwu_config.h` and `boot_fwu_config.h` and says so in its comments).

## 1. Choose a target and its port [mandatory, target]

| Target | Flash port | Board layout header | Status (details: [`TEST_RESULTS.md`](TEST_RESULTS.md)) |
|---|---|---|---|
| TI TMS320F280049C | `ports/c2000/f28004x` (`flash_port` + `fwu_flash_adapter`) | `src/config/layout_f280049c.h` (`-DFWU_BOARD_F280049C`) | exercised on a LAUNCHXL-F280049C; layout numbers not checked against the TRM |
| ST STM32G0B1 | `ports/stm32g0/flash` | `src/config/layout_stm32g0b1.h` (`-DFWU_BOARD_STM32G0B1`) | PC-stub tested, compiles with ARM GCC, update completed on a device (maintainer-reported + host log) |
| Anything else | write a port ([`PORTING_GUIDE.md`](PORTING_GUIDE.md)) | write a `layout_<board>.h` | - |

Do not assume another member of the same family works: flash geometry, alignment and the flash API differ (the C2000 port converts
octet offsets to 16-bit word addresses, two octets per word; the STM32 port uses 2 KB pages and 64-bit double-word writes).

## 2. Add sources and include paths [mandatory]

Compile these `src/` files in the **bootloader**:

```
activation/fwu_activate.c   boot/fwu_boot.c            counters/fwu_counters.c
crypto/fwu_crypto.c         crypto/fwu_crypto_sw.c     crypto/fwu_sha256.c
flash/fwu_flash.c           journal/fwu_journal.c      journal/fwu_journal_ab.c
manifest/fwu_manifest.c     verify/fwu_verify.c
```

plus the flash port. The **application** additionally needs (this is the list the C2000 application project compiles):

```
frame/fwu_decode.c  frame/fwu_frame.c  frame/fwu_parser.c  ring_buffer/fwu_ringbuf.c
session/fwu_proto.c session/fwu_session.c  transport/fwu_link.c
pull/fwu_pull.c pull/fwu_http.c          (only for pull mode)
```

Do **not** add `crypto/fwu_crypto_sim.c`, `flash/fwu_flash_sim.c`, `transport/fwu_linksim.c` (test infrastructure; the simulated crypto is
deliberately insecure and refuses to build with `FWU_PRODUCTION`). `client/fwu_client.c` and `transport/fwu_isotp.c` are not used by
either example; `auth/fwu_auth.c` is only needed if you enable authorization.

Include paths: `src` and each `src/<module>` directory used (headers are included by plain name, e.g. `#include "fwu_session.h"`), the
port directory, and your project directory holding the configuration headers. Compiler defines (the C2000 example uses these, see its
README for the full option list):

```
-DC2000                      # C2000 only: fwu_byte_t becomes a 16-bit container (src/fwu_types.h)
-DFWU_BOARD_<BOARD>          # exactly one board: F280049C, STM32G0B1 or F2800157
-DFWU_PROFILE_DEV            # exactly one profile: DEV, INDUSTRIAL or AUTOMOTIVE
```

`src/config/fwu_config.h` stops the build (`#error`) if not exactly one profile or board is selected, if DEV is combined with
`FWU_PRODUCTION`, or if the spare-page requirements are not met.

## 3. Pick a protection profile and flash layout [mandatory]

| Profile | Verification | Activation | Anti-rollback | Notes |
|---|---|---|---|---|
| `FWU_PROFILE_DEV` | SHA-256 digest vs manifest, no signature | COPY (or SWAP with `-DFWU_ACTIVATION_SWAP_ROLLBACK`) | off | **not buildable with `FWU_PRODUCTION`** |
| `FWU_PROFILE_INDUSTRIAL` | signed (Ed25519 hook) | COPY or SWAP | off | needs a real signature implementation (see below) |
| `FWU_PROFILE_AUTOMOTIVE` | signed | SWAP+ROLLBACK | on | needs counters, 3+ spare pages |

**Signed profiles do not work with the code in this repository**: the software crypto port's Ed25519 hook always returns
`FWU_ERR_CRYPTO_FAILURE`, so every signature check fails closed. To use them you must supply your own crypto port
(`fwu_crypto_t` callbacks; see [`PORTING_GUIDE.md`](PORTING_GUIDE.md)). The DEV profile gives integrity only, not authenticity.

The flash layout (bootloader / application / staging / counters / journal / scratch) is derived from the board header; the generic
rules and compile-time checks are in `src/config/fwu_layout.h`. Make your linker scripts agree with it: application start address,
application region length, and the rule that **the staging area must be the same size as the application region** (the first staging
page holds the manifest, so the image capacity is one page smaller).

## 4. Implement or connect the platform pieces [mandatory, target]

### 4.1 Flash port

Use the supplied port or write one. It must fill a `fwu_flash_t` (callbacks `flash_erase`, `flash_write`, `flash_read`, geometry
`page_length`, `flash_length`, `write_granule`, `erased_value`) and call `fwu_flash_validate`. The contract, including what the core
guarantees before each callback and what happens when one fails, is in [`PORTING_GUIDE.md`](PORTING_GUIDE.md).

C2000 excerpt (`examples/c2000_launchxl_f280049c/application/port/app_fwu.c`):

```c
status = fwu_flash_adapter_init(&g_flash, APP_FWU_FLASH_BASE_WORD, APP_FWU_FLASH_LENGTH_OCTETS);
```

STM32 excerpt (`examples/stm32g0b1/bootloader/boot_app.c`):

```c
fwu_flash_stm32g0_init_mcu(&g_flash_port, FWU_BOARD_TOTAL_PAGES * FWU_BOARD_PAGE_SIZE, NULL, NULL)
```

C2000 additionally needs the TI Flash API library, the RAM-resident code section (`.TI.ramfunc`) in the linker file, and a per-project
`flash_port_config.h` that lists the flash ranges each binary may write.

### 4.2 Crypto port

```c
status = fwu_crypto_sw_init(&g_crypto_sw, &g_crypto);   /* SHA-256 only; Ed25519 fails closed */
```

### 4.3 Transport (application only) [mandatory for the application]

You provide: (a) an ISR or poll loop that pushes every received byte into a ring buffer, and (b) a send function with this
signature (`src/transport/fwu_transport.h`):

```c
typedef fwu_return_e (*fwu_transport_send_fn)(void *context, const fwu_byte_t *data, size_t length);
```

Contract: return `FWU_OK` when the **whole** frame was accepted; `FWU_ERR_BUFFER_FULL` if nothing was taken (the link retries the same
bytes later); any other error means the frame is dropped (the host recovers by retransmission). The reference transports are
`app_sci.c` (UART) and `app_ble.c` (FC41D BLE: frames travel as hex text; device-to-host pieces of 47 bytes at least 150 ms apart, `APP_BLE_PIECE_BYTES` / `APP_BLE_TX_GAP_MS`) in
`examples/c2000_launchxl_f280049c/application/port/`. A CAN transport (`transport/fwu_isotp.c`) exists but is only host-tested, and its
CAN identifiers are placeholders that stop a production build until you set `FWU_TRANSPORT_CAN_IDS_VERIFIED`.

Frames are parsed with a 500 ms frame timeout (`fwu_parser`): a transport that delivers a frame in pieces slower than that must buffer
and hand the parser complete frames (the BLE adapter does this with a staging buffer and a 250 ms idle flush).

### 4.4 Bootloader hooks [mandatory for the bootloader]

`app_valid(context)` returns true when the application may be jumped to (the C2000 example checks that the first word at 0x083000 is not
0xFFFF; the STM32 example checks the stack pointer and reset vector). `watchdog_arm(context)` is called only for a SWAP trial boot (the
C2000 example passes `NULL`). The jump itself is yours (see `jump_to_app` in `examples/stm32g0b1/bootloader/boot_app.c` and `app_jump` in
`examples/c2000_launchxl_f280049c/bootloader/main.c`). On the STM32, call `boot_app_nmi` from `NMI_Handler` so a flash ECC double error
does not reset the device in a loop (the helper `fwu_flash_stm32g0_handle_ecc_nmi` clears the flags).

## 5. Initialise the driver [mandatory]

### 5.1 Bootloader (`examples/c2000_launchxl_f280049c/bootloader/main.c`, `boot_update_and_decide`)

```c
g_boot_config.policy = fwu_boot_policy_from_profile();
g_boot_config.layout = fwu_boot_layout_from_board();
g_boot_config.flash = &g_flash.flash;
g_boot_config.crypto = &g_crypto;
g_boot_config.keys = NULL;                      // DEV profile: no signature, SHA-256 digest check only
g_boot_config.key_count = 0u;
g_boot_config.product_id = BOOT_FWU_PRODUCT_ID;
g_boot_config.max_trial_boots = BOOT_FWU_MAX_TRIAL_BOOTS;
g_boot_config.verify_work = g_verify_work;      g_boot_config.verify_work_length = BOOT_FWU_WORK_SIZE;
g_boot_config.activate_work = g_activate_work;  g_boot_config.activate_work_length = BOOT_FWU_WORK_SIZE;
g_boot_config.app_valid = app_valid_hook;
g_boot_config.watchdog_arm = NULL;
g_boot_config.context = NULL;
decision = fwu_boot_start(&g_boot, &g_boot_config, &g_boot_result);   // FWU_BOOT_JUMP_APP or FWU_BOOT_STAY
```

Call it once, early, before the application would start. `fwu_boot_start` blocks while it applies a pending update. On `FWU_BOOT_STAY`
**do not jump**; `g_boot_result.reason` and `.status` say why (`FWU_BOOT_REASON_*`). The `fwu_boot_t` object must not move after
initialisation (it holds pointers to its own fields), so declare it `static`.

### 5.2 Application (`examples/c2000_launchxl_f280049c/application/port/app_fwu.c`)

The application fills the **same** `fwu_boot_config_t` (with `app_valid = NULL`, `watchdog_arm = NULL`) and calls `fwu_boot_init`; the session then
takes the activation object and verification configuration from it so the commit record the application writes is the one the bootloader reads:

```c
status = fwu_boot_init(&g_boot, &g_boot_config);          /* then: */
status = app_rx_init(&g_rx);                              /* ring buffer defined with FWU_RINGBUF_DEFINE(app_rx, APP_FWU_RX_RING_SIZE) */
status = fwu_parser_init(&g_parser, &g_rx.base);

fwu_session_config_t config;
config.flash = &g_flash.flash;
config.activate = fwu_boot_activate(&g_boot);
config.verify = fwu_boot_verify_config(&g_boot);
config.auth = NULL;                         /* DEV profile: no authorization */
config.require_auth = false;
config.running_image_version = (uint32_t)APP_FWU_RUNNING_VERSION;
config.chunk_size = APP_FWU_CHUNK_SIZE;     /* multiple of the granule, divides the page, 1..508 */
config.idle_timeout_ms = APP_FWU_IDLE_TIMEOUT_MS;
config.auth_max_failures = 3u;
config.auth_lockout_ms = 60000u;
config.work = g_session_work;               /* >= chunk_size elements */
config.work_length = (uint32_t)APP_FWU_CHUNK_SIZE;
status = fwu_session_init(&g_session, &config);

g_transport.send = app_ble_send;            /* your send function, see 4.3 */
g_transport.poll = NULL;
g_transport.context = NULL;
link_config.session = &g_session;
link_config.parser = &g_parser;
link_config.transport = &g_transport;
status = fwu_link_init(&g_link, &link_config);
```

Every call returns `FWU_OK` on success; keep the first failing status (the example stores it in `g_status` and, while not ready, only
drains the receive ring so it does not fill up). Parameter validation rules and error codes: [`api/session-and-basics.md`](api/session-and-basics.md)
section 6, [`api/transport-client.md`](api/transport-client.md), [`api/activation-boot-counters.md`](api/activation-boot-counters.md).

## 6. Run it [mandatory]

**Interrupt/receive side:** push each received byte into the ring buffer (`fwu_ringbuf_push(&g_rx.base, &value, 1u)`, as `app_fwu_rx_push`
does). The ring buffer is single-producer/single-consumer only.

**Main loop:**

```c
(void)fwu_link_poll(&g_link, now_ms);          /* parse frames, run the session, send responses, run verification */
if(fwu_link_reboot_ready(&g_link)) {           /* ACTIVATE_OK was handed to the transport */
    /* flush your transmitter, wait a moment, then reset the MCU */
}
```

`now_ms` is a free-running millisecond counter (32-bit wrap is handled). `fwu_link_poll` never blocks except for the work inside it:
flash operations and, after `TRANSFER_EXIT`, the whole-image SHA-256. Feed your watchdog accordingly (known issues 40, 44, 49).
After `fwu_link_reboot_ready()` is true the example drains the UART/BLE transmitter, waits 20 ms and calls the reset function. Resetting
earlier loses `ACTIVATE_OK` and the host sees a timeout.

**Pull mode [optional].** Initialise `fwu_pull` after the link (`examples/.../app_pull.c`): `fwu_pull_init` with a send function that writes
the HTTP request to your TCP socket, a range buffer of at least `range_bytes` elements, then
`fwu_link_set_extension(link, pull_handles, pull_on_frame, NULL)` so `PULL_START` / `PULL_QUERY` reach the pull module. Feed TCP bytes
with `fwu_pull_feed` (fast, copies into a ring) and call `fwu_pull_poll(now_ms)` from the main loop **only while your TCP receive path is
quiet** (it writes flash). The C2000 example hands flow control to the FC41D driver with a credit window; see the comments in `app_pull.c`.
Wi-Fi credentials are yours to set (placeholders in the example's `main.c`). The server URL is not hard-coded: the host sends the IPv4
address, port and path in `PULL_START`.

## 7. Host side [mandatory for a working system]

1. Build the manifest and image: `python tools/fwu_package.py build app.bin --product <id> --version <n> --max-image-size <bytes>`
   (DEV profile: signature field zero). The product id must equal `product_id` in both `fwu_boot_config_t` objects, and the version must be
   higher than the running one if the verify configuration enforces a minimum version.
2. Send HELLO, REQUEST_DOWNLOAD (manifest), TRANSFER_DATA chunks, TRANSFER_EXIT, wait for VERIFY_RESULT, send ACTIVATE. `src/client`
   implements this flow in C including retransmission and resynchronisation; message layouts are in
   [`api/session-and-basics.md`](api/session-and-basics.md) section 5.
3. Never feed the driver a raw `.bin` produced without trimming the erased padding if your image tool pads: see
   `examples/c2000_launchxl_f280049c/application/image_tools/finalize_image.ps1`, which trims trailing `0xFF`, rounds up to the write
   granule and **refuses to produce an image larger than the staging capacity** (an earlier fixed-length image was silently truncated
   and the board did not start).

## 8. Monitor and handle failures [mandatory]

* Host: `INFO` shows state and the running version; `ERROR` frames carry a `fwu_return_e` code and the request type; `VERIFY_RESULT` carries
  the verification result; `PULL_STATUS` (pull mode) carries state, flash offset, HTTP status and error. Code meanings:
  [`api/session-and-basics.md`](api/session-and-basics.md) section 2; diagnostic procedures: [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md).
* Device: `fwu_link_get_stats` (frames in/out, tx busy, tx errors) and, in pull mode, `fwu_pull_get_stats` (requests, responses, retries,
  chunks written, overflows). The bootloader result (`g_boot_result`) is the first thing to inspect when the board does not start.
* After a successful update the application resets, the bootloader installs the image and starts it. A host can only confirm the new version
  if the application reports it in the next `INFO` (`running_image_version` in `fwu_session_config_t`; the reference example does this).

## 9. Optional features and what they need

| Feature | What to add | Caveat |
|---|---|---|
| Authorization (challenge-response) | `auth/fwu_auth.c`, a `fwu_session_auth_t`, a random-number source, operator public keys | requires a working Ed25519 verify; no example wires it (known issues 68-70) |
| Anti-rollback counters | profile AUTOMOTIVE; counters region; SWAP mode; call `fwu_boot_app_finish_trial(boot, true)` after the new application proves itself | nothing in this repository calls the confirm function: you must |
| SWAP + rollback | `-DFWU_ACTIVATION_SWAP_ROLLBACK`, scratch pages in the layout, a `watchdog_arm` hook | host-tested only; not exercised in hardware examples |
| CAN transport | `transport/fwu_isotp.c` and your CAN driver | host-tested only; identifiers are placeholders |

## 10. Checklist before shipping

* Bootloader and application built from the **same** profile, board, product id, chunk size.
* Linker scripts match `fwu_layout.h` (application address, staging size = application size, bootloader size).
* `FWU_PRODUCTION` build passes (it rejects DEV and unverified board numbers): you cannot ship the DEV profile.
* A real signature scheme is implemented and tested (the shipped Ed25519 hook always fails).
* Watchdog behaviour during flash erase, SHA-256 verification and the bootloader copy is measured on your hardware.
* Power-loss test of the bootloader copy on your hardware (not recorded for any target in this repository).
