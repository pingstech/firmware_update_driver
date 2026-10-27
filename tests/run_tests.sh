#!/usr/bin/env bash
# fwu_git_repo: compiles and runs all module tests.
#
# Every module is tried in 2 modes and at 2 optimisation levels:
#   mode : normal  and  -DC2000 (emulates the 16-bit char type of the C28x on the PC)
#   opt  : -O0 and -O2 (undefined behaviour usually shows up only at -O2)
# A strict warning set is used and EVERY warning is an error (-Werror).
# In addition, GCC static analysis (-fanalyzer) is applied to the module sources.
#
# Usage (from anywhere):   bash tests/run_tests.sh
# Exit code: 0 if everything passes, 1 otherwise.
# Note: the stress test (two threads) is separate: see the header of src/ring_buffer/stress_ringbuf.c.
# Output markers: PASS / FAIL, BUILD-FAILED, SKIPPED, WARNING, VIOLATION. Individual test descriptions
# inside the module tests are still partly in Turkish (see docs/TESTING.md).

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$REPO/src"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

WARN="-std=c99 -pedantic-errors -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion \
-Wshadow -Wundef -Wcast-qual -Wcast-align -Wstrict-prototypes -Wmissing-prototypes \
-Wmissing-declarations -Wredundant-decls -Wold-style-definition -Wnull-dereference -Wvla \
-Wformat=2 -Wswitch-default -Wswitch-enum -Wdouble-promotion -Wfloat-equal -Werror"
INC="-I. -I.. -I../error -I../bytes -I../crc -I../ring_buffer -I../flash -I../journal -I../activation -I../manifest -I../crypto -I../verify -I../counters -I../frame -I../session -I../transport -I../client -I../boot -I../auth -I../pull"

# name | directory | sources compiled for the test | module sources for static analysis | (optional) extra compiler flags
MODULES=(
  "crc|src/crc|test_crc16.c|"
  "bytes|src/bytes|test_bytes.c|"
  "error|src/error|test_error.c|"
  "ring_buffer|src/ring_buffer|fwu_ringbuf.c test_ringbuf.c|fwu_ringbuf.c"
  "flash|src/flash|fwu_flash.c flash_test.c|fwu_flash.c"
  "flash_sim|src/flash|fwu_flash.c fwu_flash_sim.c test_flash_sim.c|fwu_flash_sim.c"
  "frame|src/frame|fwu_frame.c test_frame.c|fwu_frame.c"
  "frame_decode|src/frame|fwu_decode.c fwu_frame.c test_decode.c|fwu_decode.c"
  "frame_parser|src/frame|fwu_parser.c fwu_decode.c fwu_frame.c ../ring_buffer/fwu_ringbuf.c test_parser.c|fwu_parser.c"
  "journal|src/journal|fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_journal.c|fwu_journal.c"
  "journal_ab|src/journal|fwu_journal_ab.c fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_journal_ab.c|fwu_journal_ab.c"
  "activate|src/activation|fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_activate.c|fwu_activate.c"
  "manifest|src/manifest|fwu_manifest.c test_manifest.c|fwu_manifest.c"
  "crypto|src/crypto|fwu_crypto.c test_crypto.c|fwu_crypto.c"
  "sha256|src/crypto|fwu_sha256.c fwu_crypto_sw.c fwu_crypto.c fwu_crypto_sim.c test_sha256.c|fwu_sha256.c fwu_crypto_sw.c"
  "crypto_sim|src/crypto|fwu_crypto.c fwu_crypto_sim.c test_crypto_sim.c|fwu_crypto_sim.c"
  "flash_stm32g0|ports/stm32g0/flash|fwu_flash_stm32g0.c hal_stub/hal_stub.c ../../../src/flash/fwu_flash.c ../../../src/journal/fwu_journal.c ../../../src/journal/fwu_journal_ab.c ../../../src/activation/fwu_activate.c test_stm32g0_port.c|fwu_flash_stm32g0.c|-DFWU_BOARD_STM32G0B1 -Ihal_stub -I../../../src -I../../../src/error -I../../../src/bytes -I../../../src/crc -I../../../src/journal -I../../../src/activation -I../../../src/config -I../../../src/flash"
  "flash_f28004x|ports/c2000/f28004x_direct|fwu_flash_f28004x.c hal_stub/hal_stub.c ../../../src/flash/fwu_flash.c ../../../src/journal/fwu_journal.c ../../../src/journal/fwu_journal_ab.c ../../../src/activation/fwu_activate.c test_f28004x_port.c|fwu_flash_f28004x.c|-DFWU_BOARD_F280049C -Ihal_stub -I../../../src -I../../../src/error -I../../../src/bytes -I../../../src/crc -I../../../src/journal -I../../../src/activation -I../../../src/config -I../../../src/flash"
  "verify|src/verify|fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_verify.c|fwu_verify.c"
  "session_proto|src/session|fwu_proto.c test_proto.c|fwu_proto.c"
  "session|src/session|fwu_session.c fwu_proto.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c ../frame/fwu_frame.c ../frame/fwu_decode.c ../frame/fwu_parser.c ../ring_buffer/fwu_ringbuf.c test_session.c|fwu_session.c"
  "pull_http|src/pull|fwu_http.c test_http.c|fwu_http.c"
  "pull|src/pull|fwu_pull.c fwu_http.c ../client/fwu_client.c ../session/fwu_session.c ../session/fwu_proto.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c ../frame/fwu_frame.c ../frame/fwu_decode.c ../ring_buffer/fwu_ringbuf.c test_pull.c|fwu_pull.c fwu_http.c|-DFWU_PULL_INPUT_SIZE=256u"
  "counters|src/counters|fwu_counters.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_counters.c|fwu_counters.c"
  "linksim|src/transport|fwu_linksim.c test_linksim.c|fwu_linksim.c"
  "isotp|src/transport|fwu_isotp.c test_isotp.c|fwu_isotp.c"
  "link|src/transport|fwu_link.c ../session/fwu_session.c ../session/fwu_proto.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c ../frame/fwu_frame.c ../frame/fwu_decode.c ../frame/fwu_parser.c ../ring_buffer/fwu_ringbuf.c test_link.c|fwu_link.c"
  "client|src/client|fwu_client.c ../transport/fwu_link.c ../transport/fwu_linksim.c ../transport/fwu_isotp.c ../session/fwu_session.c ../session/fwu_proto.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c ../frame/fwu_frame.c ../frame/fwu_decode.c ../frame/fwu_parser.c ../ring_buffer/fwu_ringbuf.c test_client.c|fwu_client.c"
  "auth|src/auth|fwu_auth.c ../client/fwu_client.c ../transport/fwu_link.c ../transport/fwu_linksim.c ../session/fwu_session.c ../session/fwu_proto.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c ../frame/fwu_frame.c ../frame/fwu_decode.c ../frame/fwu_parser.c ../ring_buffer/fwu_ringbuf.c test_auth.c|fwu_auth.c"
  "boot|src/boot|fwu_boot.c ../counters/fwu_counters.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_boot.c|fwu_boot.c"
  "boot_profile_dev|src/boot|test_boot_profile.c||-DFWU_BOARD_STM32G0B1 -I../config -DFWU_PROFILE_DEV -DEXPECT_VERIFY=1 -DEXPECT_MODE=0 -DEXPECT_AR=0"
  "boot_profile_industrial|src/boot|test_boot_profile.c||-DFWU_BOARD_STM32G0B1 -I../config -DFWU_PROFILE_INDUSTRIAL -DEXPECT_VERIFY=2 -DEXPECT_MODE=0 -DEXPECT_AR=0"
  "boot_profile_ind_swap|src/boot|test_boot_profile.c||-DFWU_BOARD_STM32G0B1 -I../config -DFWU_PROFILE_INDUSTRIAL -DFWU_ACTIVATION_SWAP_ROLLBACK -DEXPECT_VERIFY=2 -DEXPECT_MODE=1 -DEXPECT_AR=0"
  "boot_profile_automotive|src/boot|test_boot_profile.c||-DFWU_BOARD_STM32G0B1 -I../config -DFWU_PROFILE_AUTOMOTIVE -DEXPECT_VERIFY=2 -DEXPECT_MODE=1 -DEXPECT_AR=1"
)

fail=0
printf "%-12s %-8s %-4s %s\n" "MODULE" "MODE" "OPT" "RESULT"
for entry in "${MODULES[@]}"; do
  IFS='|' read -r name dir sources analyze extra <<< "$entry"
  for mode in "" "-DC2000"; do
    # The STM32 port is STM32-specific (fwu_byte_t = 8 bit, ST HAL stub): the -DC2000 emulation is meaningless there.
    if [ "$mode" = "-DC2000" ] && [ "${name#flash_stm32g0}" != "$name" ]; then
      printf "%-12s %-8s %-4s SKIPPED  (STM32-specific port)\n" "$name" "-DC2000" "-"; continue
    fi
    for opt in "-O0" "-O2"; do
      exe="$BUILD/${name}${mode}${opt}.exe"
      if ! log=$(cd "$REPO/$dir" && gcc $WARN $mode $opt $INC $extra $sources -o "$exe" 2>&1); then
        printf "%-12s %-8s %-4s BUILD-FAILED\n%s\n" "$name" "${mode:-normal}" "$opt" "$log"; fail=1; continue
      fi
      if (cd "$REPO/$dir" && "$exe" > "$BUILD/out.txt" 2>&1); then
        printf "%-12s %-8s %-4s PASS  (%s; %s checks)\n" "$name" "${mode:-normal}" "$opt" "$(grep -E 'Result' "$BUILD/out.txt" | tail -1)" "$(grep -c '^\[PASS\]' "$BUILD/out.txt")"
      else
        printf "%-12s %-8s %-4s FAIL\n" "$name" "${mode:-normal}" "$opt"; grep '^\[FAIL\]' "$BUILD/out.txt" | head -5; fail=1
      fi
    done
  done
  for src in $analyze; do
    if log=$(cd "$REPO/$dir" && gcc -fanalyzer -O1 -std=c99 $INC $extra -c "$src" -o "$BUILD/an.o" 2>&1) && ! grep -q "warning" <<< "$log"; then
      printf "%-12s %-8s %-4s PASS  (static analysis: no warnings)\n" "$name" "-" "-fanalyzer"
    else
      printf "%-12s %-8s %-4s WARNING\n%s\n" "$name" "-" "-fanalyzer" "$log"; fail=1
    fi
  done
done

# Protection profile (config/fwu_config.h): valid combinations must compile, forbidden ones must be rejected.
if "$ROOT/config/check_config.sh" > "$BUILD/cfg.txt" 2>&1; then
  printf "%-12s %-8s %-4s PASS  (profile and forbidden-combination checks: %s)
" "config" "-" "-" "$(grep -c 'PASS (' "$BUILD/cfg.txt") combinations"
else
  printf "%-12s %-8s %-4s FAIL
" "config" "-" "-"; grep FAIL "$BUILD/cfg.txt" | head -5; fail=1
fi

# Coding policy: fwu_ringbuf_init_raw (an internal function that takes the size by hand) may only be used in the
# ring buffer module itself and in its test. Production code must use FWU_RINGBUF_DEFINE (this structurally
# prevents a mismatch between size and array).
violations=$(grep -rn "fwu_ringbuf_init_raw" "$REPO/src" "$REPO/ports" "$REPO/examples" --include=*.c --include=*.h \
             | grep -v -E "/ring_buffer/(fwu_ringbuf\.(c|h)|test_ringbuf\.c):" || true)
if [ -z "$violations" ]; then
  printf "%-12s %-8s %-4s PASS  (policy: fwu_ringbuf_init_raw only in allowed files)\n" "policy" "-" "-"
else
  printf "%-12s %-8s %-4s VIOLATION: fwu_ringbuf_init_raw must not be used in production code, use FWU_RINGBUF_DEFINE\n%s\n" "policy" "-" "-" "$violations"; fail=1
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL MODULES PASSED"; else echo "FAILURES"; fi
exit "$fail"
