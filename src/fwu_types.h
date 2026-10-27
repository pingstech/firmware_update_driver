/**
 * @file    fwu_types.h
 * @brief   Common basic types shared by all fwu modules.
 *
 * This header collects the platform-dependent type decisions in ONE place.
 * Core modules (crc, frame, journal, ...) do not use uint8_t directly as the
 * byte type; they use fwu_byte_t. When porting to a new MCU (e.g. C28x),
 * only this file needs to be looked at.
 *
 * Platform selection: for TI C2000, "-DC2000" (the C2000 macro) must be added
 * to the compiler command line / project settings. If it is not defined, the
 * default (ARM / ST / PC) definition is used.
 */

#ifndef FWU_TYPES_H_
#define FWU_TYPES_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Type that carries one byte value.
 *
 * Rule: one element = one byte value (0..255), unpacked.
 *   - ARM / ST : 8 bits.
 *   - C28x     : 16-bit container, the value is kept in the range 0..255
 *                (the upper 8 bits must be zero).
 */
#if defined(C2000)

/* C28x: char is already 16 bits and there is no uint8_t. A 16-bit container is used. */
typedef uint16_t fwu_byte_t;

#else

/* ARM / ST / PC: "unsigned char", 8 bits. */
typedef unsigned char fwu_byte_t;

#endif

/**
 * @brief  Mask that limits a byte value (low 8 bits).
 *
 * On C28x fwu_byte_t is 16 bits wide, so the upper bits may contain garbage.
 * When a byte value is read it is limited with (data & FWU_BYTE_MASK).
 * The "u" suffix marks the constant as unsigned (avoids mixing signed and
 * unsigned arithmetic).
 */
#define FWU_BYTE_MASK 0xFFu

#ifdef __cplusplus
}
#endif

#endif /* FWU_TYPES_H_ */
