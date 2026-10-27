/**
 * @file    fwu_bytes.h
 * @brief   Number <-> byte array conversions (platform independent).
 *
 * Data arriving from the network/UART is a byte array (fwu_byte_t). This module:
 *   - WRITE (fwu_write_*): writes a number into a byte array,
 *   - READ  (fwu_read_*)  : produces a number from a byte array,
 * in both "big-endian" (high byte first) and "little-endian" (low byte first) order.
 *
 * Byte order is a property of how bytes are LAID OUT IN THE ARRAY, not of a NUMBER:
 *   0x1234 big-endian    -> { 0x12, 0x34 }
 *   0x1234 little-endian -> { 0x34, 0x12 }
 *
 * Naming: fwu_<write|read>_u<bit>_<big|little>_endian
 *   write = number -> array (writing),  read = array -> number (reading).
 *
 * Rules:
 *   - The CPU's own byte order is NOT relied upon (no pointer casts / unions).
 *     Every byte is obtained with shifts and masks.
 *   - Every element written is in the range 0..255 (FWU_BYTE_MASK).
 *   - When reading, every element is limited with FWU_BYTE_MASK (on C28x the upper bits may hold garbage).
 *   - Order when reading: MASK -> WIDEN -> SHIFT. Widening the byte to the target
 *     type (uint16_t/uint32_t) BEFORE shifting avoids signed int overflow.
 *
 */

#ifndef FWU_BYTES_H_
#define FWU_BYTES_H_

#include "fwu_types.h"

/**
 * @brief   Writes a 16-bit value into a 2-byte array in big-endian order.
 *
 * buffer[0] = high byte, buffer[1] = low byte.  E.g. 0x1234 -> { 0x12, 0x34 }.
 *
 * @param[out] buffer  Destination array with at least 2 elements (not checked,
 *                     the caller's responsibility). Only buffer[0] and buffer[1] are modified.
 * @param[in]  value   16-bit value to write.
 */
static inline void fwu_write_u16_big_endian(fwu_byte_t *buffer, uint16_t value) {
    buffer[0] = (fwu_byte_t)((value >> 8) & FWU_BYTE_MASK);   /* high byte */
    buffer[1] = (fwu_byte_t)(value & FWU_BYTE_MASK);          /* low byte */
}

/**
 * @brief   Writes a 16-bit value into a 2-byte array in little-endian order.
 *
 * buffer[0] = low byte, buffer[1] = high byte.  E.g. 0x1234 -> { 0x34, 0x12 }.
 *
 * @param[out] buffer  Destination array with at least 2 elements (not checked,
 *                     the caller's responsibility). Only buffer[0] and buffer[1] are modified.
 * @param[in]  value   16-bit value to write.
 */
static inline void fwu_write_u16_little_endian(fwu_byte_t *buffer, uint16_t value) {
    buffer[0] = (fwu_byte_t)(value & FWU_BYTE_MASK);          /* low byte */
    buffer[1] = (fwu_byte_t)((value >> 8) & FWU_BYTE_MASK);   /* high byte */
}

/**
 * @brief   Converts a 2-byte array to a 16-bit number in big-endian order.
 *
 * buffer[0] = high byte, buffer[1] = low byte.  E.g. { 0x12, 0x34 } -> 0x1234.
 *
 * @param[in] buffer  Source array with at least 2 elements (not checked,
 *                    the caller's responsibility). The array is not modified.
 *
 * @return  The combined 16-bit value.
 */
static inline uint16_t fwu_read_u16_big_endian(const fwu_byte_t *buffer) {
    /* High byte: mask (low 8 bits) -> widen to uint16_t -> shift left by 8 bits. */
    uint16_t high_byte = ((uint16_t)(buffer[0] & FWU_BYTE_MASK))<< 8;
    /* Low byte: mask -> widen to uint16_t (no shift, it is already in the low position). */
    uint16_t low_byte = (uint16_t)(buffer[1]  & FWU_BYTE_MASK);

    /* Combine the two halves with bitwise OR (the high and low halves do not overlap). */
    return high_byte | low_byte;
}

/**
 * @brief   Converts a 2-byte array to a 16-bit number in little-endian order.
 *
 * buffer[0] = low byte, buffer[1] = high byte.  E.g. { 0x34, 0x12 } -> 0x1234.
 *
 * @param[in] buffer  Source array with at least 2 elements (not checked,
 *                    the caller's responsibility). The array is not modified.
 *
 * @return  The combined 16-bit value.
 */
static inline uint16_t fwu_read_u16_little_endian(const fwu_byte_t *buffer) {
    /* High byte: buffer[1]. Mask -> widen to uint16_t -> shift left by 8 bits. */
    uint16_t high_byte = ((uint16_t)(buffer[1] & FWU_BYTE_MASK)) << 8;
    /* Low byte: buffer[0]. Mask -> widen to uint16_t. */
    uint16_t low_byte = (uint16_t)(buffer[0] & FWU_BYTE_MASK);

    /* Combine the two halves with bitwise OR. */
    return high_byte | low_byte;
}

/**
 * @brief   Writes a 32-bit value into a 4-byte array in big-endian order.
 *
 * buffer[0] = most significant byte ... buffer[3] = least significant byte.
 * E.g. 0x89ABCDEF -> { 0x89, 0xAB, 0xCD, 0xEF }.
 *
 * @param[out] buffer  Destination array with at least 4 elements (not checked).
 * @param[in]  value   32-bit value to write (uint32_t, shift-safe).
 */
static inline void fwu_write_u32_big_endian(fwu_byte_t *buffer, uint32_t value) {
    buffer[0] = (fwu_byte_t)((value >> 24) & FWU_BYTE_MASK);   /* high byte */
    buffer[1] = (fwu_byte_t)((value >> 16) & FWU_BYTE_MASK);
    buffer[2] = (fwu_byte_t)((value >> 8) & FWU_BYTE_MASK);
    buffer[3] = (fwu_byte_t)(value & FWU_BYTE_MASK);          /* low byte */
}

/**
 * @brief   Writes a 32-bit value into a 4-byte array in little-endian order.
 *
 * buffer[0] = least significant byte ... buffer[3] = most significant byte.
 * E.g. 0x89ABCDEF -> { 0xEF, 0xCD, 0xAB, 0x89 }.
 *
 * @param[out] buffer  Destination array with at least 4 elements (not checked).
 * @param[in]  value   32-bit value to write.
 */
static inline void fwu_write_u32_little_endian(fwu_byte_t *buffer, uint32_t value) {
    buffer[0] = (fwu_byte_t)(value & FWU_BYTE_MASK);          /* low byte */
    buffer[1] = (fwu_byte_t)((value >> 8) & FWU_BYTE_MASK);
    buffer[2] = (fwu_byte_t)((value >> 16) & FWU_BYTE_MASK);
    buffer[3] = (fwu_byte_t)((value >> 24) & FWU_BYTE_MASK);   /* high byte */
}

/**
 * @brief   Converts a 4-byte array to a 32-bit number in big-endian order.
 *
 * buffer[0] = most significant byte ... buffer[3] = least significant byte.
 * E.g. { 0x12, 0x34, 0x56, 0x78 } -> 0x12345678.
 *
 * @param[in] buffer  Source array with at least 4 elements (not checked).
 *
 * @return  The combined 32-bit value.
 */
/* Note: Each byte is first masked, then WIDENED to uint32_t, THEN shifted.
 * If widening is not done before the shift, the byte is shifted as an "int" and
 * 0x80 << 24 overflows into signed int (undefined behaviour; with a 16-bit int
 * on C28x, a 24-bit shift is larger than the width of the type). */
static inline uint32_t fwu_read_u32_big_endian(const fwu_byte_t *buffer) {
    uint32_t byte_3 = ((uint32_t)(buffer[0] & FWU_BYTE_MASK)) << 24;
    uint32_t byte_2 = ((uint32_t)(buffer[1] & FWU_BYTE_MASK)) << 16;
    uint32_t byte_1 = ((uint32_t)(buffer[2] & FWU_BYTE_MASK)) << 8;
    uint32_t byte_0 = ((uint32_t)(buffer[3] & FWU_BYTE_MASK));

    return byte_3 | byte_2 | byte_1 | byte_0;
}

/**
 * @brief   Converts a 4-byte array to a 32-bit number in little-endian order.
 *
 * buffer[0] = least significant byte ... buffer[3] = most significant byte.
 * E.g. { 0x78, 0x56, 0x34, 0x12 } -> 0x12345678.
 *
 * @param[in] buffer  Source array with at least 4 elements (not checked).
 *
 * @return  The combined 32-bit value.
 */
static inline uint32_t fwu_read_u32_little_endian(const fwu_byte_t *buffer) {
    uint32_t byte_3 = ((uint32_t)(buffer[3] & FWU_BYTE_MASK)) << 24;
    uint32_t byte_2 = ((uint32_t)(buffer[2] & FWU_BYTE_MASK)) << 16;
    uint32_t byte_1 = ((uint32_t)(buffer[1] & FWU_BYTE_MASK)) << 8;
    uint32_t byte_0 = (uint32_t)(buffer[0] & FWU_BYTE_MASK);

    return byte_3 | byte_2 | byte_1 | byte_0;
}

#endif /* FWU_BYTES_H_ */
