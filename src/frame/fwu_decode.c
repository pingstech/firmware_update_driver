/**
 * @file    fwu_decode.c
 * @brief   fwu_frame_decode: validates the frame at the start of a buffer (see fwu_decode.h).
 *
 * Principle: EACH field is checked as soon as the bytes in hand suffice; invalidity is reported
 * as soon as it is noticed (the remaining bytes are not awaited). Before READING a field we make sure
 * that byte is in hand (buffer_length is sufficient), otherwise memory beyond the buffer would be read.
 */

#include <stddef.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_bytes.h"
#include "fwu_crc.h"
#include "fwu_frame.h"
#include "fwu_decode.h"

fwu_return_e fwu_frame_decode(const fwu_byte_t *buffer, size_t buffer_length, fwu_frame_view_t *view)
{
    size_t   payload_length;
    size_t   total_length;
    size_t   crc_offset;
    uint16_t crc_calculated;
    uint16_t crc_received;

    /* 1) Input: buffer is the data to read, view is the output to write. Both must exist. */
    if((buffer == NULL) || (view == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }

    /* 2) SOF: if at least 1 byte is present look at the first byte IMMEDIATELY (without waiting for the rest of the frame).
     *    The data is read from `buffer` (NOT view: view is still an empty output). */
    if((buffer_length >= FWU_FRAME_SOF_SIZE) &&
       ((buffer[FWU_FRAME_OFFSET_SOF] & FWU_BYTE_MASK) != FWU_FRAME_SOF)) {
        return FWU_ERR_BAD_FRAME;
    }

    /* 3) VER: if the VER byte is in hand (SOF + VER = 2 bytes) look at it immediately. */
    if((buffer_length >= (FWU_FRAME_SOF_SIZE + FWU_FRAME_VER_SIZE)) &&
       ((buffer[FWU_FRAME_OFFSET_VER] & FWU_BYTE_MASK) != FWU_FRAME_VERSION)) {
        return FWU_ERR_BAD_FRAME;
    }

    /* 4) LEN cannot be read until the whole header (6 bytes) has arrived: this is not an error, it is "not enough yet". */
    if(buffer_length < FWU_FRAME_HEADER_SIZE) {
        return FWU_ERR_NOT_ENOUGH_DATA;
    }

    /* 5) LEN: big endian. If invalid, the rest of the frame is NOT AWAITED (so an attacker cannot send LEN = 65535 and
     *    keep the parser waiting forever). LEN = 0 is also invalid (an empty payload is not accepted). */
    payload_length = (size_t)fwu_read_u16_big_endian(&buffer[FWU_FRAME_OFFSET_LEN]);
    if((payload_length == 0u) || (payload_length > FWU_FRAME_MAX_PAYLOAD)) {
        return FWU_ERR_BAD_LENGTH;
    }

    /* 6) The total length can now be computed safely (payload_length <= 512, no overflow).
     *    If it has not all arrived, wait. The reads AFTER this rely on this check. */
    total_length = FWU_FRAME_OVERHEAD + payload_length;
    if(buffer_length < total_length) {
        return FWU_ERR_NOT_ENOUGH_DATA;
    }

    /* 7) EOF: the last byte of the frame. A cheap check, done BEFORE the CRC. */
    if((buffer[total_length - FWU_FRAME_EOF_SIZE] & FWU_BYTE_MASK) != FWU_FRAME_EOF) {
        return FWU_ERR_BAD_FRAME;
    }

    /* 8) CRC: from VER to the end of the payload (excluding SOF, CRC, EOF), the SAME range as in encode.
     *    The received CRC is right after the payload (big endian). */
    crc_offset     = FWU_FRAME_OFFSET_PAYLOAD + payload_length;
    crc_calculated = fwu_crc16_classic_calculate(&buffer[FWU_FRAME_CRC_START_OFFSET],
                                                 (FWU_FRAME_HEADER_SIZE - FWU_FRAME_SOF_SIZE) + payload_length);
    crc_received   = fwu_read_u16_big_endian(&buffer[crc_offset]);
    if(crc_calculated != crc_received) {
        return FWU_ERR_CRC_MISMATCH;
    }

    /* 9) Everything is correct: view is written ONLY now (view is unchanged in a call that returns an error).
     *    payload is an ADDRESS: it points into the buffer, there is no copy. */
    view->type           = (fwu_byte_t)(buffer[FWU_FRAME_OFFSET_TYPE] & FWU_BYTE_MASK);
    view->seq            = (fwu_byte_t)(buffer[FWU_FRAME_OFFSET_SEQ] & FWU_BYTE_MASK);
    view->payload        = &buffer[FWU_FRAME_OFFSET_PAYLOAD];
    view->payload_length = payload_length;
    view->frame_length   = total_length;
    return FWU_OK;
}
