/**
 * @file    fwu_crc.h
 * @brief   CRC-16/MODBUS computation module (firmware update driver, task 1).
 *
 * This module computes the 16-bit CRC of a byte array. A CRC is a check value used to
 * detect whether data was CORRUPTED during transmission or storage. It is NOT encryption
 * and does not prove who sent the data (authentication).
 *
 * CRC-16/MODBUS parameters:
 *   - Polynomial (reflected) : 0xA001   (normal representation: 0x8005)
 *   - Initial value          : 0xFFFF   (FWU_CRC16_INIT)
 *   - Input/output reflection: yes (implicit with the reflected polynomial)
 *   - Final XOR              : none
 *   - Check vector           : "123456789" -> 0x4B37
 *
 * Usage (incremental computation):
 *   crc = FWU_CRC16_INIT;
 *   crc = fwu_crc16_calculate_w_start_value(crc, chunk1, len1);
 *   crc = fwu_crc16_calculate_w_start_value(crc, chunk2, len2);
 *   // crc is now the CRC of the whole chunk1+chunk2 data.
 *
 * Verification hint: if the data + CRC (low byte first) are fed to the CRC together,
 * the result is 0x0000.
 *
 * @note  Design decision: the function bodies are kept in this header as "static inline"
 *        (compatible with MISRA C:2012 Rule 8.10). The header can be copied on its own into
 *        bootloader and application projects and needs no linking.
 *        If many .c files use it, each of them may get its own copy.
 */

#ifndef FWU_CRC_H_
#define FWU_CRC_H_

#include <stddef.h>
#include "fwu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Standard initial value for CRC-16/MODBUS (all bits 1). */
#define FWU_CRC16_INIT 0xFFFF

/**
 * @brief   Computes CRC-16/MODBUS, continuing from the given start value.
 *
 * Used when data arrives in chunks: FWU_CRC16_INIT is passed for the first chunk,
 * and the return value of the PREVIOUS call is passed for every following chunk.
 *
 * Algorithm: a 16-bit register starts with start_value. For each byte, the byte is
 * XORed into the low 8 bits of the register, then 8 shift steps are applied (one
 * for each bit of the byte). When all bytes are consumed the register is the CRC.
 *
 * @param[in] start_value  CRC value to start the computation from. FWU_CRC16_INIT for
 *                         the first chunk, the previous result for following chunks.
 * @param[in] data         Byte array to compute the CRC of. MUST NOT be NULL if length > 0
 *                         (not checked, the caller's responsibility).
 * @param[in] length       Number of bytes in data. If 0, start_value is returned unchanged.
 *
 * @return  The updated 16-bit CRC. The output is always 2 bytes, regardless of the
 *          input length.
 *
 * @note    The ORDER of chunks matters. Supplying the same chunks in a different order
 *          produces a different CRC (a CRC is order sensitive).
 *
 * @note    The byte type is fwu_byte_t (see fwu_types.h): one element = one byte
 *          value (0..255). The upper bits are ignored with FWU_BYTE_MASK.
 */
static inline uint16_t fwu_crc16_calculate_w_start_value(const uint16_t start_value, const fwu_byte_t *data, size_t length) {
    uint16_t crc = start_value;  /* Initial value: "start_value" */

    for(size_t index = 0; index < length; index++) {
        /* Mix the byte into the low 8 bits of the register. */
        crc ^= (uint16_t)(data[index] & FWU_BYTE_MASK);

        /* A byte has 8 bits: one shift step per bit.
         * (8 = bits per byte; not CHAR_BIT, because CHAR_BIT is 16 on C28x.) */
        for(unsigned int j_index = 0; j_index < 8; j_index++) {
            if(crc & 0x0001) {
                /* Rightmost bit is 1: shift right by 1 bit, then XOR with 0xA001. */
                crc = (crc >> 1) ^ 0xA001;
            }
            else {
                /* Rightmost bit is 0: just shift right by 1 bit. */
                crc = (crc >> 1);
            }
        }
    }

    return crc;
}

/**
 * @brief   Computes the CRC in one go, with the standard initial value (0xFFFF).
 *
 * Identical to fwu_crc16_calculate_w_start_value(FWU_CRC16_INIT, data, length).
 * Used when the data is available as a single piece (e.g. a small packet).
 *
 * @param[in] data    Byte array to compute the CRC of (must not be NULL if length > 0).
 * @param[in] length  Number of bytes in data.
 *
 * @return  The 16-bit CRC. For empty data (length = 0) it returns 0xFFFF.
 */
static inline uint16_t fwu_crc16_classic_calculate(const fwu_byte_t *data, size_t length) {
    return fwu_crc16_calculate_w_start_value(FWU_CRC16_INIT, data, length);
}

#ifdef __cplusplus
}
#endif

#endif /* FWU_CRC_H_ */
