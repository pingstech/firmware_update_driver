/**
 * @file    fwu_flash_sim.h
 * @brief   SIMULATED FLASH: implements the fwu_flash.h interface in RAM, ENFORCES the rules of real
 *          flash and can inject power loss. For TEST/PC use only.
 *
 * Enforced rules:
 *   1. Erase is page based; the erased area becomes erased_value (0xFF).
 *   2. Writes are only allowed to erased areas; if the WHOLE target is not erased,
 *      FWU_ERR_FLASH_NOT_ERASED is returned and nothing changes. This is deliberately STRICTER
 *      than real hardware (1 -> 0 AND rule): so the core does not fall into the "overwrite" trap.
 *   3. Write offset/length must be a multiple of the granule, erase offset/length a multiple of the page,
 *      and the request must be inside the region. (The wrappers already check these; the simulated flash
 *      also defends itself, so a bug in the wrapper is caught here.)
 *   4. Operations can be interrupted (power loss model below).
 *
 * POWER LOSS MODEL
 *   Basic operation unit: programming one write granule or erasing one page.
 *   Reads do not count. After a call to fwu_flash_sim_cut_power_after(sim, n), n basic operations
 *   complete normally; the (n+1)th operation is INTERRUPTED:
 *     - Interrupted erase : the FIRST HALF of the page is erased, the second half keeps the old data.
 *                           (The erase counter still increments: the wear happened.)
 *     - Interrupted write : the FIRST HALF of the granule is written, the second half stays erased.
 *   This is only ONE of many possible outcomes of an interruption (a deterministic choice; on real
 *   hardware the value of the interrupted cells is undefined). The core MUST NOT rely on this pattern;
 *   the pattern only makes the fact that "a partial result occurs" reproducible in tests.
 *   After the interruption ALL operations (including reads) return FWU_ERR_FLASH_POWER_LOSS;
 *   fwu_flash_sim_reset() restores power, memory contents are preserved.
 *
 * Memory: there is NO dynamic memory. Storage is embedded in the caller's variable by the
 * FWU_FLASH_SIM_DEFINE macro (same idea as FWU_RINGBUF_DEFINE). If a large geometry is chosen,
 * the variable must be `static` (it may not fit on the stack).
 *
 * Usage:
 *   FWU_FLASH_SIM_DEFINE(my_flash, 4, 2048, 8);     // 4 pages x 2048, granule 8
 *   static my_flash_t g_sim;
 *   my_flash_init(&g_sim);                          // calls fwu_flash_validate internally
 *   fwu_flash_write(&g_sim.base.flash, 0u, 8u, data);
 */

#ifndef FWU_FLASH_SIM_H_
#define FWU_FLASH_SIM_H_

#include <stdbool.h>
#include <stdint.h>
#include "fwu_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Working state of the simulated flash. Give the `flash` field to the core/tests. */
typedef struct {
    fwu_flash_t  flash;             /**< Interface (callback table, context = this struct, geometry). */
    fwu_byte_t  *memory;            /**< Flash contents (flash_length elements). */
    uint32_t    *erase_counts;      /**< Erase count per page (page_count elements). */
    uint32_t     page_count;        /**< Number of pages. */
    uint32_t     operations_done;   /**< Number of completed basic operations (reset to zero by reset). */
    uint32_t     cut_after;         /**< While cut_armed: cut the next operation after this many operations complete. */
    bool         cut_armed;         /**< Whether a power loss is armed. */
    bool         powered;           /**< Whether power is present (false after the interruption). */
} fwu_flash_sim_t;

/**
 * @brief  Initializes with manually supplied storage (INTERNAL: code uses FWU_FLASH_SIM_DEFINE).
 *         Puts the flash into the erased state, fills in the `flash` field and validates with fwu_flash_validate.
 * @param  memory        page_count * page_length elements.
 * @param  erase_counts  page_count elements.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL or zero values; FWU_ERR_FLASH_GEOMETRY for page_length % write_granule != 0
 *         or size overflow.
 */
fwu_return_e fwu_flash_sim_init_raw(fwu_flash_sim_t *sim, fwu_byte_t *memory,
                                    uint32_t *erase_counts, uint32_t page_count,
                                    uint32_t page_length, uint32_t write_granule);

/** Arms the power loss: `operations` basic operations complete normally, the next one is interrupted. */
void fwu_flash_sim_cut_power_after(fwu_flash_sim_t *sim, uint32_t operations);

/** Restores power, removes the power-loss setting and resets the operation counter. Contents are PRESERVED. */
void fwu_flash_sim_reset(fwu_flash_sim_t *sim);

/** Number of completed basic operations (to find the total operation count in a power-loss sweep). */
uint32_t fwu_flash_sim_operations(const fwu_flash_sim_t *sim);

/** Is power present? (false after the interruption, true after reset) */
bool fwu_flash_sim_is_powered(const fwu_flash_sim_t *sim);

/** Returns the erase count of a page. @return FWU_ERR_INVALID_ARG: NULL or invalid page. */
fwu_return_e fwu_flash_sim_get_erase_count(const fwu_flash_sim_t *sim, uint32_t page,
                                           uint32_t *count);

/**
 * @brief Defines a simulated flash type: storage is embedded in the variable. The geometry is validated
 *        at compile time.
 * @note  Deviation from MISRA C:2012 Dir 4.9 (function-like macro): required to generate a type.
 */
#define FWU_FLASH_SIM_DEFINE(name, pages, page_bytes, granule)                                   \
    typedef struct {                                                                              \
        fwu_flash_sim_t base;                                                                     \
        fwu_byte_t      memory[(pages) * (page_bytes)];                                           \
        uint32_t        erase_counts[(pages)];                                                    \
    } name##_t;                                                                                   \
    static inline fwu_return_e name##_init(name##_t *self)                                        \
    {                                                                                             \
        return fwu_flash_sim_init_raw(&self->base, self->memory, self->erase_counts,              \
                                      (uint32_t)(pages), (uint32_t)(page_bytes),                  \
                                      (uint32_t)(granule));                                       \
    }                                                                                             \
    typedef char name##_geometry_is_valid[(((pages) > 0) && ((page_bytes) > 0) &&                 \
                                           ((granule) > 0) &&                                     \
                                           (((page_bytes) % (granule)) == 0)) ? 1 : -1]

#ifdef __cplusplus
}
#endif

#endif /* FWU_FLASH_SIM_H_ */
