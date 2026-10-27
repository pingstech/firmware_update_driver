# API reference: activation, boot core, version counters

Draft derived from the source only (`src/activation/`, `src/boot/`, `src/counters/`). Items that could not be
confirmed from the code are marked `UNVERIFIED`. Line numbers refer to the repository state at the time of writing.

Error codes (`fwu_return_e`, `src/error/fwu_error.h`) used below: `FWU_OK` = 0, `FWU_ERR_INVALID_ARG` = 10,
`FWU_ERR_BUFFER_TOO_SMALL` = 11, `FWU_ERR_BAD_STATE` = 12, `FWU_ERR_NOT_ENOUGH_DATA` = 23, `FWU_ERR_CRC_MISMATCH` = 30,
`FWU_ERR_BAD_LENGTH` = 31, `FWU_ERR_BAD_FRAME` = 32, `FWU_ERR_BAD_MANIFEST` = 33, `FWU_ERR_IMAGE_TOO_LARGE` = 34,
`FWU_ERR_HASH_MISMATCH` = 35, `FWU_ERR_FLASH_WRITE` = 40, `FWU_ERR_OUT_OF_RANGE` = 41, `FWU_ERR_FLASH_GEOMETRY` = 44,
`FWU_ERR_BAD_SIGNATURE` = 50, `FWU_ERR_CRYPTO_FAILURE` = 51, `FWU_ERR_PRODUCT_MISMATCH` = 52,
`FWU_ERR_VERSION_REJECTED` = 53, `FWU_ERR_KEY_REJECTED` = 54, `FWU_ERR_CONTRACT_MISMATCH` = 70.

---

## 1. Module `activation` (`src/activation/fwu_activate.h`, `fwu_activate.c`)

**Purpose.** Applies the verified new image from the staging area to the application region in a power-loss-resilient
way, using an A/B journal (`fwu_journal_ab`). Two strategies share the same journal and recovery logic: COPY (staging to
application, one-way) and SWAP (application and staging exchanged page by page, followed by a trial boot and an
automatic rollback if the new image does not confirm itself). The mode is a compile-time constant.

**Used by / depends on.** Depends on `fwu_error.h`, `fwu_types.h`, `fwu_flash.h`, `fwu_journal_ab.h`.
Used by `src/boot/fwu_boot.c` (includes it via `fwu_boot.h`), `src/session/fwu_session.c`
(`fwu_activate_staging_writable` at line 362, `fwu_activate_commit` at line 553).

### 1.1 Macros

| Macro | Value | Meaning (from header) |
|---|---|---|
| `FWU_ACT_STATE_COMMIT` | `1u` | journal state codes (persistent, numbers never change) |
| `FWU_ACT_STATE_PROGRESS` | `2u` | |
| `FWU_ACT_STATE_DONE` | `3u` | |
| `FWU_ACT_STATE_REJECTED` | `4u` | |
| `FWU_ACT_STATE_TRIAL` | `5u` | |
| `FWU_ACT_STATE_CONFIRMED` | `6u` | |
| `FWU_ACT_STATE_REVERT` | `7u` | |
| `FWU_ACT_STATE_REVERTED` | `8u` | |
| `FWU_ACT_FLAG_SWAP` | `0x01u` | journal `flags` bit 0: record written in SWAP mode |
| `FWU_ACT_STEPS_PER_PAGE` | `3u` | SWAP steps per page |
| `FWU_ACT_MAX_PAGES` | `21845u` | 3 * 21845 = 65535 (journal counter is 16 bit) |
| `FWU_ACT_MAX_TRIAL_BOOTS` | `255u` | upper bound of `max_trial_boots` |

Source: `src/activation/fwu_activate.h:116-129`. Journal record encoding (`fwu_activate.c:14-17`, `:66-67`):
`arg = (page_count << 16) | (counter & 0xFFFF)`; `flags` = `FWU_ACT_FLAG_SWAP` in SWAP mode, else 0.

### 1.2 Types

```c
typedef enum {
    FWU_ACTIVATE_MODE_COPY = 0,
    FWU_ACTIVATE_MODE_SWAP = 1
} fwu_activate_mode_e;
```
Values equal `FWU_ACTIVATE_COPY` (0) / `FWU_ACTIVATE_SWAP_ROLLBACK` (1) of `fwu_config.h` (header comment, `fwu_activate.h:131-137`).

```c
typedef enum {
    FWU_ACT_PHASE_IDLE = 0,
    FWU_ACT_PHASE_ACTIVATING,
    FWU_ACT_PHASE_TRIAL,
    FWU_ACT_PHASE_REVERTING
} fwu_activate_phase_e;
```
Phase derived from the last journal record (`act_refresh`, `fwu_activate.c:51-118`). Values: IDLE 0, ACTIVATING 1, TRIAL 2, REVERTING 3.

```c
typedef enum {
    FWU_ACT_OUTCOME_NONE = 0,
    FWU_ACT_OUTCOME_ACTIVATED,
    FWU_ACT_OUTCOME_TRIAL,
    FWU_ACT_OUTCOME_REVERTED,
    FWU_ACT_OUTCOME_REJECTED
} fwu_activate_outcome_e;
```
What `fwu_activate_run` did (valid only on `FWU_OK`). Values: NONE 0, ACTIVATED 1, TRIAL 2, REVERTED 3, REJECTED 4.

```c
typedef fwu_return_e (*fwu_activate_verify_fn)(void *context, const fwu_flash_t *flash,
                                               uint32_t offset, uint32_t length);
typedef fwu_return_e (*fwu_activate_confirmed_fn)(void *context);
```
See section 8 (hooks).

```c
typedef struct {
    const fwu_flash_t        *flash;
    fwu_activate_mode_e       mode;
    uint32_t                  app_offset;
    uint32_t                  staging_offset;
    uint32_t                  region_length;
    uint32_t                  journal_a_offset;
    uint32_t                  journal_b_offset;
    uint32_t                  journal_length;
    uint32_t                  scratch_offset;
    uint32_t                  scratch_pages;
    fwu_byte_t               *work;
    uint32_t                  work_length;
    uint32_t                  max_trial_boots;
    bool                      require_verify;
    fwu_activate_verify_fn    verify;
    void                     *verify_context;
    fwu_activate_confirmed_fn on_confirmed;
    void                     *on_confirmed_context;
} fwu_activate_config_t;
```

| Field | Constraint enforced by `fwu_activate_init` (`fwu_activate.c:315-408`) |
|---|---|
| `flash`, `work` | non-NULL, else `FWU_ERR_INVALID_ARG`; `fwu_flash_validate(flash)` must pass, else `FWU_ERR_FLASH_GEOMETRY` |
| `mode` | must be COPY or SWAP, else `FWU_ERR_INVALID_ARG` |
| `app_offset`, `staging_offset`, `region_length` | page aligned, `region_length` > 0 and multiple of page length (`FWU_ERR_INVALID_ARG`); inside flash (`FWU_ERR_OUT_OF_RANGE`); app and staging must not overlap (`FWU_ERR_INVALID_ARG`) |
| `work`, `work_length` | `work_length` rounded down to a multiple of `write_granule` must be non-zero (else `FWU_ERR_INVALID_ARG`); the copy chunk is capped at the page length |
| `max_trial_boots` | SWAP only: 1..`FWU_ACT_MAX_TRIAL_BOOTS`, else `FWU_ERR_INVALID_ARG`; ignored in COPY |
| `scratch_offset`, `scratch_pages` | SWAP only: `scratch_pages` >= 1 and `scratch_offset` page aligned (`FWU_ERR_INVALID_ARG`); K pages must fit flash (`FWU_ERR_OUT_OF_RANGE`); must not overlap app/staging (`FWU_ERR_INVALID_ARG`); ignored in COPY |
| `journal_a_offset`, `journal_b_offset`, `journal_length` | validated by `fwu_journal_ab_init` (errors propagated); must not overlap app, staging (and scratch in SWAP) (`FWU_ERR_INVALID_ARG`) |
| `require_verify` | if true, `verify` must be non-NULL, else `FWU_ERR_INVALID_ARG` |
| `verify`, `verify_context` | optional if `require_verify` is false (NULL = no verification) |
| `on_confirmed`, `on_confirmed_context` | optional (NULL = no hook) |

The configuration is copied into `fwu_activate_t.config`; `work` stays owned by the caller (must remain valid).

```c
typedef struct {
    fwu_activate_config_t config;
    fwu_journal_ab_t      journal;
    fwu_activate_phase_e  phase;
    fwu_byte_t            last_state;
    uint32_t              page_count;
    uint32_t              counter;
    uint32_t              region_pages;
    uint32_t              scratch_length;
    uint32_t              chunk;
} fwu_activate_t;
```
Header says: read the fields only. `last_state` 0 = no record.

### 1.3 Functions

#### `fwu_activate_init`
```c
fwu_return_e fwu_activate_init(fwu_activate_t *activate, const fwu_activate_config_t *config);
```
- Responsibility: validates configuration, initializes the A/B journal, reads the last record and derives the phase. Does not write flash (reads only).
- Parameters: `activate` (out, caller storage), `config` (in, copied).
- Returns: `FWU_OK`; `FWU_ERR_INVALID_ARG`, `FWU_ERR_OUT_OF_RANGE`, `FWU_ERR_FLASH_GEOMETRY` as in the table above; errors of `fwu_journal_ab_init` as they are (not enumerated here); `FWU_ERR_BAD_STATE` from `act_refresh` if the last record is uninterpretable (unknown state, record of the other mode, page count 0 or larger than region or `FWU_ACT_MAX_PAGES`, counter out of range for the state); flash read errors as they are.
- Call order: before any other `fwu_activate_*` call.
- Source: `src/activation/fwu_activate.c:315`.

#### `fwu_activate_get_phase`
```c
fwu_return_e fwu_activate_get_phase(const fwu_activate_t *activate, fwu_activate_phase_e *phase);
```
Returns `FWU_OK`, or `FWU_ERR_INVALID_ARG` if either pointer is NULL. Source: `fwu_activate.c:410`.

#### `fwu_activate_staging_writable`
```c
bool fwu_activate_staging_writable(const fwu_activate_t *activate);
```
Returns true only if `activate != NULL` and the phase is IDLE. Source: `fwu_activate.c:419`.

#### `fwu_activate_commit`
```c
fwu_return_e fwu_activate_commit(fwu_activate_t *activate, uint32_t page_count);
```
- Responsibility (application side): appends one `COMMIT` journal record (counter 0) for `page_count` pages. Does not touch the application region. A reboot is needed afterwards so the bootloader performs the activation.
- Parameters: `page_count` in pages of the flash page length; valid 1..`region_pages` and <= `FWU_ACT_MAX_PAGES`.
- Returns: `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL or bad `page_count`); `FWU_ERR_BAD_STATE` if phase is not IDLE; journal append errors as they are.
- Source: `fwu_activate.c:424`.

#### `fwu_activate_run`
```c
fwu_return_e fwu_activate_run(fwu_activate_t *activate, fwu_activate_outcome_e *outcome);
```
- Responsibility (bootloader side): completes pending work for the current phase (verify, copy/swap steps, trial counting, rollback), see section 3/4. Blocking: performs flash erase/write/read calls and, if configured, the verify hook, for the whole pending update (no timeout or yield visible in the code).
- Parameters: `outcome` is written only on `FWU_OK`.
- Returns: `FWU_OK` (application region consistent); `FWU_ERR_INVALID_ARG` (NULL); any flash error from erase/write/read as returned by the flash layer; `FWU_ERR_FLASH_WRITE` (read-back mismatch); the verify hook's error when it is not a rejection code; journal append errors; `FWU_ERR_BAD_STATE` only from the loop guard (`fwu_activate.c:489`, commented as unreachable). On any error the caller must not jump to the application.
- Source: `fwu_activate.c:438`.

#### `fwu_activate_confirm`
```c
fwu_return_e fwu_activate_confirm(fwu_activate_t *activate);
```
- Responsibility (new application, trial boot): in phase TRIAL appends `CONFIRMED`, then calls `on_confirmed` if set. In IDLE with `last_state == FWU_ACT_STATE_CONFIRMED` it only calls the hook again.
- Returns: `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL); `FWU_ERR_BAD_STATE` (any other phase/last state); journal append errors; the hook's return value as is. If the append fails the hook is not called.
- Source: `fwu_activate.c:492`.

#### `fwu_activate_request_revert`
```c
fwu_return_e fwu_activate_request_revert(fwu_activate_t *activate);
```
Phase TRIAL only: appends `REVERT` with counter 0 (phase becomes REVERTING); a reboot is required so the bootloader performs the rollback. Returns `FWU_OK`, `FWU_ERR_INVALID_ARG` (NULL), `FWU_ERR_BAD_STATE` (phase not TRIAL), journal append errors. Source: `fwu_activate.c:518`.

---

## 2. Activation phases and outcomes

Phase is derived from the last valid journal record (`act_refresh`, `fwu_activate.c:51-118`):

| Last record state | Phase | Checked consistency (else `FWU_ERR_BAD_STATE` from init/refresh) |
|---|---|---|
| none | IDLE | (`last_state` = 0) |
| DONE, REJECTED, CONFIRMED, REVERTED | IDLE | none |
| COMMIT | ACTIVATING | counter == 0 |
| PROGRESS | ACTIVATING | 1 <= counter <= total steps (COPY: pages; SWAP: pages * 3) |
| TRIAL | TRIAL | SWAP only; 1 <= counter <= 255 |
| REVERT | REVERTING | SWAP only; counter <= pages * 3 |
| any of COMMIT/PROGRESS/TRIAL/REVERT | | also: mode flag must match configured mode; 0 < pages <= `FWU_ACT_MAX_PAGES` and <= region pages |

Record sequences (header table, `fwu_activate.h:28-38`; confirmed against the code):
`COMMIT -> PROGRESS... -> DONE` (COPY), `COMMIT -> PROGRESS... -> TRIAL(1) -> TRIAL(k+1)... -> CONFIRMED` (SWAP),
`TRIAL(k) -> REVERT(0) -> REVERT(1..3N) -> REVERTED` (SWAP, boot-triggered when `k >= max_trial_boots`),
`TRIAL -> REVERT(0)` via `fwu_activate_request_revert`, `COMMIT -> REJECTED` (verification rejected).

`fwu_activate_run` loop (`fwu_activate.c:447-488`, at most 4 iterations):

| Phase at entry | Action | Outcome reported |
|---|---|---|
| IDLE | nothing | the value accumulated so far, else `FWU_ACT_OUTCOME_NONE` |
| ACTIVATING | `act_run_activating`: verify (only if `last_state == COMMIT` and a hook is set), then steps from `counter`, then final record | COPY: `FWU_ACT_OUTCOME_ACTIVATED`; SWAP: `FWU_ACT_OUTCOME_TRIAL`; verify rejection: `FWU_ACT_OUTCOME_REJECTED` |
| TRIAL | if `counter >= max_trial_boots`: append `REVERT(0)` and continue into REVERTING; else append `TRIAL(counter+1)` and return | `FWU_ACT_OUTCOME_TRIAL` (new trial) or `FWU_ACT_OUTCOME_REVERTED` (after rollback) |
| REVERTING | `act_run_reverting`: steps from `counter` to `3 * pages`, then `REVERTED` | `FWU_ACT_OUTCOME_REVERTED` |

Verification rejection (`act_is_rejection`, `fwu_activate.c:43-48`): hook error code in 30..39 or `FWU_ERR_BAD_SIGNATURE`,
`FWU_ERR_PRODUCT_MISMATCH`, `FWU_ERR_VERSION_REJECTED`, `FWU_ERR_KEY_REJECTED` => `REJECTED` record, outcome REJECTED, `FWU_OK`,
application untouched. Any other non-OK code (e.g. `FWU_ERR_CRYPTO_FAILURE`) => no record written, `run` returns that code.

## 3. Mode COPY: steps and power-loss recovery

Page `i` (0-based) step = `act_copy_page(staging page i -> application page i)` (`fwu_activate.c:199-205`): erase destination page, then for each chunk read source into `work`, write, read back and compare (`act_compare`, mismatch = `FWU_ERR_FLASH_WRITE`). After each step `PROGRESS(i+1)` is appended.

| Power lost / interrupted during | Journal state seen at next boot | Recovery as implemented |
|---|---|---|
| after `COMMIT`, before any step | COMMIT (counter 0), phase ACTIVATING | verify hook (if set) runs again, then steps from 0 |
| during step `i` (erase/write of application page `i`) | PROGRESS(i), or COMMIT if i = 0 | step `i` repeated from the start (destination erased and rewritten; staging untouched) |
| while writing a `PROGRESS` record | previous record valid (journal CRC; see `fwu_journal_ab`) | the step is repeated |
| after last `PROGRESS(N)`, before `DONE` | PROGRESS(N), phase ACTIVATING | loop does nothing, writes `DONE` |
| after `DONE` / `REJECTED` | IDLE | nothing pending |

The verify hook is re-run only when the last record is `COMMIT` (`fwu_activate.c:241`); with a `PROGRESS` record staging is assumed already verified. There is no return to the old version in COPY mode (header, `fwu_activate.h:5`). A phase that reports an error from `run` leaves the application possibly partial; the next boot continues.

## 4. Mode SWAP + ROLLBACK: steps and power-loss recovery

Three steps per page, page `p = step / 3`, sub-step `step % 3`; scratch page index = `p % scratch_pages` (`act_swap_step`, `fwu_activate.c:208-226`). A = application page, B = staging page, S = scratch page:

| Sub-step | Operation | Source intact until completion |
|---|---|---|
| 0 | `act_copy_page(A -> S)` | A |
| 1 | `act_copy_page(B -> A)` | B |
| 2 | `act_copy_page(S -> B)` | S |

`PROGRESS(step+1)` after each step; total `3 * pages` steps. Then `TRIAL(1)` is written and `run` returns outcome TRIAL. Rollback performs the same `act_swap_step` sequence with `REVERT(step+1)` records and ends with `REVERTED`.

| Interrupted at | Journal state seen at next boot (phase) | Recovery as implemented |
|---|---|---|
| after `COMMIT` | COMMIT (ACTIVATING) | verify hook runs again (if set), steps from 0 |
| during a swap step | PROGRESS(c) (ACTIVATING) | step `c` repeated from start; the source of each step is intact (table above) |
| after last `PROGRESS(3N)`, before `TRIAL(1)` | PROGRESS (ACTIVATING) | `TRIAL(1)` written, outcome TRIAL |
| during trial boot, new app did not confirm (hang, reset, power loss) | TRIAL(k) | `k < max_trial_boots`: write `TRIAL(k+1)`, outcome TRIAL; else write `REVERT(0)` and roll back in the same `run` call |
| during rollback step | REVERT(c) (REVERTING) | step `c` repeated; continues to `REVERTED` |
| after `CONFIRMED` (hook not yet run or failed) | IDLE, `last_state` CONFIRMED | `fwu_activate_confirm` may be called again; it only re-runs the hook |
| after `CONFIRMED` / `REVERTED` | IDLE | nothing pending |

Header notes (not re-verified beyond the code above): only the new image's `N` pages are swapped; a longer old image keeps its tail in the application region. The scratch page wear figure in `fwu_activate.h:47-52` is tagged unverified in the source.

## 5. Module `boot` (`src/boot/fwu_boot.h`, `fwu_boot.c`, `fwu_boot_profile.h`)

**Purpose.** Platform-independent bootloader core. Wires counters, verification and activation in a fixed order at every boot and returns a jump/stay decision. Hardware actions (watchdog start, jump, reboot, vector table check) are hooks; the actual startup code, jump and linker script are not in this repository (header comment).

**Used by / depends on.** Depends on `fwu_error.h`, `fwu_types.h`, `fwu_flash.h`, `fwu_crypto.h`, `fwu_verify.h`, `fwu_counters.h`, `fwu_activate.h`; `fwu_boot.c` also includes `fwu_bytes.h`, `fwu_crc.h`. Used by `examples/stm32g0b1/bootloader/boot_app.c`, `examples/c2000_launchxl_f280049c/bootloader/main.c` (bootloader), `examples/c2000_launchxl_f280049c/application/port/app_fwu.c` (application: `fwu_boot_init`, `fwu_boot_activate`, `fwu_boot_verify_config`). `fwu_boot_profile.h` additionally includes `fwu_config.h`.

### 5.1 Macros

| Macro | Value | Source |
|---|---|---|
| `FWU_BOOT_CONTRACT_VERSION` | `1u` | `fwu_boot.h:63` |
| `FWU_BOOT_INFO_SIZE` | `12u` | `fwu_boot.h:64` |
| `FWU_BOOT_INFO_VERSION` | `1u` | `fwu_boot.h:65` |

### 5.2 Types

```c
typedef enum {
    FWU_BOOT_VERIFY_NONE = 0,
    FWU_BOOT_VERIFY_HASH = 1,
    FWU_BOOT_VERIFY_SIGNED = 2
} fwu_boot_verify_e;

typedef struct {
    fwu_boot_verify_e   verify;
    fwu_activate_mode_e mode;
    bool                anti_rollback;
} fwu_boot_policy_t;
```
Policy rules enforced by `boot_validate` (`fwu_boot.c:27-51`, violation = `FWU_ERR_INVALID_ARG`):
`flash` non-NULL, valid `verify` and `mode` enum values; `activate_work` non-NULL with non-zero length; if `verify != NONE`: `crypto`, `verify_work` non-NULL and `verify_work_length` non-zero; if SIGNED: `keys` non-NULL and `key_count` > 0; if `anti_rollback`: `mode == SWAP` and `verify == SIGNED`.

```c
typedef struct {
    uint32_t app_offset;
    uint32_t staging_manifest_offset;
    uint32_t staging_image_offset;
    uint32_t image_region_length;
    uint32_t journal_a_offset;
    uint32_t journal_b_offset;
    uint32_t journal_length;
    uint32_t scratch_offset;
    uint32_t scratch_pages;
    uint32_t counter_a_offset;
    uint32_t counter_b_offset;
    uint32_t counter_length;
} fwu_boot_layout_t;
```
All values are offsets/lengths in flash (`fwu_byte_t` units). Mapping done in `fwu_boot_init` (`fwu_boot.c:83-117`): `staging_manifest_offset` -> verify `manifest_offset`; `staging_image_offset` -> verify `image_offset` and activation `staging_offset`; `image_region_length` -> verify `max_image_size` and activation `region_length`.

```c
typedef bool (*fwu_boot_app_valid_fn)(void *context);
typedef void (*fwu_boot_watchdog_fn)(void *context);

typedef struct {
    fwu_boot_policy_t        policy;
    fwu_boot_layout_t        layout;
    const fwu_flash_t       *flash;
    fwu_crypto_t            *crypto;
    const fwu_verify_key_t  *keys;
    uint32_t                 key_count;
    uint32_t                 product_id;
    uint32_t                 max_trial_boots;
    fwu_byte_t              *verify_work;
    uint32_t                 verify_work_length;
    fwu_byte_t              *activate_work;
    uint32_t                 activate_work_length;
    fwu_boot_app_valid_fn    app_valid;
    fwu_boot_watchdog_fn     watchdog_arm;
    void                    *context;
} fwu_boot_config_t;
```
`max_trial_boots` is validated only by `fwu_activate_init` (SWAP: 1..255). Header says `activate_work` must be separate from `verify_work`; this is not checked in code (see report). The configuration is copied into `fwu_boot_t.config`; pointers it contains (buffers, keys, flash, crypto) stay owned by the caller.

```c
typedef struct {
    fwu_boot_config_t        config;
    bool                     initialized;
    bool                     confirm_retry;
    fwu_counters_t           counters;
    fwu_counters_confirm_t   confirm;
    fwu_verify_config_t      verify;
    fwu_activate_config_t    activate_config;
    fwu_activate_t           activate;
} fwu_boot_t;
```
Must not be moved or copied after init: `confirm`, `verify` and `activate_config` hold pointers to sibling fields (`fwu_boot.c:96-117`). Header example declares it `static`.

```c
typedef enum {
    FWU_BOOT_JUMP_APP = 0,
    FWU_BOOT_STAY = 1
} fwu_boot_decision_e;

typedef enum {
    FWU_BOOT_REASON_NONE = 0,
    FWU_BOOT_REASON_CONFIG,
    FWU_BOOT_REASON_COUNTERS,
    FWU_BOOT_REASON_ACTIVATE_INIT,
    FWU_BOOT_REASON_ACTIVATE_RUN,
    FWU_BOOT_REASON_NO_APP
} fwu_boot_reason_e;

typedef struct {
    fwu_boot_decision_e      decision;
    fwu_boot_reason_e        reason;
    fwu_return_e             status;
    fwu_activate_outcome_e   outcome;
    fwu_activate_phase_e     phase;
    bool                     trial;
} fwu_boot_result_t;
```
Decision enum: `fwu_boot.h:137-140`. Reason values: NONE 0, CONFIG 1, COUNTERS 2, ACTIVATE_INIT 3, ACTIVATE_RUN 4, NO_APP 5.

### 5.3 Functions

#### `fwu_boot_init`
```c
fwu_return_e fwu_boot_init(fwu_boot_t *boot, const fwu_boot_config_t *config);
```
Validates the policy, then (if `anti_rollback`) initializes counters and reads them, builds `fwu_verify_config_t` and `fwu_activate_config_t`, and calls `fwu_activate_init`. Does not run the activation and does not write flash. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL or policy violation); errors of `fwu_counters_init` / `fwu_counters_get` / `fwu_activate_init` as they are. On failure `initialized` stays false. Intended for the application. Source: `fwu_boot.c:131` (stage `boot_init_stage` at `:54`).

#### `fwu_boot_start`
```c
fwu_boot_decision_e fwu_boot_start(fwu_boot_t *boot, const fwu_boot_config_t *config, fwu_boot_result_t *result);
```
Bootloader entry. Returns `FWU_BOOT_JUMP_APP` or `FWU_BOOT_STAY`; `result` may be NULL. See section 6 for the decision logic. Blocking: runs `fwu_activate_run`, i.e. all pending flash work. Source: `fwu_boot.c:141`.

#### `fwu_boot_app_finish_trial`
```c
fwu_return_e fwu_boot_app_finish_trial(fwu_boot_t *boot, bool healthy);
```
- Phase TRIAL: `healthy == false` -> `fwu_activate_request_revert` (reboot required afterwards); `healthy == true` -> `fwu_activate_confirm` and `confirm_retry = (status != FWU_OK)`.
- Phase IDLE: if `healthy && confirm_retry`, calls `fwu_activate_confirm` again (re-runs the hook only) and updates `confirm_retry`; otherwise `FWU_OK` without action.
- Phase ACTIVATING / REVERTING: `FWU_ERR_BAD_STATE`.
- Returns also `FWU_ERR_INVALID_ARG` if `boot` is NULL or not initialized; confirm/revert errors as they are. `confirm_retry` is RAM only.
- Source: `fwu_boot.c:195`.

#### `fwu_boot_activate`, `fwu_boot_verify_config`
```c
fwu_activate_t *fwu_boot_activate(fwu_boot_t *boot);
const fwu_verify_config_t *fwu_boot_verify_config(const fwu_boot_t *boot);
```
Return the embedded activation object / verification configuration, or NULL if `boot` is NULL or not initialized. Used by `examples/c2000_launchxl_f280049c/application/port/app_fwu.c:42-43` to fill the session configuration. Source: `fwu_boot.c:226`, `:231`.

#### `fwu_boot_fingerprint`, `fwu_boot_info_encode`, `fwu_boot_info_check`
```c
fwu_return_e fwu_boot_fingerprint(const fwu_boot_config_t *config, uint32_t *fingerprint);
fwu_return_e fwu_boot_info_encode(const fwu_boot_config_t *config, fwu_byte_t *out, size_t out_capacity, size_t *out_length);
fwu_return_e fwu_boot_info_check(const fwu_boot_config_t *config, const fwu_byte_t *info, size_t length);
```
See section 7. Source: `fwu_boot.c:250`, `:285`, `:313`.

### 5.4 `fwu_boot_profile.h` (header-only, `static inline`)

```c
static inline fwu_boot_policy_t fwu_boot_policy_from_profile(void)
static inline fwu_boot_layout_t fwu_boot_layout_from_board(void)
```
Not included by `fwu_boot.h`; product code includes it after the profile/board macros are defined. `fwu_boot_policy_from_profile`: `FWU_CFG_VERIFY` NONE -> `FWU_BOOT_VERIFY_NONE`, `FWU_VERIFY_CRC32` -> `FWU_BOOT_VERIFY_HASH`, otherwise `FWU_BOOT_VERIFY_SIGNED`; `FWU_CFG_ACTIVATION == FWU_ACTIVATE_SWAP_ROLLBACK` -> `FWU_ACTIVATE_MODE_SWAP`, else COPY; `anti_rollback = (FWU_CFG_ANTI_ROLLBACK != 0)`. `fwu_boot_layout_from_board` exists only if `FWU_BOARD_STM32G0B1`, `FWU_BOARD_F2800157` or `FWU_BOARD_F280049C` is defined and copies the `FWU_LAYOUT_*` macros field by field. Source: `fwu_boot_profile.h:22`, `:44`.

## 6. Bootloader decision logic (`fwu_boot_start`)

Initial `result`: decision STAY, reason `FWU_BOOT_REASON_CONFIG`, status `FWU_ERR_INVALID_ARG`, outcome NONE, phase IDLE, trial false (`fwu_boot.c:149-154`). NULL `boot` or `config` returns STAY with these values. Checks in order (`fwu_boot.c:159-192`, stage details `:54-127`):

| # | Check | On failure: decision / `reason` / `status` |
|---|---|---|
| 1 | `boot_validate` (policy rules, section 5.2) | STAY / `FWU_BOOT_REASON_CONFIG` / `FWU_ERR_INVALID_ARG` |
| 2 | only if `anti_rollback`: `fwu_counters_init`, `fwu_counters_get` | STAY / `FWU_BOOT_REASON_COUNTERS` / error as returned |
| 3 | build verify and activate configuration, `fwu_activate_init` | STAY / `FWU_BOOT_REASON_ACTIVATE_INIT` / error as returned (includes invalid `max_trial_boots`, scratch/journal problems, uninterpretable journal) |
| 4 | `fwu_activate_run` (then `fwu_activate_get_phase`) | STAY / `FWU_BOOT_REASON_ACTIVATE_RUN` / error as returned |
| 5 | `app_valid(context)` if hook set; must return true | STAY / `FWU_BOOT_REASON_NO_APP` / `FWU_OK` |
| 6 | if `trial` (phase TRIAL after run) and `watchdog_arm` set: call it | cannot fail |
| - | otherwise | `FWU_BOOT_JUMP_APP`, reason NONE, status `FWU_OK` |

`result->outcome` and `result->phase` are filled after step 4 succeeds (so they are valid for STAY with reason NO_APP). `result->trial = (phase == FWU_ACT_PHASE_TRIAL)`. In steps 1-4 failure never returns `FWU_BOOT_JUMP_APP`.

Verification configuration built in step 3 (`fwu_boot.c:83-94`): `require_signature = (verify == SIGNED)`; minimum image/key versions come from the counters (0 when `anti_rollback` is off); activation gets `fwu_verify_activation_hook` and `require_verify = true` when `verify != NONE`; with `anti_rollback` the activation gets `on_confirmed = fwu_counters_on_confirmed`.

## 7. Contract fingerprint (`fwu_boot_info_*`)

Purpose (header): bootloader and application must be built with the same policy, layout and product identity. The bootloader publishes a 12-byte record; the application compares it with the fingerprint of its own configuration.

**Fingerprint** (`fwu_boot_fingerprint`, `fwu_boot.c:250-283`): 32-bit FNV-1a (offset basis 2166136261, prime 16777619), each 32-bit value hashed as 4 bytes, most significant byte first. Input order:
`FWU_BOOT_CONTRACT_VERSION`, `policy.verify`, `policy.mode`, `anti_rollback` (0/1), `product_id`, `max_trial_boots`, flash `page_length`, `write_granule`, `flash_length`, then layout `app_offset`, `staging_manifest_offset`, `staging_image_offset`, `image_region_length`, `journal_a_offset`, `journal_b_offset`, `journal_length`, `scratch_offset`, `scratch_pages`, `counter_a_offset`, `counter_b_offset`, `counter_length`. Keys, buffers and hooks are not hashed. Returns `FWU_OK`, or `FWU_ERR_INVALID_ARG` if `config`, `config->flash` or `fingerprint` is NULL.

**Record** (`fwu_boot_info_encode`, `fwu_boot.c:285-311`), 12 bytes, big endian:

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | magic `0x46 0x57 0x42 0x49` ("FWBI") |
| 4 | 2 | `FWU_BOOT_INFO_VERSION` |
| 6 | 4 | fingerprint |
| 10 | 2 | CRC-16 over bytes 0..9 (`fwu_crc16_calculate_w_start_value(FWU_CRC16_INIT, ...)`) |

`fwu_boot_info_encode` returns `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL `config`, `out` or `out_length`, or fingerprint error); `FWU_ERR_BUFFER_TOO_SMALL` if `out_capacity < FWU_BOOT_INFO_SIZE`. Sets `*out_length = 12`.

**Check** (`fwu_boot_info_check`, `fwu_boot.c:313-345`), checks in this order: NULL `config`/`info` -> `FWU_ERR_INVALID_ARG`; `length < 12` -> `FWU_ERR_NOT_ENOUGH_DATA`; bytes masked with `FWU_BYTE_MASK`; magic mismatch -> `FWU_ERR_CONTRACT_MISMATCH`; CRC mismatch -> `FWU_ERR_CONTRACT_MISMATCH`; version mismatch -> `FWU_ERR_CONTRACT_MISMATCH`; fingerprint computed from `config` (its error returned as is); stored fingerprint differs -> `FWU_ERR_CONTRACT_MISMATCH`; else `FWU_OK`. Placement of the record at a fixed flash address and its readout is integration work: `examples/stm32g0b1/bootloader/boot_app.c:115` calls `fwu_boot_info_encode`; no call to `fwu_boot_info_check` exists in the repository outside comments (`examples/stm32g0b1/bootloader/fwu_platform.h:48`, `:103`).

## 8. Hooks: who calls them

| Hook | Declared in | Called by | Where | Notes |
|---|---|---|---|---|
| `app_valid` (`fwu_boot_app_valid_fn`) | `fwu_boot_config_t.app_valid` | `fwu_boot_start`, after `fwu_activate_run` succeeded | `fwu_boot.c:181` | true = jumpable; false = STAY / NO_APP; NULL = no check. Receives `config->context`. Set by `examples/stm32g0b1/bootloader/boot_app.c:107` and `examples/c2000_launchxl_f280049c/bootloader/main.c:113`; `NULL` in the C2000 application (`app_fwu.c:78`). |
| `watchdog_arm` (`fwu_boot_watchdog_fn`) | `fwu_boot_config_t.watchdog_arm` | `fwu_boot_start`, only when `trial` is true, right before returning JUMP_APP | `fwu_boot.c:186-187` | Receives `config->context`. Set by the STM32 bootloader (`boot_app.c:108`); `NULL` in the C2000 bootloader (`main.c:114`) and application. |
| `verify` (`fwu_activate_verify_fn`) | `fwu_activate_config_t.verify` | `fwu_activate_run` -> `act_run_activating` | `fwu_activate.c:242` | Called only when `last_state == COMMIT`, with `(verify_context, flash, staging_offset, pages * page_length)`. `fwu_boot` sets it to `fwu_verify_activation_hook` when `verify != NONE` (`fwu_boot.c:114`). |
| `on_confirmed` (`fwu_activate_confirmed_fn`) | `fwu_activate_config_t.on_confirmed` | `fwu_activate_confirm` (after writing `CONFIRMED`, or alone in IDLE+CONFIRMED) | `fwu_activate.c:512-513` | `fwu_boot` sets it to `fwu_counters_on_confirmed` when `anti_rollback` (`fwu_boot.c:116`); its caller is `fwu_boot_app_finish_trial` (`fwu_boot.c:211`, `:217`). No caller of `fwu_boot_app_finish_trial` or `fwu_activate_confirm` exists in `src/`, `ports/` or `examples/` outside comments. |

Hook errors: `verify` rejection codes are handled as in section 2; `on_confirmed`'s return value is returned unchanged by `fwu_activate_confirm`.

---

## 9. Module `counters` (`src/counters/fwu_counters.h`, `fwu_counters.c`)

**Purpose.** Persistent anti-rollback state: lowest accepted image version (32 bit) and lowest accepted key version (16 bit), kept in an A/B journal (separate from the activation journal). Values only increase (`max(old, requested)`); a raise writes one journal record or none.

**Used by / depends on.** Depends on `fwu_error.h`, `fwu_types.h`, `fwu_flash.h`, `fwu_journal_ab.h`, `fwu_verify.h` (`fwu_verify_manifest`, `fwu_manifest_t`). Used by `src/boot/fwu_boot.c` (`fwu_counters_init`, `fwu_counters_get`, hook `fwu_counters_on_confirmed`). Only referenced in comments elsewhere (`src/verify/fwu_verify.h:93`).

### 9.1 Types

```c
typedef struct {
    fwu_journal_ab_t journal;
    uint32_t         min_image_version;
    uint16_t         min_key_version;
    bool             ready;
} fwu_counters_t;

typedef struct {
    fwu_counters_t            *counters;
    const fwu_verify_config_t *verify;
    const fwu_flash_t         *flash;
} fwu_counters_confirm_t;
```
Record layout (`fwu_counters.c:14-17`, `:93-94`): journal `arg` = image version, `state` = key version high byte, `flags` = key version low byte. `ready` is true after a successful load; false after a failed init/reload.

### 9.2 Functions

#### `fwu_counters_init`
```c
fwu_return_e fwu_counters_init(fwu_counters_t *counters, const fwu_flash_t *flash,
                               uint32_t offset_a, uint32_t offset_b, uint32_t length);
```
Sets `ready = false` and values to 0, calls `fwu_journal_ab_init`, loads the last record (no record gives 0, 0) and sets `ready = true`. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL `counters`); errors of `fwu_journal_ab_init` as they are (including its `FWU_ERR_INVALID_ARG`). Source: `fwu_counters.c:25`.

#### `fwu_counters_reload`
```c
fwu_return_e fwu_counters_reload(fwu_counters_t *counters);
```
Rescans the journal (`fwu_journal_ab_scan`); on success loads values and sets `ready`; on error sets `ready = false` and returns the error. `FWU_ERR_INVALID_ARG` for NULL. Source: `fwu_counters.c:45`.

#### `fwu_counters_get`
```c
fwu_return_e fwu_counters_get(const fwu_counters_t *counters, uint32_t *image_version, uint16_t *key_version);
```
Returns `FWU_OK` and the values; `FWU_ERR_INVALID_ARG` (any NULL); `FWU_ERR_BAD_STATE` if not ready (outputs unchanged). Source: `fwu_counters.c:61`.

#### `fwu_counters_raise`
```c
fwu_return_e fwu_counters_raise(fwu_counters_t *counters, uint32_t image_version, uint16_t key_version);
```
Computes `max` per field; if both unchanged returns `FWU_OK` without writing flash. Otherwise appends one journal record. On append error it calls `fwu_counters_reload` (result ignored) and returns the append error. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL); `FWU_ERR_BAD_STATE` (not ready); journal append errors. Blocking: one flash record write. Source: `fwu_counters.c:74`.

#### `fwu_counters_on_confirmed`
```c
fwu_return_e fwu_counters_on_confirmed(void *context);
```
`context` is a `fwu_counters_confirm_t *`. Calls `fwu_verify_manifest(confirm->verify, confirm->flash, &manifest)`; on success calls `fwu_counters_raise(confirm->counters, manifest.image_version, manifest.key_version)`. Returns `FWU_ERR_INVALID_ARG` if the context or any of its three pointers is NULL; otherwise errors of `fwu_verify_manifest` or `fwu_counters_raise` unchanged. Counter unchanged if verification fails. Source: `fwu_counters.c:105`.
