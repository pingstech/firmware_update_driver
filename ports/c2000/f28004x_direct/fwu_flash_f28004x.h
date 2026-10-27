/**
 * @file    fwu_flash_f28004x.h
 * @brief   TI F28004x (LAUNCHXL-F280049C) FLASH PORT: implements the fwu_flash.h interface on top of the TI Flash API (F021_API_F28004x).
 *          The core (journal, activation, session...) knows nothing about this file; it only receives the `port.flash` object.
 *
 * !!! NOT TESTED ON HARDWARE !!!  This port was tested on a PC with a stub that imitates the behaviour of the TI Flash API (hal_stub/)
 * and was written according to the official example in C2000Ware 6.00.01.00 (driverlib/f28004x/examples/flash/flashapi_ex1_program_autoecc.c)
 * and the behaviour read from the Flash API headers. The stub's assumptions must be verified on the real chip.
 *
 * UNIT AND ADDRESSING (same as config/layout_f280049c.h): fwu_byte_t = one OCTET (0..255). Flash is addressed in 16-bit WORDS and
 * one word carries TWO octets: octet 2k = low byte of the word, octet 2k+1 = high byte (little endian; same as the hex2000 --binary
 * output). For an octet offset `o` the word address = 0x080000 + o / 2.
 *   - page (erase unit, sector) = 4 KW = 8192 octets
 *   - write granule = 16 octets = 8 words = 128 bit (the largest chunk Fapi_issueProgrammingCommand accepts in one call;
 *     64-bit aligned, ECC is generated for every 64 bits, each 64 bits are programmed ONLY ONCE per erase)
 *   - erased value 0xFF (word 0xFFFF)
 *
 * FUNCTIONS
 *   erase : sector by sector Fapi_issueAsyncCommandWithAddress(Fapi_EraseSector, ...); when finished FMSTAT is checked and the
 *           sector is checked for blank with Fapi_doBlankCheck. Bank selection (0x080000 / 0x090000) is not needed: according to
 *           the TI example, once Fapi_setActiveFlashBank is called, both banks and the FMC are prepared together (init does this).
 *   write : for each 16-octet granule, first the TARGET is checked to be erased (0xFFFF) (if not FWU_ERR_FLASH_NOT_ERASED,
 *           the Flash API is NOT CALLED), then Fapi_issueProgrammingCommand(Fapi_AutoEccGeneration, 8 words), wait until the FSM
 *           is ready, FMSTAT is checked and the written value is verified with Fapi_doVerifyBy16bits.
 *   read  : memory-mapped read (the Flash API is not needed).
 *
 * RUNNING FROM RAM (MANDATORY): the Flash API and this port's erase/write functions must run from RAM (`.TI.ramfunc`); fetching
 * instructions from or reading data from the bank being programmed is forbidden. This file marks the relevant functions with
 * `#pragma CODE_SECTION(..., ".TI.ramfunc")`; the linker script must copy the `.TI.ramfunc` group from flash to RAM (Device_init does
 * this) and must put the F021 library in the same group (C2000Ware 28004x_flash_api_lnk.cmd example). Interrupts are disabled (INTM)
 * during erase/write and restored to the previous state at the end: if an interrupt handler were in flash it would access the bank
 * being programmed and hang.
 *
 * WATCHDOG: erase/write call the optional `keepalive` function BEFORE every sector and every granule (it is called from RAM
 * but while the FSM is idle: so calling a function in flash is safe). The watchdog is NOT fed while WAITING for erase/write: the
 * application/bootloader must disable the watchdog or configure it long enough (it is disabled in these projects).
 *
 * !!! ECC AND POWER LOSS [UNVERIFIED] !!!  The flash has ECC. If power is lost during programming, reading a half-written 64-bit group
 * may cause an ECC error (single: corrected; double: detected); whether this goes to an NMI on the C28x and what value is read
 * have not been tried on hardware. The journal and the manifest must defend against any garbage with magic/CRC. Power loss
 * resilience is NOT considered GUARANTEED until it has been tried on hardware.
 *
 * FIRST THINGS TO VERIFY ON HARDWARE (in order)
 *   1. After init, erase one sector, read it (all 0xFF), write 16 octets, read back (octet order), write to the same place again
 *      (FLASH_NOT_ERASED must be returned).
 *   2. Bank boundary: erase sector 15 and sector 16 (0x08F000 and 0x090000): both must be erased.
 *   3. Time to erase one sector (upper bound in the TI datasheet) and the watchdog/interrupt behaviour during it.
 *   4. Power loss: cut power during a write, then read that address: does an ECC error/NMI occur?
 *   5. Flash wait states (Flash_initModule, DEVICE_FLASH_WAITSTATES) and the prefetch workaround (Flash_Disable/EnablePrefetch_SW_Workaround
 *      in the example) are applied in this port's init through the `prefetch` hook.
 */

#ifndef FWU_FLASH_F28004X_H_
#define FWU_FLASH_F28004X_H_

#include <stdbool.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FWU_F28004X_FLASH_WORD_BASE     0x00080000u     /**< flash base, WORD address (same as layout_f280049c.h; checked in the test) */
#define FWU_F28004X_SECTOR_WORDS        0x1000u         /**< sector = 4 KW */
#define FWU_F28004X_PAGE_OCTETS         8192u           /**< erase unit, octets */
#define FWU_F28004X_GRANULE_OCTETS      16u             /**< write granule, octets (8 words) */
#define FWU_F28004X_FLASH_OCTETS        262144u         /**< 256 KB */

/** Optional hook for feeding the watchdog (may be NULL). */
typedef void (*fwu_flash_f28004x_keepalive_fn)(void *context);

/** Hook that turns prefetch off and on during the Flash API init (Flash_Disable/EnablePrefetch_SW_Workaround in the TI example), may be NULL.
 *  enable=false: called BEFORE Fapi_setActiveFlashBank, enable=true: AFTER. Must run from RAM. */
typedef void (*fwu_flash_f28004x_prefetch_fn)(bool enable);

/** State of the F28004x flash port. Give the `flash` field to the core; do not touch the rest. */
typedef struct {
    fwu_flash_t                      flash;               /**< Interface object (give this to the core). */
    fwu_flash_callback_t             callback;            /**< Function table. */
    const volatile uint16_t         *memory;              /**< Read window: memory[0] = word 0x080000 (MCU: flash base; PC test: fake memory). */
    fwu_flash_f28004x_keepalive_fn   keepalive;           /**< Watchdog hook (may be NULL). */
    void                            *keepalive_context;   /**< Data passed back to the hook. */
} fwu_flash_f28004x_t;

/**
 * @brief  Initialises the port, fills in the geometry and validates it with fwu_flash_validate. Does NOT start the Flash API (see hw_init).
 * @param  port         Port to initialise (must not be NULL).
 * @param  flash_length Flash length managed by the core (OCTETS); a multiple of the page size (8192), at most 262144.
 * @param  memory       Memory-mapped view of the flash (memory[0] = word address 0x080000); must not be NULL.
 * @return FWU_OK; FWU_ERR_INVALID_ARG if port or memory is NULL; FWU_ERR_FLASH_GEOMETRY if flash_length is 0, not a multiple of
 *         the page size, or larger than 256 KB.
 */
fwu_return_e fwu_flash_f28004x_init(fwu_flash_f28004x_t *port, uint32_t flash_length, const volatile uint16_t *memory,
                                    fwu_flash_f28004x_keepalive_fn keepalive, void *keepalive_context);

/**
 * @brief  Starts the TI Flash API: Fapi_initializeAPI(F021_CPU0_BASE_ADDRESS, hclk_mhz) and Fapi_setActiveFlashBank(Bank0)
 *         (one call prepares both banks). The wait state and ECC setting (Flash_initModule) must have been done in Device_init.
 *         Must run from RAM (it calls the Flash API).
 * @param  hclk_mhz  System clock in MHz (LAUNCHXL: 100).
 * @param  prefetch  Optional prefetch hook (may be NULL).
 * @return FWU_OK; FWU_ERR_FLASH_GEOMETRY for a Flash API error (could not be started).
 */
fwu_return_e fwu_flash_f28004x_hw_init(uint32_t hclk_mhz, fwu_flash_f28004x_prefetch_fn prefetch);

/** MCU shortcut: hw_init + init, passes the flash base (0x080000) as `memory`. Not used in the PC test. */
fwu_return_e fwu_flash_f28004x_init_mcu(fwu_flash_f28004x_t *port, uint32_t flash_length, uint32_t hclk_mhz,
                                        fwu_flash_f28004x_prefetch_fn prefetch,
                                        fwu_flash_f28004x_keepalive_fn keepalive, void *keepalive_context);

#ifdef __cplusplus
}
#endif

#endif /* FWU_FLASH_F28004X_H_ */
