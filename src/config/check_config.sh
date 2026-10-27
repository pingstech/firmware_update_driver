#!/usr/bin/env bash
# fwu_config.h / fwu_layout.h profile and forbidden-combination check: valid combinations MUST compile and give the
# expected values; forbidden combinations MUST NOT compile (#error). Exit code: 0 if everything behaves as
# expected, 1 otherwise. Usage (from anywhere):  bash src/config/check_config.sh
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
fail=0

# valid: description | flags | expected verification | activation | anti-rollback
ok_case() {
  local name="$1" flags="$2" v="$3" a="$4" r="$5"
  if gcc -std=c99 -pedantic-errors -Wall -Wextra -Werror $flags -DEXPECT_VERIFY=$v -DEXPECT_ACTIVATION=$a -DEXPECT_ANTI=$r -I. -c test_config.c -o /dev/null 2>/tmp/cfg_err.txt; then
    printf "%-62s PASS (compiled, values correct)\n" "$name"
  else
    printf "%-62s FAIL (should have compiled)\n" "$name"; head -3 /tmp/cfg_err.txt; fail=1
  fi
}

# forbidden: description | flags | text expected in the error message
bad_case() {
  local name="$1" flags="$2" text="$3"
  if gcc -std=c99 $flags -I. -c test_config.c -o /dev/null 2>/tmp/cfg_err.txt; then
    printf "%-62s FAIL (should not have compiled)\n" "$name"; fail=1
  elif grep -q "$text" /tmp/cfg_err.txt; then
    printf "%-62s PASS (build rejected: %s)\n" "$name" "$text"
  else
    printf "%-62s FAIL (rejected for another reason)\n" "$name"; head -3 /tmp/cfg_err.txt; fail=1
  fi
}

# VERIFY: 1 HASH (SHA-256 digest; the macro value is historically named CRC32), 2 SIGNED   ACTIVATION: 0 COPY, 1 SWAP_ROLLBACK
# Spare pages: the journal (A/B) needs 2 in EVERY profile; swap with rollback additionally needs a scratch page (3 in total).
ok_case  "DEV + 2 spare pages (copy)"                      "-DFWU_PROFILE_DEV -DFWU_FLASH_SPARE_PAGES=2" 1 0 0
ok_case  "INDUSTRIAL (copy) + 2 spare pages"               "-DFWU_PROFILE_INDUSTRIAL -DFWU_FLASH_SPARE_PAGES=2" 2 0 0
ok_case  "INDUSTRIAL (copy) + 3 spare pages (more is allowed)" "-DFWU_PROFILE_INDUSTRIAL -DFWU_FLASH_SPARE_PAGES=3" 2 0 0
ok_case  "INDUSTRIAL + swap with rollback + 3 spare pages"    "-DFWU_PROFILE_INDUSTRIAL -DFWU_ACTIVATION_SWAP_ROLLBACK -DFWU_FLASH_SPARE_PAGES=3" 2 1 0
ok_case  "AUTOMOTIVE + 3 spare pages"                       "-DFWU_PROFILE_AUTOMOTIVE -DFWU_FLASH_SPARE_PAGES=3" 2 1 1
ok_case  "AUTOMOTIVE + 5 spare pages"                       "-DFWU_PROFILE_AUTOMOTIVE -DFWU_FLASH_SPARE_PAGES=5" 2 1 1
ok_case  "DEV + swap with rollback + 3 spare pages (allowed)" "-DFWU_PROFILE_DEV -DFWU_ACTIVATION_SWAP_ROLLBACK -DFWU_FLASH_SPARE_PAGES=3" 1 1 0

bad_case "no profile selected"                                "" "exactly one profile"
bad_case "two profiles (DEV + INDUSTRIAL)"                    "-DFWU_PROFILE_DEV -DFWU_PROFILE_INDUSTRIAL" "exactly one profile"
bad_case "three profiles at once"                                 "-DFWU_PROFILE_DEV -DFWU_PROFILE_INDUSTRIAL -DFWU_PROFILE_AUTOMOTIVE" "exactly one profile"
bad_case "DEV + production build"                           "-DFWU_PROFILE_DEV -DFWU_PRODUCTION" "production build"
bad_case "INDUSTRIAL + swap with rollback, no spare pages"   "-DFWU_PROFILE_INDUSTRIAL -DFWU_ACTIVATION_SWAP_ROLLBACK" "at least 2 spare pages"
bad_case "INDUSTRIAL + swap with rollback, 2 spare pages (no scratch page)" "-DFWU_PROFILE_INDUSTRIAL -DFWU_ACTIVATION_SWAP_ROLLBACK -DFWU_FLASH_SPARE_PAGES=2" "at least 3 spare pages"
bad_case "AUTOMOTIVE, no spare pages"                      "-DFWU_PROFILE_AUTOMOTIVE" "at least 2 spare pages"
bad_case "AUTOMOTIVE, spare pages 0 (explicit)"               "-DFWU_PROFILE_AUTOMOTIVE -DFWU_FLASH_SPARE_PAGES=0" "at least 2 spare pages"
bad_case "AUTOMOTIVE, 1 spare page"                        "-DFWU_PROFILE_AUTOMOTIVE -DFWU_FLASH_SPARE_PAGES=1" "at least 2 spare pages"
bad_case "AUTOMOTIVE, 2 spare pages (no scratch page)"           "-DFWU_PROFILE_AUTOMOTIVE -DFWU_FLASH_SPARE_PAGES=2" "at least 3 spare pages"
bad_case "DEV (copy), no spare pages: the journal is mandatory in every profile" "-DFWU_PROFILE_DEV" "at least 2 spare pages"
bad_case "INDUSTRIAL (copy), 1 spare page"                "-DFWU_PROFILE_INDUSTRIAL -DFWU_FLASH_SPARE_PAGES=1" "at least 2 spare pages"
# INDUSTRIAL + uretim derlemesi serbest (yalniz DEV yasak)
ok_case  "INDUSTRIAL + production build (allowed)"          "-DFWU_PROFILE_INDUSTRIAL -DFWU_PRODUCTION -DFWU_FLASH_SPARE_PAGES=2" 2 0 0
ok_case  "AUTOMOTIVE + production build + 3 spare pages"    "-DFWU_PROFILE_AUTOMOTIVE -DFWU_PRODUCTION -DFWU_FLASH_SPARE_PAGES=3" 2 1 1


# ---- flash layout (generic fwu_layout.h + board numbers layout_stm32g0b1.h): spare pages come from the layout ----
L="-DFWU_BOARD_STM32G0B1"
ok_case  "layout + AUTOMOTIVE (default K=2: 4 spare pages, addresses correct)"   "$L -DFWU_PROFILE_AUTOMOTIVE" 2 1 1
ok_case  "layout + INDUSTRIAL + swap with rollback"                          "$L -DFWU_PROFILE_INDUSTRIAL -DFWU_ACTIVATION_SWAP_ROLLBACK" 2 1 0
ok_case  "layout + INDUSTRIAL (copy)"                                     "$L -DFWU_PROFILE_INDUSTRIAL" 2 0 0
ok_case  "layout + DEV"                                                    "$L -DFWU_PROFILE_DEV" 1 0 0
bad_case "layout, spare pages 0 (log 16) + AUTOMOTIVE"                     "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_LOG_PAGES=16 -DFWU_BOARD_SPARE_PAGES=0" "spare page"
bad_case "layout, 1 spare page (log 15) + DEV: too few for the journal"            "$L -DFWU_PROFILE_DEV -DFWU_BOARD_LOG_PAGES=15 -DFWU_BOARD_SPARE_PAGES=1" "at least 2 spare pages"
ok_case  "layout, 2 spare pages (log 14) + INDUSTRIAL copy: scratch page undefined" "$L -DFWU_PROFILE_INDUSTRIAL -DFWU_BOARD_LOG_PAGES=14 -DFWU_BOARD_SPARE_PAGES=2 -DEXPECT_LAYOUT_SKIP_ADDRESSES -DEXPECT_NO_SCRATCH" 2 0 0
bad_case "layout, 2 spare pages (log 14) + AUTOMOTIVE: no scratch page for swap" "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_LOG_PAGES=14 -DFWU_BOARD_SPARE_PAGES=2" "at least 3 spare pages"
bad_case "layout, staging 1 page (too few for manifest + image)"            "$L -DFWU_PROFILE_DEV -DFWU_BOARD_STAGING_PAGES=1 -DFWU_BOARD_APP_PAGES=1 -DFWU_BOARD_LOG_PAGES=217" "at least 2 pages"
bad_case "layout, total is not 256 (log 17)"                       "$L -DFWU_PROFILE_DEV -DFWU_BOARD_LOG_PAGES=17" "add up to"
bad_case "layout, staging differs from application (109)"                        "$L -DFWU_PROFILE_DEV -DFWU_BOARD_STAGING_PAGES=109 -DFWU_BOARD_LOG_PAGES=13" "SAME size"
bad_case "layout, log region below the legacy usage (6)"                 "$L -DFWU_PROFILE_DEV -DFWU_BOARD_LOG_PAGES=6 -DFWU_BOARD_SPARE_PAGES=10" "below the number of pages"
bad_case "generic layout header, no board selected"                      "-DFWU_PROFILE_DEV -include fwu_layout.h" "exactly one board"
# ---- ikinci kart: TI F2800157 (kelime birimi, dogrulanmamis sayilar) ----
C="-DFWU_BOARD_F2800157"
ok_case  "F2800157 + AUTOMOTIVE (draft partition, addresses correct)"          "$C -DFWU_PROFILE_AUTOMOTIVE" 2 1 1
ok_case  "F2800157 + INDUSTRIAL (copy)"                                   "$C -DFWU_PROFILE_INDUSTRIAL" 2 0 0
ok_case  "F2800157 + DEV"                                                  "$C -DFWU_PROFILE_DEV" 1 0 0
bad_case "F2800157 + production build, numbers not verified"               "$C -DFWU_PROFILE_INDUSTRIAL -DFWU_PRODUCTION" "not been verified"
ok_case  "F2800157 + production + verified flag"                          "$C -DFWU_PROFILE_INDUSTRIAL -DFWU_PRODUCTION -DFWU_BOARD_NUMBERS_VERIFIED" 2 0 0
bad_case "two boards at once (STM32G0B1 + F2800157)"                          "-DFWU_BOARD_STM32G0B1 -DFWU_BOARD_F2800157 -DFWU_PROFILE_DEV" "exactly one board"
bad_case "F2800157, total is not 128 (log 9)"                       "$C -DFWU_PROFILE_DEV -DFWU_BOARD_LOG_PAGES=9" "add up to"
bad_case "F2800157, staging differs from application (54)"                       "$C -DFWU_PROFILE_DEV -DFWU_BOARD_STAGING_PAGES=54 -DFWU_BOARD_LOG_PAGES=6" "SAME size"
bad_case "F2800157, 2 spare pages (log 6) + AUTOMOTIVE: no scratch page for swap" "$C -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_LOG_PAGES=6 -DFWU_BOARD_SPARE_PAGES=2" "at least 3 spare pages"
bad_case "F2800157, spare pages 0 + AUTOMOTIVE"                            "$C -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_LOG_PAGES=8 -DFWU_BOARD_SPARE_PAGES=0" "spare page"
# ---- ucuncu kart: TI F280049C (LAUNCHXL, oktet birimi, 32 sektor x 4 KW) ----
D="-DFWU_BOARD_F280049C"
ok_case  "F280049C + DEV (addresses correct)"                                 "$D -DFWU_PROFILE_DEV" 1 0 0
ok_case  "F280049C + INDUSTRIAL (copy)"                                   "$D -DFWU_PROFILE_INDUSTRIAL" 2 0 0
ok_case  "F280049C + AUTOMOTIVE (swap+rollback, 3 spare pages)"           "$D -DFWU_PROFILE_AUTOMOTIVE" 2 1 1
bad_case "F280049C + production build, numbers not verified"               "$D -DFWU_PROFILE_INDUSTRIAL -DFWU_PRODUCTION" "not been verified"
ok_case  "F280049C + production + verified flag"                          "$D -DFWU_PROFILE_INDUSTRIAL -DFWU_PRODUCTION -DFWU_BOARD_NUMBERS_VERIFIED" 2 0 0
bad_case "two boards at once (F2800157 + F280049C)"                           "-DFWU_BOARD_F2800157 -DFWU_BOARD_F280049C -DFWU_PROFILE_DEV" "exactly one board"
bad_case "F280049C, total is not 32 (log 6)"                       "$D -DFWU_PROFILE_DEV -DFWU_BOARD_LOG_PAGES=6" "add up to"
bad_case "F280049C, staging differs from application (8)"                        "$D -DFWU_PROFILE_DEV -DFWU_BOARD_STAGING_PAGES=8 -DFWU_BOARD_LOG_PAGES=6" "SAME size"
# layout values customised but valid (4 spare pages): address checks are skipped, config is still correct
ok_case  "custom layout: 4 spare pages, log 12 + AUTOMOTIVE"                 "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_LOG_PAGES=12 -DFWU_BOARD_SPARE_PAGES=4 -DEXPECT_LAYOUT_SKIP_ADDRESSES" 2 1 1
# scratch region = all spare pages left after the 2 journal pages: 3 spare -> 1, 4 -> 2, 10 -> 8 scratch pages
ok_case  "scratch page count: default STM32 (4 spare) -> 2"             "$L -DFWU_PROFILE_AUTOMOTIVE -DEXPECT_SCRATCH_PAGES=2" 2 1 1
ok_case  "scratch page count: 4 spare -> 2"                              "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_LOG_PAGES=12 -DFWU_BOARD_SPARE_PAGES=4 -DEXPECT_LAYOUT_SKIP_ADDRESSES -DEXPECT_SCRATCH_PAGES=2" 2 1 1
ok_case  "scratch page count: 9 spare (log 7) -> 7"                      "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_LOG_PAGES=7 -DFWU_BOARD_SPARE_PAGES=9 -DEXPECT_LAYOUT_SKIP_ADDRESSES -DEXPECT_SCRATCH_PAGES=7" 2 1 1
ok_case  "F2800157: scratch page count 3 spare -> 1"                     "-DFWU_BOARD_F2800157 -DFWU_PROFILE_AUTOMOTIVE -DEXPECT_SCRATCH_PAGES=1" 2 1 1

# K = FWU_BOARD_SCRATCH_PAGES: spare = 2 + K, log/config = remainder (not kept in sync by hand)
ok_case  "K=1 (spare 3, log 13 derived)"                                "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_SCRATCH_PAGES=1 -DEXPECT_LAYOUT_SKIP_ADDRESSES -DEXPECT_SCRATCH_PAGES=1" 2 1 1
ok_case  "K=4 (spare 6, log 10 derived)"                                "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_SCRATCH_PAGES=4 -DEXPECT_LAYOUT_SKIP_ADDRESSES -DEXPECT_SCRATCH_PAGES=4" 2 1 1
ok_case  "K=7 (log exactly at the lower limit: 7 pages)"                             "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_SCRATCH_PAGES=7 -DEXPECT_LAYOUT_SKIP_ADDRESSES -DEXPECT_SCRATCH_PAGES=7" 2 1 1
bad_case "K=8 (log drops to 6 pages, below the legacy usage)"              "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_SCRATCH_PAGES=8" "below the number of pages"
ok_case  "K=0 + INDUSTRIAL copy (no scratch area, journal present)"           "$L -DFWU_PROFILE_INDUSTRIAL -DFWU_BOARD_SCRATCH_PAGES=0 -DEXPECT_LAYOUT_SKIP_ADDRESSES -DEXPECT_NO_SCRATCH" 2 0 0
bad_case "K=0 + AUTOMOTIVE (swap needs a scratch area)"                     "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_SCRATCH_PAGES=0" "at least 3 spare pages"
ok_case  "K and spare may be given together if consistent (K=2, spare 4, log 12)" "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_SCRATCH_PAGES=2 -DFWU_BOARD_SPARE_PAGES=4 -DEXPECT_SCRATCH_PAGES=2" 2 1 1
bad_case "K and spare conflict (K=2 but spare 5)"                         "$L -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_SCRATCH_PAGES=2 -DFWU_BOARD_SPARE_PAGES=5" "conflict"
ok_case  "F2800157: K=2 (spare 4, log 4 derived)"                       "$C -DFWU_PROFILE_AUTOMOTIVE -DFWU_BOARD_SCRATCH_PAGES=2 -DEXPECT_LAYOUT_SKIP_ADDRESSES -DEXPECT_SCRATCH_PAGES=2" 2 1 1

# ---- counter region (G7d): must be exactly 2 pages (A/B) and is part of the total ----
bad_case "counter region 3 pages (log 11)"                                 "$L -DFWU_PROFILE_DEV -DFWU_BOARD_COUNTER_PAGES=3 -DFWU_BOARD_LOG_PAGES=11" "exactly 2 pages"
bad_case "counter region 1 page (log 13)"                                 "$L -DFWU_PROFILE_DEV -DFWU_BOARD_COUNTER_PAGES=1 -DFWU_BOARD_LOG_PAGES=13" "exactly 2 pages"
bad_case "counter region not included in the sum (log 14 by hand): total does not match"     "$L -DFWU_PROFILE_DEV -DFWU_BOARD_LOG_PAGES=14" "add up to"
bad_case "F2800157, counter region 3 sectors"                               "$C -DFWU_PROFILE_DEV -DFWU_BOARD_COUNTER_PAGES=3 -DFWU_BOARD_LOG_PAGES=3" "exactly 2 pages"

echo
if [ "$fail" -eq 0 ]; then echo "PROFILE CHECKS PASSED"; else echo "PROFILE CHECKS FAILED"; fi
exit "$fail"
