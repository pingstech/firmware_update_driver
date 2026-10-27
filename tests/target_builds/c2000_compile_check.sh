#!/usr/bin/env bash
# Compile-only check of the C2000 flash ports and of the C2000 LaunchPad example (application and bootloader) with the TI C2000 compiler.
# Nothing is linked and nothing is run. The compiler options are those of the maintainer's CCS project (configuration CPU1_FLASH):
# application -Ooff, bootloader -O2 (see examples/c2000_launchxl_f280049c/README.md, section 2.2).
#
# Required environment (none of these is part of this repository):
#   TI_CGT       TI C2000 code generation tools directory (contains bin/cl2000)
#   C2000WARE    TI C2000Ware directory
#   DEVICE_DIR   directory with TI's device.h / device.c (the 'device' folder of a CCS empty driverlib project)
#   SYSCFG_DIR   directory with the SysConfig-generated board.h / c2000ware_libraries.h (generated from c2000.syscfg by CCS)
#   FC41D_DIR    root of the FC41D module driver (contains FC41D_api.h and INC/); needed only for the application
#   FC41D_PARENT parent directory used by the FC41D driver's own includes (may equal FC41D_DIR's parent)
# Optional: OUT_DIR (default: a temporary directory).
#
# Usage:  TI_CGT=... C2000WARE=... DEVICE_DIR=... SYSCFG_DIR=... FC41D_DIR=... FC41D_PARENT=... bash tests/target_builds/c2000_compile_check.sh
# Paths containing spaces are not supported by the option handling below.
set -u
: "${TI_CGT:?}"; : "${C2000WARE:?}"; : "${DEVICE_DIR:?}"; : "${SYSCFG_DIR:?}"; : "${FC41D_DIR:?}"; : "${FC41D_PARENT:?}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${OUT_DIR:-$(mktemp -d)}"
mkdir -p "$OUT/app" "$OUT/boot"
CL="$TI_CGT/bin/cl2000"; [ -x "$CL" ] || CL="$CL.exe"
COMMON="-v28 -ml -mt --cla_support=cla2 --float_support=fpu32 --tmu_support=tmu0 --vcu_support=vcu0 --c99 --abi=eabi --gen_func_subsections=on --diag_suppress=10063 --diag_warning=225 --diag_wrap=off --display_error_number --define=C2000 --define=FWU_BOARD_F280049C --define=FWU_PROFILE_DEV --define=DEBUG --define=_FLASH"
SRCI="--include_path=$REPO/src --include_path=$REPO/src/flash --include_path=$REPO/src/error --include_path=$REPO/src/bytes --include_path=$REPO/src/crc --include_path=$REPO/src/journal --include_path=$REPO/src/activation --include_path=$REPO/src/manifest --include_path=$REPO/src/crypto --include_path=$REPO/src/verify --include_path=$REPO/src/counters --include_path=$REPO/src/boot --include_path=$REPO/src/config"
APPI="--include_path=$REPO/src/ring_buffer --include_path=$REPO/src/frame --include_path=$REPO/src/session --include_path=$REPO/src/transport --include_path=$REPO/src/pull"
PORTI="--include_path=$REPO/ports/c2000/f28004x"
TII="--include_path=$C2000WARE/libraries/flash_api/f28004x/include/FlashAPI --include_path=$C2000WARE --include_path=$DEVICE_DIR --include_path=$C2000WARE/driverlib/f28004x/driverlib --include_path=$SYSCFG_DIR --include_path=$TI_CGT/include"
fail=0
cc() { # label optlevel outdir extra-includes file...
  local label="$1" opt="$2" od="$3" inc="$4"; shift 4
  for f in "$@"; do
    b=$(basename "$f" .c)
    if "$CL" $COMMON $opt $SRCI $inc $TII --compile_only --output_file="$od/$b.obj" "$f" > "$od/$b.log" 2>&1; then
      echo "OK   [$label] $f $(grep -c -i 'warning' "$od/$b.log") warning(s)"
    else
      echo "FAIL [$label] $f"; head -6 "$od/$b.log"; fail=1
    fi
  done
}
CORE_BOOT="src/activation/fwu_activate.c src/boot/fwu_boot.c src/counters/fwu_counters.c src/crypto/fwu_crypto.c src/crypto/fwu_crypto_sw.c src/crypto/fwu_sha256.c src/flash/fwu_flash.c src/journal/fwu_journal.c src/journal/fwu_journal_ab.c src/manifest/fwu_manifest.c src/verify/fwu_verify.c"
CORE_APP="src/frame/fwu_decode.c src/frame/fwu_frame.c src/frame/fwu_parser.c src/ring_buffer/fwu_ringbuf.c src/session/fwu_proto.c src/session/fwu_session.c src/transport/fwu_link.c src/pull/fwu_pull.c src/pull/fwu_http.c"
EX=examples/c2000_launchxl_f280049c
cd "$REPO" || exit 2
A_INC="$APPI $PORTI --include_path=$REPO/$EX/application/port --include_path=$REPO/$EX/application/flash_port --include_path=$FC41D_DIR --include_path=$FC41D_DIR/INC --include_path=$FC41D_PARENT"
cc app -Ooff "$OUT/app" "$A_INC" $CORE_BOOT $CORE_APP ports/c2000/f28004x/flash_port.c ports/c2000/f28004x/fwu_flash_adapter.c $EX/application/port/app_fwu.c $EX/application/port/app_ble.c $EX/application/port/app_pull.c $EX/application/port/app_tick.c $EX/application/port/app_sci.c $EX/application/main.c
B_INC="$PORTI --include_path=$REPO/$EX/bootloader/port --include_path=$REPO/$EX/bootloader/flash_port"
cc boot -O2 "$OUT/boot" "$B_INC" $CORE_BOOT ports/c2000/f28004x/flash_port.c ports/c2000/f28004x/fwu_flash_adapter.c $EX/bootloader/main.c
cc direct -O2 "$OUT/boot" "--include_path=$REPO/ports/c2000/f28004x_direct" ports/c2000/f28004x_direct/fwu_flash_f28004x.c
echo "compiler revision: $("$CL" --compiler_revision 2>&1 | head -1)"
echo "exit=$fail"
exit $fail
