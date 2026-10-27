#ifndef FWU_PORT_FWU_FLASH_ADAPTER_H_
#define FWU_PORT_FWU_FLASH_ADAPTER_H_

#include <stdint.h>
#include "fwu_flash.h"

/* Implements the flash interface (fwu_flash_t) of fwu_driver on top of this project's flash_port.
 *
 * Two layers:
 *   fwu_flash_t interface : OFFSET and LENGTH in BYTES (fwu_byte_t), relative to the region
 *   flash_port            : ADDRESS and LENGTH in 16-bit WORDS, ABSOLUTE address
 *
 * Byte <-> word packing rule (same as the PC's hex2000 --binary output, little endian):
 *   byte offset 2k   = LOWER byte of word k  (bit 7..0)
 *   byte offset 2k+1 = UPPER byte of word k  (bit 15..8)
 *
 * Geometry:
 *   page (erase unit)    = 8192 bytes = 4096 words (one sector)
 *   write granule        = 16 bytes   = 8 words = 128 bit
 *   erased value         = 0xFF (word 0xFFFF)
 *
 * Build: the project must define the C2000 symbol (so that fwu_byte_t is 16 bit).
 */
typedef struct {
    fwu_flash_t flash;          /* object to be given to fwu_*: &adapter.flash */
    uint32_t    base_address;   /* First WORD address of the region (absolute), a multiple of 4 */
} fwu_flash_adapter_t;

/**
 * @param  self           Object to be filled in.
 * @param  base_address   Absolute word address of the region (example: 0x090000 staging).
 * @param  length_octets  Size of the region in BYTES, a multiple of 8192 (example: 10 sectors = 81920).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for a NULL object; FWU_ERR_FLASH_GEOMETRY for inconsistent geometry.
 */
fwu_return_e fwu_flash_adapter_init(fwu_flash_adapter_t *self, uint32_t base_address, uint32_t length_octets);

#endif /* FWU_PORT_FWU_FLASH_ADAPTER_H_ */
