# Security policy

## Reporting a vulnerability

Please do **not** open a public issue for a suspected vulnerability.

- If private vulnerability reporting is enabled for this repository on the hosting platform, use it.
- Otherwise contact the maintainer by e-mail at `furkan.yayla.dev@gmail.com` and include: affected files or modules,
  the configuration (protection profile, target), steps to reproduce, and the impact you observed.

No response-time commitment or support organisation is implied by this document.

## Scope and current security posture (read before relying on the driver)

The security properties actually implemented in this repository are limited. See `docs/KNOWN_ISSUES.md` for the full list.

- **Integrity check:** the DEV profile verifies a SHA-256 digest of the image against the manifest. This detects
  accidental corruption. It does **not** authenticate the sender: whoever can supply both manifest and image can supply a
  matching pair.
- **Signatures:** the manifest has a signature field and the verification pipeline supports signed profiles, but the
  Ed25519 verification hook in `src/crypto/fwu_crypto_sw.c` always fails closed. Signed profiles therefore do not work
  with the software crypto port shipped here (open issue 37).
- **Simulated crypto:** `src/crypto/fwu_crypto_sim.*` is for tests only and is intentionally insecure.
- **Authorization (challenge-response):** implemented in `src/auth/` and covered by host tests, but a random number source,
  operator key management and device identity source are integration responsibilities that are not provided (open issues
  68-70).
- **Hardware:** flash write protection, debug-port lock-down and secure boot of the bootloader itself are outside this
  repository.

Treat the DEV profile as a development configuration only. `src/config/fwu_config.h` refuses to build DEV together with
`FWU_PRODUCTION` for the reason above; verify this against the header before shipping.
