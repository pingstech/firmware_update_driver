# Porting guide

How to run the driver on another MCU without putting device-specific code into the portable core. The core (`src/`) never touches
hardware: everything device-specific goes through the interfaces below. The two existing ports are the worked examples, and the
limits of what they prove are stated in [`TEST_RESULTS.md`](TEST_RESULTS.md).

## 1. The boundary

| Core (do not edit for a port) | You provide |
|---|---|
| everything in `src/` | a **flash port** (`fwu_flash_t` callbacks), a **board layout header** (numbers only), a **transport** (send function and a byte source), a **bootloader hook set** (`app_valid`, optional `watchdog_arm`, the jump), startup/linker files, and optionally a **crypto port** |

Rule of thumb from the code: a new target should need **new files only** (`ports/<vendor>/<device>/...`, `src/config/layout_<board>.h`,
an example). If you must change a file in `src/`, it is probably a core defect or a missing abstraction; open an issue.

## 2. Flash port contract [mandatory]

A port fills a `fwu_flash_t` (`src/flash/fwu_flash.h`) and calls `fwu_flash_validate` at the end of its init:

```c
typedef struct {
    fwu_return_e (*flash_erase) (void *context, uint32_t offset, uint32_t length);
    fwu_return_e (*flash_write) (void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data);
    fwu_return_e (*flash_read)  (void *context, uint32_t offset, uint32_t length, fwu_byte_t *data);
} fwu_flash_callback_t;

typedef struct {
    uint32_t   page_length;     /* erase unit, in fwu_byte_t */
    uint32_t   flash_length;    /* size of the region in fwu_byte_t */
    uint32_t   write_granule;   /* smallest writable piece, in fwu_byte_t */
    fwu_byte_t erased_value;    /* value of an erased cell (usually 0xFF) */
} fwu_flash_geometry_t;

typedef struct {
    void                       *context;    /* passed unchanged as the first callback argument; may be NULL */
    const fwu_flash_callback_t *callback;   /* all three pointers must be non-NULL */
    fwu_flash_geometry_t        geometry;
} fwu_flash_t;
```

`fwu_flash_validate` rejects NULL pointers (`FWU_ERR_INVALID_ARG`) and a geometry where any length is 0, `flash_length` is not a multiple
of `page_length`, or `page_length` is not a multiple of `write_granule` (`FWU_ERR_FLASH_GEOMETRY`). The wrappers do **not** call it:
use before validation divides by zero.

What the core guarantees before it calls a callback (the wrappers check in this order: argument, range, alignment):

| Callback | Guaranteed on entry | Not guaranteed |
|---|---|---|
| `flash_erase` | `length != 0`, range inside the region, `offset` and `length` multiples of `page_length` | - |
| `flash_write` | `length != 0`, `data != NULL`, range inside the region, `offset` and `length` multiples of `write_granule` | that the target is erased (the core's own code always erases first; the wrapper does not check) |
| `flash_read` | `length != 0`, `data != NULL`, range inside the region | alignment: the core reads at arbitrary offsets and lengths |

What the port must do:

* **Offsets are relative to the start of the region** described by the object and are in `fwu_byte_t` units. A port whose hardware
  addresses differ converts them itself (the C2000 adapter maps two octets to one 16-bit flash word: word address =
  `base + offset / 2`; the STM32 port adds `FLASH_BASE`).
* Callbacks are **synchronous**: return only when the operation is finished. The core has no completion mechanism and no flash watchdog
  hook; if your flash operation blocks longer than your watchdog period, feed it inside the port (the STM32 port offers an optional
  `keepalive` callback for this).
* After an erase every cell must read `erased_value` (the core compares and pads with it).
* After a successful write the data must read back as written: the core verifies every write by reading it back and treats a mismatch as
  `FWU_ERR_FLASH_WRITE`.
* Return `FWU_OK` only on success. The core tests only `!= FWU_OK` and gives up on anything else; useful codes are `FWU_ERR_FLASH_ERASE`,
  `FWU_ERR_FLASH_WRITE`, `FWU_ERR_FLASH_NOT_ERASED` (write into a non-erased cell). **Do not return `FWU_ERR_FLASH_WRITE` for a genuine read
  failure**: the session's duplicate-chunk check converts that code to `FWU_ERR_BAD_BLOCK` and would misreport it as a content mismatch.
* No locking is done by the core; it is single-threaded. Re-entrancy requirements are **UNVERIFIED**.

What happens when a callback fails (all callers are tabulated in
[`api/pull-flash-ringbuffer.md`](api/pull-flash-ringbuffer.md) section 1.5.4): no layer retries; a staging write/erase failure resets
the session to IDLE and reaches the host as an `ERROR` frame; a failure while the bootloader installs the image makes `fwu_boot_start`
return STAY (the application is not started) and the journal lets the next boot repeat the interrupted step; the in-RAM journal
state is never advanced on a failed write.

### 2.1 Existing ports compared (from the code)

| | STM32G0 (`ports/stm32g0/flash`) | C2000 (`ports/c2000/f28004x`) |
|---|---|---|
| Vendor API | ST HAL (`HAL_FLASH_*`, `HAL_FLASHEx_Erase`) | TI Flash API through `flash_port` (`Fapi_*`, ~2.9 K words of RAM-resident code) |
| Page / sector | 2048 octets | 8192 octets (4 K words) |
| Write granule | 8 octets (double word) | 16 octets (8 words = 128 bit) |
| Unit of `fwu_byte_t` | 1 octet | one 16-bit element carrying one octet; flash holds two octets per word |
| Erased check | the port returns `FWU_ERR_FLASH_NOT_ERASED` before calling the HAL | `flash_write` has no erased check; a write into non-erased cells surfaces as a Flash API status mapped to `FWU_ERR_FLASH_WRITE` |
| Write permission | whole region | allow-list per project (`FLASH_PORT_WRITABLE` in `flash_port_config.h`): the application may write only staging and journal, the bootloader only the application, counters and journal |
| Execution constraints | flash locked after every erase/write; optional `keepalive` | erase/write run from RAM with global interrupts disabled (UART bytes arriving meanwhile can overflow the 16-element FIFO) |
| Extra for integrators | NMI/ECC helper `fwu_flash_stm32g0_handle_ecc_nmi` | the Flash API library and `.TI.ramfunc` section in the linker file |

Note: the code comments of the C2000 port state the flash write granule and page size above; the board numbers
(`layout_f280049c.h`) come from a TI linker file and were **not checked against the TRM**. `ports/c2000/f28004x_direct` is a second C2000
port that was only run against a PC stub of the TI API.

## 3. Board layout header [mandatory]

Add `src/config/layout_<board>.h` providing only numbers (the generic derivation and checks live in `fwu_layout.h`):

```
FWU_BOARD_FLASH_BASE   FWU_BOARD_PAGE_SIZE    FWU_BOARD_TOTAL_PAGES
FWU_BOARD_BOOT_PAGES   FWU_BOARD_APP_PAGES    FWU_BOARD_STAGING_PAGES
FWU_BOARD_LOG_PAGES    FWU_BOARD_COUNTER_PAGES  FWU_BOARD_SPARE_PAGES   FWU_BOARD_LOG_MIN_PAGES
```

(and optionally `FWU_BOARD_SCRATCH_PAGES`). Rules enforced at compile time: the region page counts add up to the total; staging has the same
size as the application region and at least 2 pages (1 manifest page + image); the counter region is exactly 2 pages; spare pages are at
least 2 (journal A/B), and at least 3 for SWAP mode (journal A/B + a scratch page); the log region does not shrink below
`FWU_BOARD_LOG_MIN_PAGES`. Register the board in `fwu_config.h` / `fwu_layout.h` (the `FWU_BOARD_*` selection list) and in
`src/config/check_config.sh`.

**16-bit-`int` pitfall:** on C2000 `int` is 16 bits. Page-size macros must carry a `UL` suffix (`8192UL`), otherwise offsets above 65535 wrap
silently (this caused a real initialisation failure on the C2000 target and is why `layout_f280049c.h` says so). Verify macro values with the
target compiler, not only on the PC.

The unit of the layout is whatever one `fwu_byte_t` represents. For a 16-bit-word flash you must decide (and document) whether an element
carries one or two octets; the C2000 board chose two octets per word (open issue 10 in `docs/KNOWN_ISSUES.md`).

## 4. Transport [mandatory for the application]

Implement `fwu_transport_send_fn` (`FWU_OK` only if the whole frame was accepted; `FWU_ERR_BUFFER_FULL` if nothing was taken, the link retries
later; any other error drops the frame) and feed received bytes into a `FWU_RINGBUF_DEFINE` ring buffer attached to `fwu_parser_init`.
Frames are at most `FWU_FRAME_MAX_SIZE` (521) octets and the parser discards a frame that is incomplete after 500 ms
(`FWU_PARSER_FRAME_TIMEOUT_MS`). If your link delivers bytes slower or in odd pieces, buffer and hand over complete frames
(see `app_ble.c`). After `fwu_link_reboot_ready()` becomes true, drain your transmitter before resetting.

## 5. Bootloader hooks and jump [mandatory for the bootloader]

* `app_valid(context)`: true if the application region contains a startable image (examples: first word not erased; stack pointer in RAM
  and reset vector in the application region). Returning false gives STAY with reason `FWU_BOOT_REASON_NO_APP`.
* `watchdog_arm(context)`: called only when a SWAP trial boot starts, right before the jump; it must start a watchdog that outlives the
  new application's self-test.
* The jump: disable interrupts, clear pending/enabled interrupt state, set the vector table/stack as your core requires, branch. The
  STM32 example (`boot_app.c`) and the C2000 example (`bootloader/main.c`) show two complete variants.
* The bootloader must decide STAY on every failure of `fwu_boot_start`; never jump to an application it could not install.
* Publish the contract record (`fwu_boot_info_encode`) to a place the application can read, and have the application call
  `fwu_boot_info_check`, if you want a mismatch between the two binaries detected. No code in this repository calls the check.

## 6. Crypto port [optional, mandatory for signed profiles]

`fwu_crypto_t` takes four callbacks (`src/crypto/fwu_crypto.h`): `sha256_begin`, `sha256_update`, `sha256_finish` and `ed25519_verify`
(`FWU_OK` valid, `FWU_ERR_BAD_SIGNATURE` invalid, `FWU_ERR_CRYPTO_FAILURE` could not be performed). `fwu_crypto_sw` implements SHA-256
and a hook that always fails. For a signed profile you need a real Ed25519 verification that works with your `fwu_byte_t` (many
published implementations assume an 8-bit `char`, which is not true on C2000) and a measured execution time (known issues 35, 37, 40).

## 7. What to test before claiming support for a new target

Minimum, in this order. Record the evidence (logs, tool versions) under `docs/evidence/` and add a row to the support matrix in the
README and to [`TEST_RESULTS.md`](TEST_RESULTS.md); do not mark a target supported on the basis of compilation alone.

1. **Host:** a PC stub of the vendor flash API plus a port test like `ports/stm32g0/flash/test_stm32g0_port.c`, registered in `tests/run_tests.sh`
   (erase/write/read mapping, alignment, error mapping, power-cut injection).
2. **Compile** with the vendor toolchain with warnings enabled (record compiler name and version).
3. **Flash behaviour on the device:** erase, write, read back; a write into non-erased cells; the behaviour when power is cut during an
   erase and during a write (the simulated flash assumes a half-done pattern; real cells are undefined); ECC/NMI behaviour where the
   flash has ECC; erase duration of one page versus your watchdog.
4. **Full update on the device** over each transport you ship, including a transfer interrupted at several points (the host client
   resynchronises to the device's offset).
5. **Power loss during the bootloader copy** at several points (journal progress records make each step repeatable; this has not been
   recorded on any target in this repository).
6. **Failure paths:** corrupted image (digest mismatch must give `FWU_ERR_HASH_MISMATCH` and leave the old application running), wrong
   product id, truncated image, image larger than staging.
7. **Measurements** you will otherwise be asked for: verification time of the largest image, flash use, RAM use, watchdog margins.
