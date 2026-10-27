# API reference: pull, flash, ring buffer

Modules covered (all paths relative to the repository root):

| Module | Files |
|---|---|
| Flash abstraction (core wrappers + port contract) | `src/flash/fwu_flash.h`, `src/flash/fwu_flash.c` |
| Simulated flash (test/PC only) | `src/flash/fwu_flash_sim.h`, `src/flash/fwu_flash_sim.c` |
| Ring buffer | `src/ring_buffer/fwu_ringbuf.h`, `src/ring_buffer/fwu_ringbuf.c` |
| Minimal HTTP/1.1 client layer | `src/pull/fwu_http.h`, `src/pull/fwu_http.c` |
| Pull-from-server controller | `src/pull/fwu_pull.h`, `src/pull/fwu_pull.c` |

`fwu_byte_t` is `uint16_t` or `unsigned char` depending on the target (`src/fwu_types.h:36`, `:41`); every length/offset below is counted in `fwu_byte_t` elements. Return type `fwu_return_e` is defined in `src/error/fwu_error.h`; the only success value is `FWU_OK` (0).

---

## 1. Flash (`fwu_flash`)

**Purpose.** The core never touches hardware flash directly. It calls three wrapper functions (`fwu_flash_erase`, `fwu_flash_write`, `fwu_flash_read`) which validate arguments against the geometry stored in a `fwu_flash_t` and then forward to a port-supplied callback table (`fwu_flash_callback_t`). `fwu_flash_validate` must be run once on a filled-in object before first use.

**Used by.** `#include "fwu_flash.h"` in: `src/activation/fwu_activate.h`, `src/boot/fwu_boot.h`, `src/counters/fwu_counters.h`, `src/journal/fwu_journal.h`, `src/journal/fwu_journal_ab.h`, `src/session/fwu_session.h`, `src/verify/fwu_verify.h`, `src/flash/fwu_flash_sim.h`, and the port headers `ports/c2000/f28004x/fwu_flash_adapter.h`, `ports/c2000/f28004x_direct/fwu_flash_f28004x.h`, `ports/stm32g0/flash/fwu_flash_stm32g0.h`. Call sites of the wrappers are listed in section 1.5. **Depends on:** `fwu_types.h`, `fwu_error.h`.

### 1.1 Types

```c
typedef struct
{
    fwu_return_e (*flash_erase) (void *context, uint32_t offset, uint32_t length);
    fwu_return_e (*flash_write) (void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data);
    fwu_return_e (*flash_read)  (void *context, uint32_t offset, uint32_t length, fwu_byte_t *data);
} fwu_flash_callback_t;
```
`src/flash/fwu_flash.h:12`

```c
typedef struct
{
    uint32_t   page_length;
    uint32_t   flash_length;
    uint32_t   write_granule;
    fwu_byte_t erased_value;
} fwu_flash_geometry_t;
```
`src/flash/fwu_flash.h:19`

| Field | Meaning (from the header comments) |
|---|---|
| `page_length` | Page size in `fwu_byte_t`; erase is done in this unit. |
| `flash_length` | Total size of the flash region in `fwu_byte_t`. |
| `write_granule` | Smallest writable piece in `fwu_byte_t`; write offset and length must be multiples of it. |
| `erased_value` | Value left in every cell after an erase (usually `0xFF`). |

```c
typedef struct {
    void                       *context;
    const fwu_flash_callback_t *callback;
    fwu_flash_geometry_t       geometry;
} fwu_flash_t;
```
`src/flash/fwu_flash.h:27`

| Field | Meaning |
|---|---|
| `context` | Port's own data (e.g. base address). The core never dereferences it; it is passed unchanged as the first argument of every callback. Must stay valid while the flash is used. May be `NULL`. |
| `callback` | Port function table. Must not be `NULL`; may be shared between flash objects. |
| `geometry` | Filled in by the port; the core assumes nothing beyond what `fwu_flash_validate` checks. |

All offsets passed to the wrappers and callbacks are relative to the start of the flash region described by the object (they are compared against `geometry.flash_length`).

### 1.2 `fwu_flash_validate`

```c
fwu_return_e fwu_flash_validate(const fwu_flash_t *self);
```

- Responsibility: checks the object after the port has filled it in; call it BEFORE first use. Does not modify `self`.
- Checks, in this order (`src/flash/fwu_flash.c:5-21`): (1) `self`, `self->callback`, and all three function pointers non-`NULL`; (2) `flash_length`, `page_length`, `write_granule` all non-zero; (3) `flash_length % page_length == 0`; (4) `page_length % write_granule == 0`. `erased_value` and `context` are not checked.
- Returns: `FWU_OK`; `FWU_ERR_INVALID_ARG` if check 1 fails; `FWU_ERR_FLASH_GEOMETRY` if check 2, 3 or 4 fails.
- Precondition: none. Note that `fwu_flash_erase/write/read` do NOT call it; with a non-validated object (zero `page_length`/`write_granule`) the wrappers perform a modulo by zero (`src/flash/fwu_flash.c:30`, `:45`).
- Source: `src/flash/fwu_flash.h:50`, `src/flash/fwu_flash.c:5`.
- Who calls it: `fwu_activate_init` (`src/activation/fwu_activate.c:331`, maps failure to `FWU_ERR_FLASH_GEOMETRY`), `fwu_journal_init` (`src/journal/fwu_journal.c:123`, maps failure to `FWU_ERR_FLASH_GEOMETRY`), `fwu_session_init` (`src/session/fwu_session.c:174`), and each port's init and `fwu_flash_sim_init_raw` (`src/flash/fwu_flash_sim.c:188`).

### 1.3 `fwu_flash_erase`

```c
fwu_return_e fwu_flash_erase(const fwu_flash_t *self, uint32_t offset, uint32_t length);
```

| Item | Detail |
|---|---|
| Responsibility | Validate then erase `length` elements starting at `offset`, in whole pages. |
| `offset` | Multiple of `page_length`. |
| `length` | Non-zero, multiple of `page_length`. |
| Check order | (1) `self`, `self->callback`, `self->callback->flash_erase` non-`NULL` and `length != 0` -> `FWU_ERR_INVALID_ARG`; (2) `offset <= flash_length` and `length <= flash_length - offset` else `FWU_ERR_OUT_OF_RANGE`; (3) `offset % page_length == 0` and `length % page_length == 0` else `FWU_ERR_ERASE_ALIGNMENT`; (4) forward to `callback->flash_erase(self->context, offset, length)` and return its value unchanged. A request that is both out of range and misaligned returns `FWU_ERR_OUT_OF_RANGE`. |
| Returns | `FWU_OK`, `FWU_ERR_INVALID_ARG`, `FWU_ERR_OUT_OF_RANGE`, `FWU_ERR_ERASE_ALIGNMENT`, or whatever the port returns. |
| Blocking | Blocks for the duration of the port callback (the wrapper adds no waiting of its own). |
| Source | `src/flash/fwu_flash.h:63`, `src/flash/fwu_flash.c:23` |

### 1.4 `fwu_flash_write` and `fwu_flash_read`

```c
fwu_return_e fwu_flash_write(const fwu_flash_t *self, uint32_t offset, uint32_t length, const fwu_byte_t *data);
```

| Item | Detail |
|---|---|
| Responsibility | Validate then program `length` elements at `offset`. The header states the target area must have been erased beforehand; the wrapper itself does NOT check this. |
| `offset`, `length` | Multiples of `write_granule`; `length != 0`. |
| `data` | Non-`NULL`, at least `length` elements; read only. |
| Check order | (1) `self`, `callback`, `callback->flash_write` non-`NULL`, `length != 0`, `data != NULL` -> `FWU_ERR_INVALID_ARG`; (2) range -> `FWU_ERR_OUT_OF_RANGE`; (3) `offset % write_granule` / `length % write_granule` -> `FWU_ERR_WRITE_ALIGNMENT`; (4) forward to `callback->flash_write(self->context, offset, length, data)`. |
| Returns | `FWU_OK`, `FWU_ERR_INVALID_ARG`, `FWU_ERR_OUT_OF_RANGE`, `FWU_ERR_WRITE_ALIGNMENT`, or the port's code (e.g. `FWU_ERR_FLASH_WRITE`). |
| Source | `src/flash/fwu_flash.h:76`, `src/flash/fwu_flash.c:38` |

```c
fwu_return_e fwu_flash_read(const fwu_flash_t *self, uint32_t offset, uint32_t length, fwu_byte_t *data);
```

| Item | Detail |
|---|---|
| Responsibility | Validate then copy `length` elements from `offset` into `data`. No alignment requirement. |
| `data` | Non-`NULL` output buffer of at least `length` elements. |
| Check order | (1) `self`, `callback`, `callback->flash_read` non-`NULL`, `length != 0`, `data != NULL` -> `FWU_ERR_INVALID_ARG`; (2) range -> `FWU_ERR_OUT_OF_RANGE`; (3) forward to `callback->flash_read(self->context, offset, length, data)`. |
| Returns | `FWU_OK`, `FWU_ERR_INVALID_ARG`, `FWU_ERR_OUT_OF_RANGE`, or the port's code. |
| Source | `src/flash/fwu_flash.h:87`, `src/flash/fwu_flash.c:53` |

### 1.5 Flash port contract

This section is the contract a flash port (the code that fills in `fwu_flash_callback_t`) must satisfy, derived from the wrappers and from every call site in `src/`.

#### 1.5.1 What a port must provide

1. A `fwu_flash_callback_t` table with all three pointers non-`NULL` (`src/flash/fwu_flash.c:6`).
2. A `fwu_flash_t` whose `callback` points to that table, whose `context` is whatever the callbacks need (may be `NULL`), and whose `geometry` satisfies the four validate conditions (section 1.2).
3. A call to `fwu_flash_validate(&flash)` at the end of the port's init, returning its result. All in-tree ports do this: `ports/stm32g0/flash/fwu_flash_stm32g0.c:184`, `ports/c2000/f28004x/fwu_flash_adapter.c:137`, `ports/c2000/f28004x_direct/fwu_flash_f28004x.c:192`, `src/flash/fwu_flash_sim.c:188`.

The core stores and passes the object as `const fwu_flash_t *`.

#### 1.5.2 Exact callback declarations and what the core guarantees before each call

The callback is invoked only from the wrapper, as the last statement after the checks below (`src/flash/fwu_flash.c:34`, `:49`, `:61`). `context` is `self->context` unchanged.

```c
fwu_return_e (*flash_erase) (void *context, uint32_t offset, uint32_t length);
```
Guaranteed on entry: `length != 0`; `offset <= flash_length`; `length <= flash_length - offset` (no overflow); `offset % page_length == 0`; `length % page_length == 0`. `offset` is region-relative. The callback must leave every erased cell reading `geometry.erased_value` (the core compares against `erased_value`, e.g. `src/journal/fwu_journal.c:17`, and pads with it, `src/session/fwu_session.c:455`).

```c
fwu_return_e (*flash_write) (void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data);
```
Guaranteed on entry: `length != 0`; `data != NULL`; range as above; `offset % write_granule == 0`; `length % write_granule == 0`. NOT guaranteed: that the target is erased (the core's own code always erases first, but the wrapper does not check; the simulated flash and the STM32G0 port check it themselves and return `FWU_ERR_FLASH_NOT_ERASED`, `src/flash/fwu_flash_sim.c:126-130`, `ports/stm32g0/flash/fwu_flash_stm32g0.c:121`). On success the data must be readable back via `flash_read` as written: the core verifies this by read-back (see 1.5.4) and treats a mismatch as `FWU_ERR_FLASH_WRITE`.

```c
fwu_return_e (*flash_read)  (void *context, uint32_t offset, uint32_t length, fwu_byte_t *data);
```
Guaranteed on entry: `length != 0`; `data != NULL`; range as above. No alignment guarantee (the core reads at arbitrary offsets and lengths, e.g. `FWU_MANIFEST_SIZE` bytes at `manifest_offset`, `src/verify/fwu_verify.c:24`).

In all three cases the callback is NOT guaranteed that the geometry is valid unless the caller ran `fwu_flash_validate` (the wrappers divide by `page_length` / `write_granule`).

Execution model shown by the code: callbacks are synchronous and return only when the operation is finished; the core has no completion/poll mechanism and no watchdog hook for flash (a port that needs one must provide it internally, as the STM32G0 port's optional `keepalive` does, per its header `ports/stm32g0/flash/fwu_flash_stm32g0.h`). No locking is done by the core; single-threaded use is assumed (UNVERIFIED: no statement in `src/flash` about re-entrancy).

#### 1.5.3 Return codes the core expects

- `FWU_OK` means the operation completed. Every call site tests `!= FWU_OK` and treats anything else as failure; no call site in `src/` branches on a specific flash error code except the two cases below.
- Codes documented for flash in `src/error/fwu_error.h`: `FWU_ERR_FLASH_WRITE` (40), `FWU_ERR_OUT_OF_RANGE` (41), `FWU_ERR_WRITE_ALIGNMENT` (42), `FWU_ERR_ERASE_ALIGNMENT` (43), `FWU_ERR_FLASH_GEOMETRY` (44), `FWU_ERR_FLASH_NOT_ERASED` (45), `FWU_ERR_FLASH_POWER_LOSS` (46, simulated flash only), `FWU_ERR_FLASH_ERASE` (47). Ports in the repository return `FWU_ERR_FLASH_ERASE` for erase failures and `FWU_ERR_FLASH_WRITE` for write failures; the STM32G0 port documents that its read never returns an error.
- Port-side range/alignment checks (`FWU_ERR_OUT_OF_RANGE`, `FWU_ERR_*_ALIGNMENT`) are redundant defence: the wrappers already filter these. The simulated flash and the C2000 adapter repeat them.
- Two code-specific interpretations exist in the core. (a) `FWU_ERR_FLASH_WRITE` is also the core's own "read-back mismatch" code (`src/session/fwu_session.c:102`, `src/activation/fwu_activate.c:152`). In the duplicate-chunk path of the session, `session_flash_matches` returning `FWU_ERR_FLASH_WRITE` is converted to `FWU_ERR_BAD_BLOCK` (`src/session/fwu_session.c:429-431`); therefore a port's `flash_read` should not return `FWU_ERR_FLASH_WRITE` for a hardware read failure, or that path will misreport it as a content mismatch. (b) Session status codes must fit in one byte: `session_code` maps values outside 1..255 to `FWU_ERR_CRYPTO_FAILURE` before sending (`src/session/fwu_session.c:23-29`); all flash codes are below 255.
- Code classes in `fwu_error.h` comments: `FWU_ERR_FLASH_WRITE` and `FWU_ERR_FLASH_ERASE` are described as "may be retried once, treat as permanent if repeated" - but the core itself never retries a flash call (no retry loop in any caller listed in 1.5.4).

#### 1.5.4 What the core does when a callback fails

Every caller is listed; "propagate" means the status is returned to the caller unchanged and no retry is made at that layer.

| Caller (source) | Operation | On failure |
|---|---|---|
| `act_copy_page` (`src/activation/fwu_activate.c:165-196`) | erase destination page; then per chunk read source, write destination, read back and compare (`act_compare`, mismatch = `FWU_ERR_FLASH_WRITE`) | Returns the status at once; no journal record is appended for that step. The page copy is documented as repeatable from the start (destination is erased again, source untouched). |
| `act_run_activating` / `act_run_reverting` (`src/activation/fwu_activate.c:232-298`) | loop of copy/swap steps, each followed by `act_append(... PROGRESS/REVERT ...)` | Returns the failing status; `*result` is not set. On the next call the loop restarts at `activate->counter` (the step count recorded in the journal), so the unfinished step is redone. |
| `act_append` (`src/activation/fwu_activate.c:121-129`) | `fwu_journal_ab_append`, then `act_refresh` | If the append fails, returns the status before `act_refresh`; the in-RAM phase is not updated. |
| `fwu_activate_run` (`src/activation/fwu_activate.c:437-`) | drives the above | Returns the status; no outcome is reported. |
| `fwu_boot_start` (`src/boot/fwu_boot.c:166-171`) | calls `fwu_activate_run` | Sets `reason = FWU_BOOT_REASON_ACTIVATE_RUN`, `status`, and returns `FWU_BOOT_STAY` (code comment: "application may be half-written: NEVER jump"). Init-stage failures (`fwu_activate_init`, counters init) likewise return `FWU_BOOT_STAY` with reasons `FWU_BOOT_REASON_ACTIVATE_INIT` / `FWU_BOOT_REASON_COUNTERS` (`src/boot/fwu_boot.c:158-164`, `:72`). |
| `fwu_journal_scan` (`src/journal/fwu_journal.c:76-81`) | `fwu_flash_read` per slot | Returns the read status immediately; the journal's RAM state is not updated (it is assigned only after the loop, `:95-102`). `fwu_journal_init` returns the scan status. |
| `fwu_journal_append` (`src/journal/fwu_journal.c:174-178`) | `fwu_flash_write` of one record | Returns the status; RAM state (`last`, `next_slot`, `next_seq`) is updated only on success. Code comment: "state is untrusted: the caller must re-read with `fwu_journal_scan`". |
| `journal_ab_rotate` (`src/journal/fwu_journal_ab.c:48`, `:58`) | erase the other region (or the active one if it holds no valid record), rescan, append snapshot | Returns the failing status; the active region is not modified before the erase of the other one succeeds, and `journal->active` switches only after the snapshot append succeeds (`:75`). |
| `fwu_counters_raise` path (`src/counters/fwu_counters.c:93-98`) | `fwu_journal_ab_append` | Calls `fwu_counters_reload` (re-read) and returns the original status; counters become "not ready" if the re-read also fails (`src/counters/fwu_counters.c:54`). |
| `session_store_chunk` (`src/session/fwu_session.c:410-476`) | erase page at page start, `session_write_checked` (write + read back) | On any flash failure calls `session_reset` (session -> `FWU_SESSION_IDLE`, progress forgotten) and returns the status (`:466-469`). In the TRANSFER_DATA handler the status becomes an ERROR response (`:490-493`). |
| `handle_request_download` manifest write (`src/session/fwu_session.c:376-383`) | erase manifest page, write + read back | Replies with an ERROR frame carrying the status; the session state is not changed (no `session_reset` on this path). |
| duplicate-chunk check (`src/session/fwu_session.c:424-436`) | read-back compare | `FWU_ERR_FLASH_WRITE` -> `FWU_ERR_BAD_BLOCK` (session unchanged); any other failure -> `session_reset` and return status. |
| `fwu_session_poll` verification (`src/session/fwu_session.c:624-638`) | `fwu_verify_image` (reads flash) | Any non-`FWU_OK` status: `session_reset`, and the status is sent in `VERIFY_RESULT`. |
| `verify_read_manifest` / hash loop (`src/verify/fwu_verify.c:24-30`, `:89-98`) | `fwu_flash_read` | Returns the status as-is. In the activation hook (`src/activation/fwu_activate.c:242-254`) a flash/read error is NOT a "rejection" (`act_is_rejection` covers only codes 30-39 and specific security codes), so activation returns the error and does not mark the image rejected. |

Line numbers in files outside this document's modules (session, activation, journal, boot, counters, verify) may drift as those files are edited; the function names are authoritative.

Summary of core behaviour on a callback failure: no layer retries; the journal-backed activation is resumable from its last recorded step; the bootloader decision is "stay in the bootloader"; the update session is dropped to idle on a staging write/erase failure; in-RAM journal state is never advanced on a failed write.

### 1.6 Wrappers: dependencies and source map

| Function | Declaration line | Definition line |
|---|---|---|
| `fwu_flash_validate` | `src/flash/fwu_flash.h:50` | `src/flash/fwu_flash.c:5` |
| `fwu_flash_erase` | `src/flash/fwu_flash.h:63` | `src/flash/fwu_flash.c:23` |
| `fwu_flash_write` | `src/flash/fwu_flash.h:76` | `src/flash/fwu_flash.c:38` |
| `fwu_flash_read` | `src/flash/fwu_flash.h:87` | `src/flash/fwu_flash.c:53` |

---

## 2. Simulated flash (`fwu_flash_sim`)

**Purpose.** A RAM implementation of the flash port for tests and PC builds. It enforces flash rules (erase to `erased_value`, write only to fully erased cells, alignment and range checks of its own) and can inject a power loss after a chosen number of basic operations. Storage is embedded in the caller's variable by a macro; there is no dynamic memory.

**Used by / depends on.** Includes `fwu_flash.h`. Referenced only by test code in this repository (no non-test `src/`, `ports/` or `examples/` file includes it; verified by grep for `fwu_flash_sim.h`). `UNVERIFIED`: whether other tests outside `src/`, `ports/`, `examples/` use it.

### 2.1 Types and macro

```c
typedef struct {
    fwu_flash_t  flash;
    fwu_byte_t  *memory;
    uint32_t    *erase_counts;
    uint32_t     page_count;
    uint32_t     operations_done;
    uint32_t     cut_after;
    bool         cut_armed;
    bool         powered;
} fwu_flash_sim_t;
```
`src/flash/fwu_flash_sim.h:52`. Members: `flash` is the interface to hand to the core (its `context` is the `fwu_flash_sim_t` itself); `memory` has `flash_length` elements; `erase_counts` has `page_count` elements; `operations_done` counts completed basic operations; `cut_after`/`cut_armed` hold the armed power-loss point; `powered` becomes false after the cut.

```c
#define FWU_FLASH_SIM_DEFINE(name, pages, page_bytes, granule)
```
`src/flash/fwu_flash_sim.h:96`. Generates `name##_t` (`{ fwu_flash_sim_t base; fwu_byte_t memory[pages*page_bytes]; uint32_t erase_counts[pages]; }`), `static inline fwu_return_e name##_init(name##_t *self)` (calls `fwu_flash_sim_init_raw`), and a compile-time check typedef that fails if `pages`, `page_bytes`, `granule` are not positive or `page_bytes % granule != 0`.

### 2.2 Behaviour model (from `fwu_flash_sim.c`)

- Geometry set by init: `flash_length = page_count * page_length`, `erased_value = 0xFF` (`src/flash/fwu_flash_sim.c:169-172`).
- Callback order of checks (e.g. `sim_write`, `:105-145`): `NULL` context/data -> `FWU_ERR_INVALID_ARG`; power off -> `FWU_ERR_FLASH_POWER_LOSS` (checked before the zero-length check); `length == 0` -> `FWU_ERR_INVALID_ARG`; range -> `FWU_ERR_OUT_OF_RANGE`; alignment -> `FWU_ERR_WRITE_ALIGNMENT` / `FWU_ERR_ERASE_ALIGNMENT`; for write, any target cell not equal to `erased_value` -> `FWU_ERR_FLASH_NOT_ERASED` and nothing changes.
- Basic operation = one page erase or one granule program. Reads do not count. With power loss armed after `n`, operation `n+1` is interrupted: an interrupted erase clears the first half of the page (and still increments the erase counter); an interrupted write programs the first half of the granule. Interrupted operations return `FWU_ERR_FLASH_POWER_LOSS`, and every later call (reads included) returns it until `fwu_flash_sim_reset`.

### 2.3 Functions

```c
fwu_return_e fwu_flash_sim_init_raw(fwu_flash_sim_t *sim, fwu_byte_t *memory,
                                    uint32_t *erase_counts, uint32_t page_count,
                                    uint32_t page_length, uint32_t write_granule);
```
- Responsibility: wires storage, erases everything to `0xFF`, zeroes erase counts, powers on, and validates with `fwu_flash_validate`. Intended to be reached via `FWU_FLASH_SIM_DEFINE`.
- Returns: `FWU_ERR_INVALID_ARG` for `NULL` pointers or zero `page_count`/`page_length`/`write_granule`; `FWU_ERR_FLASH_GEOMETRY` if `page_count * page_length` would overflow `uint32_t`; otherwise the result of `fwu_flash_validate` (`FWU_OK` or `FWU_ERR_FLASH_GEOMETRY` when `page_length % write_granule != 0`).
- Source: `src/flash/fwu_flash_sim.h:71`, `src/flash/fwu_flash_sim.c:155`.

```c
void fwu_flash_sim_cut_power_after(fwu_flash_sim_t *sim, uint32_t operations);
```
Arms the cut: `operations` basic operations complete, the next is interrupted. No effect if `sim` is `NULL`. `src/flash/fwu_flash_sim.h:76`, `src/flash/fwu_flash_sim.c:191`.

```c
void fwu_flash_sim_reset(fwu_flash_sim_t *sim);
```
Restores power, disarms the cut, sets `operations_done` to 0; memory contents and erase counts are kept. `src/flash/fwu_flash_sim.h:79`, `src/flash/fwu_flash_sim.c:199`.

```c
uint32_t fwu_flash_sim_operations(const fwu_flash_sim_t *sim);
```
Returns `operations_done` (0 for `NULL`). `src/flash/fwu_flash_sim.h:82`, `src/flash/fwu_flash_sim.c:209`.

```c
bool fwu_flash_sim_is_powered(const fwu_flash_sim_t *sim);
```
Returns `powered` (false for `NULL`). `src/flash/fwu_flash_sim.h:85`, `src/flash/fwu_flash_sim.c:214`.

```c
fwu_return_e fwu_flash_sim_get_erase_count(const fwu_flash_sim_t *sim, uint32_t page,
                                           uint32_t *count);
```
Writes the erase count of `page` to `*count`. Returns `FWU_OK`, or `FWU_ERR_INVALID_ARG` for `NULL` `sim`/`count` or `page >= page_count`. `src/flash/fwu_flash_sim.h:88`, `src/flash/fwu_flash_sim.c:219`.

---

## 3. Ring buffer (`fwu_ringbuf`)

**Purpose.** A byte FIFO for a single producer (e.g. a UART RX interrupt) and a single consumer (main loop). Storage is embedded in the structure by `FWU_RINGBUF_DEFINE`. `head` is written only by the producer, `tail` only by the consumer, both kept in `[0, 2*capacity)` so full/empty is distinguished without a shared counter. Bulk push/pull are all-or-nothing.

**Used by.** `src/frame/fwu_parser.h` (includes `fwu_ringbuf.h`; `src/frame/fwu_parser.c:48` calls `fwu_ringbuf_used_size`), `src/pull/fwu_pull.h` (the input ring `fwu_pull_rb`), `examples/c2000_launchxl_f280049c/application/port/app_fwu.c:14`. **Depends on:** `fwu_types.h`, `fwu_error.h`.

### 3.1 Macros and types

```c
#define FWU_RINGBUF_MAX_CAPACITY 32767u
```
`src/ring_buffer/fwu_ringbuf.h:76`. Largest capacity; `2 * capacity` must fit `uint16_t`.

```c
typedef struct {
    volatile fwu_byte_t *storage;
    uint16_t capacity;
    volatile uint16_t head;
    volatile uint16_t tail;
} fwu_ringbuf_t;
```
`src/ring_buffer/fwu_ringbuf.h:84`. Do not modify fields directly. A zeroed (static) instance that was never initialised is rejected by all operations (`ringbuf_is_valid`: non-`NULL`, `storage != NULL`, `capacity != 0`).

```c
#define FWU_RINGBUF_DEFINE(name, capacity_value)
```
`src/ring_buffer/fwu_ringbuf.h:185`. Generates `name##_t` (`{ fwu_ringbuf_t base; fwu_byte_t storage[capacity_value]; }`), `static inline fwu_return_e name##_init(name##_t *self)` (returns `FWU_ERR_INVALID_ARG` for `NULL`, else `fwu_ringbuf_init_raw` with the real array size), and a typedef that is a compile error if the capacity is outside `1 .. FWU_RINGBUF_MAX_CAPACITY`. The header states this is the only valid way to create a queue in production code and that `fwu_ringbuf_init_raw` is internal.

### 3.2 Functions

```c
fwu_return_e fwu_ringbuf_init_raw(fwu_ringbuf_t *self, fwu_byte_t *storage, size_t capacity);
```
Sets storage and capacity, `head = tail = 0`. Returns `FWU_OK`, or `FWU_ERR_INVALID_ARG` for `NULL` `self`/`storage`, `capacity == 0`, or `capacity > FWU_RINGBUF_MAX_CAPACITY`. Must be called while neither context is running. `src/ring_buffer/fwu_ringbuf.h:103`, `src/ring_buffer/fwu_ringbuf.c:75`.

```c
fwu_return_e fwu_ringbuf_push(fwu_ringbuf_t *self, const fwu_byte_t *data, size_t length);
```
Producer context only. Copies `length` elements, then publishes `head` once. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` (invalid/uninitialised `self`, `data == NULL`, or `length > capacity`); `FWU_ERR_BUFFER_FULL` (not enough free space now; buffer unchanged). `length == 0` with a valid buffer and non-`NULL` data returns `FWU_OK` (code path: no check against zero). `src/ring_buffer/fwu_ringbuf.h:117`, `src/ring_buffer/fwu_ringbuf.c:89`.

```c
fwu_return_e fwu_ringbuf_pull(fwu_ringbuf_t *self, fwu_byte_t *data, size_t length);
```
Consumer context only. Copies `length` elements out, then publishes `tail` once. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` (invalid `self`, `data == NULL`, `length > capacity`); `FWU_ERR_NOT_ENOUGH_DATA` (fewer than `length` bytes stored; buffer unchanged). `src/ring_buffer/fwu_ringbuf.h:130`, `src/ring_buffer/fwu_ringbuf.c:119`.

```c
fwu_return_e fwu_ringbuf_clear(fwu_ringbuf_t *self);
```
Consumer context only. Sets `tail = head` (data is not erased). Returns `FWU_OK` or `FWU_ERR_INVALID_ARG`. `src/ring_buffer/fwu_ringbuf.h:137`, `src/ring_buffer/fwu_ringbuf.c:148`.

```c
bool fwu_ringbuf_is_empty(const fwu_ringbuf_t *self);
bool fwu_ringbuf_is_full(const fwu_ringbuf_t *self);
uint16_t fwu_ringbuf_used_size(const fwu_ringbuf_t *self);
uint16_t fwu_ringbuf_free_size(const fwu_ringbuf_t *self);
```
Queries, callable from either context; each reads `head` and `tail` once, so results are snapshots (conservative as described in the header). Invalid/uninitialised buffer: `false` / `false` / `0` / `0`. `src/ring_buffer/fwu_ringbuf.h:143`, `:149`, `:155`, `:161`; definitions `src/ring_buffer/fwu_ringbuf.c:160`, `:165`, `:170`, `:175`.

Concurrency note from the header: fields are `volatile` and no memory barriers are used; this is stated to be sufficient for single-core in-order MCUs only.

---

## 4. HTTP client layer (`fwu_http`)

**Purpose.** A deliberately narrow HTTP/1.1 client for the flow "GET with Range -> 206 Partial Content": it builds the request and incrementally parses the response from arbitrarily split byte groups. It supports only `Content-Length` bodies (chunked encoding and responses without `Content-Length` are errors), no TLS, case-insensitive header names, CRLF or LF line endings, ignores unrecognised headers, and rejects a header section larger than `FWU_HTTP_HEADER_MAX_BYTES`. The layer has no notion of time.

**Used by.** `src/pull/fwu_pull.h` (includes it), `src/pull/fwu_pull.c`. **Depends on:** `fwu_error.h`, `fwu_types.h`, `fwu_proto.h` (for `FWU_PROTO_PULL_TARGET_MAX`).

### 4.1 Macros

| Macro | Value | Meaning | Source |
|---|---|---|---|
| `FWU_HTTP_HEADER_MAX_BYTES` | `2048u` | Maximum total length of status line + headers + blank line | `src/pull/fwu_http.h:55` |
| `FWU_HTTP_LINE_MAX` | `128u` | Retained length of one line; longer lines keep only their start and are flagged overflow | `src/pull/fwu_http.h:57` |
| `FWU_HTTP_REQUEST_MAX` | `256u` | Safe upper bound for the request built by `fwu_http_build_range_request` | `src/pull/fwu_http.h:59` |

### 4.2 Types

```c
typedef enum {
    FWU_HTTP_NEED_MORE = 0,
    FWU_HTTP_HEADERS_DONE,
    FWU_HTTP_BODY,
    FWU_HTTP_ERROR
} fwu_http_event_e;
```
`src/pull/fwu_http.h:62`. Values: `FWU_HTTP_NEED_MORE` = 0, `FWU_HTTP_HEADERS_DONE` = 1, `FWU_HTTP_BODY` = 2, `FWU_HTTP_ERROR` = 3.

```c
typedef struct {
    uint16_t status;
    bool     has_content_length;
    uint32_t content_length;
    bool     has_content_range;
    uint32_t range_first;
    uint32_t range_last;
    uint32_t range_total;
    bool     connection_close;
} fwu_http_response_t;
```
`src/pull/fwu_http.h:70`. Meaningful only after `FWU_HTTP_HEADERS_DONE`. `has_content_range` is set only if the header has the form `bytes a-b/total` with three valid numbers and `b >= a`; if the format does not match the flag simply stays false (`src/pull/fwu_http.c:281-312`).

```c
typedef struct {
    uint16_t            phase;
    fwu_return_e        error;
    fwu_byte_t          line[FWU_HTTP_LINE_MAX];
    uint16_t            line_length;
    bool                line_overflow;
    uint32_t            header_bytes;
    bool                chunked;
    uint32_t            body_remaining;
    fwu_http_response_t response;
} fwu_http_parser_t;
```
`src/pull/fwu_http.h:82`. Opaque state; do not touch fields. Internal phases (`src/pull/fwu_http.c:11-15`): status line (0), headers (1), body (2), done (3), failed (4).

### 4.3 Functions

```c
fwu_return_e fwu_http_build_range_request(const fwu_byte_t ip[4], uint16_t port, const fwu_byte_t *target, size_t target_length,
                                          uint32_t first, uint32_t last, fwu_byte_t *out, size_t out_capacity, size_t *out_length);
```
- Responsibility: writes `GET <target> HTTP/1.1`, `Host: a.b.c.d:port`, `Range: bytes=<first>-<last>`, `Connection: keep-alive`, blank line (CRLF line ends).
- Parameters: `ip` 4 elements; `port` 1..65535; `target` starts with `/`, 1..`FWU_PROTO_PULL_TARGET_MAX` elements, every element in 0x21..0x7E; `first <= last` (inclusive); `out` with `out_capacity` elements (`FWU_HTTP_REQUEST_MAX` suffices); `out_length` receives the byte count.
- Returns: `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL `ip`/`out`/`out_length`/`target`, `port == 0`, `last < first`, empty/too long/non-`/` start/forbidden-character target); `FWU_ERR_BUFFER_TOO_SMALL` (internal 256-element scratch or `out_capacity` too small). `out` and `*out_length` are written only on `FWU_OK`.
- Source: `src/pull/fwu_http.h:107`, `src/pull/fwu_http.c:128`.

```c
void fwu_http_parser_init(fwu_http_parser_t *parser);
```
Resets for a new response (call before each new request). No effect on `NULL`. `src/pull/fwu_http.h:111`, `src/pull/fwu_http.c:186`.

```c
fwu_http_event_e fwu_http_feed(fwu_http_parser_t *parser, const fwu_byte_t *data, size_t length, size_t *consumed,
                               const fwu_byte_t **body, size_t *body_length);
```
- Responsibility: consumes bytes and reports one event per call. `*consumed`, `*body`, `*body_length` are set at the start (0 / `NULL` / 0).
- Events: `FWU_HTTP_NEED_MORE` (all input consumed during header parsing; or body phase with nothing to take; or response already complete - extra bytes are consumed and discarded, `:352-354`); `FWU_HTTP_HEADERS_DONE` (blank line found; input after the blank line stays with the caller); `FWU_HTTP_BODY` (up to the remaining `Content-Length` bytes, `*body` points into the caller's `data`, no copy); `FWU_HTTP_ERROR` (see below).
- Errors (`fwu_http_error()` code): `FWU_ERR_PULL_PROTOCOL` for header section > `FWU_HTTP_HEADER_MAX_BYTES`, malformed status line (must be `HTTP/1.0` or `HTTP/1.1`, exactly three digits, 100..599), unparsable `Content-Length`, two different `Content-Length` values, `Transfer-Encoding` containing `chunked`, or no `Content-Length` at the end of headers. `FWU_ERR_INVALID_ARG` for `NULL` `parser`/`consumed`/`body`/`body_length` or `data == NULL` with `length > 0` (for `NULL` `parser` the error cannot be stored). Once failed the parser returns `FWU_HTTP_ERROR` without consuming input until `fwu_http_parser_init`.
- A zero `Content-Length` goes straight to the done phase at `HEADERS_DONE` (`fwu_http_complete` true).
- Source: `src/pull/fwu_http.h:123`, `src/pull/fwu_http.c:329`.

```c
const fwu_http_response_t *fwu_http_response(const fwu_http_parser_t *parser);
bool fwu_http_complete(const fwu_http_parser_t *parser);
fwu_return_e fwu_http_error(const fwu_http_parser_t *parser);
bool fwu_http_headers_done(const fwu_http_parser_t *parser);
```
Queries. `fwu_http_response`: pointer to the response summary, `NULL` for `NULL` parser. `fwu_http_complete`: true when the body is complete (done phase). `fwu_http_error`: stored error, `FWU_OK` if none, `FWU_ERR_INVALID_ARG` for `NULL` parser. `fwu_http_headers_done`: true in body or done phase. Sources: `src/pull/fwu_http.h:127`, `:130`, `:133`, `:136`; definitions `src/pull/fwu_http.c:431`, `:436`, `:441`, `:446`.

---

## 5. Pull controller (`fwu_pull`)

**Purpose.** On a host command (`PULL_START`: server IP, port, request target) the device downloads the image itself with HTTP range requests and writes it through `fwu_session_push_chunk` into the session's staging area, so all session rules (offset order, page erase, write and read back, later verification, commit) are unchanged. The module is independent of the transport: the caller supplies a `send` hook, feeds received bytes with `fwu_pull_feed` (fast: copy to a ring) and calls `fwu_pull_poll` from the main loop (heavy: HTTP parsing, flash writes, timeouts, requests).

**Used by.** `examples/c2000_launchxl_f280049c/application/port/app_pull.c` (calls `fwu_pull_init`, `fwu_pull_handles`, `fwu_pull_on_frame`, `fwu_pull_feed`, `fwu_pull_set_link`, `fwu_pull_poll`, `fwu_pull_input_free`, `fwu_pull_input_used`, `fwu_pull_active`). **Depends on:** `fwu_error.h`, `fwu_types.h`, `fwu_frame.h`, `fwu_decode.h`, `fwu_proto.h`, `fwu_ringbuf.h`, `fwu_session.h`, `fwu_transport.h`, `fwu_http.h` (`src/pull/fwu_pull.h:51-59`); calls `fwu_session_get_image_size`, `fwu_session_get_next_offset`, `fwu_session_get_chunk_size`, `fwu_session_get_state`, `fwu_session_push_chunk`, `fwu_session_touch`.

### 5.1 Macros

| Macro | Value | Meaning | Source |
|---|---|---|---|
| `FWU_PULL_INPUT_SIZE` | `1024u` (overridable: `#ifndef`) | Capacity of the input ring | `src/pull/fwu_pull.h:70` |
| `FWU_PULL_SCRATCH_SIZE` | `64u` | Bytes `fwu_pull_poll` takes from the ring per iteration (stack buffer) | `:74` |
| `FWU_PULL_MIN_OUT` | `(FWU_FRAME_OVERHEAD + FWU_PROTO_PULL_STATUS_SIZE)` | Minimum `out_capacity` for `fwu_pull_on_frame` | `:76` |
| `FWU_PULL_DEFAULT_RANGE_BYTES` | `8192u` | Suggested bytes per GET | `:79` |
| `FWU_PULL_DEFAULT_TIMEOUT_MS` | `8000u` | Suggested response timeout | `:80` |
| `FWU_PULL_DEFAULT_MAX_RETRIES` | `5u` | Suggested retries per range | `:81` |

Internal (in `.c`): `FWU_PULL_BACKOFF_STEP_MS` 100u, `FWU_PULL_BACKOFF_MAX_MS` 1000u (`src/pull/fwu_pull.c:10-11`).

`FWU_RINGBUF_DEFINE(fwu_pull_rb, FWU_PULL_INPUT_SIZE);` (`src/pull/fwu_pull.h:83`) defines `fwu_pull_rb_t` and `fwu_pull_rb_init`.

### 5.2 Types

```c
typedef struct {
    fwu_session_t        *session;
    fwu_transport_send_fn send;
    void                 *send_context;
    fwu_byte_t           *chunk_buffer;
    uint32_t              chunk_buffer_length;
    uint32_t              range_bytes;
    uint32_t              response_timeout_ms;
    uint32_t              max_retries;
} fwu_pull_config_t;
```
`src/pull/fwu_pull.h:86`

| Field | Meaning / constraint (from `fwu_pull_init`) |
|---|---|
| `session` | Non-`NULL`, initialised session (staging is written through it). |
| `send` | Non-`NULL` `fwu_transport_send_fn` (`typedef fwu_return_e (*fwu_transport_send_fn)(void *context, const fwu_byte_t *data, size_t length);`, `src/transport/fwu_transport.h:54`): `FWU_OK` = accepted entirely, `FWU_ERR_BUFFER_FULL` = busy, retry later, other = send failed. |
| `send_context` | Passed to `send`. |
| `chunk_buffer` | Non-`NULL` range buffer; holds one whole response body until it is complete. |
| `chunk_buffer_length` | Elements in `chunk_buffer`; must be `>= range_bytes`. |
| `range_bytes` | Non-zero, multiple of the session's chunk size. |
| `response_timeout_ms` | `>= 1`. |
| `max_retries` | Retries per range; 0 = no retry. |

```c
typedef struct {
    uint32_t requests;
    uint32_t responses;
    uint32_t retries;
    uint32_t chunks_written;
    uint32_t bytes_received;
    uint32_t rx_overflows;
    uint32_t link_drops;
} fwu_pull_stats_t;
```
`src/pull/fwu_pull.h:97`. `requests`: GETs sent (including retries); `responses`: completed responses (counted after a successful commit); `retries`: retry attempts counted (also the attempt that exhausts the allowance); `chunks_written`: chunks accepted by `fwu_session_push_chunk`; `bytes_received`: bytes processed by `fwu_pull_poll` (headers included); `rx_overflows`: groups dropped because the ring was full; `link_drops`: transitions from link up to down.

```c
typedef struct { /* fields below */ } fwu_pull_t;
```
`src/pull/fwu_pull.h:108`. State object; do not touch fields (use `fwu_pull_get_status` / `fwu_pull_get_stats`). Fields: `config`, `initialized`, `state` (`FWU_PROTO_PULL_*`), `error`, `http_status`, `ip[4]`, `port`, `target[FWU_PROTO_PULL_TARGET_MAX]`, `target_length`, `link_up`, `request_outstanding`, `send_pending`, `wait_reconnect`, `input_overflowed`, `range_first`, `range_last`, `retry_count`, `not_before_ms`, `last_rx_ms`, `wait_start_ms`, `headers_checked`, `stale_response`, `chunk_fill`, `chunk_offset`, `parser`, `stats`, `input` (`fwu_pull_rb_t`).

State numbers used in `fwu_pull_t.state` and in PULL_STATUS (defined in `fwu_proto.h`, lines 110-114): `FWU_PROTO_PULL_IDLE` 0, `FWU_PROTO_PULL_REQUESTING` 1, `FWU_PROTO_PULL_DOWNLOADING` 2, `FWU_PROTO_PULL_DONE` 3, `FWU_PROTO_PULL_FAILED` 4.

Error codes owned by this feature (`src/error/fwu_error.h`): `FWU_ERR_PULL_PROTOCOL` 80, `FWU_ERR_PULL_HTTP_STATUS` 81, `FWU_ERR_PULL_TIMEOUT` 82, `FWU_ERR_PULL_SIZE_MISMATCH` 83.

### 5.3 Functions

```c
fwu_return_e fwu_pull_init(fwu_pull_t *pull, const fwu_pull_config_t *config);
```
- Responsibility: copies `config`, resets everything to IDLE, initialises the input ring.
- Returns: `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL `pull`/`config`, or NULL `session`/`send`/`chunk_buffer`, `response_timeout_ms == 0`, `range_bytes == 0`, `chunk_buffer_length < range_bytes`, `range_bytes` not a multiple of the chunk size, or ring init failure); `FWU_ERR_BAD_STATE` if the session reports chunk size 0 (not initialised). Note: `pull->initialized` is set to false before argument validation of `config` (for a non-`NULL` `pull`), so a failed re-init marks an existing controller uninitialised.
- Precondition: `fwu_session_init` already done on the session.
- Source: `src/pull/fwu_pull.h:144`, `src/pull/fwu_pull.c:331`.

```c
bool fwu_pull_handles(fwu_byte_t type);
```
True for `FWU_MSG_PULL_START` and `FWU_MSG_PULL_QUERY`; all other frame types belong to `fwu_session_on_frame`. `src/pull/fwu_pull.h:147`, `src/pull/fwu_pull.c:390`.

```c
fwu_return_e fwu_pull_on_frame(fwu_pull_t *pull, const fwu_frame_view_t *request, uint32_t now_ms,
                               fwu_byte_t *out, size_t out_capacity, size_t *out_length);
```
- Responsibility: handles `PULL_START` and `PULL_QUERY`, writes the reply frame into `out`.
- Immediate returns without a reply frame: `FWU_ERR_INVALID_ARG` (NULL `pull`/`request`/`out`/`out_length`, not initialised, `request->payload == NULL`, payload length 0 or `> FWU_FRAME_MAX_PAYLOAD`); `FWU_ERR_UNKNOWN_MESSAGE` (type not handled); `FWU_ERR_BUFFER_TOO_SMALL` (`out_capacity < FWU_PULL_MIN_OUT`).
- Otherwise it calls `fwu_session_touch(session, now_ms)` and replies with `FWU_OK` (reply frames carry errors as `FWU_MSG_ERROR` via `pull_reply_error`):
  - `PULL_QUERY`: payload length must be 1, else ERROR(`FWU_ERR_BAD_LENGTH`); reply `FWU_MSG_PULL_STATUS` with current status.
  - `PULL_START`: session not `FWU_SESSION_DOWNLOADING` -> ERROR(`FWU_ERR_BAD_STATE`); payload parse error -> ERROR(parse status); state active (REQUESTING/DOWNLOADING): same ip/port/target -> reply `FWU_MSG_PULL_STARTED` with no change, different -> ERROR(`FWU_ERR_BAD_STATE`); state DONE with same target and image fully written -> reply `FWU_MSG_PULL_STARTED` unchanged; otherwise (re)start (section 5.4).
- Source: `src/pull/fwu_pull.h:157`, `src/pull/fwu_pull.c:416`.

```c
void fwu_pull_set_link(fwu_pull_t *pull, bool up, uint32_t now_ms);
```
Records the socket link state. On a down transition (`link_up` was true): increments `stats.link_drops`; if a pull is active, discards the ongoing response and partial chunk (`pull_prepare_range`), sets `not_before_ms = now_ms`, clears `wait_reconnect`, resets `last_rx_ms`; the retry count is NOT incremented. `src/pull/fwu_pull.h:162`, `src/pull/fwu_pull.c:489`.

```c
fwu_return_e fwu_pull_feed(fwu_pull_t *pull, const fwu_byte_t *data, size_t length, uint32_t now_ms);
```
- Responsibility: copies received bytes into the input ring; does nothing heavy (`now_ms` is unused, `:512`).
- Returns: `FWU_OK` (including `length == 0`, and when no request is outstanding - the bytes are silently dropped); `FWU_ERR_INVALID_ARG` (NULL `pull`, not initialised, or `data == NULL` with `length > 0`); `FWU_ERR_BUFFER_FULL` when the ring has no room (increments `stats.rx_overflows`, sets `input_overflowed`) and for every later call while `input_overflowed` is set (bytes dropped without touching the counter).
- Source: `src/pull/fwu_pull.h:169`, `src/pull/fwu_pull.c:508`.

```c
void fwu_pull_poll(fwu_pull_t *pull, uint32_t now_ms);
```
- Responsibility: all heavy work, in the fixed step order given in 5.5. Blocking: the flash erase/write inside `fwu_session_push_chunk` run inside this call (header: may take long; call it while no data flows from the module).
- Source: `src/pull/fwu_pull.h:174`, `src/pull/fwu_pull.c:536`.

```c
void fwu_pull_abort(fwu_pull_t *pull);
```
Sets `error = FWU_OK` and stops with state IDLE (discards partial chunk and input). `src/pull/fwu_pull.h:177`, `src/pull/fwu_pull.c:606`.

```c
void fwu_pull_get_status(const fwu_pull_t *pull, fwu_proto_pull_status_t *status);
void fwu_pull_get_stats(const fwu_pull_t *pull, fwu_pull_stats_t *stats);
bool fwu_pull_active(const fwu_pull_t *pull);
uint16_t fwu_pull_input_free(const fwu_pull_t *pull);
uint16_t fwu_pull_input_used(const fwu_pull_t *pull);
```
`get_status` fills `state`, `next_offset` (the SESSION's `next_offset`, i.e. bytes written to flash), `error` (the stored error only when state is FAILED, else 0) and `http_status`; for NULL/uninitialised `pull` it reports IDLE and zeros; `NULL` `status` is ignored. `get_stats` copies the counters (zeros if NULL/uninitialised). `fwu_pull_active` is true while state is REQUESTING or DOWNLOADING. `input_free` / `input_used` return the ring's free/used elements (0 if NULL/uninitialised). Sources: `src/pull/fwu_pull.h:181`, `:184`, `:187`, `:190`, `:191`; definitions `src/pull/fwu_pull.c:615`, `:633`, `:651`, `:656`, `:661`.

### 5.4 State machine

States (values in section 5.2): IDLE, REQUESTING, DOWNLOADING, DONE, FAILED. "Active" = REQUESTING or DOWNLOADING (`pull_state_is_active`, `src/pull/fwu_pull.c:15`).

Transitions, as implemented:

| From | Event | To | Where |
|---|---|---|---|
| (init) | `fwu_pull_init` | IDLE | `:352` |
| IDLE / FAILED / DONE (different target, or image not fully written) | `PULL_START` accepted (session DOWNLOADING, valid payload), image not yet fully written | REQUESTING, `send_pending` set, range computed | `:482-484` |
| same | `PULL_START` accepted but `next_offset >= image_size` | DONE (`pull_stop`) | `:478-480` |
| REQUESTING / DOWNLOADING | `PULL_START` same target | unchanged (reply `PULL_STARTED`) | `:449-453` |
| REQUESTING / DOWNLOADING | `PULL_START` different target | unchanged (ERROR `FWU_ERR_BAD_STATE`) | `:454` |
| DONE | `PULL_START` same target and `next_offset >= image_size` | unchanged | `:456-461` |
| REQUESTING | headers valid, range equals the requested range (`pull_check_headers`) | DOWNLOADING | `:168-170` |
| REQUESTING / DOWNLOADING | response complete, length verified, all chunks committed, `next_offset >= image_size` | DONE (`error = FWU_OK`) | `:236-239` |
| REQUESTING / DOWNLOADING | response complete and committed, more image remains | unchanged (next range prepared, `send_pending` set; see note) | `:241-246` |
| REQUESTING / DOWNLOADING | `pull_retry` with allowance left | REQUESTING | `:90-96` |
| REQUESTING / DOWNLOADING | `pull_fail` (permanent error or retries exhausted) | FAILED, `error` set | `:51-55` |
| REQUESTING / DOWNLOADING | `fwu_pull_poll` finds session state != `FWU_SESSION_DOWNLOADING` | IDLE (`error = FWU_OK`) | `:551-554` |
| DONE | `fwu_pull_poll` finds session state == `FWU_SESSION_IDLE` | IDLE (`error = FWU_OK`) | `:545-548` |
| any | `fwu_pull_abort` | IDLE (`error = FWU_OK`) | `:606-613` |

Note on the "more image remains" row: `pull_response_complete` calls `pull_prepare_range` but does not assign `state`; the controller therefore stays in whatever active state it had (DOWNLOADING after a normal response) while the next request is pending and awaited, and moves to REQUESTING only through `pull_retry` or a restart. UNVERIFIED: whether this is intended, since the header describes REQUESTING as "request sent / waiting for headers / retrying" (`fwu_proto.h:111`); the code never sets REQUESTING between consecutive ranges.

`FAILED` is not left automatically; the host recovers with a new `PULL_START` (session must still be `FWU_SESSION_DOWNLOADING`). A FAILED pull whose session has since gone IDLE stays FAILED in `fwu_pull_poll` (that function only converts DONE and active states).

Error assignment (`pull_fail` codes):

| Condition | Code | Permanent? | Where |
|---|---|---|---|
| HTTP status != 206 | `FWU_ERR_PULL_HTTP_STATUS` | yes | `:144-147` |
| `Content-Range` total != session image size | `FWU_ERR_PULL_SIZE_MISMATCH` | yes | `:152-155` |
| `fwu_http_build_range_request` fails when sending | its returned code | yes | `:108-111` |
| `fwu_session_push_chunk` fails during commit | that code | yes | `:209-212` |
| Retry allowance exhausted | the code passed to `pull_retry` (`FWU_ERR_PULL_PROTOCOL` or `FWU_ERR_PULL_TIMEOUT`) | - | `:86-89` |

### 5.5 Retry and timeout rules

Range selection (`pull_prepare_range`, `:61-77`): `range_first = session next_offset` (always a chunk boundary); `range_last = image_size - 1` if the remaining bytes `<= range_bytes`, else `range_first + range_bytes - 1`. It also clears `request_outstanding`, `headers_checked`, `chunk_fill`, `input_overflowed`, `stale_response` and re-initialises the HTTP parser; it sets `send_pending`. It does not clear the input ring (the ring is cleared when the request is sent, `:118`, and by poll while no request is outstanding, `:562-565`).

`pull_retry(code)` (`:80-97`): `retry_count++`, `stats.retries++`; if `retry_count > max_retries` -> `pull_fail(code)`. Otherwise state = REQUESTING, `pull_prepare_range` (restart from the last written offset), and `not_before_ms = now_ms + min(retry_count * 100, 1000)` ms.

`retry_count` is reset to 0 after a response is committed (`:235`) and on (re)start (`:474`). So `max_retries` counts consecutive failures per range.

Events that trigger `pull_retry`:

| Event | Code passed | Where |
|---|---|---|
| `send` returns a failure other than `FWU_ERR_BUFFER_FULL` | `FWU_ERR_PULL_TIMEOUT` | `:126-127` |
| Headers lack `Content-Range` or `Content-Length` | `FWU_ERR_PULL_PROTOCOL` | `:148-151` |
| Inconsistent headers (`range_last < range_first`, `range_last >= image_size`, `content_length` != range length) | `FWU_ERR_PULL_PROTOCOL` | `:156-160` |
| Body would overflow `chunk_buffer` | `FWU_ERR_PULL_PROTOCOL` | `:183-186` |
| Completed body length != `range_last - range_first + 1` | `FWU_ERR_PULL_PROTOCOL` | `:226-229` |
| HTTP parser error | `FWU_ERR_PULL_PROTOCOL` | `:273-275` |
| Input ring overflow while a request is outstanding | `FWU_ERR_PULL_PROTOCOL` | `:579-584` |
| No byte received for `response_timeout_ms` | `FWU_ERR_PULL_TIMEOUT` | `:587-592` |

Events that do NOT consume the retry allowance: link drop (`fwu_pull_set_link`, `:496-503`); a stale/duplicate response (5.6); `send` returning `FWU_ERR_BUFFER_FULL` (request stays pending, `:129-131`).

Timeout: measured as `(uint32_t)(now_ms - last_rx_ms) >= response_timeout_ms` while `request_outstanding`. `last_rx_ms` is set when a request is sent (`:116`), when a pull is (re)started (`:477`) or the link drops (`:502`), and at every group of bytes processed by `pull_consume` (`:252`), including bytes of a stale response. `pull_time_reached` and `pull_elapsed` use unsigned 32-bit subtraction, so wraparound is handled.

Connection close: if a committed response had `Connection: close` and more image remains, `wait_reconnect = true` and `wait_start_ms = now_ms` (`:243-246`); requests are not sent while it is set. It is cleared by `fwu_pull_set_link(down)` or by `response_timeout_ms` elapsing from `wait_start_ms` (`:595-597`).

`fwu_pull_poll` order (`:536-604`):
1. If not active: clear ring; DONE + session IDLE -> IDLE; return. If session not DOWNLOADING: stop, IDLE; return. Otherwise `fwu_session_touch`.
2. Move bytes from the ring in pieces of `FWU_PULL_SCRATCH_SIZE` to `pull_consume` (HTTP parser, header check, body accumulation, completion) while active.
3. If `input_overflowed` and a request is outstanding: `pull_retry(FWU_ERR_PULL_PROTOCOL)`.
4. Timeout check (above).
5. Clear `wait_reconnect` if its timeout expired.
6. Send the next request iff `send_pending && !request_outstanding && link_up && !wait_reconnect && pull_time_reached(now_ms, not_before_ms)`. On success `request_outstanding = true`, `last_rx_ms = now_ms`, the ring is cleared and the parser reset, `stats.requests++`.

At most one request is sent per `fwu_pull_poll` call.

### 5.6 Range-buffer commit rule and stale responses

- Body bytes are only copied into `config.chunk_buffer` (`pull_store_body`, `:178-192`). Nothing is written to flash while a response is incomplete.
- When the parser reports the response complete and headers were checked (`:281`), `pull_response_complete` (`:222-247`) first verifies `chunk_fill == range_last - range_first + 1`. On mismatch: `pull_retry(FWU_ERR_PULL_PROTOCOL)` and nothing is written.
- Only then `pull_commit_range` (`:195-219`) loops over the buffer in pieces of the session chunk size calling `fwu_session_push_chunk(session, range_first + done, &chunk_buffer[done], n, now_ms, &next)`; `stats.chunks_written++` per accepted piece. If any call fails: `pull_fail(code)` and the commit stops; earlier pieces of the same range stay written in staging (and the session's `next_offset` reflects them).
- Restart point after any failure, retry or link loss is the session's `next_offset` (the last chunk actually written); a partial range in `chunk_buffer` is discarded.
- Input overflow: once `fwu_pull_feed` drops a group, `input_overflowed` blocks all later bytes of that response (`:522-526`) so a response with a hole is never parsed further; the next `fwu_pull_poll` re-requests the range.
- Stale responses: a header block that passes the consistency checks but whose `range_first`/`range_last` differ from the outstanding request sets `stale_response` (`:161-166`). Its body is discarded (`:180-182`). When it completes (`:282-288`) the parser is re-initialised and the controller keeps waiting for its own response; no retry is consumed. Status/size checks (non-206, wrong total) are applied to stale responses too, since they precede the range comparison.
- Bytes arriving after a complete response in the same input are discarded by the parser (`fwu_http_feed`, done phase) and, once `request_outstanding` is false, by `fwu_pull_feed` and `fwu_pull_poll`.

---

## 6. Cross-module facts

- Dependency chain: `fwu_pull` -> `fwu_http` + `fwu_ringbuf` + `fwu_session` -> `fwu_flash`.
- The pull controller never calls the flash wrappers itself; all flash access happens inside `fwu_session_push_chunk` (`src/session/fwu_session.c:645`), which shares its body with the TRANSFER_DATA handler (`session_store_chunk`).
- `fwu_pull_poll` and `fwu_session_poll` both call `fwu_session_touch` / refresh `last_activity_ms`, so a long pull does not trip the session idle timeout (`src/pull/fwu_pull.c:556`, `src/session/fwu_session.c:651`).
