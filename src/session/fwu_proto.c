/**
 * @file    fwu_proto.c
 * @brief   Session protocol encode/parse implementation (see fwu_proto.h for the formats).
 */

#include <stdbool.h>
#include <stddef.h>
#include "fwu_proto.h"
#include "fwu_bytes.h"

/* ------------------------------------------------------------------ helpers */

/** Common encode validation: out/out_length are not NULL and the capacity is sufficient. */
static fwu_return_e proto_check_out(const fwu_byte_t *out, size_t out_capacity, const size_t *out_length, size_t needed)
{
    if((out == NULL) || (out_length == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(out_capacity < needed) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    return FWU_OK;
}

/** Common parse validation: payload is not NULL and the length is exactly the expected one. */
static fwu_return_e proto_check_in(const fwu_byte_t *payload, size_t payload_length, size_t expected)
{
    if(payload == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    if(payload_length != expected) {
        return FWU_ERR_BAD_LENGTH;
    }
    return FWU_OK;
}

static fwu_byte_t proto_byte(const fwu_byte_t *p)
{
    return (fwu_byte_t)(*p & FWU_BYTE_MASK);
}

/* ------------------------------------------------------------------ INFO */

fwu_return_e fwu_proto_encode_info(const fwu_proto_info_t *info, fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_return_e status;

    if(info == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    status = proto_check_out(out, out_capacity, out_length, FWU_PROTO_INFO_SIZE);
    if(status != FWU_OK) {
        return status;
    }
    out[0] = (fwu_byte_t)(info->proto_version & FWU_BYTE_MASK);
    out[1] = (fwu_byte_t)(info->flags & FWU_BYTE_MASK);
    fwu_write_u32_big_endian(&out[2], info->product_id);
    fwu_write_u32_big_endian(&out[6], info->running_image_version);
    fwu_write_u16_big_endian(&out[10], info->max_chunk);
    out[12] = (fwu_byte_t)(info->state & FWU_BYTE_MASK);
    *out_length = FWU_PROTO_INFO_SIZE;
    return FWU_OK;
}

fwu_return_e fwu_proto_parse_info(const fwu_byte_t *payload, size_t payload_length, fwu_proto_info_t *info)
{
    fwu_return_e status = proto_check_in(payload, payload_length, FWU_PROTO_INFO_SIZE);

    if(status != FWU_OK) {
        return status;
    }
    if(info == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    info->proto_version = proto_byte(&payload[0]);
    info->flags = proto_byte(&payload[1]);
    info->product_id = fwu_read_u32_big_endian(&payload[2]);
    info->running_image_version = fwu_read_u32_big_endian(&payload[6]);
    info->max_chunk = fwu_read_u16_big_endian(&payload[10]);
    info->state = proto_byte(&payload[12]);
    return FWU_OK;
}

/* ------------------------------------------------------------------ DOWNLOAD_ACCEPTED */

fwu_return_e fwu_proto_encode_accepted(const fwu_proto_accepted_t *accepted, fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_return_e status;

    if(accepted == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    status = proto_check_out(out, out_capacity, out_length, FWU_PROTO_ACCEPTED_SIZE);
    if(status != FWU_OK) {
        return status;
    }
    fwu_write_u16_big_endian(&out[0], accepted->chunk_size);
    out[2] = (fwu_byte_t)(accepted->mode & FWU_BYTE_MASK);
    fwu_write_u16_big_endian(&out[3], accepted->timeout_s);
    fwu_write_u32_big_endian(&out[5], accepted->next_offset);
    *out_length = FWU_PROTO_ACCEPTED_SIZE;
    return FWU_OK;
}

fwu_return_e fwu_proto_parse_accepted(const fwu_byte_t *payload, size_t payload_length, fwu_proto_accepted_t *accepted)
{
    fwu_return_e status = proto_check_in(payload, payload_length, FWU_PROTO_ACCEPTED_SIZE);

    if(status != FWU_OK) {
        return status;
    }
    if(accepted == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    accepted->chunk_size = fwu_read_u16_big_endian(&payload[0]);
    accepted->mode = proto_byte(&payload[2]);
    accepted->timeout_s = fwu_read_u16_big_endian(&payload[3]);
    accepted->next_offset = fwu_read_u32_big_endian(&payload[5]);
    return FWU_OK;
}

/* ------------------------------------------------------------------ single 32-bit value */

fwu_return_e fwu_proto_encode_u32(uint32_t value, fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    const fwu_return_e status = proto_check_out(out, out_capacity, out_length, FWU_PROTO_U32_SIZE);

    if(status != FWU_OK) {
        return status;
    }
    fwu_write_u32_big_endian(out, value);
    *out_length = FWU_PROTO_U32_SIZE;
    return FWU_OK;
}

fwu_return_e fwu_proto_parse_u32(const fwu_byte_t *payload, size_t payload_length, uint32_t *value)
{
    const fwu_return_e status = proto_check_in(payload, payload_length, FWU_PROTO_U32_SIZE);

    if(status != FWU_OK) {
        return status;
    }
    if(value == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    *value = fwu_read_u32_big_endian(payload);
    return FWU_OK;
}

/* ------------------------------------------------------------------ TRANSFER_DATA */

fwu_return_e fwu_proto_encode_data(uint32_t offset, const fwu_byte_t *data, size_t data_length,
                                   fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_return_e status;

    if((data == NULL) || (data_length == 0u) || (data_length > FWU_PROTO_MAX_CHUNK)) {
        return FWU_ERR_INVALID_ARG;
    }
    status = proto_check_out(out, out_capacity, out_length, FWU_PROTO_DATA_HEADER_SIZE + data_length);
    if(status != FWU_OK) {
        return status;
    }
    fwu_write_u32_big_endian(out, offset);
    for(size_t i = 0u; i < data_length; i++) {
        out[FWU_PROTO_DATA_HEADER_SIZE + i] = (fwu_byte_t)(data[i] & FWU_BYTE_MASK);
    }
    *out_length = FWU_PROTO_DATA_HEADER_SIZE + data_length;
    return FWU_OK;
}

fwu_return_e fwu_proto_parse_data(const fwu_byte_t *payload, size_t payload_length, fwu_proto_data_t *data)
{
    if(payload == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    if((payload_length <= FWU_PROTO_DATA_HEADER_SIZE) || (payload_length > FWU_FRAME_MAX_PAYLOAD)) {
        return FWU_ERR_BAD_LENGTH;
    }
    if(data == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    data->offset = fwu_read_u32_big_endian(payload);
    data->data = &payload[FWU_PROTO_DATA_HEADER_SIZE];
    data->data_length = payload_length - FWU_PROTO_DATA_HEADER_SIZE;
    return FWU_OK;
}

/* ------------------------------------------------------------------ VERIFY_RESULT and ERROR */

fwu_return_e fwu_proto_encode_result(fwu_return_e code, fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_return_e status;

    if(((int)code < 0) || ((int)code > 255)) {
        return FWU_ERR_INVALID_ARG;
    }
    status = proto_check_out(out, out_capacity, out_length, 1u);
    if(status != FWU_OK) {
        return status;
    }
    out[0] = (fwu_byte_t)code;
    *out_length = 1u;
    return FWU_OK;
}

fwu_return_e fwu_proto_parse_result(const fwu_byte_t *payload, size_t payload_length, fwu_byte_t *code)
{
    const fwu_return_e status = proto_check_in(payload, payload_length, 1u);

    if(status != FWU_OK) {
        return status;
    }
    if(code == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    *code = proto_byte(&payload[0]);
    return FWU_OK;
}

fwu_return_e fwu_proto_encode_error(fwu_return_e code, fwu_byte_t request_type,
                                    fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_return_e status;

    if(((int)code < 1) || ((int)code > 255)) {
        return FWU_ERR_INVALID_ARG;
    }
    status = proto_check_out(out, out_capacity, out_length, FWU_PROTO_ERROR_SIZE);
    if(status != FWU_OK) {
        return status;
    }
    out[0] = (fwu_byte_t)code;
    out[1] = (fwu_byte_t)(request_type & FWU_BYTE_MASK);
    *out_length = FWU_PROTO_ERROR_SIZE;
    return FWU_OK;
}

fwu_return_e fwu_proto_parse_error(const fwu_byte_t *payload, size_t payload_length, fwu_byte_t *code, fwu_byte_t *request_type)
{
    const fwu_return_e status = proto_check_in(payload, payload_length, FWU_PROTO_ERROR_SIZE);

    if(status != FWU_OK) {
        return status;
    }
    if((code == NULL) || (request_type == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    *code = proto_byte(&payload[0]);
    *request_type = proto_byte(&payload[1]);
    return FWU_OK;
}

/* ------------------------------------------------------------------ PULL_START and PULL_STATUS */

/** Is the target valid: starts with '/', every character is printable ASCII (excluding space), length 1..96. */
static bool proto_target_valid(const fwu_byte_t *target, size_t target_length)
{
    if((target == NULL) || (target_length == 0u) || (target_length > FWU_PROTO_PULL_TARGET_MAX)) {
        return false;
    }
    if((target[0] & FWU_BYTE_MASK) != (fwu_byte_t)'/') {
        return false;
    }
    for(size_t i = 0u; i < target_length; i++) {
        const fwu_byte_t c = (fwu_byte_t)(target[i] & FWU_BYTE_MASK);

        if((c < 0x21u) || (c > 0x7Eu)) {
            return false;
        }
    }
    return true;
}

fwu_return_e fwu_proto_encode_pull_start(const fwu_byte_t ip[4], uint16_t port, const fwu_byte_t *target, size_t target_length,
                                         fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_return_e status;

    if((ip == NULL) || (port == 0u) || !proto_target_valid(target, target_length)) {
        return FWU_ERR_INVALID_ARG;
    }
    status = proto_check_out(out, out_capacity, out_length, FWU_PROTO_PULL_START_FIXED + target_length);
    if(status != FWU_OK) {
        return status;
    }
    for(size_t i = 0u; i < 4u; i++) {
        out[i] = (fwu_byte_t)(ip[i] & FWU_BYTE_MASK);
    }
    fwu_write_u16_big_endian(&out[4], port);
    out[6] = (fwu_byte_t)target_length;
    for(size_t i = 0u; i < target_length; i++) {
        out[FWU_PROTO_PULL_START_FIXED + i] = (fwu_byte_t)(target[i] & FWU_BYTE_MASK);
    }
    *out_length = FWU_PROTO_PULL_START_FIXED + target_length;
    return FWU_OK;
}

fwu_return_e fwu_proto_parse_pull_start(const fwu_byte_t *payload, size_t payload_length, fwu_proto_pull_start_t *start)
{
    size_t target_length;
    uint16_t port;

    if(payload == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    if(payload_length <= FWU_PROTO_PULL_START_FIXED) {
        return FWU_ERR_BAD_LENGTH;
    }
    target_length = (size_t)(payload[6] & FWU_BYTE_MASK);
    if((target_length == 0u) || (target_length > FWU_PROTO_PULL_TARGET_MAX) ||
       (payload_length != (FWU_PROTO_PULL_START_FIXED + target_length))) {
        return FWU_ERR_BAD_LENGTH;
    }
    if(start == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    port = fwu_read_u16_big_endian(&payload[4]);
    if((port == 0u) || !proto_target_valid(&payload[FWU_PROTO_PULL_START_FIXED], target_length)) {
        return FWU_ERR_INVALID_ARG;
    }
    for(size_t i = 0u; i < 4u; i++) {
        start->ip[i] = proto_byte(&payload[i]);
    }
    start->port = port;
    start->target = &payload[FWU_PROTO_PULL_START_FIXED];
    start->target_length = target_length;
    return FWU_OK;
}

fwu_return_e fwu_proto_encode_pull_status(const fwu_proto_pull_status_t *status, fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_return_e result;

    if((status == NULL) || (status->state > FWU_PROTO_PULL_FAILED)) {
        return FWU_ERR_INVALID_ARG;
    }
    result = proto_check_out(out, out_capacity, out_length, FWU_PROTO_PULL_STATUS_SIZE);
    if(result != FWU_OK) {
        return result;
    }
    out[0] = (fwu_byte_t)(status->state & FWU_BYTE_MASK);
    fwu_write_u32_big_endian(&out[1], status->next_offset);
    out[5] = (fwu_byte_t)(status->error & FWU_BYTE_MASK);
    fwu_write_u16_big_endian(&out[6], status->http_status);
    *out_length = FWU_PROTO_PULL_STATUS_SIZE;
    return FWU_OK;
}

fwu_return_e fwu_proto_parse_pull_status(const fwu_byte_t *payload, size_t payload_length, fwu_proto_pull_status_t *status)
{
    const fwu_return_e result = proto_check_in(payload, payload_length, FWU_PROTO_PULL_STATUS_SIZE);

    if(result != FWU_OK) {
        return result;
    }
    if(status == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    if(proto_byte(&payload[0]) > FWU_PROTO_PULL_FAILED) {
        return FWU_ERR_INVALID_ARG;
    }
    status->state = proto_byte(&payload[0]);
    status->next_offset = fwu_read_u32_big_endian(&payload[1]);
    status->error = proto_byte(&payload[5]);
    status->http_status = fwu_read_u16_big_endian(&payload[6]);
    return FWU_OK;
}
