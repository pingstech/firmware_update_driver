#!/usr/bin/env bash
# Compile-only check of the STM32G0 flash port, the core files used by a bootloader and the STM32 bootloader example with GNU Arm.
# Nothing is linked and nothing is run: this only shows that the sources compile against the real ST HAL/CMSIS headers.
#
# Required environment (no defaults, because these are NOT part of this repository):
#   ARM_GCC_BIN      directory containing arm-none-eabi-gcc (e.g. the one shipped with STM32CubeIDE)
#   STM32_PROJECT    an STM32CubeIDE project for the STM32G0B1 that provides Core/Inc/main.h, Core/Inc/stm32g0xx_hal_conf.h and
#                    Drivers/{STM32G0xx_HAL_Driver,CMSIS} (see examples/stm32g0b1/README.md)
# Optional:
#   OUT_DIR          directory for objects and logs (default: a temporary directory)
#
# Usage:  ARM_GCC_BIN=... STM32_PROJECT=... bash tests/target_builds/arm_gcc_compile_check.sh
# Note: paths containing spaces are not supported by the include handling below; use a space-free copy of the repository if needed.
set -u
: "${ARM_GCC_BIN:?set ARM_GCC_BIN}"
: "${STM32_PROJECT:?set STM32_PROJECT}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$(mktemp -d)}"
mkdir -p "$OUT_DIR"
GCC="$ARM_GCC_BIN/arm-none-eabi-gcc"
[ -x "$GCC" ] || GCC="$ARM_GCC_BIN/arm-none-eabi-gcc.exe"
INCS=(-I"$REPO/examples/stm32g0b1/bootloader" -I"$REPO/src" -I"$REPO/src/activation" -I"$REPO/src/boot" -I"$REPO/src/bytes" -I"$REPO/src/config"
      -I"$REPO/src/counters" -I"$REPO/src/crc" -I"$REPO/src/crypto" -I"$REPO/src/error" -I"$REPO/src/flash" -I"$REPO/src/journal"
      -I"$REPO/src/manifest" -I"$REPO/src/verify" -I"$REPO/ports/stm32g0/flash"
      -I"$STM32_PROJECT/Core/Inc" -I"$STM32_PROJECT/Drivers/STM32G0xx_HAL_Driver/Inc" -I"$STM32_PROJECT/Drivers/STM32G0xx_HAL_Driver/Inc/Legacy"
      -I"$STM32_PROJECT/Drivers/CMSIS/Device/ST/STM32G0xx/Include" -I"$STM32_PROJECT/Drivers/CMSIS/Include")
FILES="src/activation/fwu_activate.c src/boot/fwu_boot.c src/counters/fwu_counters.c src/crypto/fwu_crypto.c src/crypto/fwu_crypto_sw.c
 src/crypto/fwu_sha256.c src/flash/fwu_flash.c src/journal/fwu_journal.c src/journal/fwu_journal_ab.c src/manifest/fwu_manifest.c
 src/verify/fwu_verify.c ports/stm32g0/flash/fwu_flash_stm32g0.c examples/stm32g0b1/bootloader/boot_app.c"
fail=0
for f in $FILES; do
  b=$(basename "$f" .c)
  if "$GCC" "$REPO/$f" -mcpu=cortex-m0plus -std=gnu11 -DUSE_HAL_DRIVER -DSTM32G0B1xx -c "${INCS[@]}" -Os -ffunction-sections -fdata-sections \
        -Wall -Wextra -mfloat-abi=soft -mthumb -o "$OUT_DIR/$b.o" 2> "$OUT_DIR/$b.log"; then
    echo "OK   $b"
  else
    echo "FAIL $b"; head -4 "$OUT_DIR/$b.log"; fail=1
  fi
done
echo "warnings: $(cat "$OUT_DIR"/*.log | grep -c 'warning:')"
"$GCC" --version | head -1
SIZE="$ARM_GCC_BIN/arm-none-eabi-size"; [ -x "$SIZE" ] || SIZE="$ARM_GCC_BIN/arm-none-eabi-size.exe"
"$SIZE" -t "$OUT_DIR"/*.o | tail -16
exit $fail
