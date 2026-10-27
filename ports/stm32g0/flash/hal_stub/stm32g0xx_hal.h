/**
 * @file    stm32g0xx_hal.h  (PC TEST STUB, NOT the real ST HAL, not vendor code)
 * @brief   PC imitation of the small subset of the ST HAL surface (types, macros, functions) used by
 *          fwu_flash_stm32g0.c. Compiled only in tests (-I hal_stub); never built for a target. A real build uses
 *          ST's own stm32g0xx_hal.h.
 *
 * What it imitates (behaviour read from the ST HAL sources, from the third_party copy in the source repository):
 *   - HAL_FLASH_Unlock/Lock: lock state; while locked, Program/Erase return HAL_ERROR.
 *   - HAL_FLASHEx_Erase: page erase, `Banks` + page index WITHIN THE BANK `Page` + `NbPages`; on error
 *     *PageError = the faulty page.
 *   - HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address, 64-bit value): 8-byte aligned, the target double word
 *     must be ERASED (otherwise HAL_ERROR), the value is written little-endian.
 *   - FLASH_SIZE / FLASH_BANK_SIZE: values read at run time (the test changes them with `hal_stub_set_chip`).
 *   - ECC flags (ECCC/ECCD, bank 1 and 2): flags set by the test with `hal_stub_set_flag`.
 *
 * !!! These assumptions are based on a reading of the vendor sources; they are [UNVERIFIED] ON REAL SILICON. A correct
 * stub does not show that the chip behaves the same way (in particular: reading a half-programmed double word, ECC/NMI,
 * single-bank configuration). !!!
 */

#ifndef STM32G0XX_HAL_H_
#define STM32G0XX_HAL_H_

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ HAL surface (real names) */

typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03
} HAL_StatusTypeDef;

typedef struct {
    uint32_t TypeErase;
    uint32_t Banks;
    uint32_t Page;
    uint32_t NbPages;
} FLASH_EraseInitTypeDef;

#define FLASH_DBANK_SUPPORT             1
#define FLASH_BASE                      (0x08000000UL)
#define FLASH_PAGE_SIZE                 0x00000800U

uint32_t hal_stub_flash_size(void);
uint32_t hal_stub_bank_size(void);
#define FLASH_SIZE                      (hal_stub_flash_size())
#define FLASH_BANK_SIZE                 (hal_stub_bank_size())

#define FLASH_TYPEERASE_PAGES           0x00000002U
#define FLASH_BANK_1                    0x00000001U
#define FLASH_BANK_2                    0x00000002U
#define FLASH_TYPEPROGRAM_DOUBLEWORD    0x00000001U

#define FLASH_FLAG_OPTVERR              1u
#define FLASH_FLAG_ECCC1                2u
#define FLASH_FLAG_ECCD1                3u
#define FLASH_FLAG_ECCC2                4u
#define FLASH_FLAG_ECCD2                5u

bool hal_stub_get_flag(uint32_t flag);
void hal_stub_clear_flag(uint32_t flag);
#define __HAL_FLASH_GET_FLAG(__FLAG__)      (hal_stub_get_flag(__FLAG__))
#define __HAL_FLASH_CLEAR_FLAG(__FLAG__)    (hal_stub_clear_flag(__FLAG__))

HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError);

/* ------------------------------------------------------------------ TEST CONTROL API (not part of the HAL) */

#define HAL_STUB_FLASH_MAX      0x80000u        /* 512 KB */
#define HAL_STUB_ERASE_LOG_MAX  1024u

typedef struct {
    uint32_t banks;
    uint32_t page;
    uint32_t nb_pages;
} hal_stub_erase_record_t;

typedef struct {
    uint32_t unlock_calls;
    uint32_t lock_calls;
    uint32_t erase_calls;               /**< Number of HAL_FLASHEx_Erase calls. */
    uint32_t program_calls;             /**< Number of HAL_FLASH_Program calls. */
    uint32_t optverr_clears;            /**< Number of FLASH_FLAG_OPTVERR clears. */
    uint32_t last_program_address;
    uint64_t last_program_data;
    uint32_t erase_log_count;
    hal_stub_erase_record_t erase_log[HAL_STUB_ERASE_LOG_MAX];
} hal_stub_stats_t;

/** Resets everything: memory 0xFF, locked, power on, flags/faults/power cut disabled, counters zero, chip 512 KB dual bank. */
void hal_stub_reset(void);
/** Changes the chip properties (flash size <= 512 KB; dual_bank false: single bank). */
void hal_stub_set_chip(uint32_t flash_size, bool dual_bank);
/** Memory to be given to the port as its read window (HAL_STUB_FLASH_MAX elements). */
uint8_t *hal_stub_memory(void);
const hal_stub_stats_t *hal_stub_stats(void);
bool hal_stub_is_locked(void);

void hal_stub_fail_unlock(bool fail);   /**< true: HAL_FLASH_Unlock returns HAL_ERROR. */
void hal_stub_fail_erase(bool fail);    /**< true: HAL_FLASHEx_Erase returns HAL_ERROR (no effect on memory). */
void hal_stub_fail_program(bool fail);  /**< true: HAL_FLASH_Program returns HAL_ERROR (no effect on memory). */
void hal_stub_set_flag(uint32_t flag);  /**< Sets an ECC flag (imitates the hardware detecting an ECC error). */

/** Power loss: let `operations` basic operations (page erase or double-word write) complete, then leave the next one HALF
 *  done and cut power (erase: the first half of the page is erased; write: the first 4 bytes are written). After that all
 *  HAL calls return HAL_ERROR. */
void hal_stub_cut_power_after(uint32_t operations);
void hal_stub_power_on(void);           /**< Restores power (contents are kept, flash is locked, the counter is reset). */
bool hal_stub_powered(void);
uint32_t hal_stub_operations(void);     /**< Number of completed basic operations. */

#endif /* STM32G0XX_HAL_H_ */
