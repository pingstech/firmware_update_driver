/**
 * @file    fwu_flash_stm32g0.h
 * @brief   STM32G0B1 FLASH PORT: implements the fwu_flash.h interface on top of the ST HAL (stm32g0xx_hal_flash*.c).
 *          The core (journal, activation, ...) knows nothing about this file; it only receives the `port.flash` object.
 *
 * HARDWARE STATUS: the port logic is tested on a PC with a stub that imitates the behaviour of the ST HAL (hal_stub/) and it
 * was written according to the behaviour read from the ST HAL source (stm32g0xx_hal_flash.c / _ex.c). The source also compiles
 * with ARM GCC against the real ST HAL headers (see docs/TEST_RESULTS.md). Use on a real device is MAINTAINER-REPORTED: a
 * firmware update whose bootloader used this port completed on an STM32G0B1-based device (host log
 * docs/evidence/stm32g0b1_tcp_update_log.csv). No record in this repository covers the flash-level behaviour under power loss or the
 * ECC/NMI path, so the stub's assumptions must still be verified on the real chip (see "FIRST THINGS TO VERIFY ON HARDWARE").
 *
 * GEOMETRY (from ST HAL macros): page 2 KB (FLASH_PAGE_SIZE), write granule 8 bytes (double word, 64 bit),
 * erased value 0xFF. Offsets are bytes from the START of flash (FLASH_BASE = 0x08000000) (fwu_byte_t = 1 byte).
 *
 * FUNCTIONS
 *   erase : page by page HAL_FLASHEx_Erase (1 page per call). The bank and the page number within the bank are computed
 *           from the absolute address with HAL macros (FLASH_BASE, FLASH_BANK_SIZE, FLASH_PAGE_SIZE): if address >=
 *           FLASH_BASE + FLASH_BANK_SIZE it is bank 2. (The HAL reads this at run time from the chip's bank
 *           configuration: in a single-bank configuration this condition is never true and everything is treated as
 *           bank 1, consistent with the HAL.)
 *   write : HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD) for each 8-byte granule. The 8 bytes are converted to a
 *           64-bit value in little endian order (byte 0 = least significant byte; the Cortex-M0+ is little endian, so the
 *           byte order in memory is preserved).
 *           BEFORE writing, the target is checked to be erased (0xFF): if not, FWU_ERR_FLASH_NOT_ERASED, the HAL is NOT
 *           CALLED (so that no hardware programming error flag is raised).
 *   read  : memory-mapped read (no HAL needed). ECC flags are counted and cleared (see below).
 *   Every erase/write call unlocks the flash (HAL_FLASH_Unlock), clears the option byte validation error flag (OPTVERR)
 *   (the earlier project's flash_api.c does the same before erasing), performs the operation and locks in EVERY CASE
 *   (HAL_FLASH_Lock).
 *
 * ERRORS: erase error FWU_ERR_FLASH_ERASE, write/unlock error FWU_ERR_FLASH_WRITE, target not erased
 * FWU_ERR_FLASH_NOT_ERASED; alignment/range errors are caught by the core's wrappers (fwu_flash_erase/write/read)
 * before the port is called. A read returns no error.
 *
 * WATCHDOG: erasing one page takes milliseconds (an image of 110 pages takes hundreds of ms to seconds). The optional
 * `keepalive` function is called BEFORE EVERY page erase and BEFORE EVERY double-word write: to feed the watchdog.
 *
 * !!! ECC AND POWER LOSS (THE MOST IMPORTANT HARDWARE RISK) [UNVERIFIED] !!!
 *   The STM32G0B1 flash has ECC (per 64 bits, per bank an ECCC correction and an ECCD double error detection flag).
 *   If power is lost while a double word is being programmed, the cells may be left inconsistent; READING that address
 *   may produce a double error detection and (to be verified against RM0444) may lead to an NMI. The journal scan READS
 *   a record that was left half-written by the power loss. The earlier project's NMI_Handler calls NVIC_SystemReset():
 *   when a half-written record is read, the device may reset on every power-up and enter a BOOT LOOP (because the
 *   record will always stay at the same place). MITIGATION: the bootloader's NMI_Handler must first call
 *   fwu_flash_stm32g0_handle_ecc_nmi(); if it returns true (the cause was a flash ECC detection, the flags were cleared)
 *   it must return WITHOUT resetting. What the data read will be after returning is [UNVERIFIED] (the journal and the
 *   manifest must defend against any garbage with magic/CRC/signature). Until this behaviour has been tried on hardware
 *   with a REAL interrupted write, power loss resilience is NOT considered GUARANTEED on hardware.
 *
 * FIRST THINGS TO VERIFY ON HARDWARE (in order)
 *   1. After init, erase one page, read it (all 0xFF), write 8 bytes, read back (byte order), write to the same place
 *      again (FLASH_NOT_ERASED must be returned).
 *   2. Bank boundary: erase page 127 and page 128 (0x0803F800 and 0x08040000): verify that both are erased.
 *   3. Time to erase one page and the total time of a 110-page erase; together with the watchdog.
 *   4. Power loss: cut power during a write, then read that address: do an NMI/ECCD occur? (the risk above)
 *   5. Chip size: if FLASH_SIZE (F_SIZE register) is smaller than the layout, init returns FWU_ERR_FLASH_GEOMETRY
 *      (128/256 KB parts).
 *   6. If the single-bank configuration (DBANK option bit) is used, the erase page/bank mapping.
 */

#ifndef FWU_FLASH_STM32G0_H_
#define FWU_FLASH_STM32G0_H_

#include <stdbool.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Optional hook for feeding the watchdog (may be NULL). */
typedef void (*fwu_flash_stm32g0_keepalive_fn)(void *context);

/** State of the STM32G0 flash port. Give the `flash` field to the core; do not touch the rest (the counters may be read). */
typedef struct {
    fwu_flash_t                      flash;               /**< Interface object (give this to the core). */
    fwu_flash_callback_t             callback;            /**< Function table. */
    const volatile fwu_byte_t       *memory;              /**< Read window (MCU: flash base; PC test: fake memory). */
    fwu_flash_stm32g0_keepalive_fn   keepalive;           /**< Watchdog hook (may be NULL). */
    void                            *keepalive_context;   /**< Data passed back to the hook. */
    uint32_t                         ecc_corrected;       /**< Number of ECC CORRECTIONS seen during reads (flag count). */
    uint32_t                         ecc_detected;        /**< Number of ECC DOUBLE ERROR DETECTIONS seen during reads. */
} fwu_flash_stm32g0_t;

/**
 * @brief  Initialises the port, fills in the geometry and validates it with fwu_flash_validate.
 * @param  port              Port to initialise (must not be NULL).
 * @param  flash_length      Flash length managed by the core (bytes; fwu_layout: FWU_BOARD_TOTAL_PAGES * FWU_BOARD_PAGE_SIZE).
 *                           Must be a multiple of the page size and must not exceed the ACTUAL chip size (FLASH_SIZE).
 * @param  memory            Memory-mapped view of the flash (on the MCU the FLASH_BASE address; init_mcu does this).
 * @param  keepalive         Watchdog hook or NULL.
 * @param  keepalive_context Data passed back to the hook.
 * @return FWU_OK; FWU_ERR_INVALID_ARG if port or memory is NULL; FWU_ERR_FLASH_GEOMETRY if flash_length is 0, not a
 *         multiple of the page size, or larger than the chip.
 */
fwu_return_e fwu_flash_stm32g0_init(fwu_flash_stm32g0_t *port, uint32_t flash_length, const volatile fwu_byte_t *memory,
                                    fwu_flash_stm32g0_keepalive_fn keepalive, void *keepalive_context);

/**
 * @brief  Shortcut for the MCU: passes the FLASH_BASE address as `memory` (the integer-to-pointer conversion is here, a
 *         MISRA 11.4 deviation). Not used in the PC test.
 */
fwu_return_e fwu_flash_stm32g0_init_mcu(fwu_flash_stm32g0_t *port, uint32_t flash_length,
                                        fwu_flash_stm32g0_keepalive_fn keepalive, void *keepalive_context);

/**
 * @brief  Helper for the NMI handler: if the cause of the NMI was a flash ECC double error detection, clears the flags,
 *         increments `ecc_detected` and returns true (the handler must return WITHOUT resetting). Otherwise returns false
 *         (the NMI has another cause: the handler applies its own policy, e.g. reset).
 * @param  port  May be NULL (only looks at the flags, the counter is not incremented).
 */
bool fwu_flash_stm32g0_handle_ecc_nmi(fwu_flash_stm32g0_t *port);

#ifdef __cplusplus
}
#endif

#endif /* FWU_FLASH_STM32G0_H_ */
