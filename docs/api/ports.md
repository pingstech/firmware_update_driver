# API reference: flash ports and the STM32G0B1 bootloader example glue

Scope: the platform ports that implement the `fwu_flash_t` interface (`src/flash/fwu_flash.h`) and the STM32G0B1 bootloader example glue.
All statements below were derived from the source files named in each section. Anything that could not be confirmed from the code is
marked `UNVERIFIED`.

Contents

1. [Common contract: `fwu_flash_t`](#1-common-contract-fwu_flash_t)
2. [STM32G0 flash port](#2-stm32g0-flash-port)
3. [C2000 F28004x flash port (`flash_port` + `fwu_flash_adapter`)](#3-c2000-f28004x-flash-port-flash_port--fwu_flash_adapter)
4. [C2000 F28004x direct port (`f28004x_direct`)](#4-c2000-f28004x-direct-port-f28004x_direct)
5. [STM32G0B1 bootloader example glue](#5-stm32g0b1-bootloader-example-glue)
6. [Port status summary](#6-port-status-summary)

---

## 1. Common contract: `fwu_flash_t`

Source: `src/flash/fwu_flash.h`, `src/flash/fwu_flash.c`.

A port fills a `fwu_flash_t` object (context, a `fwu_flash_callback_t` table with `flash_erase`, `flash_write`, `flash_read`, and a
`fwu_flash_geometry_t` with `page_length`, `flash_length`, `write_granule`, `erased_value`) and calls `fwu_flash_validate`. All units are
`fwu_byte_t`. The core never calls the callbacks directly; it calls `fwu_flash_erase`, `fwu_flash_write`, `fwu_flash_read`, which check
before calling the port (in this order):

| Check | Erase | Write | Read | Error |
|---|---|---|---|---|
| NULL object/table/function, `length == 0`, (write/read) `data == NULL` | yes | yes | yes | `FWU_ERR_INVALID_ARG` |
| `offset`/`length` inside `flash_length` | yes | yes | yes | `FWU_ERR_OUT_OF_RANGE` |
| `offset` and `length` multiples of `page_length` | yes | no | no | `FWU_ERR_ERASE_ALIGNMENT` |
| `offset` and `length` multiples of `write_granule` | no | yes | no | `FWU_ERR_WRITE_ALIGNMENT` |

Source: `src/flash/fwu_flash.c:19-59`. Consequence: the port callbacks may assume offset, length and alignment are already valid
when they are reached through the wrappers. `fwu_flash_validate` (`src/flash/fwu_flash.c:5-17`) returns `FWU_ERR_INVALID_ARG` for a
NULL object/table/function pointer and `FWU_ERR_FLASH_GEOMETRY` if any geometry value is 0, `flash_length` is not a multiple of
`page_length`, or `page_length` is not a multiple of `write_granule`.

Relevant error codes (`src/error/fwu_error.h`): `FWU_ERR_INVALID_ARG` = 10, `FWU_ERR_FLASH_WRITE` = 40, `FWU_ERR_OUT_OF_RANGE` = 41,
`FWU_ERR_WRITE_ALIGNMENT` = 42, `FWU_ERR_ERASE_ALIGNMENT` = 43, `FWU_ERR_FLASH_GEOMETRY` = 44, `FWU_ERR_FLASH_NOT_ERASED` = 45,
`FWU_ERR_FLASH_ERASE` = 47.

`fwu_byte_t` is `uint16_t` on C2000 and `unsigned char` otherwise (`src/fwu_types.h:36,41`); `FWU_BYTE_MASK` = `0xFFu` limits a
value read from a `fwu_byte_t` (`src/fwu_types.h:53`).

---

## 2. STM32G0 flash port

Files: `ports/stm32g0/flash/fwu_flash_stm32g0.h`, `ports/stm32g0/flash/fwu_flash_stm32g0.c`.
Depends on: `stm32g0xx_hal.h` (ST HAL), `fwu_error.h`, `fwu_types.h`, `fwu_flash.h`.
Used by: `examples/stm32g0b1/bootloader/boot_app.c`; PC test `ports/stm32g0/flash/test_stm32g0_port.c` (with `hal_stub/`).
Status: see [section 6](#6-port-status-summary).

Purpose: implements `fwu_flash_t` for the STM32G0B1 internal flash on top of the ST HAL. Erase is page by page, write is 64-bit double
word by double word, read is a memory-mapped copy. An optional keep-alive hook is called before every page erase and before every
double-word write.

### 2.1 Geometry

| Item | Value | Source |
|---|---|---|
| Page size (erase unit) | `FLASH_PAGE_SIZE` (2 KB: `0x800` in the stub, header text says 2 KB) | ST HAL macro; `fwu_flash_stm32g0.c:78,174`; header `fwu_flash_stm32g0.h:10` |
| Write granule | 8 bytes (double word, 64 bit) | `PORT_GRANULE`, `fwu_flash_stm32g0.c:10,176` |
| Erased value | `0xFF` | `fwu_flash_stm32g0.c:177` |
| Base address | `FLASH_BASE` (`0x08000000`) | ST HAL macro; `fwu_flash_stm32g0.c:77,135,191` |
| Offset origin | start of flash (offset 0 = `FLASH_BASE`), 1 `fwu_byte_t` = 1 byte | `fwu_flash_stm32g0.h:10-11` |
| Bank split | `FLASH_BANK_SIZE`; address >= `FLASH_BASE + FLASH_BANK_SIZE` is bank 2 | `fwu_flash_stm32g0.c:88,94` |
| Chip size limit | `FLASH_SIZE`; `init` rejects a larger `flash_length` | `fwu_flash_stm32g0.c:165` |

The values come from ST HAL macros; the HAL source itself is not part of this repository (only the stub `hal_stub/stm32g0xx_hal.h`).

### 2.2 Callbacks and the vendor API they call

| `fwu_flash_t` callback | Port function | HAL calls (all that are made) |
|---|---|---|
| `flash_erase` | `port_erase` (`.c:74`) | `HAL_FLASH_Unlock`, `__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR)` (via `port_unlock`), `HAL_FLASHEx_Erase` with `FLASH_EraseInitTypeDef` (`TypeErase = FLASH_TYPEERASE_PAGES`, `Banks = FLASH_BANK_1` or `FLASH_BANK_2`, `Page` = page index within the bank, `NbPages = 1`), `HAL_FLASH_Lock` |
| `flash_write` | `port_write` (`.c:112`) | `HAL_FLASH_Unlock`, `__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR)`, `HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address, value)`, `HAL_FLASH_Lock` |
| `flash_read` | `port_read` (`.c:144`) | none for the data (reads through the `memory` pointer); ECC flag handling uses `__HAL_FLASH_CLEAR_FLAG` and `__HAL_FLASH_GET_FLAG` with `FLASH_FLAG_ECCC1`, `FLASH_FLAG_ECCD1` and, if `FLASH_DBANK_SUPPORT` is defined, `FLASH_FLAG_ECCC2`, `FLASH_FLAG_ECCD2` |

Behaviour read from the code:

- Erase: one `HAL_FLASHEx_Erase` call per page; `port_keepalive` is called before each. The flash is locked again in every case
  (`(void)HAL_FLASH_Lock()`, `.c:108`).
- Write: before any HAL call the whole target range is read through `port->memory` and compared with `0xFF`; if any byte differs the
  function returns `FWU_ERR_FLASH_NOT_ERASED` without calling the HAL (`.c:119-123`). Each 8-byte granule is assembled little-endian
  (byte 0 = least significant) into a `uint64_t`. `port_keepalive` is called before each granule. Locks in every case after the unlock.
- Read: clears the ECC flags, copies `length` bytes (`& FWU_BYTE_MASK`), then counts and clears the ECC flags. Always returns `FWU_OK`.

### 2.3 Error mapping

| Condition | Result |
|---|---|
| `HAL_FLASH_Unlock` fails in erase | `FWU_ERR_FLASH_ERASE` (`.c:83-85`) |
| `HAL_FLASHEx_Erase` returns other than `HAL_OK` | `FWU_ERR_FLASH_ERASE` |
| Target not all `0xFF` in write | `FWU_ERR_FLASH_NOT_ERASED` |
| `HAL_FLASH_Unlock` fails in write | `FWU_ERR_FLASH_WRITE` |
| `HAL_FLASH_Program` returns other than `HAL_OK` | `FWU_ERR_FLASH_WRITE` |
| Read | always `FWU_OK` (ECC events are only counted) |
| `init`: `port` or `memory` NULL | `FWU_ERR_INVALID_ARG` |
| `init`: `flash_length` is 0, not a multiple of `FLASH_PAGE_SIZE`, or greater than `FLASH_SIZE` | `FWU_ERR_FLASH_GEOMETRY` |
| `init`: otherwise | result of `fwu_flash_validate` |

The HAL status is not inspected beyond `HAL_OK` / not `HAL_OK`; `page_error` from `HAL_FLASHEx_Erase` is not used.

### 2.4 Alignment and range checks done by the port

The port callbacks do not check offset, length or alignment themselves: `port_erase` computes `length / page_size` and `port_write`
computes `length / PORT_GRANULE` (integer division), and they rely on the `fwu_flash_*` wrappers having rejected misaligned or
out-of-range requests (section 1). The only checks inside the port are the `0xFF` check in write and the geometry check in `init`.

### 2.5 Execution-context constraints (visible in the code)

- Erase and write are blocking: they return when the HAL call returns. No RAM-resident function, no interrupt masking, and no
  `#pragma` placement appear in this port.
- The optional keep-alive hook (`fwu_flash_stm32g0_keepalive_fn`) is called before every page erase and every double-word write, never
  during a HAL call. The header text gives the intended use (feeding the watchdog).
- The port is not re-entrant: a single `fwu_flash_stm32g0_t` holds the state; the ECC counters are plain `uint32_t` fields.
- `fwu_flash_stm32g0_handle_ecc_nmi` is intended to be called from the NMI handler context (see 2.7); it only reads and clears flags.

### 2.6 Macros, types and functions

No public macros. `PORT_GRANULE` is private to the `.c` file.

```c
typedef void (*fwu_flash_stm32g0_keepalive_fn)(void *context);
```
Optional hook, may be NULL (`fwu_flash_stm32g0.h:68`).

```c
typedef struct {
    fwu_flash_t                      flash;
    fwu_flash_callback_t             callback;
    const volatile fwu_byte_t       *memory;
    fwu_flash_stm32g0_keepalive_fn   keepalive;
    void                            *keepalive_context;
    uint32_t                         ecc_corrected;
    uint32_t                         ecc_detected;
} fwu_flash_stm32g0_t;
```
(`fwu_flash_stm32g0.h:71-79`). Give `&port.flash` to the core. `ecc_corrected` counts ECCC flags seen during reads; `ecc_detected` counts
ECCD (double error) detections during reads and in `fwu_flash_stm32g0_handle_ecc_nmi`. Both are reset by `init`. The object must outlive
all use of `flash` (`flash.context` points back to it).

```c
fwu_return_e fwu_flash_stm32g0_init(fwu_flash_stm32g0_t *port, uint32_t flash_length, const volatile fwu_byte_t *memory,
                                    fwu_flash_stm32g0_keepalive_fn keepalive, void *keepalive_context);
```

| | |
|---|---|
| Responsibility | Fills the callback table and geometry (see 2.1), stores `memory`, hook and context, zeroes both ECC counters, then calls `fwu_flash_validate`. |
| `port` | Output, must not be NULL. |
| `flash_length` | Managed length in bytes; multiple of `FLASH_PAGE_SIZE`, not larger than `FLASH_SIZE`. The header comment names `FWU_BOARD_TOTAL_PAGES * FWU_BOARD_PAGE_SIZE` as the source in the integration. |
| `memory` | Memory-mapped view of flash, must not be NULL (the MCU passes `FLASH_BASE`; the PC test passes a fake memory). The pointer is dereferenced as `memory[offset + i]`, i.e. offsets are relative to the start of flash. |
| `keepalive`, `keepalive_context` | Hook and data, may be NULL. |
| Returns | `FWU_OK`, `FWU_ERR_INVALID_ARG`, `FWU_ERR_FLASH_GEOMETRY`, or the `fwu_flash_validate` result (section 2.3). |
| Call order | Before any use of `port->flash`. Does not touch hardware registers. |
| Source | `fwu_flash_stm32g0.c:158-185` |

```c
fwu_return_e fwu_flash_stm32g0_init_mcu(fwu_flash_stm32g0_t *port, uint32_t flash_length,
                                        fwu_flash_stm32g0_keepalive_fn keepalive, void *keepalive_context);
```

| | |
|---|---|
| Responsibility | Calls `fwu_flash_stm32g0_init` with `memory = (const volatile fwu_byte_t *)(uintptr_t)FLASH_BASE`. |
| Returns | Same as `fwu_flash_stm32g0_init`. |
| Source | `fwu_flash_stm32g0.c:187-193` |

```c
bool fwu_flash_stm32g0_handle_ecc_nmi(fwu_flash_stm32g0_t *port);
```

| | |
|---|---|
| Responsibility | NMI helper. Reads the ECC flags (`ECCD1`, `ECCC1`, and bank-2 flags if `FLASH_DBANK_SUPPORT`), increments `ecc_corrected` for each correction flag and `ecc_detected` once if a double-error flag is set, clears all ECC flags. |
| `port` | May be NULL: flags are still evaluated and cleared, counters are not incremented. |
| Returns | `true` if a double-error detection flag (`ECCD1`, or `ECCD2` with `FLASH_DBANK_SUPPORT`) was set; `false` otherwise. A set correction flag alone (ECCC) gives `false`. |
| Who must call it | The bootloader's NMI handler, before any reset. If it returns `true` the handler must return without resetting; if `false` the handler applies its own policy. See `examples/stm32g0b1/bootloader/boot_app.c:129-135` (section 5.7). |
| Source | `fwu_flash_stm32g0.c:195-198` |

### 2.7 Documented hardware risks (from the header, not verified by the code)

The header (`fwu_flash_stm32g0.h:33-42`) describes the ECC / power-loss risk: reading a double word whose programming was interrupted
may raise a double-error detection and possibly an NMI (marked `[UNVERIFIED]`, to be checked against RM0444); what data a read returns after
the NMI returns is `[UNVERIFIED]`. Power-loss resilience on hardware is explicitly not guaranteed. The header lists six items to verify
first on hardware (`fwu_flash_stm32g0.h:44-51`).

---

## 3. C2000 F28004x flash port (`flash_port` + `fwu_flash_adapter`)

Files: `ports/c2000/f28004x/flash_port.h`, `flash_port.c`, `fwu_flash_adapter.h`, `fwu_flash_adapter.c`. Project-specific
`flash_port_config.h` is NOT in this directory; each example project supplies its own.
Depends on: TI `driverlib.h`, `device.h`, `F021_F28004x_C28x.h`; `flash_port_config.h`; `fwu_flash.h`, `fwu_error.h`.
Used by: `examples/c2000_launchxl_f280049c/bootloader/main.c` (`fwu_flash_adapter_init`, `fwu_flash_adapter.h`),
`examples/c2000_launchxl_f280049c/application/port/app_fwu.c` (`fwu_flash_adapter_init`), and
`examples/c2000_launchxl_f280049c/application/main.c` (includes `flash_port.h`; its `flash_selftest` / `adapter_selftest` calls are commented
out at `main.c:496,501`).
Status: see [section 6](#6-port-status-summary).

Two layers:

- `flash_port` (`flash_*`): word-addressed (16-bit words), absolute word addresses, TI Flash API (F021) calls, with a per-project
  write/erase allow-list.
- `fwu_flash_adapter`: implements `fwu_flash_t` on top of `flash_port`; converts octet offsets to word addresses and packs two octets per word.

### 3.1 Geometry

| Item | Value | Source |
|---|---|---|
| Flash base | word address `0x080000` (`FLASH_START_ADDRESS`); last word `0x09FFFF` (`FLASH_END_ADDRESS`) | `flash_port.c:9-10` |
| Sector (erase unit) | `0x1000` words (`FLASH_SECTOR_WORDS`) = 8192 octets (`ADAPTER_PAGE_OCTETS`) | `flash_port.c:16`, `fwu_flash_adapter.c:5-6` |
| Write granule, port level | 4 words = 64 bit (`FLASH_GROUP_WORDS`); max 8 words per Fapi call (`FLASH_CHUNK_WORDS`) | `flash_port.c:17-18` |
| Write granule, `fwu_flash_t` level | 16 octets = 8 words = 128 bit (`ADAPTER_GRANULE_OCTETS`) | `fwu_flash_adapter.c:7,135` |
| Erased value | word `0xFFFF` (`FLASH_ERASED_WORD`); `erased_value = 0xFFu` | `flash_port.c:19`, `fwu_flash_adapter.c:136` |
| Octet <-> word packing | octet `2k` = low byte of word `k`, octet `2k+1` = high byte; word address = `base_address + offset / 2` | `fwu_flash_adapter.h:13-15`, `fwu_flash_adapter.c:29,49` |
| System clock for the API | `DEVICE_SYSCLK_FREQ / 1000000UL` (`FLASH_SYSCLK_MHZ`) | `flash_port.c:20`; `DEVICE_SYSCLK_FREQ` comes from TI `device.h` |
| Writable regions | macro `FLASH_PORT_WRITABLE(first, last)` from the project's `flash_port_config.h` | `flash_port.c:14,57` |

`flash_port_config.h` of the two example projects (word addresses, both ends inclusive):

| Project | Writable (erase + write) |
|---|---|
| bootloader (`bootloader/flash_port/flash_port_config.h`) | `0x083000..0x08CFFF`, `0x09B000..0x09CFFF`, `0x09D000..0x09EFFF` |
| application (`application/flash_port/flash_port_config.h`) | `0x08D000..0x096FFF`, `0x09D000..0x09EFFF` |

Reads are not restricted by `FLASH_PORT_WRITABLE`, only by `FLASH_START_ADDRESS..FLASH_END_ADDRESS`.

### 3.2 `flash_port.h`

```c
typedef enum {
    FLASH_OK = 0,
    FLASH_ERR_PARAM,     /* invalid parameter (NULL, length 0, ...) */
    FLASH_ERR_RANGE,     /* outside the permitted region */
    FLASH_ERR_ALIGN,     /* alignment rule violated */
    FLASH_ERR_API,       /* a Fapi_ function returned an error */
    FLASH_ERR_FSM,       /* Fapi_getFsmStatus() is not zero */
    FLASH_ERR_VERIFY     /* written and read-back data differ */
} flash_status_e;
```
Values are 0..6 in the order shown (`flash_port.h:6-14`).

```c
flash_status_e flash_erase_sector(uint32_t address);                              /* address: word address */
```

| | |
|---|---|
| Responsibility | Erases one sector. |
| `address` | Absolute word address of the first word of the sector; must be a multiple of `0x1000`, and `[address, address+0xFFF]` must be inside `FLASH_PORT_WRITABLE`. |
| Returns | `FLASH_OK`; `FLASH_ERR_ALIGN` (not sector aligned); `FLASH_ERR_RANGE` (not entirely inside a writable region); `FLASH_ERR_API` (from `flash_prepare` or from `Fapi_issueAsyncCommandWithAddress`); `FLASH_ERR_FSM` (`Fapi_getFsmStatus() != 0`); `FLASH_ERR_VERIFY` (`Fapi_doBlankCheck` not successful). |
| Vendor calls | `Fapi_initializeAPI`, `Fapi_setActiveFlashBank(Fapi_FlashBank0)`, `Flash_disablePrefetch(FLASH0CTRL_BASE)`, `Flash_enablePrefetch(FLASH0CTRL_BASE)` (these four only on first use, in `flash_prepare`), `Fapi_issueAsyncCommandWithAddress(Fapi_EraseSector, ...)`, `Fapi_checkFsmForReady`, `Fapi_getFsmStatus`, `Fapi_doBlankCheck` (length `0x800` 32-bit words), `Interrupt_disableGlobal`, `Interrupt_enableGlobal`; macro `FLASH_DELAY_CONFIG` (from TI headers, not defined in this repository). |
| Execution context | RAM-resident (`.TI.ramfunc`). Interrupts are globally disabled for the whole call (the previous state is restored). Blocking: waits in an unbounded loop `while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady)`. Lazy API initialisation on the first call (no separate init function). |
| Source | `flash_port.c:94-126` (core), `flash_port.c:186-195` (wrapper), declaration `flash_port.h:16` |

```c
flash_status_e flash_write(uint32_t address, const uint16_t *data, uint16_t words);
```

| | |
|---|---|
| Responsibility | Programs `words` 16-bit words. If `words` is not a multiple of 4 the last 64-bit group is padded with `0xFFFF`. |
| `address` | Absolute word address, multiple of 4; the range rounded up to a multiple of 4 words must be inside `FLASH_PORT_WRITABLE`. |
| `data`, `words` | Not NULL, `words > 0`. |
| Returns | `FLASH_OK`; `FLASH_ERR_PARAM` (NULL or 0 words); `FLASH_ERR_ALIGN` (address not multiple of 4); `FLASH_ERR_RANGE`; `FLASH_ERR_API` (from `flash_prepare` or `Fapi_issueProgrammingCommand`); `FLASH_ERR_FSM`; `FLASH_ERR_VERIFY` (`Fapi_doVerifyBy16bits` not successful). |
| Vendor calls | `Fapi_issueProgrammingCommand(..., Fapi_AutoEccGeneration)` in chunks of 8 words (4 for the final partial group), `Fapi_checkFsmForReady` (waits while `Fapi_Status_FsmBusy`, unbounded), `Fapi_getFsmStatus`, `Fapi_doVerifyBy16bits`; plus the first-use calls listed above and `Interrupt_disableGlobal` / `Interrupt_enableGlobal`. |
| Execution context | RAM-resident, interrupts disabled for the whole call, blocking. Does NOT check that the target is erased (a write to non-erased cells is reported by the FSM status or verify, not by a dedicated code). Stops at the first failing chunk (earlier chunks stay written). |
| Source | `flash_port.c:130-180` (core), `flash_port.c:197-206` (wrapper), `flash_port.h:17` |

```c
flash_status_e flash_read(uint32_t address, uint16_t *data, uint16_t words);
```

| | |
|---|---|
| Responsibility | Copies `words` words from memory-mapped flash. |
| Returns | `FLASH_OK`; `FLASH_ERR_PARAM` (NULL or 0 words); `FLASH_ERR_RANGE` (outside `0x080000..0x09FFFF`). |
| Execution context | Plain CPU reads; not in `.TI.ramfunc`, no interrupt masking, no Flash API. The comment says not to read a bank that is being programmed or erased. |
| Source | `flash_port.c:210-225`, `flash_port.h:18` |

Private RAM-resident helpers (not public): `flash_range_allowed`, `flash_disable_prefetch`, `flash_enable_prefetch`, `flash_prepare`,
`flash_erase_sector_core`, `flash_write_core`. Interrupts are disabled because application code and the UART ISR may sit in the same
bank as the area being programmed (comment, `flash_port.c:182-185`); the comment also states that bytes arriving meanwhile do not fit
the 16-byte FIFO, so the protocol must be stop-and-wait.

### 3.3 `fwu_flash_adapter.h`

```c
typedef struct {
    fwu_flash_t flash;          /* object to be given to fwu_*: &adapter.flash */
    uint32_t    base_address;   /* First WORD address of the region (absolute), a multiple of 4 */
} fwu_flash_adapter_t;
```
(`fwu_flash_adapter.h:24-27`). Requires the C2000 build symbol so that `fwu_byte_t` is 16 bit (header comment, line 22).

```c
fwu_return_e fwu_flash_adapter_init(fwu_flash_adapter_t *self, uint32_t base_address, uint32_t length_octets);
```

| | |
|---|---|
| Responsibility | Stores `base_address`, installs the three callbacks (static table `g_adapter_callbacks`), sets geometry `page_length = 8192`, `flash_length = length_octets`, `write_granule = 16`, `erased_value = 0xFF`, then calls `fwu_flash_validate`. |
| `self` | Output, must not be NULL; must outlive use of `self->flash`. |
| `base_address` | Absolute word address of the region start. Not range-checked by `init`; the allow-list is enforced later by `flash_port`. |
| `length_octets` | Region size in octets, multiple of 8192 (enforced by `fwu_flash_validate`). |
| Returns | `FWU_OK`; `FWU_ERR_INVALID_ARG` (`self == NULL`); `FWU_ERR_FLASH_GEOMETRY` (from `fwu_flash_validate`). |
| Source | `fwu_flash_adapter.c:126-138`, `fwu_flash_adapter.h:35` |

Examples: bootloader and application both pass base `0x080000` and `FWU_BOARD_TOTAL_PAGES * FWU_BOARD_PAGE_SIZE` octets (whole flash,
`bootloader/port/boot_fwu_config.h:14-15`, `application/port/app_fwu_config.h:27-28`).

### 3.4 Callbacks of `fwu_flash_adapter`

| Callback | Function | Mapping to `flash_port` | Source |
|---|---|---|---|
| `flash_erase` | `adapter_erase` | one `flash_erase_sector(address + index * 4096)` per page; `address = base_address + offset / 2`; stops at the first error | `fwu_flash_adapter.c:28-43` |
| `flash_write` | `adapter_write` | packs octets into 16-bit words (low byte OR high byte shifted left by 8, each masked with `0xFF`), calls `flash_write` in chunks of at most 32 words (`ADAPTER_CHUNK_WORDS`) | `fwu_flash_adapter.c:46-76` |
| `flash_read` | `adapter_read` | calls `flash_read` in chunks of at most 32 words; supports odd offset/length (first octet at an odd offset is the high byte of a word) | `fwu_flash_adapter.c:79-118` |

Error mapping (`adapter_map_status`, `fwu_flash_adapter.c:12-25`):

| `flash_status_e` | erase | write | read |
|---|---|---|---|
| `FLASH_OK` | `FWU_OK` | `FWU_OK` | `FWU_OK` |
| `FLASH_ERR_PARAM` | `FWU_ERR_INVALID_ARG` | `FWU_ERR_INVALID_ARG` | `FWU_ERR_INVALID_ARG` |
| `FLASH_ERR_RANGE` | `FWU_ERR_OUT_OF_RANGE` | `FWU_ERR_OUT_OF_RANGE` | `FWU_ERR_OUT_OF_RANGE` |
| `FLASH_ERR_ALIGN` | `FWU_ERR_ERASE_ALIGNMENT` | `FWU_ERR_WRITE_ALIGNMENT` | `FWU_ERR_INVALID_ARG` |
| `FLASH_ERR_API`, `FLASH_ERR_FSM`, `FLASH_ERR_VERIFY` | `FWU_ERR_FLASH_ERASE` | `FWU_ERR_FLASH_WRITE` | `FWU_ERR_INVALID_ARG` (`default` branch) |

`FWU_ERR_FLASH_NOT_ERASED` is never returned by this port.

Alignment and range checks: the adapter itself does none (it relies on the `fwu_flash_*` wrappers); `flash_port` checks sector / 64-bit
alignment and the `FLASH_PORT_WRITABLE` region (3.2). Because the adapter always passes whole 16-octet granules, `flash_write` never
needs its padding behaviour on this path.

### 3.5 Execution-context summary

- Erase and write run with global interrupts disabled and from RAM (`.TI.ramfunc`, requires the linker script to place and copy that
  section; the comment says `Device_init()` copies it). The adapter callbacks themselves are not RAM-resident and call into the RAM
  wrappers.
- No keep-alive hook exists in this port. The erase wait loop has no timeout.
- Erase of one sector includes a blank check; write includes a read-back verify per chunk.

---

## 4. C2000 F28004x direct port (`f28004x_direct`)

Files: `ports/c2000/f28004x_direct/fwu_flash_f28004x.h`, `fwu_flash_f28004x.c`.
Depends on: `F021_F28004x_C28x.h` (TI Flash API, or `hal_stub/F021_F28004x_C28x.h` in the PC test), `fwu_flash.h`, `fwu_error.h`, `fwu_types.h`.
Used by: `ports/c2000/f28004x_direct/test_f28004x_port.c` and `tests/run_tests.sh` (host test target `flash_f28004x`). No file under
`examples/` includes `fwu_flash_f28004x.h` (verified by grep).
Status: see [section 6](#6-port-status-summary).

Purpose: a single-layer implementation of `fwu_flash_t` for the F28004x that calls the TI Flash API directly (no `flash_port` layer, no
write allow-list), with a bounded FSM wait, an optional keep-alive hook and an optional prefetch hook.

### 4.1 Geometry and macros

```c
#define FWU_F28004X_FLASH_WORD_BASE     0x00080000u
#define FWU_F28004X_SECTOR_WORDS        0x1000u
#define FWU_F28004X_PAGE_OCTETS         8192u
#define FWU_F28004X_GRANULE_OCTETS      16u
#define FWU_F28004X_FLASH_OCTETS        262144u
```
(`fwu_flash_f28004x.h:65-69`): flash base word address, sector in words, erase unit in octets, write granule in octets (8 words),
total flash in octets (256 KB). The header comment says the numbers match `config/layout_f280049c.h` and that the flash base is checked in the test.

Private (in the `.c`): `PORT_MAX_FSM_WAIT` = `0x7FFFFFFF` (FSM wait loop bound), `PORT_BLANK_WORD` = `0xFFFF`, and the overridable
`FWU_F28004X_MEMORY_WINDOW` (defaults to `FWU_F28004X_FLASH_WORD_BASE` cast to `const volatile uint16_t *`; the PC test redefines it).

Octet packing is the same as in section 3.1 (octet `2k` low byte, `2k+1` high byte). Erased value `0xFF`.

### 4.2 Types

```c
typedef void (*fwu_flash_f28004x_keepalive_fn)(void *context);
typedef void (*fwu_flash_f28004x_prefetch_fn)(bool enable);

typedef struct {
    fwu_flash_t                      flash;
    fwu_flash_callback_t             callback;
    const volatile uint16_t         *memory;
    fwu_flash_f28004x_keepalive_fn   keepalive;
    void                            *keepalive_context;
} fwu_flash_f28004x_t;
```
(`fwu_flash_f28004x.h:72,76,79-85`). The prefetch hook is called with `false` before `Fapi_setActiveFlashBank` and with `true` after; the
header says it must run from RAM. `memory[0]` is word `0x080000`.

### 4.3 Callbacks and vendor calls

| Callback | Function | Vendor calls | Source |
|---|---|---|---|
| `flash_erase` | `port_erase` | per sector: `Fapi_issueAsyncCommandWithAddress(Fapi_EraseSector, ...)`, `Fapi_checkFsmForReady`, `Fapi_getFsmStatus`, `Fapi_doBlankCheck(..., PORT_SECTOR_WORDS / 2u, ...)` | `fwu_flash_f28004x.c:82-107` |
| `flash_write` | `port_write` | per 16-octet granule: `Fapi_issueProgrammingCommand(..., 8 words, 0, 0, Fapi_AutoEccGeneration)`, `Fapi_checkFsmForReady`, `Fapi_getFsmStatus`, `Fapi_doVerifyBy16bits`; uses a static 64-bit-aligned (`#pragma DATA_ALIGN(g_word_buffer, 8)`, TI compiler only) word buffer | `fwu_flash_f28004x.c:109-152` |
| `flash_read` | `port_read` | none (memory-mapped through `port->memory`) | `fwu_flash_f28004x.c:154-165` |

Other vendor calls (init path): `Fapi_initializeAPI(F021_CPU0_BASE_ADDRESS, hclk_mhz)`, `Fapi_setActiveFlashBank(Fapi_FlashBank0)`.
Interrupt masking: on the TI compiler `__disable_interrupts()` / `__restore_interrupts(s)` (macros `PORT_IRQ_SAVE` / `PORT_IRQ_RESTORE`); on
other compilers they are no-ops.

Behaviour read from the code:

- Erase: for each sector, interrupts are disabled, the keep-alive hook is called, then the erase is issued and waited for (bounded loop),
  FMSTAT is checked, and a blank check is made; interrupts are restored per sector.
- Write: the whole target range is first compared with `0xFFFF` through `port->memory`; any difference returns `FWU_ERR_FLASH_NOT_ERASED`
  with no Flash API call (`.c:115-120`). Then per granule: build the word buffer, disable interrupts, call the keep-alive hook, program,
  wait, check FMSTAT, verify, restore interrupts. Stops at the first error.
- Read: always `FWU_OK`.

### 4.4 Error mapping

| Condition | Result |
|---|---|
| `Fapi_issueAsyncCommandWithAddress` not `Fapi_Status_Success`, FSM wait timed out, or `Fapi_getFsmStatus() != 0` (erase) | `FWU_ERR_FLASH_ERASE` |
| `Fapi_doBlankCheck` not `Fapi_Status_Success` | `FWU_ERR_FLASH_ERASE` |
| Target not erased (write) | `FWU_ERR_FLASH_NOT_ERASED` |
| `Fapi_issueProgrammingCommand` not success, FSM wait timed out, `Fapi_getFsmStatus() != 0`, or `Fapi_doVerifyBy16bits` not success | `FWU_ERR_FLASH_WRITE` |
| `init`: `port` or `memory` NULL | `FWU_ERR_INVALID_ARG` |
| `init`: `flash_length` 0, not a multiple of `FWU_F28004X_PAGE_OCTETS`, or > `FWU_F28004X_FLASH_OCTETS` | `FWU_ERR_FLASH_GEOMETRY` |
| `hw_init`: `Fapi_initializeAPI` or `Fapi_setActiveFlashBank` fails | `FWU_ERR_FLASH_GEOMETRY` (the header documents this as "could not be started") |

Alignment/range: like the STM32 port, the callbacks rely on the `fwu_flash_*` wrappers for alignment and range; there is no
allow-list. The only in-port checks are the erased check and the geometry check in `init`.

### 4.5 Functions

```c
fwu_return_e fwu_flash_f28004x_init(fwu_flash_f28004x_t *port, uint32_t flash_length, const volatile uint16_t *memory,
                                    fwu_flash_f28004x_keepalive_fn keepalive, void *keepalive_context);
```

| | |
|---|---|
| Responsibility | Fills callbacks and geometry (`page_length = 8192`, `write_granule = 16`, `erased_value = 0xFF`), stores `memory`, hook and context, then `fwu_flash_validate`. Does not start the Flash API. |
| Parameters | `port` not NULL (output); `flash_length` in octets; `memory` not NULL (`memory[0]` = word `0x080000`). |
| Returns | `FWU_OK`, `FWU_ERR_INVALID_ARG`, `FWU_ERR_FLASH_GEOMETRY`, or the `fwu_flash_validate` result. |
| Source | `fwu_flash_f28004x.c:169-193`; declaration `fwu_flash_f28004x.h:95-96` |

```c
fwu_return_e fwu_flash_f28004x_hw_init(uint32_t hclk_mhz, fwu_flash_f28004x_prefetch_fn prefetch);
```

| | |
|---|---|
| Responsibility | `Fapi_initializeAPI(F021_CPU0_BASE_ADDRESS, hclk_mhz)`; optional `prefetch(false)`; `Fapi_setActiveFlashBank(Fapi_FlashBank0)`; optional `prefetch(true)`. `prefetch(true)` is called even if `Fapi_setActiveFlashBank` failed; it is not called if `Fapi_initializeAPI` failed. |
| Parameters | `hclk_mhz`: system clock in MHz (header: 100 on the LaunchPad); `prefetch` may be NULL. |
| Returns | `FWU_OK` or `FWU_ERR_FLASH_GEOMETRY`. |
| Preconditions | Per the header: wait states and ECC setup (`Flash_initModule`) done in `Device_init`; must run from RAM. The `.c` file does not mark `fwu_flash_f28004x_hw_init` with `#pragma CODE_SECTION` (only `port_wait_fsm`, `port_word_pointer`, `port_erase`, `port_write` are marked), so RAM placement of this function is the caller's responsibility. |
| Source | `fwu_flash_f28004x.c:195-212`; declaration `fwu_flash_f28004x.h:106` |

```c
fwu_return_e fwu_flash_f28004x_init_mcu(fwu_flash_f28004x_t *port, uint32_t flash_length, uint32_t hclk_mhz,
                                        fwu_flash_f28004x_prefetch_fn prefetch,
                                        fwu_flash_f28004x_keepalive_fn keepalive, void *keepalive_context);
```

| | |
|---|---|
| Responsibility | `fwu_flash_f28004x_hw_init`, then `fwu_flash_f28004x_init` with `FWU_F28004X_MEMORY_WINDOW`. |
| Returns | The `hw_init` error if it fails, otherwise the `init` result. |
| Source | `fwu_flash_f28004x.c:214-225`; declaration `fwu_flash_f28004x.h:109-111` |

### 4.6 Execution-context summary

- RAM-resident by `#pragma CODE_SECTION(..., ".TI.ramfunc")` (TI compiler only): `port_wait_fsm`, `port_word_pointer`, `port_erase`, `port_write`.
  `port_keepalive` and `port_read` are not marked; the keep-alive hook is called with interrupts disabled but with the FSM idle
  (header comment: calling a function in flash is safe at that moment).
- Interrupts disabled per sector (erase) / per granule (write); previous state restored.
- Blocking, but with a bounded wait (`PORT_MAX_FSM_WAIT` iterations, then error). The watchdog is not fed while waiting.

---

## 5. STM32G0B1 bootloader example glue

Files: `examples/stm32g0b1/bootloader/boot_app.c`, `boot_app.h`, `fwu_platform.h`, `STM32G0B1KEUX_FLASH.ld`.
Depends on: `main.h`, `fwu_boot.h`, `fwu_boot_profile.h`, `fwu_crypto_sw.h`, `fwu_flash_stm32g0.h`, ST HAL/CMSIS (`IWDG`, `SysTick`, `NVIC`, `SCB`,
`HAL_RCC_DeInit`, `__set_MSP`, `__disable_irq`, `__enable_irq`, `NVIC_SystemReset`).
Used by: not determined from this repository. The `main()` and `NMI_Handler` that call `boot_app_run` / `boot_app_nmi` are not part of these files
(`UNVERIFIED`: the CubeIDE project files are not in `examples/stm32g0b1`).

### 5.1 Role

`boot_app.c` connects the generic bootloader core (`fwu_boot`) to the STM32G0B1: it sets up the flash port and the software SHA-256 crypto
port, fills a `fwu_boot_config_t`, calls `fwu_boot_start`, publishes the result in a RAM mailbox, and either jumps to the application or
parks. The bootloader uses only flash, RCC and SysTick; no UART/ESP/GPIO (`boot_app.h:9`).

### 5.2 Public interface (`boot_app.h`)

```c
void boot_app_run(void);
```
Whole bootloader job; does not return (it jumps to the application or loops forever). Source `boot_app.c:81-127`, `boot_app.h:19`.

```c
void boot_app_nmi(void);
```
To be called from the NMI handler. If `fwu_flash_stm32g0_handle_ecc_nmi(&g_flash_port)` returns true it returns without reset; otherwise it
calls `NVIC_SystemReset()`. Source `boot_app.c:129-135`, `boot_app.h:22`.

```c
extern volatile fwu_boot_result_t g_boot_result;
```
Last result of `fwu_boot_start` for the debugger; fields: `decision`, `reason`, `status`, `outcome`, `phase`, `trial` (`src/boot/fwu_boot.h:151-158`).
Defined at `boot_app.c:12`, declared `boot_app.h:25`.

### 5.3 `boot_app_run` flow

1. Initialise `result` to a default "not started" value: `decision = FWU_BOOT_STAY`, `reason = FWU_BOOT_REASON_CONFIG`,
   `status = FWU_ERR_BAD_STATE`, `outcome = FWU_ACT_OUTCOME_NONE`, `phase = FWU_ACT_PHASE_IDLE`, `trial = false` (`boot_app.c:86-91`).
2. `fwu_flash_stm32g0_init_mcu(&g_flash_port, FWU_BOARD_TOTAL_PAGES * FWU_BOARD_PAGE_SIZE, NULL, NULL)` (no keep-alive hook) and
   `fwu_crypto_sw_init(&g_crypto_sw, &g_crypto)`. Both must return `FWU_OK`; if either fails the rest is skipped and `decision` stays
   `FWU_BOOT_STAY` with the default `result` (`boot_app.c:93-94`).
3. Fill `g_config` (`fwu_boot_config_t`): `policy = fwu_boot_policy_from_profile()`, `layout = fwu_platform_boot_layout()`,
   `flash = &g_flash_port.flash`, `crypto = &g_crypto`, `keys = NULL` and `key_count = 0` (DEV profile: no signature), `product_id =
   FWU_PLATFORM_PRODUCT_ID`, `max_trial_boots = FWU_PLATFORM_MAX_TRIAL_BOOTS`, `verify_work` / `activate_work` = two separate static buffers of
   `FWU_PLATFORM_WORK_SIZE` bytes, `app_valid = app_valid`, `watchdog_arm = watchdog_arm`, `context = NULL` (`boot_app.c:95-109`).
4. `decision = fwu_boot_start(&g_boot, &g_config, &result)` (`boot_app.c:110`): verifies and activates a pending update (see
   `src/boot/fwu_boot.h`).
5. Contract fingerprint: `fwu_boot_info_encode(&g_config, record, sizeof(record), &record_length)`; on `FWU_OK` the record is written to the
   mailbox with `fwu_bootinfo_put_contract(record)` (`boot_app.c:112-117`). The application compares it with its own configuration.
6. `g_boot_result = result;` then `fwu_bootinfo_put((uint32_t)result.outcome, (uint32_t)result.reason)` (always, even if step 2 failed)
   (`boot_app.c:119-120`).
7. If `decision == FWU_BOOT_JUMP_APP` call `jump_to_app()`; otherwise (and if `jump_to_app` returned) loop forever. The comment says there
   is deliberately no sleep so that a debugger can attach; the reason is in `g_boot_result.reason` (`boot_app.c:122-126`).

### 5.4 `app_valid` hook (`boot_app.c:25-39`)

Called by `fwu_boot_start` through `app_valid` to decide whether the application can be jumped to. It reads the first two words at
`FWU_PLATFORM_APP_ADDRESS` (initial stack pointer and reset vector) and returns false when:

- stack `<= FWU_PLATFORM_RAM_BASE`, or `> FWU_PLATFORM_RAM_END`, or not 4-byte aligned (empty flash `0xFFFFFFFF` falls here), or
- reset vector has bit 0 clear (not Thumb), or `reset - 1` is below `FWU_PLATFORM_APP_ADDRESS`, or `reset - 1` is at or above
  `FWU_PLATFORM_APP_ADDRESS + FWU_PLATFORM_APP_MAX_SIZE`.

Otherwise true. The `context` argument is ignored. When the hook says false, `fwu_boot_start` reports `FWU_BOOT_REASON_NO_APP` (per the
reason enum comment in `src/boot/fwu_boot.h`).

### 5.5 `watchdog_arm` hook (`boot_app.c:42-52`)

Passed as `watchdog_arm`; per `src/boot/fwu_boot.h` it is called when a trial boot starts, immediately before the jump (swap mode only).
It starts the independent watchdog: `IWDG->KR = 0xCCCC` (start), `0x5555` (unlock), `IWDG->PR = 4` (comment: LSI/64, about 500 Hz),
`IWDG->RLR = 2000` (comment: about 4 s), busy-waits until `IWDG->SR == 0`, then `IWDG->KR = 0xAAAA` (reload). If the new application
hangs, the watchdog reset leads to the rollback logic at the next start (comment).

### 5.6 `jump_to_app` step (`boot_app.c:56-77`)

Reads stack and reset vector from `FWU_PLATFORM_APP_ADDRESS`, then, in this order:

1. `__disable_irq()`
2. Stop SysTick: `SysTick->CTRL = 0`, `LOAD = 0`, `VAL = 0`
3. Disable and clear all 32 NVIC lines: `NVIC->ICER[0] = 0xFFFFFFFF`, `NVIC->ICPR[0] = 0xFFFFFFFF`
4. Clear pending SysTick and PendSV: `SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk`
5. `HAL_RCC_DeInit()` (clock back to reset state)
6. `SCB->VTOR = FWU_PLATFORM_APP_ADDRESS`
7. `__set_MSP(stack)`
8. `__enable_irq()` (comment: PRIMASK is 0 after a real reset; leaving it at 1 would stop the application's SysTick, so `HAL_Delay` would wait forever)
9. Call the reset vector as `void (*)(void)`; `for(;;)` follows if it ever returns.

### 5.7 NMI / ECC handling

`boot_app_nmi` is the example integration of `fwu_flash_stm32g0_handle_ecc_nmi` (section 2.6). The NMI handler of the project must call
`boot_app_nmi`; the port helper clears the flash ECC flags and returns true for a double-error detection, in which case `boot_app_nmi`
returns without reset. For any other cause it calls `NVIC_SystemReset()`. Whether a double-error detection really occurs on a half-written
record, and what is read afterwards, is `UNVERIFIED` (see 2.7).

### 5.8 RAM mailbox (`fwu_platform.h`)

Address and magic:

```c
#define FWU_BOOTINFO_ADDRESS            0x20023FE0u
#define FWU_BOOTINFO_MAGIC              0x46574249u     /* "FWBI" */
```
(`fwu_platform.h:49-50`). Location: the last 32 bytes of the 144 KB RAM (`FWU_PLATFORM_RAM_END` = `0x20024000`). Both linker scripts
define `RAM LENGTH = 144K - 32`, so stack and `.bss` do not overlap it (the bootloader's script: `STM32G0B1KEUX_FLASH.ld:47`).

```c
typedef struct {
    uint32_t magic;         /**< FWU_BOOTINFO_MAGIC */
    uint32_t outcome;       /**< fwu_activate_outcome_e (0 NONE, 1 ACTIVATED, 2 TRIAL, 3 REVERTED, 4 REJECTED) */
    uint32_t reason;        /**< fwu_boot_reason_e */
    uint32_t check;         /**< ~(magic ^ outcome ^ reason) */
    uint8_t  contract[FWU_BOOT_INFO_SIZE];  /**< fwu_boot_info_encode record */
    uint32_t reserved;
} fwu_bootinfo_t;
```
(`fwu_platform.h:52-59`; comments abbreviated). `FWU_BOOT_INFO_SIZE` is 12 (`src/boot/fwu_boot.h:64`). With default struct packing the size is
4 x 4 + 12 + 4 = 32 bytes, which matches the reserved 32 bytes (struct size is computed from these members; no `static_assert` exists in the file).

Inline helpers (all `static inline`, `fwu_platform.h:61-107`):

| Function | Caller | Behaviour |
|---|---|---|
| `fwu_bootinfo()` | both | Returns `(volatile fwu_bootinfo_t *)FWU_BOOTINFO_ADDRESS`. |
| `fwu_bootinfo_put(outcome, reason)` | bootloader | Writes `magic`, `outcome`, `reason`, `check = ~(MAGIC ^ outcome ^ reason)`. Does not touch `contract`. |
| `fwu_bootinfo_take(&outcome, &reason)` | application | If `magic` matches and `check` is consistent: returns the two values, sets `magic = 0` and `check = 0` (clears the record), returns 1; else returns 0 and leaves the record. |
| `fwu_bootinfo_put_contract(record)` | bootloader | Copies `FWU_BOOT_INFO_SIZE` bytes into `contract`, sets `reserved = 0`. |
| `fwu_bootinfo_contract()` | application | Returns a pointer to `contract`; validity is judged by `fwu_boot_info_check` (per the comment). |
| `fwu_platform_boot_layout()` | both | `fwu_boot_layout_from_board()` with `image_region_length` set to `FWU_PLATFORM_IMAGE_MAX_PAGES * FWU_BOARD_PAGE_SIZE`. |

Other constants in `fwu_platform.h` (the bootloader and the application must share this file; the contract fingerprint derives from these values):

| Macro | Value | Note |
|---|---|---|
| `FWU_PROFILE_DEV`, `FWU_BOARD_STM32G0B1` | defined | DEV profile: SHA-256 digest check, no signature; not for production (comment). `FWU_ACTIVATION_SWAP_ROLLBACK` is commented out, i.e. COPY activation is the default. |
| `FWU_PLATFORM_PRODUCT_ID` | `1001u` | Must equal `product_id` in the manifest. |
| `FWU_PLATFORM_MAX_TRIAL_BOOTS` | `3u` | Swap mode only. |
| `FWU_PLATFORM_CHUNK_SIZE` | `256u` | Not used by `boot_app.c`. |
| `FWU_PLATFORM_IDLE_TIMEOUT_MS` | `30000u` | Not used by `boot_app.c`. |
| `FWU_PLATFORM_WORK_SIZE` | `256u` | Size of both work buffers. |
| `FWU_PLATFORM_APP_ADDRESS` | `0x08009000u` | Page 18. |
| `FWU_PLATFORM_IMAGE_MAX_PAGES` | `78u` | Pages 18..95. |
| `FWU_PLATFORM_APP_MAX_SIZE` | `78 * 2048` = `0x27000` | Comment: same as the application linker `FLASH LENGTH = 156K`. |
| `FWU_PLATFORM_RAM_BASE`, `FWU_PLATFORM_RAM_END` | `0x20000000u`, `0x20024000u` | Used by `app_valid`. |

### 5.9 Linker script `STM32G0B1KEUX_FLASH.ld`

Memory regions (`.ld:46-48`): `RAM (xrw) ORIGIN = 0x20000000, LENGTH = 144K - 32`; `FLASH (rx) ORIGIN = 0x8000000, LENGTH = 36K`. The
bootloader occupies pages 0..17 (18 x 2 KB = 36 KB); the application starts at `0x08009000`. `_Min_Heap_Size = 0x200`, `_Min_Stack_Size = 0x400`.
The rest of the file is the standard ST-generated layout.

---

## 6. Port status summary

Evidence considered: header status text, the files each example project actually includes, host test sources, and `docs/evidence`.

| Port | Status | Evidence |
|---|---|---|
| `ports/c2000/f28004x` (`flash_port` + `fwu_flash_adapter`) | Path used by the C2000 LaunchPad example projects (bootloader and application). This is the only C2000 port that the example projects build against. | `bootloader/main.c:61,92` and `application/port/app_fwu.c:12,61` call `fwu_flash_adapter_init`. The files carry no "not tested" banner, unlike the other ports. Self-test code exists in `application/main.c` but the calls are commented out (`main.c:496,501`). No recorded hardware log for C2000 exists in the repository, so the hardware-tested claim rests on the project's use, not on stored results (`UNVERIFIED`). |
| `ports/c2000/f28004x_direct` (`fwu_flash_f28004x`) | Host stub-tested only; NOT used by any example project. | Header states "NOT TESTED ON HARDWARE", tested against the Flash API stub `hal_stub/` (`fwu_flash_f28004x.h:6-8`). No `examples/` file includes it; it is built only in `tests/run_tests.sh` (target `flash_f28004x`) with `test_f28004x_port.c`. |
| `ports/stm32g0/flash` (`fwu_flash_stm32g0`) | Header states "NOT TESTED ON HARDWARE"; tested against the HAL stub `hal_stub/` on a PC. It is used by the STM32G0B1 bootloader example (`boot_app.c`). | `fwu_flash_stm32g0.h:6-8`; `docs/KNOWN_ISSUES.md` issues 9 and 41. `docs/evidence/stm32g0b1_tcp_update_log.csv` is a recorded update session that ends with result `Confirmed`, version 3, which suggests the STM32 bootloader and port ran on a device; the log contains no flash-port level data (ECC, power loss), and the header banner was not updated. The power-loss / ECC behaviour is explicitly unverified. |
