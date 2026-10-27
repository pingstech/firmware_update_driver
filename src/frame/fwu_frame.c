#include <stddef.h>

#include "fwu_frame.h"
#include "fwu_types.h"
#include "fwu_error.h"
#include "fwu_bytes.h"
#include "fwu_crc.h"

fwu_return_e fwu_frame_encode(fwu_byte_t type, fwu_byte_t seq,
                              const fwu_byte_t *payload, size_t payload_length,
                              fwu_byte_t *out, size_t out_capacity, size_t *out_length) {
    if(out == NULL || out_length == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    else if(payload == NULL || payload_length == 0u) {
        return FWU_ERR_INVALID_ARG;
    }
    else if(payload_length > FWU_FRAME_MAX_PAYLOAD) {
        return FWU_ERR_INVALID_ARG;
    }
    else if(out_capacity < payload_length + FWU_FRAME_OVERHEAD) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    else {
        /* All validation passed: from here on out is written. */
        const size_t total_length = FWU_FRAME_OVERHEAD + payload_length;
        const size_t crc_offset   = FWU_FRAME_OFFSET_PAYLOAD + payload_length;   /* CRC and EOF shift with the end of the payload */
        uint16_t crc;

        /* 1-4) The single-byte fields of the header. */
        out[FWU_FRAME_OFFSET_SOF]  = (fwu_byte_t)(FWU_FRAME_SOF & FWU_BYTE_MASK);
        out[FWU_FRAME_OFFSET_VER]  = (fwu_byte_t)(FWU_FRAME_VERSION & FWU_BYTE_MASK);
        out[FWU_FRAME_OFFSET_TYPE] = (fwu_byte_t)(type & FWU_BYTE_MASK);
        out[FWU_FRAME_OFFSET_SEQ]  = (fwu_byte_t)(seq & FWU_BYTE_MASK);

        /* 5) LEN: payload length, big endian (at most FWU_FRAME_MAX_PAYLOAD, fits in 16 bits). */
        fwu_write_u16_big_endian(&out[FWU_FRAME_OFFSET_LEN], (uint16_t)payload_length);

        /* 6) Copy exactly payload_length payload bytes; each byte is masked to its low 8 bits (C28x). */
        for(size_t i = 0u; i < payload_length; i++) {
            out[FWU_FRAME_OFFSET_PAYLOAD + i] = (fwu_byte_t)(payload[i] & FWU_BYTE_MASK);
        }

        /* 7) CRC: from VER to the end of the payload (excluding SOF, CRC and EOF), computed from the buffer. */
        crc = fwu_crc16_classic_calculate(&out[FWU_FRAME_CRC_START_OFFSET],
                                          (FWU_FRAME_HEADER_SIZE - FWU_FRAME_SOF_SIZE) + payload_length);

        /* 8) CRC, right after the payload, big endian. */
        fwu_write_u16_big_endian(&out[crc_offset], crc);

        /* 9) EOF, right after the CRC. */
        out[crc_offset + FWU_FRAME_CRC_SIZE] = (fwu_byte_t)(FWU_FRAME_EOF & FWU_BYTE_MASK);

        /* 10) The number of bytes written is reported last. */
        *out_length = total_length;
        return FWU_OK;
    }
}
