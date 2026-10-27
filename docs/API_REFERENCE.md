# API reference

The public interface is documented per module group, with the exact declarations copied from the headers, parameter and return-value
semantics confirmed in the `.c` files, call ordering, blocking behaviour, and `file:line` references. **Source of truth is the code**: the documents were
written against the repository state of 2026-10-26; line numbers drift, function names do not. Every `fwu_*` / `FWU_*` identifier used in the
module documents was checked to exist in the repository.

The message formats and sequences, independent of the C API, are described in [`PROTOCOL.md`](PROTOCOL.md).

| Document | Modules |
|---|---|
| [`api/session-and-basics.md`](api/session-and-basics.md) | `fwu_types.h`, **`fwu_error.h`** (all status codes), `fwu_bytes.h`, `fwu_crc.h`, **`fwu_proto`** (message types and payload layouts), **`fwu_session`** (state machine) |
| [`api/transport-client.md`](api/transport-client.md) | `fwu_transport.h` (transport contract), **`fwu_link`**, `fwu_isotp`, `fwu_linksim`, `fwu_client` (host side) |
| [`api/pull-flash-ringbuffer.md`](api/pull-flash-ringbuffer.md) | **`fwu_flash`** (flash port contract), `fwu_flash_sim`, `fwu_ringbuf`, `fwu_http`, **`fwu_pull`** |
| [`api/activation-boot-counters.md`](api/activation-boot-counters.md) | **`fwu_activate`**, **`fwu_boot`** (bootloader core), `fwu_boot_profile.h`, `fwu_counters` |
| [`api/config-journal-manifest.md`](api/config-journal-manifest.md) | `fwu_config.h` (profiles), `fwu_layout.h` and the `layout_*.h` board headers, `fwu_journal`, `fwu_journal_ab`, `fwu_manifest` |
| [`api/crypto-verify-auth-frame.md`](api/crypto-verify-auth-frame.md) | `fwu_crypto`, `fwu_crypto_sw`, `fwu_sha256`, `fwu_crypto_sim` (tests only), `fwu_verify`, `fwu_auth`, `fwu_frame`, `fwu_decode`, `fwu_parser` |
| [`api/ports.md`](api/ports.md) | `ports/stm32g0`, `ports/c2000/f28004x`, `ports/c2000/f28004x_direct`, and the STM32 bootloader example glue |

The example integration glue (`examples/c2000_launchxl_f280049c/.../app_*.h`) is described in the example's
[README](../examples/c2000_launchxl_f280049c/README.md); it is application code, not a stable API.

## Typical call order

| Role | Order (see [`INTEGRATION_GUIDE.md`](INTEGRATION_GUIDE.md) for code) |
|---|---|
| Bootloader | flash port init -> `fwu_crypto_sw_init` -> fill `fwu_boot_config_t` -> `fwu_boot_start` -> jump or stay |
| Application, setup | flash port init -> `fwu_crypto_sw_init` -> `fwu_boot_init` -> ring buffer init -> `fwu_parser_init` -> `fwu_session_init` -> `fwu_link_init` (-> `fwu_pull_init` + `fwu_link_set_extension` for pull mode) |
| Application, run | bytes into the ring buffer from the ISR; `fwu_link_poll(now_ms)` from the main loop; reset when `fwu_link_reboot_ready()` is true |
| Host (C client) | `fwu_client_init` -> `fwu_client_start` -> loop `fwu_client_poll` / `fwu_client_on_frame` until `fwu_client_is_finished` -> `fwu_client_get_result` |

All fallible functions return `fwu_return_e` (`FWU_OK` = 0). Protocol-level failures of the session are not C error returns: they are `ERROR`
frames sent to the host while the function returns `FWU_OK` (details in the session document, section 7.5).

## Function index

Generated from the headers on 2026-10-26 (every function name declared in a public header, without comments, macros or types). Use it to find the
module document that declares a function.

| Header | Functions declared (names only; declarations and semantics in the module document) |
|---|---|
| `src/bytes/fwu_bytes.h` | `fwu_write_u16_big_endian`, `fwu_write_u16_little_endian`, `fwu_read_u16_big_endian`, `fwu_read_u16_little_endian`, `fwu_write_u32_big_endian`, `fwu_write_u32_little_endian`, `fwu_read_u32_big_endian`, `fwu_read_u32_little_endian` |
| `src/crc/fwu_crc.h` | `fwu_crc16_calculate_w_start_value`, `fwu_crc16_classic_calculate` |
| `src/ring_buffer/fwu_ringbuf.h` | `fwu_ringbuf_init_raw`, `fwu_ringbuf_push`, `fwu_ringbuf_pull`, `fwu_ringbuf_clear`, `fwu_ringbuf_is_empty`, `fwu_ringbuf_is_full`, `fwu_ringbuf_used_size`, `fwu_ringbuf_free_size` |
| `src/frame/fwu_decode.h` | `fwu_frame_decode` |
| `src/frame/fwu_frame.h` | `fwu_frame_encode` |
| `src/frame/fwu_parser.h` | `fwu_parser_init`, `fwu_parser_poll` |
| `src/session/fwu_proto.h` | `fwu_proto_encode_info`, `fwu_proto_parse_info`, `fwu_proto_encode_accepted`, `fwu_proto_parse_accepted`, `fwu_proto_encode_u32`, `fwu_proto_parse_u32`, `fwu_proto_encode_data`, `fwu_proto_parse_data`, `fwu_proto_encode_result`, `fwu_proto_parse_result`, `fwu_proto_encode_error`, `fwu_proto_parse_error`, `fwu_proto_encode_pull_start`, `fwu_proto_parse_pull_start`, `fwu_proto_encode_pull_status`, `fwu_proto_parse_pull_status` |
| `src/session/fwu_session.h` | `fwu_session_init`, `fwu_session_on_frame`, `fwu_session_poll`, `fwu_session_push_chunk`, `fwu_session_touch`, `fwu_session_get_image_size`, `fwu_session_get_next_offset`, `fwu_session_get_chunk_size`, `fwu_session_get_state`, `fwu_session_reboot_requested` |
| `src/transport/fwu_isotp.h` | `fwu_isotp_init`, `fwu_isotp_send`, `fwu_isotp_on_can_frame`, `fwu_isotp_poll`, `fwu_isotp_tx_busy`, `fwu_isotp_get_stats`, `fwu_isotp_transport_send`, `fwu_isotp_transport_poll` |
| `src/transport/fwu_link.h` | `fwu_link_init`, `fwu_link_set_extension`, `fwu_link_poll`, `fwu_link_reboot_ready`, `fwu_link_tx_pending`, `fwu_link_get_stats` |
| `src/transport/fwu_linksim.h` | `fwu_linksim_init`, `fwu_linksim_set_profile`, `fwu_linksim_send`, `fwu_linksim_receive`, `fwu_linksim_in_flight` |
| `src/client/fwu_client.h` | `fwu_client_init`, `fwu_client_start`, `fwu_client_cancel`, `fwu_client_poll`, `fwu_client_on_frame`, `fwu_client_is_finished`, `fwu_client_get_state`, `fwu_client_get_result` |
| `src/flash/fwu_flash.h` | `fwu_flash_validate`, `fwu_flash_erase`, `fwu_flash_write`, `fwu_flash_read` |
| `src/flash/fwu_flash_sim.h` | `fwu_flash_sim_init_raw`, `fwu_flash_sim_cut_power_after`, `fwu_flash_sim_reset`, `fwu_flash_sim_operations`, `fwu_flash_sim_is_powered`, `fwu_flash_sim_get_erase_count` |
| `src/journal/fwu_journal.h` | `fwu_journal_init`, `fwu_journal_scan`, `fwu_journal_append`, `fwu_journal_get_last` |
| `src/journal/fwu_journal_ab.h` | `fwu_journal_ab_init`, `fwu_journal_ab_scan`, `fwu_journal_ab_append`, `fwu_journal_ab_get_last` |
| `src/activation/fwu_activate.h` | `fwu_activate_init`, `fwu_activate_get_phase`, `fwu_activate_staging_writable`, `fwu_activate_commit`, `fwu_activate_run`, `fwu_activate_confirm`, `fwu_activate_request_revert` |
| `src/manifest/fwu_manifest.h` | `fwu_manifest_parse` |
| `src/crypto/fwu_crypto.h` | `fwu_crypto_init`, `fwu_crypto_sha256_begin`, `fwu_crypto_sha256_update`, `fwu_crypto_sha256_finish`, `fwu_crypto_ed25519_verify` |
| `src/crypto/fwu_crypto_sim.h` | `fwu_crypto_sim_init`, `fwu_crypto_sim_sha256`, `fwu_crypto_sim_sign` |
| `src/crypto/fwu_crypto_sw.h` | `fwu_crypto_sw_init` |
| `src/crypto/fwu_sha256.h` | `fwu_sha256_init`, `fwu_sha256_update`, `fwu_sha256_final` |
| `src/verify/fwu_verify.h` | `fwu_verify_image`, `fwu_verify_manifest`, `fwu_verify_offer`, `fwu_verify_activation_hook` |
| `src/counters/fwu_counters.h` | `fwu_counters_init`, `fwu_counters_reload`, `fwu_counters_get`, `fwu_counters_raise`, `fwu_counters_on_confirmed` |
| `src/boot/fwu_boot.h` | `fwu_boot_init`, `fwu_boot_start`, `fwu_boot_app_finish_trial`, `fwu_boot_activate`, `fwu_boot_verify_config`, `fwu_boot_fingerprint`, `fwu_boot_info_encode`, `fwu_boot_info_check` |
| `src/boot/fwu_boot_profile.h` | `fwu_boot_policy_from_profile`, `fwu_boot_layout_from_board` |
| `src/auth/fwu_auth.h` | `fwu_auth_build_message`, `fwu_auth_build_response`, `fwu_auth_init`, `fwu_auth_hook`, `fwu_auth_host_respond` |
| `src/pull/fwu_http.h` | `fwu_http_build_range_request`, `fwu_http_parser_init`, `fwu_http_feed`, `fwu_http_response`, `fwu_http_complete`, `fwu_http_error`, `fwu_http_headers_done` |
| `src/pull/fwu_pull.h` | `fwu_pull_init`, `fwu_pull_handles`, `fwu_pull_on_frame`, `fwu_pull_set_link`, `fwu_pull_feed`, `fwu_pull_poll`, `fwu_pull_abort`, `fwu_pull_get_status`, `fwu_pull_get_stats`, `fwu_pull_active`, `fwu_pull_input_free`, `fwu_pull_input_used` |
| `ports/stm32g0/flash/fwu_flash_stm32g0.h` | `fwu_flash_stm32g0_init`, `fwu_flash_stm32g0_init_mcu`, `fwu_flash_stm32g0_handle_ecc_nmi` |
| `ports/c2000/f28004x/fwu_flash_adapter.h` | `fwu_flash_adapter_init` |
| `ports/c2000/f28004x_direct/fwu_flash_f28004x.h` | `fwu_flash_f28004x_init`, `fwu_flash_f28004x_hw_init`, `fwu_flash_f28004x_init_mcu` |
