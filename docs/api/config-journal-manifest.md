# API reference: config, journal, manifest

Scope: `src/config/` (`fwu_config.h`, `fwu_layout.h`, `layout_stm32g0b1.h`, `layout_f280049c.h`,
`layout_f2800157.h`), `src/journal/` (`fwu_journal.[ch]`, `fwu_journal_ab.[ch]`) and `src/manifest/`
(`fwu_manifest.[ch]`). Everything below was read from the sources; items that could not be confirmed
are marked `UNVERIFIED`.

Common types used here: `fwu_return_e` (`src/error/fwu_error.h`), `fwu_byte_t` (`src/fwu_types.h`;
`uint16_t` or `unsigned char` depending on the target, values are limited with `FWU_BYTE_MASK` = `0xFFu`),
`fwu_flash_t` (`src/flash/fwu_flash.h`).

---

## 1. Module `config` (`src/config/fwu_config.h`)

**Purpose.** Compile-time protection profile. The integrator selects the profile (and optionally the
swap-based activation) on the compiler command line; it cannot be changed at run time (to prevent a
downgrade attack). The header derives the verification mode, the activation strategy and the
anti-rollback flag, and turns forbidden combinations into `#error`.

**Used by / depends on.** Includes `fwu_layout.h` only when one of `FWU_BOARD_STM32G0B1`,
`FWU_BOARD_F2800157`, `FWU_BOARD_F280049C` is defined (`fwu_config.h:50-52`). Consumers found by grep:
`src/boot/fwu_boot_profile.h` (includes it), comments in `src/activation/fwu_activate.h`,
`src/verify/fwu_verify.h`, `src/boot/fwu_boot.h`, `src/counters/fwu_counters.h`.

### 1.1 Value constants

| Macro | Value | Meaning |
|---|---|---|
| `FWU_VERIFY_NONE` | 0 | development only |
| `FWU_VERIFY_CRC32` | 1 | integrity (accidental corruption), no authorization |
| `FWU_VERIFY_SIGNED` | 2 | SHA-256 + signature |
| `FWU_ACTIVATE_COPY` | 0 | copy from staging, one-way |
| `FWU_ACTIVATE_SWAP_ROLLBACK` | 1 | swap + trial boot + return to the old version |

### 1.2 Inputs (build command line)

| Macro | Role |
|---|---|
| `FWU_PROFILE_DEV`, `FWU_PROFILE_INDUSTRIAL`, `FWU_PROFILE_AUTOMOTIVE` | exactly one must be defined |
| `FWU_ACTIVATION_SWAP_ROLLBACK` | optional; honoured in DEV and INDUSTRIAL, ignored in AUTOMOTIVE (always swap) |
| `FWU_PRODUCTION` | marks a production build; forbidden together with DEV |
| `FWU_FLASH_SPARE_PAGES` | number of spare pages; defaults to 0 if undefined. When a board is selected, `fwu_layout.h` redefines it as `FWU_BOARD_SPARE_PAGES` |
| `FWU_BOARD_*` | selects a board layout (see section 2) |

### 1.3 Outputs

| Macro | Definition |
|---|---|
| `FWU_CFG_VERIFY` | one of the `FWU_VERIFY_*` values (matrix below) |
| `FWU_CFG_ANTI_ROLLBACK` | 0 or 1 (matrix below) |
| `FWU_CFG_ACTIVATION` | `FWU_ACTIVATE_COPY` or `FWU_ACTIVATE_SWAP_ROLLBACK` |
| `FWU_CFG_POWER_SAFE` | always 1 (`fwu_config.h:98`), not an option |
| `FWU_CFG_SPARE_PAGES_JOURNAL` | 2 (journal A + B) |
| `FWU_CFG_SPARE_PAGES_SWAP` | 3 (journal A + B + swap scratch page) |

### 1.4 Protection-profile matrix

| Profile | `FWU_CFG_VERIFY` | `FWU_CFG_ANTI_ROLLBACK` | `FWU_CFG_ACTIVATION` | Spare pages needed |
|---|---|---|---|---|
| `FWU_PROFILE_DEV` | `FWU_VERIFY_CRC32` | 0 | `FWU_ACTIVATE_COPY`; `FWU_ACTIVATE_SWAP_ROLLBACK` if `FWU_ACTIVATION_SWAP_ROLLBACK` is defined | 2 (copy) / 3 (swap) |
| `FWU_PROFILE_INDUSTRIAL` | `FWU_VERIFY_SIGNED` | 0 | `FWU_ACTIVATE_COPY`; `FWU_ACTIVATE_SWAP_ROLLBACK` if `FWU_ACTIVATION_SWAP_ROLLBACK` is defined | 2 (copy) / 3 (swap) |
| `FWU_PROFILE_AUTOMOTIVE` | `FWU_VERIFY_SIGNED` | 1 | `FWU_ACTIVATE_SWAP_ROLLBACK` (mandatory) | 3 |

`FWU_CFG_POWER_SAFE` is 1 in every profile.

### 1.5 Forbidden combinations (compile errors)

| Condition | Result |
|---|---|
| number of defined profiles among DEV / INDUSTRIAL / AUTOMOTIVE is not exactly 1 | `#error` (`fwu_config.h:60-63`) |
| `FWU_PROFILE_DEV` together with `FWU_PRODUCTION` | `#error` (`fwu_config.h:67-69`) |
| `FWU_FLASH_SPARE_PAGES` < `FWU_CFG_SPARE_PAGES_JOURNAL` (2) | `#error` (`fwu_config.h:105-107`) |
| `FWU_CFG_ACTIVATION == FWU_ACTIVATE_SWAP_ROLLBACK` and `FWU_FLASH_SPARE_PAGES` < `FWU_CFG_SPARE_PAGES_SWAP` (3) | `#error` (`fwu_config.h:109-111`) |

Notes from the header comments: the bootloader and the CI signing tool must be built with the same
profile and layout. The version counter (anti-rollback) must be incremented only after the new version
has confirmed itself.

---

## 2. Module `layout` (`src/config/fwu_layout.h` and `layout_*.h`)

**Purpose.** `fwu_layout.h` is the single source of truth of the flash layout: the board file provides
only numbers (`FWU_BOARD_*`); `fwu_layout.h` derives first pages, offsets, sizes and addresses and checks
consistency at compile time. The core works with offsets from the start of the flash region
(`FWU_LAYOUT_*_OFFSET`, unit `fwu_byte_t`); absolute addresses are meant for the port, the bootloader and
the linker script.

**Used by / depends on.** Includes exactly one of `layout_stm32g0b1.h`, `layout_f2800157.h`,
`layout_f280049c.h` (`fwu_layout.h:34-40`). Included by `fwu_config.h`. Consumers found by grep:
`src/boot/fwu_boot_profile.h` (uses `FWU_LAYOUT_IMAGE_MAX_SIZE`, `FWU_LAYOUT_JOURNAL_A_OFFSET`,
`FWU_LAYOUT_SCRATCH_OFFSET`, `FWU_LAYOUT_COUNTER_A_OFFSET`, ...), `src/counters/fwu_counters.h` (comments),
`src/verify/fwu_verify.h` (comments).

### 2.1 Board selection

Exactly one of `FWU_BOARD_STM32G0B1`, `FWU_BOARD_F2800157`, `FWU_BOARD_F280049C` must be defined, else
`#error` (`fwu_layout.h:30-32`). Adding a board = writing `layout_<board>.h` and adding it to the selection
and to the count.

### 2.2 Names every board file must define

`FWU_BOARD_FLASH_BASE`, `FWU_BOARD_PAGE_SIZE`, `FWU_BOARD_TOTAL_PAGES`, `FWU_BOARD_BOOT_PAGES`,
`FWU_BOARD_APP_PAGES`, `FWU_BOARD_STAGING_PAGES`, `FWU_BOARD_LOG_PAGES`, `FWU_BOARD_COUNTER_PAGES`,
`FWU_BOARD_SPARE_PAGES`, `FWU_BOARD_LOG_MIN_PAGES`. Unit: `fwu_byte_t`; addresses in the MCU's address unit.

### 2.3 Regions (increasing page index) and derived macros

Order: bootloader, application, staging, log/config, counter, spare.

| Region | First page (`fwu_layout.h`) | Offset | Size / address macros |
|---|---|---|---|
| bootloader | `FWU_LAYOUT_BOOT_FIRST_PAGE` = 0 | `FWU_LAYOUT_BOOT_OFFSET` | `FWU_LAYOUT_BOOT_ADDRESS` |
| application | `FWU_LAYOUT_APP_FIRST_PAGE` = boot first + `FWU_BOARD_BOOT_PAGES` | `FWU_LAYOUT_APP_OFFSET` | `FWU_LAYOUT_APP_SIZE`, `FWU_LAYOUT_APP_ADDRESS` |
| staging | `FWU_LAYOUT_STAGING_FIRST_PAGE` = app first + `FWU_BOARD_APP_PAGES` | `FWU_LAYOUT_STAGING_OFFSET` | `FWU_LAYOUT_STAGING_SIZE`, `FWU_LAYOUT_STAGING_ADDRESS` |
| log/config | `FWU_LAYOUT_LOG_FIRST_PAGE` = staging first + `FWU_BOARD_STAGING_PAGES` | `FWU_LAYOUT_LOG_OFFSET` | `FWU_LAYOUT_LOG_SIZE`, `FWU_LAYOUT_LOG_ADDRESS` |
| counter | `FWU_LAYOUT_COUNTER_FIRST_PAGE` = log first + `FWU_BOARD_LOG_PAGES` | `FWU_LAYOUT_COUNTER_OFFSET` | `FWU_LAYOUT_COUNTER_ADDRESS` |
| spare | `FWU_LAYOUT_SPARE_FIRST_PAGE` = counter first + `FWU_BOARD_COUNTER_PAGES` | `FWU_LAYOUT_SPARE_OFFSET` | `FWU_LAYOUT_SPARE_SIZE`, `FWU_LAYOUT_SPARE_ADDRESS` |

Helper: `FWU_LAYOUT_OFFSET(page)` = `(page) * FWU_BOARD_PAGE_SIZE`. Every `*_ADDRESS` =
`FWU_BOARD_FLASH_BASE` + the matching `*_OFFSET`.

Inside regions:

| Sub-region | Macros |
|---|---|
| counter region = [counter A][counter B], one page each | `FWU_LAYOUT_COUNTER_PAGES` (2u), `FWU_LAYOUT_COUNTER_A_OFFSET`, `FWU_LAYOUT_COUNTER_B_OFFSET` (= counter offset + page size), `FWU_LAYOUT_COUNTER_LENGTH` (= page size) |
| staging = [manifest page][image pages] | `FWU_LAYOUT_STAGING_MANIFEST_OFFSET` (= staging offset), `FWU_LAYOUT_STAGING_IMAGE_OFFSET` (= staging offset + one page), `FWU_LAYOUT_IMAGE_MAX_PAGES` (= staging pages - 1), `FWU_LAYOUT_IMAGE_MAX_SIZE` |
| spare = [journal A][journal B][swap scratch x K] | `FWU_LAYOUT_SPARE_PAGES_JOURNAL` (2u), `FWU_LAYOUT_SPARE_PAGES_SWAP` (3u), `FWU_LAYOUT_JOURNAL_A_OFFSET` (= spare offset), `FWU_LAYOUT_JOURNAL_B_OFFSET` (= spare offset + one page), `FWU_LAYOUT_JOURNAL_LENGTH` (= page size). Only if `FWU_BOARD_SPARE_PAGES >= FWU_LAYOUT_SPARE_PAGES_SWAP`: `FWU_LAYOUT_SCRATCH_OFFSET` (= spare offset + 2 pages), `FWU_LAYOUT_SCRATCH_PAGES` (= spare pages - 2) |

The manifest page never takes part in the swap/copy (comment, `fwu_layout.h:79-82`); activation sees the
image at `FWU_LAYOUT_STAGING_IMAGE_OFFSET` with length `FWU_LAYOUT_IMAGE_MAX_SIZE`.

`FWU_FLASH_SPARE_PAGES` is `#undef`-ed if already defined and redefined as `FWU_BOARD_SPARE_PAGES`
(`fwu_layout.h:117-120`); `fwu_config.h` reads it.

### 2.4 Consistency checks (all are `#error`)

| # | Condition that triggers the error | Line |
|---|---|---|
| 1 | not exactly one board defined | `fwu_layout.h:30` |
| 2 | boot + app + staging + log + counter + spare pages != `FWU_BOARD_TOTAL_PAGES` | `fwu_layout.h:124` |
| 3 | `FWU_BOARD_COUNTER_PAGES` != `FWU_LAYOUT_COUNTER_PAGES` (must be exactly 2) | `fwu_layout.h:129` |
| 4 | `FWU_BOARD_STAGING_PAGES` != `FWU_BOARD_APP_PAGES` | `fwu_layout.h:133` |
| 5 | `FWU_BOARD_STAGING_PAGES` < 2 (1 manifest page + at least 1 image page) | `fwu_layout.h:137` |
| 6 | `FWU_BOARD_SCRATCH_PAGES` defined and `FWU_BOARD_SCRATCH_PAGES + FWU_LAYOUT_SPARE_PAGES_JOURNAL` != `FWU_BOARD_SPARE_PAGES` | `fwu_layout.h:141` |
| 7 | `FWU_BOARD_SPARE_PAGES` < `FWU_LAYOUT_SPARE_PAGES_JOURNAL` (2) | `fwu_layout.h:145` |
| 8 | `FWU_BOARD_LOG_PAGES` < `FWU_BOARD_LOG_MIN_PAGES` | `fwu_layout.h:149` |

### 2.5 Board files

All three files take the page counts from overridable macros (`#ifndef ... #define`); the defaults are
below. The spare count defaults to `2u + FWU_BOARD_SCRATCH_PAGES`; `FWU_BOARD_LOG_PAGES` defaults to total
minus all other regions; `FWU_BOARD_COUNTER_PAGES` defaults to 2u. The header comments say overriding is
for tests and new product layouts only; in production the bootloader, driver and CI must use the same values.

| Item | `layout_stm32g0b1.h` | `layout_f280049c.h` | `layout_f2800157.h` |
|---|---|---|---|
| Selected by | `FWU_BOARD_STM32G0B1` | `FWU_BOARD_F280049C` | `FWU_BOARD_F2800157` |
| `FWU_BOARD_FLASH_BASE` | `0x08000000u` | `0u` (offset base, not a real address) | `0x00080000u` (word address) |
| `FWU_BOARD_PAGE_SIZE` | `2048u` | `8192UL` (UL required, see comment: `int` is 16 bit on C28x) | `1024u` |
| `FWU_BOARD_TOTAL_PAGES` | `256u` | `32u` | `128u` |
| Unit of `fwu_byte_t` in layout | byte | octet (2 octets per flash word) | 16-bit word |
| `FWU_BOARD_BOOT_PAGES` | 18u | 3u | 8u |
| `FWU_BOARD_APP_PAGES` | 110u | 10u | 55u |
| `FWU_BOARD_STAGING_PAGES` | 110u | 10u | 55u |
| `FWU_BOARD_SCRATCH_PAGES` (K) default | 2u | 1u | 1u |
| `FWU_BOARD_SPARE_PAGES` default | 4u | 3u | 3u |
| `FWU_BOARD_COUNTER_PAGES` | 2u | 2u | 2u |
| `FWU_BOARD_LOG_PAGES` default (derived) | 12u | 4u | 5u |
| `FWU_BOARD_LOG_MIN_PAGES` | 7u | 1u | 1u |
| Image capacity (staging - 1 pages) | 109 pages | 9 pages = 73728 octets (per header) | 54 pages |
| Extra macros | - | `FWU_C28_FLASH_WORD_BASE` (`0x00080000u`), `FWU_C28_BANK1_WORD_BASE` (`0x00090000u`), `FWU_C28_SECTOR_WORDS` (`0x1000u`), `FWU_C28_OCTETS_PER_WORD` (`2u`), `FWU_C28_WORD_ADDRESS(octet_offset)` | - |
| Production guard | none | `#error` if `FWU_PRODUCTION` and not `FWU_BOARD_NUMBERS_VERIFIED` (line 86) | `#error` if `FWU_PRODUCTION` and not `FWU_BOARD_NUMBERS_VERIFIED` (line 89) |

Default page maps (from the header tables; the derived-value rows above were recomputed from the macros):

| Board | boot | app | staging | log/config | counter | spare |
|---|---|---|---|---|---|---|
| STM32G0B1 (pages) | 0..17 | 18..127 | 128..237 | 238..249 | 250..251 | 252..255 |
| F280049C (sectors) | 0..2 | 3..12 | 13..22 | 23..26 | 27..28 | 29..31 |
| F2800157 (sectors) | 0..7 | 8..62 | 63..117 | 118..122 | 123..124 | 125..127 |

`FWU_C28_WORD_ADDRESS(octet_offset)` = `FWU_C28_FLASH_WORD_BASE + (octet_offset) / FWU_C28_OCTETS_PER_WORD`
(offset must be even). On the F280049C board `FWU_LAYOUT_*_ADDRESS` are octet offsets, not real addresses.
The write granule (16 octets on F280049C) is given by the port geometry, not by the layout file.

#### Values the headers themselves mark as unverified

| Header | Unverified items (as stated in the header) |
|---|---|
| `layout_stm32g0b1.h` | Chip properties "2 KB page, dual bank, 64-bit programming" are marked `[UNVERIFIED]` (data sheet). The ~10 thousand erase endurance used in the K example is marked `[UNVERIFIED]`. The part number `STM32G0B1KEU6` is an assumption (the requested "KBEU6" is not a standard code); xB (128 KB) and xC (256 KB) parts need their own numbers file. No `FWU_PRODUCTION` guard in this file. |
| `layout_f2800157.h` | The whole hardware block is marked `[UNVERIFIED]`: 256 KB single-bank flash; sector size 2 KB (the header warns that F28003x has 4 KB sectors; if the sector is 2 KW the page size/total pages must change); flash base `0x00080000` for F28001x; Flash API programming rules (max 128 bit per call, 64-bit aligned, ECC per 64-bit group, each 64-bit word programmed once per erase). The region split is declared a DRAFT (product decision). Open design question: 1 or 2 bytes per word for the image payload ("README open issue 10"). Production builds need `-DFWU_BOARD_NUMBERS_VERIFIED`. |
| `layout_f280049c.h` | Numbers come from TI's `28004x_generic_flash_lnk.cmd` (C2000Ware 6.00.01.00) but are "not additionally verified against the TRM"; production builds need `-DFWU_BOARD_NUMBERS_VERIFIED`. Region split is overridable and "tuned by measuring in CCS". |

---

## 3. Module `fwu_manifest` (`src/manifest/fwu_manifest.[ch]`)

**Purpose.** Reads the fixed-size, signed manifest ("identity card" of an image) from a byte buffer and
validates its structure only. It does not verify the signature, product, version or hash; the signature
is only copied. Pure operation: `buffer` is not modified; the output is written only on success.

**Used by / depends on.** Includes `fwu_error.h`, `fwu_types.h` (header) and `fwu_bytes.h` (source).
Called from `src/verify/fwu_verify.c:29` and `:200`, `src/client/fwu_client.c:363`.

### 3.1 Byte layout (multi-byte fields big endian)

| Offset | Size | Field | Offset macro | Size macro |
|---|---|---|---|---|
| 0 | 4 | magic (`0x46574D46`, "FWMF") | `FWU_MANIFEST_OFFSET_MAGIC` | `FWU_MANIFEST_MAGIC_SIZE` |
| 4 | 1 | format_version (now 1) | `FWU_MANIFEST_OFFSET_FORMAT` | `FWU_MANIFEST_FORMAT_SIZE` |
| 5 | 1 | key_id | `FWU_MANIFEST_OFFSET_KEY_ID` | `FWU_MANIFEST_KEY_ID_SIZE` |
| 6 | 2 | reserved (must be zero) | `FWU_MANIFEST_OFFSET_RESERVED` | `FWU_MANIFEST_RESERVED_SIZE` |
| 8 | 4 | product_id | `FWU_MANIFEST_OFFSET_PRODUCT_ID` | `FWU_MANIFEST_PRODUCT_ID_SIZE` |
| 12 | 4 | image_version | `FWU_MANIFEST_OFFSET_IMAGE_VERSION` | `FWU_MANIFEST_IMAGE_VERSION_SIZE` |
| 16 | 2 | key_version | `FWU_MANIFEST_OFFSET_KEY_VERSION` | `FWU_MANIFEST_KEY_VERSION_SIZE` |
| 18 | 4 | image_size (number of `fwu_byte_t`; 0 invalid) | `FWU_MANIFEST_OFFSET_IMAGE_SIZE` | `FWU_MANIFEST_IMAGE_SIZE_SIZE` |
| 22 | 32 | sha256 of the image | `FWU_MANIFEST_OFFSET_SHA256` | `FWU_MANIFEST_SHA256_SIZE` |
| 54 | 20 | commit_hash | `FWU_MANIFEST_OFFSET_COMMIT_HASH` | `FWU_MANIFEST_COMMIT_HASH_SIZE` |
| 74 | 64 | signature (Ed25519, 64 bytes) | `FWU_MANIFEST_OFFSET_SIGNATURE` | `FWU_MANIFEST_SIGNATURE_SIZE` |
| total 138 | | | | |

Offsets are derived in the header from the previous offset plus size. `FWU_MANIFEST_SIGNED_SIZE` (= offset of
the signature = 74) is the length of the signed region (everything except the signature);
`FWU_MANIFEST_SIZE` (= 138) is the total length. Other macros: `FWU_MANIFEST_MAGIC` (`0x46574D46u`),
`FWU_MANIFEST_FORMAT_VERSION` (`1u`). In staging the manifest occupies the first page
(`FWU_LAYOUT_STAGING_MANIFEST_OFFSET`).

### 3.2 Type

```c
typedef struct {
    fwu_byte_t format_version;
    fwu_byte_t key_id;
    uint32_t   product_id;
    uint32_t   image_version;
    uint16_t   key_version;
    uint32_t   image_size;
    fwu_byte_t sha256[FWU_MANIFEST_SHA256_SIZE];
    fwu_byte_t commit_hash[FWU_MANIFEST_COMMIT_HASH_SIZE];
    fwu_byte_t signature[FWU_MANIFEST_SIGNATURE_SIZE];
} fwu_manifest_t;
```

(`fwu_manifest.h:98-108`; `reserved` is not stored.) Each array element holds one byte value 0..255.

### 3.3 `fwu_manifest_parse`

```c
fwu_return_e fwu_manifest_parse(const fwu_byte_t *buffer, uint32_t buffer_length, fwu_manifest_t *manifest);
```

Source: declaration `src/manifest/fwu_manifest.h:129`, definition `src/manifest/fwu_manifest.c:18`.

| Item | Detail |
|---|---|
| Responsibility | Decode the manifest at the start of `buffer`, check structure, copy fields |
| `buffer` | at least `FWU_MANIFEST_SIZE` elements; read only; elements are masked with `FWU_BYTE_MASK` |
| `buffer_length` | element count; larger than `FWU_MANIFEST_SIZE` is allowed, the excess is ignored |
| `manifest` | written only on `FWU_OK` (a local copy is filled, then assigned) |
| Returns | `FWU_OK`; `FWU_ERR_INVALID_ARG`; `FWU_ERR_NOT_ENOUGH_DATA`; `FWU_ERR_BAD_MANIFEST` |
| Check order (first mismatch wins) | 1. NULL `buffer`/`manifest` -> `FWU_ERR_INVALID_ARG`; 2. `buffer_length` < `FWU_MANIFEST_SIZE` -> `FWU_ERR_NOT_ENOUGH_DATA`; 3. magic mismatch -> `FWU_ERR_BAD_MANIFEST`; 4. `format_version` != `FWU_MANIFEST_FORMAT_VERSION` -> `FWU_ERR_BAD_MANIFEST`; 5. any reserved element non-zero -> `FWU_ERR_BAD_MANIFEST`; 6. `image_size` == 0 -> `FWU_ERR_BAD_MANIFEST` |
| Not checked | signature, product id, versions, hash, key id, `image_size` against capacity |
| Context | no flash access, no blocking, no allocation in the code; fixed-size loops only |

---

## 4. Module `fwu_journal` (`src/journal/fwu_journal.[ch]`)

**Purpose.** A single-region journal: small self-validating records appended to the first empty slot of
a page-aligned flash region. It provides only a reliable ledger; the meaning of `state`, `flags` and `arg`
belongs to the activation logic (header names task G8b). Nothing is ever erased by this module. If the
region is full, append returns `FWU_ERR_BUFFER_FULL`; rotation between regions is done by `fwu_journal_ab`.

**Used by / depends on.** Includes `fwu_error.h`, `fwu_types.h`, `fwu_flash.h` (header); `fwu_bytes.h`,
`fwu_crc.h` (source: `fwu_crc16_classic_calculate`, big-endian read/write helpers). Used by
`src/journal/fwu_journal_ab.c`. No other direct call sites outside `src/journal/` and tests were found.

### 4.1 Record format (16 `fwu_byte_t`, multi-byte fields big endian)

| Offset | Size | Field | Macro |
|---|---|---|---|
| 0 | 2 | magic `0x4A52` ("JR") | `FWU_JOURNAL_OFFSET_MAGIC`, `FWU_JOURNAL_MAGIC` |
| 2 | 1 | state (opaque) | `FWU_JOURNAL_OFFSET_STATE` |
| 3 | 1 | flags (opaque) | `FWU_JOURNAL_OFFSET_FLAGS` |
| 4 | 4 | seq | `FWU_JOURNAL_OFFSET_SEQ` |
| 8 | 4 | arg (opaque) | `FWU_JOURNAL_OFFSET_ARG` |
| 12 | 2 | reserved, written as zero | `FWU_JOURNAL_OFFSET_RESERVED` |
| 14 | 2 | crc | `FWU_JOURNAL_OFFSET_CRC` |

`FWU_JOURNAL_RECORD_SIZE` = 16u. `FWU_JOURNAL_CRC_COVERED_SIZE` = 14u: the CRC covers offsets 0..13 and is
computed with `fwu_crc16_classic_calculate` (CRC-16, reflected polynomial `0xA001`, initial value `0xFFFF`,
per `src/crc/fwu_crc.h`) and stored big endian. The reserved bytes are not checked when parsing.

### 4.2 Types

```c
typedef struct {
    fwu_byte_t state;
    fwu_byte_t flags;
    uint32_t   seq;
    uint32_t   arg;
} fwu_journal_record_t;

typedef struct {
    const fwu_flash_t   *flash;
    uint32_t             offset;
    uint32_t             slot_count;
    uint32_t             next_slot;
    uint32_t             next_seq;
    bool                 has_last;
    fwu_journal_record_t last;
    uint32_t             valid_slots;
    uint32_t             invalid_slots;
} fwu_journal_t;
```

Meaning: `offset` is the region offset from flash start; `slot_count` = region length / 16; `next_slot` is the
first empty slot (equals `slot_count` if full); `next_seq` is the sequence number for the next record
(1 if no valid record); `valid_slots` / `invalid_slots` are counters of the last scan (`valid_slots` is also
incremented by a successful append). The header says: do not touch the fields directly. `flash` must stay
valid while the journal is used.

### 4.3 Scan rules as implemented (`fwu_journal.c:60-106`)

* Slots are read in order from slot 0. A slot whose 16 elements all equal `erased_value` (masked) is empty:
  scanning stops there and that slot index becomes `next_slot`. Records after the first empty slot are not seen.
* A non-empty slot is accepted only if magic and CRC match and, when a previous valid record exists,
  `seq == previous seq + 1` (32-bit wrap included). Otherwise it is counted in `invalid_slots` and skipped.
* `next_seq` = last valid seq + 1, or 1 if there is no valid record.
* Append writes one 16-element record at `offset + next_slot * 16` with `fwu_flash_write` and updates the RAM
  state only if the write returned `FWU_OK`.

### 4.4 Functions

```c
fwu_return_e fwu_journal_init(fwu_journal_t *journal, const fwu_flash_t *flash, uint32_t offset, uint32_t length);
```

Source: `fwu_journal.h:101`, `fwu_journal.c:108`.

| Item | Detail |
|---|---|
| Responsibility | Bind the journal to a region and scan it |
| Parameters | `journal` non-NULL, output; `flash` non-NULL, validated inside, must outlive the journal; `offset` page aligned, `fwu_byte_t` from flash start; `length` non-zero multiple of page size |
| Returns (in check order) | `FWU_ERR_INVALID_ARG` (NULL `journal`/`flash`); `FWU_ERR_FLASH_GEOMETRY` (`fwu_flash_validate` failed, or 16 is not a multiple of `write_granule`); `FWU_ERR_INVALID_ARG` (`length` 0, or `offset`/`length` not page aligned); `FWU_ERR_OUT_OF_RANGE` (region outside flash); otherwise the result of `fwu_journal_scan` (`FWU_OK` or a flash read error such as `FWU_ERR_INVALID_ARG`, `FWU_ERR_OUT_OF_RANGE` or a port error) |
| Context | reads the whole region slot by slot through `fwu_flash_read` (one read per slot until the first empty slot) |

```c
fwu_return_e fwu_journal_scan(fwu_journal_t *journal);
```

Source: `fwu_journal.h:108`, `fwu_journal.c:60`.

| Item | Detail |
|---|---|
| Responsibility | Re-read the region; update `next_slot`, `has_last`, `last`, `next_seq`, `valid_slots`, `invalid_slots` |
| Returns | `FWU_ERR_INVALID_ARG` (`journal` or `journal->flash` NULL); a flash read error as returned by `fwu_flash_read`; `FWU_OK`. On a read error the journal fields are not updated |
| Preconditions | journal was initialized by `fwu_journal_init`; must be used after a failed append or after power loss |

```c
fwu_return_e fwu_journal_append(fwu_journal_t *journal, fwu_byte_t state, fwu_byte_t flags, uint32_t arg);
```

Source: `fwu_journal.h:120`, `fwu_journal.c:155`.

| Item | Detail |
|---|---|
| Responsibility | Append a record (seq = `next_seq`) to the first empty slot |
| Parameters | `state`, `flags`: masked to 8 bits (`FWU_BYTE_MASK`); `arg`: 32-bit opaque value |
| Returns | `FWU_OK`; `FWU_ERR_INVALID_ARG` (`journal` or `journal->flash` NULL); `FWU_ERR_BUFFER_FULL` (`next_slot >= slot_count`); any error from `fwu_flash_write` (for example `FWU_ERR_OUT_OF_RANGE`, `FWU_ERR_WRITE_ALIGNMENT`, `FWU_ERR_FLASH_NOT_ERASED`, `FWU_ERR_FLASH_POWER_LOSS`, `FWU_ERR_FLASH_WRITE`; the latter ones are produced by ports/simulator, not by this module) |
| After an error | RAM state is stale/unreliable: call `fwu_journal_scan` |
| Context | one `fwu_flash_write` of 16 elements; blocking behaviour is that of the flash port callback (UNVERIFIED for the ports) |

```c
bool fwu_journal_get_last(const fwu_journal_t *journal, fwu_journal_record_t *out);
```

Source: `fwu_journal.h:128`, `fwu_journal.c:189`.

| Item | Detail |
|---|---|
| Responsibility | Query the last valid record from the RAM state (no flash access) |
| Returns | `true` if `has_last` (and copies to `out` when `out` is non-NULL); `false` if `journal` is NULL or has no valid record |

---

## 5. Module `fwu_journal_ab` (`src/journal/fwu_journal_ab.[ch]`)

**Purpose.** Two `fwu_journal_t` regions (A and B) used alternately so that the journal never fills up
permanently, with a power-loss-safe switch between them. Meaning of records stays with the caller: the
snapshot copies `state`, `flags` and `arg` unchanged, so the last record must be a sufficient state on its own.

**Used by / depends on.** Includes `fwu_error.h`, `fwu_types.h`, `fwu_flash.h`, `fwu_journal.h`. Used by
`src/activation/fwu_activate.c:58,123,387` (activation journal, region offsets from the layout) and
`src/counters/fwu_counters.c:14,37,52,93` (counter A/B pages).

### 5.1 Type

```c
typedef struct {
    fwu_journal_t part[2];
    uint32_t      active;
    uint32_t      region_length;
} fwu_journal_ab_t;
```

`part[0]` = region A, `part[1]` = region B; `active` is 0 or 1; `region_length` in `fwu_byte_t`. Do not touch
the fields directly.

### 5.2 Region requirements

Same length, page aligned, not overlapping, each with at least 2 slots (snapshot + real record). Both
regions should be erased before first use (the header calls this a factory/bootloader provisioning task).

### 5.3 Active-region rule (`fwu_journal_ab.c:19-33`)

After both regions are scanned:

| Region state | Active |
|---|---|
| both have a valid record | B (1) if B's last `seq` is strictly newer than A's (wrap-safe: difference 1..`0x7FFFFFFF`), else A (0); equal seq selects A |
| only B has a valid record | B |
| only A has a valid record, or neither | A |

### 5.4 Rotation (A/B switch) as implemented (`fwu_journal_ab.c:41-77`, called from append)

Triggered in `fwu_journal_ab_append` when the active region has `next_slot >= slot_count`.

1. If the active region has no valid record (full of garbage/partial records): erase the active region itself
   (`fwu_flash_erase` over `region_length`), rescan it, return (no snapshot is written).
2. Otherwise: erase the other region, rescan it (RAM state reset to empty).
3. Set the other region's `next_seq` = active last `seq` + 1 and append a snapshot: a copy of the active
   region's last `state`, `flags`, `arg` into slot 0 of the other region.
4. Set `active` to the other region.
5. Back in append: the requested record is appended to the (new) active region by `fwu_journal_append`.

Power-loss reasoning given in the header: loss during step 2 (erase) leaves the active region intact; loss
during the snapshot leaves an invalid partial record that is ignored; loss after the snapshot leaves the snapshot as a
valid latest state (the requested record is then simply not written). In each case the last valid record is
either the old state or the new one. Any error returned leaves the RAM state unreliable; call
`fwu_journal_ab_scan`.

### 5.5 Functions

```c
fwu_return_e fwu_journal_ab_init(fwu_journal_ab_t *journal, const fwu_flash_t *flash,
                                 uint32_t offset_a, uint32_t offset_b, uint32_t length);
```

Source: `fwu_journal_ab.h:70-72`, `fwu_journal_ab.c:100`.

| Item | Detail |
|---|---|
| Responsibility | Init both regions (`fwu_journal_init` for A then B), check non-overlap and slot count, select the active region |
| Parameters | `journal` non-NULL; `flash` non-NULL and must outlive the journal; `offset_a`, `offset_b` page aligned; `length` each region, multiple of page size, at least 2 records (32 elements) |
| Returns | `FWU_ERR_INVALID_ARG` (NULL arguments; region misaligned, zero length, overlapping regions, fewer than 2 slots); `FWU_ERR_FLASH_GEOMETRY`; `FWU_ERR_OUT_OF_RANGE`; flash read errors as is (all propagated from `fwu_journal_init`); `FWU_OK` |
| Context | reads both regions through `fwu_flash_read` |

```c
fwu_return_e fwu_journal_ab_scan(fwu_journal_ab_t *journal);
```

Source: `fwu_journal_ab.h:78`, `fwu_journal_ab.c:81`. Rescans A then B and re-selects the active region.
Returns `FWU_ERR_INVALID_ARG` if `journal` is NULL, a flash read error as is, or `FWU_OK`.

```c
fwu_return_e fwu_journal_ab_append(fwu_journal_ab_t *journal, fwu_byte_t state, fwu_byte_t flags,
                                   uint32_t arg);
```

Source: `fwu_journal_ab.h:86-88`, `fwu_journal_ab.c:132`.

| Item | Detail |
|---|---|
| Responsibility | Append a record to the active region; rotate first if the active region is full |
| Returns | `FWU_ERR_INVALID_ARG` (NULL `journal`); errors from `fwu_flash_erase` / `fwu_journal_scan` / `fwu_journal_append` during rotation or the final append, as is (note: `FWU_ERR_BUFFER_FULL` cannot be produced by the final append after a successful rotation because the new region has a free slot, given at least 2 slots; not separately tested here); `FWU_OK` |
| Context | may perform a full-region `fwu_flash_erase` (blocking time = port erase time, UNVERIFIED for ports) when rotating |
| After an error | call `fwu_journal_ab_scan` |

```c
bool fwu_journal_ab_get_last(const fwu_journal_ab_t *journal, fwu_journal_record_t *out);
```

Source: `fwu_journal_ab.h:94`, `fwu_journal_ab.c:153`. Returns `false` if `journal` is NULL; otherwise forwards to
`fwu_journal_get_last` on the active region (`true` with `out` filled when a valid record exists).
