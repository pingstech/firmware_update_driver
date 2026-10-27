# API reference: crypto, verify, auth, frame

Modules covered: `src/crypto/` (`fwu_crypto`, `fwu_crypto_sw`, `fwu_sha256`, `fwu_crypto_sim`), `src/verify/` (`fwu_verify`), `src/auth/` (`fwu_auth`), `src/frame/` (`fwu_frame`, `fwu_decode`, `fwu_parser`).

Conventions: `fwu_byte_t` holds one byte value 0..255 per element (low 8 bits significant, `FWU_BYTE_MASK` = `0xFFu`, `src/fwu_types.h:53`). Return codes are `fwu_return_e` (`src/error/fwu_error.h`); numeric values quoted below are from that header. None of these modules uses dynamic memory. None of them takes a lock or is documented as reentrant or ISR-safe (`fwu_crypto.h` states "not reentrant and not thread-safe"; `fwu_auth.h` states "not called from an ISR").

Source line numbers refer to the repository state after the comment translation (code lines are unchanged).

---

## 1. What is really implemented (crypto status at a glance)

| Capability | Status | Where |
|---|---|---|
| SHA-256 (FIPS 180-4), incremental | **Implemented** (real, production-usable, no test guard) | `src/crypto/fwu_sha256.c` |
| SHA-256 behind the crypto port (`sha256_begin/update/finish`) | **Implemented** by the software port | `src/crypto/fwu_crypto_sw.c:9-40` |
| Ed25519 verification (`ed25519_verify`) in the software port | **NOT implemented. Fail-closed stub**: unconditionally returns `FWU_ERR_CRYPTO_FAILURE`, never `FWU_OK` | `src/crypto/fwu_crypto_sw.c:42-52` |
| Ed25519 verification in any real library/hardware | **Not present in this repository.** The integrator must supply a port (`fwu_crypto_callback_t`) | `src/crypto/fwu_crypto.h:65` |
| Signing (Ed25519 or otherwise) | Not part of the device. Only `fwu_crypto_sim_sign` (insecure, test only) and the host-side `sign` hook of `fwu_auth_host_t` exist | see section 4 and 6 |
| Simulated crypto `fwu_crypto_sim` | SHA-256 real; "signature" is a **fake, insecure** construction; **TEST/PC ONLY** | `src/crypto/fwu_crypto_sim.h` |

Consequence: with `fwu_crypto_sw` and `require_signature == true`, no image can ever be accepted (every verification that reaches the signature step ends in `FWU_ERR_CRYPTO_FAILURE`). With `require_signature == false` (hash-only profile) the pipeline works with `fwu_crypto_sw`. Examples that use `fwu_crypto_sw_init`: `examples/c2000_launchxl_f280049c/application/port/app_fwu.c:63`, `examples/c2000_launchxl_f280049c/bootloader/main.c:94`, `examples/stm32g0b1/bootloader/boot_app.c:94`.

---

## 2. Module `fwu_crypto` (crypto port interface and wrappers)

Header: `src/crypto/fwu_crypto.h`, implementation: `src/crypto/fwu_crypto.c`.

Purpose: a port interface (function table + `context`) behind which a SHA-256 and an Ed25519 *verification* implementation is plugged. The wrappers validate arguments, track whether a SHA-256 computation is open (`hashing`), and normalise port return codes. The device only verifies signatures; it never signs.

Used by (files that include `fwu_crypto.h` or `fwu_crypto_sw.h`, non-test): `src/verify/fwu_verify.h`, `src/auth/fwu_auth.h`, `src/boot/fwu_boot.h`, `src/crypto/fwu_crypto_sw.*`, `src/crypto/fwu_crypto_sim.h`, and the three examples in section 1. Depends on: `fwu_error.h`, `fwu_types.h`.

### Macros

| Macro | Value | Meaning |
|---|---|---|
| `FWU_CRYPTO_SHA256_DIGEST_SIZE` | `32u` | digest elements |
| `FWU_CRYPTO_ED25519_PUBLIC_KEY_SIZE` | `32u` | public key elements |
| `FWU_CRYPTO_ED25519_SIGNATURE_SIZE` | `64u` | signature elements |

### Types

```c
typedef struct {
    fwu_return_e (*sha256_begin)(void *context);
    fwu_return_e (*sha256_update)(void *context, const fwu_byte_t *data, uint32_t length);
    fwu_return_e (*sha256_finish)(void *context, fwu_byte_t *digest);
    fwu_return_e (*ed25519_verify)(void *context, const fwu_byte_t *message, uint32_t message_length,
                                   const fwu_byte_t *signature, const fwu_byte_t *public_key);
} fwu_crypto_callback_t;

typedef struct {
    void                        *context;
    const fwu_crypto_callback_t *callback;
    bool                         hashing;
} fwu_crypto_t;
```

Port contract (from the header): all four callbacks are mandatory. `sha256_update` is only called with `length >= 1` and non-NULL `data`. `ed25519_verify` is only called with `message_length >= 1`; `signature` is 64 and `public_key` is 32 elements. `fwu_crypto_t::hashing` is managed by the wrappers (do not touch).

### Return-code normalisation (guarantees, `fwu_crypto.c:14-17, 89-107`)

* SHA-256 callbacks: a port result of `FWU_OK` is kept; **any other value becomes `FWU_ERR_CRYPTO_FAILURE` (51)**.
* Ed25519: port `FWU_OK` -> `FWU_OK`; port `FWU_ERR_BAD_SIGNATURE` (50) -> `FWU_ERR_BAD_SIGNATURE`; **any other value -> `FWU_ERR_CRYPTO_FAILURE`**. The wrapper returns `FWU_OK` only if the port returned `FWU_OK`.
* `FWU_ERR_BAD_SIGNATURE` means "signature invalid, reject the image"; `FWU_ERR_CRYPTO_FAILURE` means "could not be performed, do not reject, retry possible".

### SHA-256 state machine (tracked by `hashing`)

| Call | Precondition | Effect |
|---|---|---|
| `fwu_crypto_sha256_begin` | `hashing == false`, else `FWU_ERR_BAD_STATE` | calls port begin; `hashing = (result == FWU_OK)` |
| `fwu_crypto_sha256_update` | `hashing == true`, else `FWU_ERR_BAD_STATE` | `length == 0` returns `FWU_OK` without calling the port; port error closes the digest (`hashing = false`) |
| `fwu_crypto_sha256_finish` | `hashing == true`, else `FWU_ERR_BAD_STATE` | `hashing = false` in every case (success or error) |

Argument errors (`FWU_ERR_INVALID_ARG`) do not change `hashing`. Only one digest can be open at a time.

### Functions

All functions: return `fwu_return_e`; no blocking behaviour of their own (they call the port callbacks synchronously, so blocking depends on the port). `INVALID_ARG` for a NULL `self`, NULL `self->callback`, or any NULL callback is returned by every wrapper except `fwu_crypto_init` (which checks the same conditions on its own arguments).

```c
fwu_return_e fwu_crypto_init(fwu_crypto_t *self, void *context, const fwu_crypto_callback_t *callback);
```
Fills `self`, sets `hashing = false`. Returns `FWU_OK`, or `FWU_ERR_INVALID_ARG` if `self`, `callback` or any of the four function pointers is NULL (`context` may be NULL). `src/crypto/fwu_crypto.c:23`.

```c
fwu_return_e fwu_crypto_sha256_begin(fwu_crypto_t *self);
```
Returns `FWU_OK`, `FWU_ERR_INVALID_ARG`, `FWU_ERR_BAD_STATE` (already open), `FWU_ERR_CRYPTO_FAILURE` (port error). `src/crypto/fwu_crypto.c:36`.

```c
fwu_return_e fwu_crypto_sha256_update(fwu_crypto_t *self, const fwu_byte_t *data, uint32_t length);
```
Returns `FWU_OK`, `FWU_ERR_INVALID_ARG` (NULL object, or `length > 0` with NULL `data`; checked before the state check), `FWU_ERR_BAD_STATE`, `FWU_ERR_CRYPTO_FAILURE`. `src/crypto/fwu_crypto.c:51`.

```c
fwu_return_e fwu_crypto_sha256_finish(fwu_crypto_t *self, fwu_byte_t *digest);
```
Writes `FWU_CRYPTO_SHA256_DIGEST_SIZE` (32) elements to `digest`. Returns `FWU_OK`, `FWU_ERR_INVALID_ARG` (NULL object or `digest`), `FWU_ERR_BAD_STATE`, `FWU_ERR_CRYPTO_FAILURE` (`digest` content not trustworthy). `src/crypto/fwu_crypto.c:74`.

```c
fwu_return_e fwu_crypto_ed25519_verify(fwu_crypto_t *self, const fwu_byte_t *message, uint32_t message_length,
                                       const fwu_byte_t *signature, const fwu_byte_t *public_key);
```
Returns `FWU_OK` (valid), `FWU_ERR_BAD_SIGNATURE` (invalid), `FWU_ERR_CRYPTO_FAILURE` (not performed / any other port code), `FWU_ERR_INVALID_ARG` (NULL pointer or `message_length == 0`). Does not use or change `hashing`. `src/crypto/fwu_crypto.c:89`.

---

## 3. Module `fwu_sha256` (software SHA-256)

Header: `src/crypto/fwu_sha256.h`, implementation `src/crypto/fwu_sha256.c`. Streaming FIPS 180-4 SHA-256, no dynamic memory. Elements are masked with `FWU_BYTE_MASK`. The header states correctness up to a total length of 2^61 - 1 bytes (bit counter in two 32-bit words).

Used by: `src/crypto/fwu_crypto_sw.c`. Depends on: `fwu_types.h`.

Macros: `FWU_SHA256_DIGEST_SIZE` = `32u`, `FWU_SHA256_BLOCK_SIZE` = `64u`.

```c
typedef struct {
    uint32_t   h[8];
    uint32_t   bits_hi;
    uint32_t   bits_lo;
    uint32_t   block_length;
    fwu_byte_t block[FWU_SHA256_BLOCK_SIZE];
} fwu_sha256_t;
```
Fields are internal ("do not touch").

```c
void fwu_sha256_init(fwu_sha256_t *s);
void fwu_sha256_update(fwu_sha256_t *s, const fwu_byte_t *data, uint32_t length);
void fwu_sha256_final(fwu_sha256_t *s, fwu_byte_t *digest);
```
No return values and no argument checking (NULL pointers are not handled: callers must pass valid pointers). `update` with `length == 0` does nothing. `final` writes 32 elements to `digest` and leaves the state unusable until `init` is called again. Compute time is proportional to the data (a 64-round loop per 64-byte block). `src/crypto/fwu_sha256.c:70, 80, 99`.

---

## 4. Module `fwu_crypto_sw` (software port: real SHA-256, Ed25519 stub)

Header: `src/crypto/fwu_crypto_sw.h`, implementation `src/crypto/fwu_crypto_sw.c`.

Purpose: implements the `fwu_crypto.h` port with real SHA-256 (`fwu_sha256`) and an Ed25519 placeholder.

**Ed25519 is a stub.** `sw_ed25519_verify` ignores all its arguments and returns `FWU_ERR_CRYPTO_FAILURE` (`fwu_crypto_sw.c:42-52`). It never reports "valid", so it fails closed; a signed profile built on this port accepts no image. The header ties this to README open issue 37 for the hash-only (DEV) case; it is not a solution for signed profiles.

Used by: the three examples listed in section 1. Depends on: `fwu_crypto.h`, `fwu_sha256.h`, `fwu_error.h`.

```c
typedef struct {
    fwu_sha256_t          sha;
    fwu_crypto_callback_t callback;
} fwu_crypto_sw_t;
```
Must stay at a fixed address (`crypto.context` points to it).

```c
fwu_return_e fwu_crypto_sw_init(fwu_crypto_sw_t *sw, fwu_crypto_t *crypto);
```
Initialises the SHA state, fills the function table and calls `fwu_crypto_init(crypto, sw, &sw->callback)`. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` if `sw` or `crypto` is NULL. `src/crypto/fwu_crypto_sw.c:53`.

Usage (from the header): `static fwu_crypto_sw_t sw; static fwu_crypto_t crypto; fwu_crypto_sw_init(&sw, &crypto);`

---

## 5. Module `fwu_crypto_sim` (SIMULATED crypto - TEST / PC ONLY)

Header: `src/crypto/fwu_crypto_sim.h`, implementation `src/crypto/fwu_crypto_sim.c`.

> **WARNING: NOT SECURE. TEST / PC ONLY. MUST NOT BE USED IN PRODUCTION.**
> The header raises `#error` when `FWU_PRODUCTION` is defined. The SHA-256 in this module is a real implementation, but the "signature" is an insecure fake with **no private key**: anyone who knows the public key can forge it.

Fake signature construction (`fwu_crypto_sim.c:144-157`): 64 elements = `SHA-256(public_key || message)` in bytes 0..31, followed by `SHA-256(message || public_key)` in bytes 32..63. `ed25519_verify` recomputes this and returns `FWU_OK` if all 64 elements match, otherwise `FWU_ERR_BAD_SIGNATURE` (`fwu_crypto_sim.c:195-213`).

Used by: tests (`test_*.c`), `src/transport/fwu_linksim.h` (mentioned in a comment). Depends on: `fwu_crypto.h`.

### Types

```c
typedef struct {
    uint32_t   h[8];
    fwu_byte_t block[64];
    uint32_t   block_length;
    uint32_t   total_length;
} fwu_crypto_sim_sha_t;            /* internal */

typedef struct {
    fwu_crypto_t           crypto;
    fwu_crypto_callback_t  callback;
    fwu_crypto_sim_sha_t   sha;
    fwu_return_e           fail_begin;
    fwu_return_e           fail_update;
    fwu_return_e           fail_finish;
    fwu_return_e           fail_verify;
    uint32_t               begin_calls;
    uint32_t               update_calls;
    uint32_t               finish_calls;
    uint32_t               verify_calls;
    uint32_t               last_verify_message_length;
} fwu_crypto_sim_t;
```
Fault injection: if a `fail_*` field is not `FWU_OK` the corresponding port function returns that value (counters still increment). The `*_calls` counters count port-function invocations.

### Functions

```c
fwu_return_e fwu_crypto_sim_init(fwu_crypto_sim_t *sim);
void fwu_crypto_sim_sha256(const fwu_byte_t *data, uint32_t length, fwu_byte_t *digest);
void fwu_crypto_sim_sign(const fwu_byte_t *public_key, const fwu_byte_t *message, uint32_t message_length,
                         fwu_byte_t *signature);
```
* `fwu_crypto_sim_init`: clears injection and counters, fills `sim->crypto` via `fwu_crypto_init`. Returns `FWU_OK`, or `FWU_ERR_INVALID_ARG` for NULL (`fwu_crypto_sim.c:216`).
* `fwu_crypto_sim_sha256`: one-shot SHA-256, `digest` receives 32 elements; no argument checks (`fwu_crypto_sim.c:134`). Slow (byte by byte), test use only.
* `fwu_crypto_sim_sign`: produces the fake signature above; no argument checks (`fwu_crypto_sim.c:144`).

---

## 6. Module `fwu_verify` (verification pipeline)

Header: `src/verify/fwu_verify.h`, implementation `src/verify/fwu_verify.c`.

Purpose: verifies the manifest and image in staging in a fixed order, cheapest check first. Built on `fwu_manifest_parse` (`src/manifest/fwu_manifest.h`), `fwu_flash_read` and the crypto wrappers.

Used by: `src/boot/fwu_boot.c` (sets `boot->verify.require_signature` at line 92 and installs `fwu_verify_activation_hook` as the activation verify hook at lines 113-115), `src/session/fwu_session.c:366` (`fwu_verify_offer`), `src/counters/fwu_counters.c:114` (`fwu_verify_manifest`), `src/auth/fwu_auth.h` (uses `fwu_verify_key_t`). Depends on: `fwu_flash.h`, `fwu_manifest.h`, `fwu_crypto.h`.

### Manifest geometry used here (`src/manifest/fwu_manifest.h`)

Signed region `FWU_MANIFEST_SIGNED_SIZE` = 74 elements (magic 4, format 1, key_id 1, reserved 2, product_id 4, image_version 4, key_version 2, image_size 4, sha256 32, commit_hash 20); signature 64 elements; total `FWU_MANIFEST_SIZE` = 138.

### Types

```c
typedef struct {
    fwu_byte_t id;
    fwu_byte_t public_key[FWU_CRYPTO_ED25519_PUBLIC_KEY_SIZE];
} fwu_verify_key_t;

typedef struct {
    fwu_crypto_t           *crypto;
    uint32_t                manifest_offset;
    uint32_t                image_offset;
    uint32_t                max_image_size;
    uint32_t                product_id;
    uint32_t                min_image_version;
    uint16_t                min_key_version;
    const fwu_verify_key_t *keys;
    uint32_t                key_count;
    bool                    require_signature;
    fwu_byte_t             *work;
    uint32_t                work_length;
} fwu_verify_config_t;
```

| Field | Meaning (from header/code) |
|---|---|
| `crypto` | crypto object; its SHA-256 state is modified |
| `manifest_offset`, `image_offset` | flash offsets of the manifest (first staging page) and image |
| `max_image_size` | largest accepted image size (staging capacity) |
| `product_id` | this device's product id |
| `min_image_version` | lowest accepted image version; **equal is accepted** (`image_version < min` is rejected) |
| `min_key_version` | lowest accepted key version |
| `keys`, `key_count` | trusted public keys; `keys` may be NULL only if `key_count == 0` |
| `require_signature` | `true`: steps 6-7 enforced; `false`: skipped |
| `work`, `work_length` | read buffer for hashing, `work_length >= 1`; must remain valid during verification |

### Verification pipeline order (`fwu_verify_image`, via `verify_core`, `fwu_verify.c:112-150`)

Pre-checks (each gives `FWU_ERR_INVALID_ARG`): `config`, `config->crypto`, `flash`, `config->work` not NULL; `work_length != 0`; `keys != NULL` when `key_count > 0`; `image_offset <= 0xFFFFFFFF - max_image_size` (no 32-bit wrap).

| Step | Check | Error code returned |
|---|---|---|
| 1 | read `FWU_MANIFEST_SIZE` (138) elements from `manifest_offset` | the flash error, unchanged (not a rejection) |
| 2 | manifest structure (`fwu_manifest_parse`) | `FWU_ERR_BAD_MANIFEST` (33); also `FWU_ERR_NOT_ENOUGH_DATA` / `FWU_ERR_INVALID_ARG` are possible parse results but cannot occur here because a full-length buffer is passed |
| 3 | `manifest.product_id == config->product_id` | `FWU_ERR_PRODUCT_MISMATCH` (52) |
| 4 | `manifest.image_version >= config->min_image_version` | `FWU_ERR_VERSION_REJECTED` (53) |
| 5 | `image_size <= max_image_size` and `image_size <= available_length` (for `fwu_verify_image` `available_length == max_image_size`; for the activation hook it is the commit `length`) | `FWU_ERR_IMAGE_TOO_LARGE` (34) |
| 6 | only if `require_signature`: key with `id == manifest.key_id` exists in `keys` **and** `manifest.key_version >= min_key_version` | `FWU_ERR_KEY_REJECTED` (54) |
| 7 | only if `require_signature`: `fwu_crypto_ed25519_verify(crypto, raw, FWU_MANIFEST_SIGNED_SIZE, manifest.signature, key->public_key)` over the first 74 raw manifest elements | `FWU_ERR_BAD_SIGNATURE` (50) or `FWU_ERR_CRYPTO_FAILURE` (51), as returned by the wrapper |
| 8 | SHA-256 of `image_size` elements read from `image_offset` in `work_length` chunks, compared with `manifest.sha256` | `FWU_ERR_HASH_MISMATCH` (35); `fwu_crypto_sha256_begin/update/finish` errors and flash read errors are returned as is. On a mid-way error the digest is closed with `fwu_crypto_sha256_finish` (`fwu_verify.c:94`) |

The first failing check determines the result; later checks are not run. On success `*manifest` is written (if non-NULL); on error it is unchanged.

Classification used by activation (from `fwu_verify.h`): *unacceptable image* = `BAD_MANIFEST`, `PRODUCT_MISMATCH`, `VERSION_REJECTED`, `IMAGE_TOO_LARGE`, `KEY_REJECTED`, `BAD_SIGNATURE`, `HASH_MISMATCH`; *could not be performed* = `CRYPTO_FAILURE`, flash read errors, `INVALID_ARG`. The mapping to "REJECTED written / not written" is done in `src/activation/` (not part of this module group; not verified here).

### What the DEV (hash-only) configuration does and does not verify

`require_signature == false` is the only difference: steps 6-7 are skipped (no key needed; `keys`/`key_count` are not consulted). Everything else still runs: manifest structure, product id, image version >= `min_image_version`, image size, SHA-256 of the image against the manifest.

* **Does verify:** that the staged image is intact and matches its (unsigned) manifest, and that product/version/size policy holds.
* **Does NOT verify:** the origin or authenticity of the manifest or image. The manifest is not authenticated, so an attacker who can write to staging can supply a matching image + SHA-256. There is no key check and no signature check. Hash-only is integrity (corruption detection), not authority.
* Mapping in `src/boot`: `FWU_BOOT_VERIFY_HASH` -> `require_signature = false`; `FWU_BOOT_VERIFY_SIGNED` -> `true` (`fwu_boot.c:92`). `FWU_BOOT_VERIFY_NONE` installs **no** verify hook at all (`fwu_boot.c:113-115`), i.e. neither this pipeline nor anything else runs; the `fwu_boot.h` comment calls it "development only".
* `require_signature` is a compile-time/integration decision in the intended design (header comment); the struct itself does not enforce this.

### Functions

```c
fwu_return_e fwu_verify_image(const fwu_verify_config_t *config, const fwu_flash_t *flash, fwu_manifest_t *manifest);
```
Runs steps 1-8. Returns `FWU_OK` or any code from the table above; `FWU_ERR_INVALID_ARG` for the pre-checks. Blocking: performs flash reads and a full hash computation over the image (duration proportional to `image_size`); cooperative, no explicit yield. `src/verify/fwu_verify.c:152`.

```c
fwu_return_e fwu_verify_manifest(const fwu_verify_config_t *config, const fwu_flash_t *flash, fwu_manifest_t *manifest);
```
Verifies only the manifest: steps 1, 2, 3, then 6-7 (same codes). Skips image version, size and image hash. Uses `crypto`, `manifest_offset`, `product_id`, `min_key_version`, `keys`, `key_count`, `require_signature`; does not use `work` or the image fields. Pre-checks: `config`, `crypto`, `flash` not NULL, `keys` not NULL when `key_count > 0` (else `FWU_ERR_INVALID_ARG`). Used for the counter-raise approval so that counters rise only from a signed manifest (`fwu_counters.c:114`). `src/verify/fwu_verify.c:160`.

```c
fwu_return_e fwu_verify_offer(const fwu_verify_config_t *config, const fwu_byte_t *manifest_bytes,
                              uint32_t manifest_length, fwu_manifest_t *manifest);
```
Evaluates an offer whose manifest is in RAM: `fwu_manifest_parse(manifest_bytes, manifest_length, ...)` then steps 3-7 with `available_length = max_image_size`; no image hash and no flash access. Returns `FWU_OK`; `FWU_ERR_NOT_ENOUGH_DATA` (short manifest, from the parser), `FWU_ERR_BAD_MANIFEST`, `PRODUCT_MISMATCH`, `VERSION_REJECTED`, `IMAGE_TOO_LARGE`, `KEY_REJECTED`, `BAD_SIGNATURE`, `CRYPTO_FAILURE`; `FWU_ERR_INVALID_ARG` if `config`, `crypto` or `manifest_bytes` is NULL or `keys` is NULL with `key_count > 0`. Note: `manifest_bytes` is used directly as the signed region, so it must hold at least 74 valid elements (guaranteed by the length check in parse). Called from `src/session/fwu_session.c:366` with `FWU_MANIFEST_SIZE`. `src/verify/fwu_verify.c:189`.

```c
fwu_return_e fwu_verify_activation_hook(void *context, const fwu_flash_t *flash, uint32_t offset, uint32_t length);
```
Activation verify hook. `context` must be a `const fwu_verify_config_t *`. Returns `FWU_ERR_INVALID_ARG` if `context` or `flash` is NULL or `offset != config->image_offset`; otherwise runs the full pipeline with `available_length = length` (the area covered by the commit), so a commit that covers fewer bytes than the manifest's `image_size` gives `FWU_ERR_IMAGE_TOO_LARGE`. The parsed manifest is not returned. Installed by `src/boot/fwu_boot.c:114`. `src/verify/fwu_verify.c:215`.

---

## 7. Module `fwu_auth` (challenge-response authorization)

Header: `src/auth/fwu_auth.h`, implementation `src/auth/fwu_auth.c`.

Purpose: provides the session's authorization hook (`fwu_session_auth_t`, defined in `src/session/fwu_session.h`) on the device and the response function on the host side (matches `fwu_client_auth_fn`, `src/client/fwu_client.h`). Asymmetric: the operator holds a private key; the device holds only operator public keys. Verification uses the existing Ed25519 crypto port; consequently **with `fwu_crypto_sw` (Ed25519 stub) every authorization attempt ends in `FWU_ERR_CRYPTO_FAILURE`**, so authorization cannot succeed until a real Ed25519 port is supplied.

Used by / depends on: includes `fwu_crypto.h`, `fwu_verify.h` (for `fwu_verify_key_t`), `fwu_session.h`, `fwu_bytes.h`. No non-test call sites of the `fwu_auth` functions were found outside `src/auth/` in `src/`, `ports/`, `examples/` (UNVERIFIED: whether integrators wire it up elsewhere); the session and client only see the hook types.

### Message and response layout

Signed message built by `fwu_auth_build_message` (`fwu_auth.c:12-35`); the host signs exactly the bytes the device verifies.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | ASCII `"FWUA"` = `0x46 0x57 0x55 0x41` |
| 4 | 1 | version = `FWU_AUTH_VERSION` (1) |
| 5 | 4 | `product_id`, big endian (`fwu_write_u32_big_endian`) |
| 9 | `challenge_length` | the whole challenge: nonce (32) followed by the device id (1..16) |

Message length = 9 + `challenge_length` = 42..57 (`FWU_AUTH_MESSAGE_MAX` = 57).

Challenge (device -> host, produced by `begin`): 32-byte nonce from the RNG hook, then 1..16 bytes of device id: length `FWU_AUTH_NONCE_SIZE + device_id_length` = 33..48.

Response (host -> device, 65 bytes = `FWU_AUTH_RESPONSE_SIZE`): byte 0 = operator key id; bytes 1..64 = Ed25519 signature over the message above.

Flow: host AUTH_REQ -> device `begin` -> AUTH_CHALLENGE -> host builds message, signs with the operator key, sends response -> device `check` -> AUTH_OK or an error (the frame-level message names are defined in `src/session/`, not in this module).

### Nonce rules on the device (`auth_begin`, `fwu_auth.c:56-97`)

* If `capacity < 32 + device_id_length`: `FWU_ERR_BUFFER_TOO_SMALL`, RNG not called.
* RNG returning anything other than `FWU_OK`: `FWU_ERR_CRYPTO_FAILURE` (authorization not opened).
* Nonce all zero, or identical to the previous nonce (`have_last` true and all 32 bytes equal): `FWU_ERR_CRYPTO_FAILURE`.
* The previous nonce is kept in RAM only (`last_nonce`); an RNG that repeats its sequence after a restart is not detected (header warning). There is no software RNG fallback.

### Check rules on the device (`auth_check`, `fwu_auth.c:99-135`), in order

1. NULL or not initialised: `FWU_ERR_INVALID_ARG`.
2. `challenge_length != 32 + device_id_length`, or the device-id bytes of the challenge do not match this device: `FWU_ERR_INVALID_ARG` (this code is not listed in the header's "ERROR CODES (check)" paragraph, see report).
3. `response_length != 65`: `FWU_ERR_BAD_LENGTH` (31).
4. Operator key id not in `operator_keys`: `FWU_ERR_KEY_REJECTED` (54) (no signature computation).
5. `fwu_crypto_ed25519_verify` over the rebuilt message: `FWU_OK`, `FWU_ERR_BAD_SIGNATURE` (50) or `FWU_ERR_CRYPTO_FAILURE` (51).

The session counts all failures (header: lockout after `auth_max_failures` consecutive failures, including `CRYPTO_FAILURE`; this counting is implemented in `src/session/`, not verified here).

### Macros

| Macro | Value |
|---|---|
| `FWU_AUTH_VERSION` | `1u` |
| `FWU_AUTH_NONCE_SIZE` | `32u` |
| `FWU_AUTH_DEVICE_ID_MAX` | `16u` |
| `FWU_AUTH_CHALLENGE_MIN` | `(FWU_AUTH_NONCE_SIZE + 1u)` = 33 |
| `FWU_AUTH_CHALLENGE_MAX` | `(FWU_AUTH_NONCE_SIZE + FWU_AUTH_DEVICE_ID_MAX)` = 48 (<= `FWU_PROTO_AUTH_CHALLENGE_MAX` = 64) |
| `FWU_AUTH_MESSAGE_HEADER` | `9u` |
| `FWU_AUTH_MESSAGE_MAX` | `(FWU_AUTH_MESSAGE_HEADER + FWU_AUTH_CHALLENGE_MAX)` = 57 |
| `FWU_AUTH_RESPONSE_SIZE` | `(1u + FWU_CRYPTO_ED25519_SIGNATURE_SIZE)` = 65 |

### Types

```c
typedef fwu_return_e (*fwu_auth_rng_fn)(void *context, fwu_byte_t *out, uint32_t length);

typedef struct {
    fwu_crypto_t           *crypto;
    fwu_auth_rng_fn         rng;
    void                   *rng_context;
    const fwu_byte_t       *device_id;
    uint32_t                device_id_length;
    uint32_t                product_id;
    const fwu_verify_key_t *operator_keys;
    uint32_t                operator_key_count;
} fwu_auth_config_t;

typedef struct {
    fwu_auth_config_t    config;
    fwu_byte_t           device_id[FWU_AUTH_DEVICE_ID_MAX];
    fwu_byte_t           last_nonce[FWU_AUTH_NONCE_SIZE];
    bool                 have_last;
    bool                 initialized;
    fwu_session_auth_t   hook;
} fwu_auth_t;

typedef fwu_return_e (*fwu_auth_sign_fn)(void *context, const fwu_byte_t *message, uint32_t message_length,
                                         fwu_byte_t *key_id, fwu_byte_t *signature);

typedef struct {
    uint32_t          product_id;
    fwu_auth_sign_fn  sign;
    void             *sign_context;
} fwu_auth_host_t;
```
Notes: `fwu_auth_init` stores a copy of `config` but only a **pointer** to `operator_keys` (and `crypto`, `rng_context`), so those must outlive the object; the device id is copied into `device_id`. `fwu_auth_t` must stay at a fixed address (`hook.context` points to it). Operator keys are separate from image keys; ids must be unique.

### Functions

```c
fwu_return_e fwu_auth_build_message(uint32_t product_id, const fwu_byte_t *challenge, uint32_t challenge_length,
                                    fwu_byte_t *out, uint32_t capacity, uint32_t *out_length);
```
Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL `challenge`/`out`/`out_length`); `FWU_ERR_BAD_LENGTH` (`challenge_length` outside 33..48); `FWU_ERR_BUFFER_TOO_SMALL` (`capacity < 9 + challenge_length`). Shared by host and device. `src/auth/fwu_auth.c:12`.

```c
fwu_return_e fwu_auth_build_response(fwu_byte_t key_id, const fwu_byte_t *signature, fwu_byte_t *out, uint32_t capacity,
                                     uint32_t *out_length);
```
Writes `key_id` then 64 signature elements; `*out_length = 65`. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL pointer); `FWU_ERR_BUFFER_TOO_SMALL` (`capacity < 65`). `src/auth/fwu_auth.c:37`.

```c
fwu_return_e fwu_auth_init(fwu_auth_t *auth, const fwu_auth_config_t *config);
```
Sets `initialized = false` first, then validates. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` for NULL `auth`/`config`, NULL `crypto`/`rng`/`operator_keys`/`device_id`, `operator_key_count == 0`, `device_id_length` 0 or > 16, or two operator keys with the same `id` (the object is then not initialised). `src/auth/fwu_auth.c:137`.

```c
const fwu_session_auth_t *fwu_auth_hook(const fwu_auth_t *auth);
```
Returns `&auth->hook`, or NULL if `auth` is NULL or not initialised. The result goes into `fwu_session_config_t.auth`. `src/auth/fwu_auth.c:169`.

```c
fwu_return_e fwu_auth_host_respond(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                                   fwu_byte_t *response, uint32_t capacity, uint32_t *response_length);
```
Host side. `context` is a `const fwu_auth_host_t *`. Builds the message, calls `sign`, builds the 65-byte response. Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` (NULL `host`/`sign`/`challenge`/`response`/`response_length`); `FWU_ERR_BUFFER_TOO_SMALL` (`capacity < 65`, checked before building); `FWU_ERR_BAD_LENGTH` (challenge length outside 33..48); any non-`FWU_OK` value from `sign` unchanged. Blocking behaviour depends on the `sign` hook. `src/auth/fwu_auth.c:176`.

The device-side callbacks (`auth_begin`, `auth_check`) are `static`, reachable only through the hook returned by `fwu_auth_hook`.

---

## 8. Frame layer: `fwu_frame`, `fwu_decode`, `fwu_parser`

Headers: `src/frame/fwu_frame.h`, `src/frame/fwu_decode.h`, `src/frame/fwu_parser.h`. Implementation: `fwu_frame.c` (encode), `fwu_decode.c`, `fwu_parser.c`.

Purpose: `fwu_frame.h` is the single source of truth for the frame format; `fwu_frame_encode` builds a frame; `fwu_frame_decode` validates the frame at the start of a contiguous buffer; `fwu_parser` pulls bytes from a ring buffer into its own one-frame buffer and extracts frames from a noisy byte stream.

Used by: `src/client/fwu_client.c:30`, `src/session/fwu_session.c:35`, `src/pull/fwu_pull.c:310,326` (encode); `src/transport/fwu_link.c:85` (parser poll). Depends on: `fwu_crc.h` (`fwu_crc16_classic_calculate`), `fwu_bytes.h`, `fwu_ringbuf.h` (parser), `fwu_error.h`, `fwu_types.h`.

### Frame byte layout

All multi-byte fields are big endian (high byte first). "Byte" = one `fwu_byte_t`, low 8 bits significant.

| Offset | Field | Size | Value / meaning |
|---|---|---|---|
| 0 | SOF | 1 | `FWU_FRAME_SOF` = `0xA5` |
| 1 | VER | 1 | `FWU_FRAME_VERSION` = `1` |
| 2 | TYPE | 1 | message type (meaning defined by the upper protocol) |
| 3 | SEQ | 1 | sequence number 0..255 (wraps) |
| 4 | LEN | 2 | payload length, big endian; valid range 1..`FWU_FRAME_MAX_PAYLOAD` (512) |
| 6 | PAYLOAD | LEN | data (zero-length payload is rejected) |
| 6 + LEN | CRC | 2 | big endian, see below |
| 8 + LEN | EOF | 1 | `FWU_FRAME_EOF` = `0x5A` |

Header = 6 bytes (`FWU_FRAME_HEADER_SIZE`), trailer = 3 bytes (`FWU_FRAME_TRAILER_SIZE`), overhead = 9 bytes (`FWU_FRAME_OVERHEAD`); total frame length = 9 + LEN; largest frame `FWU_FRAME_MAX_SIZE` = 9 + 512 = 521 bytes.

CRC: `fwu_crc16_classic_calculate` = CRC-16/MODBUS: reflected polynomial `0xA001` (poly 0x8005 reflected), initial value `0xFFFF` (`FWU_CRC16_INIT`), no final XOR (`src/crc/fwu_crc.h`). **Coverage:** starts at VER (offset 1) and ends at the last payload byte, i.e. VER, TYPE, SEQ, LEN(2), PAYLOAD = 5 + LEN bytes. SOF, CRC and EOF are not covered. The 16-bit CRC value is written **high byte first** (big endian), not in the low-byte-first order commonly used on Modbus wires.

### Macros (`fwu_frame.h`)

Field sizes: `FWU_FRAME_SOF_SIZE` 1, `FWU_FRAME_VER_SIZE` 1, `FWU_FRAME_TYPE_SIZE` 1, `FWU_FRAME_SEQ_SIZE` 1, `FWU_FRAME_LEN_SIZE` 2, `FWU_FRAME_CRC_SIZE` 2, `FWU_FRAME_EOF_SIZE` 1 (all `u`-suffixed).
Offsets: `FWU_FRAME_OFFSET_SOF` 0, `FWU_FRAME_OFFSET_VER` 1, `FWU_FRAME_OFFSET_TYPE` 2, `FWU_FRAME_OFFSET_SEQ` 3, `FWU_FRAME_OFFSET_LEN` 4, `FWU_FRAME_OFFSET_PAYLOAD` 6, `FWU_FRAME_CRC_START_OFFSET` (= `FWU_FRAME_OFFSET_VER`).
Constants: `FWU_FRAME_SOF` `0xA5u`, `FWU_FRAME_EOF` `0x5Au`, `FWU_FRAME_VERSION` `1u`, `FWU_FRAME_MAX_PAYLOAD` `512u`.
Totals: `FWU_FRAME_HEADER_SIZE` (6), `FWU_FRAME_TRAILER_SIZE` (3), `FWU_FRAME_OVERHEAD` (9), `FWU_FRAME_MAX_SIZE` (521).
Parser (`fwu_parser.h`): `FWU_PARSER_FRAME_TIMEOUT_MS` `500u`, `FWU_PARSER_MAX_DROP_PER_POLL` `1024u`.

### `fwu_frame_encode`

```c
fwu_return_e fwu_frame_encode(fwu_byte_t type, fwu_byte_t seq,
                              const fwu_byte_t *payload, size_t payload_length,
                              fwu_byte_t *out, size_t out_capacity, size_t *out_length);
```
Writes SOF, VER, TYPE, SEQ, LEN, PAYLOAD, CRC, EOF to `out`; all validation happens before the first write, so `out` and `*out_length` are untouched on error. `type`, `seq` and payload bytes are masked to 8 bits. Writes exactly `9 + payload_length` elements and stores that in `*out_length`.

Returns: `FWU_OK`; `FWU_ERR_INVALID_ARG` (`out`, `out_length` or `payload` NULL, `payload_length == 0`, or `> FWU_FRAME_MAX_PAYLOAD`); `FWU_ERR_BUFFER_TOO_SMALL` (`out_capacity < payload_length + FWU_FRAME_OVERHEAD`). `payload` is only read. No blocking. `src/frame/fwu_frame.c:9`.

### `fwu_frame_view_t` and `fwu_frame_decode`

```c
typedef struct {
    fwu_byte_t        type;
    fwu_byte_t        seq;
    const fwu_byte_t *payload;
    size_t            payload_length;
    size_t            frame_length;
} fwu_frame_view_t;
```
`payload` points **into the caller's buffer** (no copy) and is valid only while that buffer is unchanged. `frame_length` is the number of bytes to consume from the stream.

```c
fwu_return_e fwu_frame_decode(const fwu_byte_t *buffer, size_t buffer_length, fwu_frame_view_t *view);
```
Validates the frame at the start of `buffer`. Checks are done in this order, each as soon as the needed bytes are present (`fwu_decode.c:18-84`):

| Order | Check | Result |
|---|---|---|
| 1 | `buffer` or `view` NULL | `FWU_ERR_INVALID_ARG` |
| 2 | if `buffer_length >= 1`: first byte `!= 0xA5` | `FWU_ERR_BAD_FRAME` |
| 3 | if `buffer_length >= 2`: VER `!= 1` | `FWU_ERR_BAD_FRAME` |
| 4 | `buffer_length < 6` (incl. 0) | `FWU_ERR_NOT_ENOUGH_DATA` |
| 5 | LEN `== 0` or `> 512` | `FWU_ERR_BAD_LENGTH` (returned immediately, without waiting for more bytes) |
| 6 | `buffer_length < 9 + LEN` | `FWU_ERR_NOT_ENOUGH_DATA` |
| 7 | last byte of the frame `!= 0x5A` | `FWU_ERR_BAD_FRAME` |
| 8 | recomputed CRC `!=` received CRC | `FWU_ERR_CRC_MISMATCH` |
| 9 | all passed: `view` written | `FWU_OK` |

`view` is written only on `FWU_OK`. Bytes beyond `frame_length` (the next frame) are ignored. Pure function, no blocking. Note that EOF is checked before the CRC. `src/frame/fwu_decode.c:18`.

### `fwu_parser`

```c
typedef struct {
    uint32_t frames_ok;
    uint32_t bytes_dropped;
    uint32_t bad_frame;
    uint32_t bad_length;
    uint32_t crc_mismatch;
    uint32_t timeouts;
} fwu_parser_stats_t;

typedef struct {
    fwu_ringbuf_t     *source;
    fwu_byte_t         buffer[FWU_FRAME_MAX_SIZE];
    size_t             count;
    size_t             delivered;
    uint32_t           head_time_ms;
    fwu_parser_stats_t stats;
} fwu_parser_t;
```
`stats` is readable; other fields are internal. Counters are 32 bit and wrap.

```c
fwu_return_e fwu_parser_init(fwu_parser_t *parser, fwu_ringbuf_t *source);
```
Clears state and counters, binds the ring buffer (must outlive the parser). Returns `FWU_OK`; `FWU_ERR_INVALID_ARG` if `parser` or `source` is NULL. `src/frame/fwu_parser.c:66`.

```c
fwu_return_e fwu_parser_poll(fwu_parser_t *parser, uint32_t now_ms, fwu_frame_view_t *view);
```
Returns `FWU_OK` (frame ready, `view` filled), `FWU_ERR_NOT_ENOUGH_DATA` (no frame to deliver now, the normal idle case), `FWU_ERR_INVALID_ARG` (NULL `parser`, `view` or `parser->source`), or an error from `fwu_ringbuf_pull` returned unchanged. `view` is written only on `FWU_OK`. `view->payload` points into the parser's internal buffer and is valid **only until the next `fwu_parser_poll` call** (the next poll removes the delivered frame first). The caller calls poll in its main loop, repeatedly until `NOT_ENOUGH_DATA`; `now_ms` is a 32-bit millisecond counter supplied by the caller (wrap-safe: `(uint32_t)(now_ms - head_time_ms)`). Not documented as ISR-safe; it reads from the ring buffer (single-consumer side). No blocking, bounded by the drop limit below. `src/frame/fwu_parser.c:85`.

#### Parser behaviour as implemented (`fwu_parser.c`)

Per poll call:

1. If a frame was delivered by the previous poll (`delivered > 0`), remove it from the buffer and set `head_time_ms = now_ms`.
2. Loop: pull as many bytes as the ring buffer has and the parser buffer has room for (`FWU_FRAME_MAX_SIZE` = 521). If the buffer was empty, `head_time_ms = now_ms`. A pull error returns immediately. If the buffer is empty: return `NOT_ENOUGH_DATA`.
3. Call `fwu_frame_decode` on the buffer:
   * `FWU_OK`: `*view` set, `delivered = frame_length`, `frames_ok++`, return `FWU_OK`.
   * `FWU_ERR_NOT_ENOUGH_DATA`: if `(uint32_t)(now_ms - head_time_ms) > 500` then `timeouts++` and resynchronise; otherwise return `NOT_ENOUGH_DATA` (wait).
   * `FWU_ERR_BAD_FRAME`, `FWU_ERR_BAD_LENGTH`, `FWU_ERR_CRC_MISMATCH`: increment the matching counter (`bad_frame`, `bad_length`, `crc_mismatch`; any other code would be counted as `crc_mismatch`) and resynchronise.
4. **Resynchronisation** (`parser_resync`): discard bytes from the start of the buffer up to, but not including, the next SOF byte (`0xA5`) found at index >= 1; if no further SOF exists the whole buffer is discarded. At least 1 byte is always dropped. `bytes_dropped` is incremented by the number discarded, and `head_time_ms = now_ms` (new timeout window). The remaining bytes (which may start a real frame) are kept and decoded again.
5. After each resync, if the bytes dropped in this poll call reach `FWU_PARSER_MAX_DROP_PER_POLL` (1024), return `FWU_ERR_NOT_ENOUGH_DATA` (call poll again).

Timeout rule: the 500 ms (`FWU_PARSER_FRAME_TIMEOUT_MS`) window is measured from `head_time_ms` (the time the current head started: first byte arrived into an empty buffer, or the last resync/delivery) and is only evaluated when decode reports an incomplete frame. A timed-out partial frame is dropped via the same resync (up to the next SOF), not by clearing the buffer.

Source-level design notes taken from the header: bytes are pulled into the parser's own buffer so the ring buffer is only consumed (no insertion, preserving the SPSC property); a SOF byte inside payload or garbage can be a false alarm, which is why resync does not discard the whole buffer.
