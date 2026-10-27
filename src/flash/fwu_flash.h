#ifndef FWU_FLASH_H_
#define FWU_FLASH_H_

#include <stdint.h>
#include "fwu_types.h"
#include "fwu_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    fwu_return_e (*flash_erase) (void *context, uint32_t offset, uint32_t length);
    fwu_return_e (*flash_write) (void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data);
    fwu_return_e (*flash_read)  (void *context, uint32_t offset, uint32_t length, fwu_byte_t *data);
} fwu_flash_callback_t;

typedef struct
{
    uint32_t   page_length;         /**< Size of one page (unit: number of fwu_byte_t). Erase is done in this unit. */
    uint32_t   flash_length;        /**< Total size of the flash region (unit: number of fwu_byte_t). */
    uint32_t   write_granule;       /**< Smallest writable piece (unit: number of fwu_byte_t). Write offset and length must be multiples of it. */
    fwu_byte_t erased_value;        /**< Value left in every cell after an "erase" call (usually 0xFF). */
} fwu_flash_geometry_t;

typedef struct {
    void                       *context;   /**< Address of the port's own data (e.g. base address). The core does not look inside; it only hands it back to the port functions. Must stay valid as long as the flash is used. May be NULL (if the port keeps no state). */
    const fwu_flash_callback_t *callback;  /**< Address of the port's function table (erase/write/read). Must not be NULL. Shared by all flashes of the same port type. */
    fwu_flash_geometry_t       geometry;   /**< Properties of this flash region (size, page, write unit, erased value). Filled in by the port; the core assumes nothing. */
} fwu_flash_t;


/**
 * @brief  Validates the geometry and function table of a flash object.
 *         Called by the port after it fills in the object, BEFORE first use. If an invalid object
 *         is used, the wrappers may suffer silent corruption such as division by zero.
 *
 *         What is validated:
 *           1. self, self->callback and the three function pointers (flash_erase/write/read) are not NULL.
 *           2. flash_length, page_length and write_granule are greater than zero.
 *           3. flash_length is a multiple of page_length.
 *           4. page_length is a multiple of write_granule.
 *         (3 and 4 together also guarantee that flash_length is a multiple of write_granule.)
 * @param  self  Flash object to validate (must not be NULL, not modified).
 * @return FWU_OK; FWU_ERR_INVALID_ARG if condition 1 fails (NULL); FWU_ERR_FLASH_GEOMETRY if condition 2, 3
 *         or 4 fails (inconsistent geometry).
 *         On error the object must not be used (no update must be started).
 */
fwu_return_e fwu_flash_validate(const fwu_flash_t *self);

/**
 * @brief  Erases a flash region page by page. The core accesses flash only through these wrappers.
 * @param  self    Flash object (must not be NULL).
 * @param  offset  Offset from the start of the region, unit: fwu_byte_t. Must be a multiple of page_length.
 * @param  length  Length to erase, unit: fwu_byte_t. Must not be 0, must be a multiple of page_length.
 * @return FWU_OK; FWU_ERR_INVALID_ARG (NULL object/table/function, length == 0);
 *         FWU_ERR_OUT_OF_RANGE (outside the region); FWU_ERR_ERASE_ALIGNMENT (misaligned);
 *         or a hardware error returned by the port. On error the port is not called (except for a port error).
 *         Check order: invalid argument, then range, then alignment. A request that is both out of range and
 *         misaligned returns FWU_ERR_OUT_OF_RANGE (out of range is reported before the alignment error).
 */
fwu_return_e fwu_flash_erase(const fwu_flash_t *self, uint32_t offset, uint32_t length);

/**
 * @brief  Writes to a flash region. The target area must have been erased beforehand.
 * @param  self    Flash object (must not be NULL).
 * @param  offset  Offset from the start of the region, unit: fwu_byte_t. Must be a multiple of write_granule.
 * @param  length  Length to write, unit: fwu_byte_t. Must not be 0, must be a multiple of write_granule.
 * @param  data    Data to write (must not be NULL, at least length elements). Read, not modified.
 * @return FWU_OK; FWU_ERR_INVALID_ARG (NULL object/table/function/data, length == 0);
 *         FWU_ERR_OUT_OF_RANGE (outside the region); FWU_ERR_WRITE_ALIGNMENT (misaligned);
 *         or a hardware error returned by the port (e.g. FWU_ERR_FLASH_WRITE).
 *         Check order is the same as for erase: invalid argument, then range, then alignment.
 */
fwu_return_e fwu_flash_write(const fwu_flash_t *self, uint32_t offset, uint32_t length, const fwu_byte_t *data);

/**
 * @brief  Reads from a flash region (by copying). There is no alignment requirement for reads.
 * @param  self    Flash object (must not be NULL).
 * @param  offset  Offset from the start of the region, unit: fwu_byte_t.
 * @param  length  Length to read, unit: fwu_byte_t. Must not be 0.
 * @param  data    Output buffer (must not be NULL, at least length elements). The function writes here.
 * @return FWU_OK; FWU_ERR_INVALID_ARG (NULL object/table/function/buffer, length == 0);
 *         FWU_ERR_OUT_OF_RANGE (outside the region); or an error returned by the port.
 */
fwu_return_e fwu_flash_read(const fwu_flash_t *self, uint32_t offset, uint32_t length, fwu_byte_t *data);


#ifdef __cplusplus
}
#endif

#endif /* FWU_FLASH_H_ */
