/**
 * @file    fwu_parser.c
 * @brief   Streaming frame parser implementation (see fwu_parser.h for the design and behaviour).
 */

#include <stddef.h>
#include "fwu_parser.h"

/* ------------------------------------------------------------------ helpers */

/** Removes `amount` bytes from the start of the buffer (shifts the rest to the left). amount <= count is required. */
static void parser_shift(fwu_parser_t *parser, size_t amount)
{
    for(size_t i = 0u; (i + amount) < parser->count; i++) {
        parser->buffer[i] = parser->buffer[i + amount];
    }
    parser->count -= amount;
}

/**
 * Assuming the byte at the head of the buffer was a PREVIOUS false alarm, returns the index of the next
 * SOF (at least 1). If there is no SOF it returns the whole buffer (count).
 */
static size_t parser_next_sof(const fwu_parser_t *parser)
{
    for(size_t i = 1u; i < parser->count; i++) {
        if((parser->buffer[i] & FWU_BYTE_MASK) == FWU_FRAME_SOF) {
            return i;
        }
    }
    return parser->count;
}

/** Discards the corrupt/false start (up to the next SOF) and updates the counters. */
static size_t parser_resync(fwu_parser_t *parser, uint32_t now_ms)
{
    const size_t amount = parser_next_sof(parser);
    parser_shift(parser, amount);
    parser->stats.bytes_dropped += (uint32_t)amount;
    parser->head_time_ms = now_ms;      /* new head, new timeout window */
    return amount;
}

/** Pulls the bytes waiting in the ring buffer into the buffer (as many as there is room for). */
static fwu_return_e parser_fill(fwu_parser_t *parser, uint32_t now_ms)
{
    const size_t room = FWU_FRAME_MAX_SIZE - parser->count;
    const size_t available = (size_t)fwu_ringbuf_used_size(parser->source);
    const size_t amount = (available < room) ? available : room;
    fwu_return_e status = FWU_OK;

    if(amount > 0u) {
        status = fwu_ringbuf_pull(parser->source, &parser->buffer[parser->count], amount);
        if(status == FWU_OK) {
            if(parser->count == 0u) {
                parser->head_time_ms = now_ms;   /* the buffer was empty: the head starts now */
            }
            parser->count += amount;
        }
    }
    return status;
}

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_parser_init(fwu_parser_t *parser, fwu_ringbuf_t *source)
{
    if((parser == NULL) || (source == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }

    parser->source       = source;
    parser->count        = 0u;
    parser->delivered    = 0u;
    parser->head_time_ms = 0u;
    parser->stats.frames_ok     = 0u;
    parser->stats.bytes_dropped = 0u;
    parser->stats.bad_frame     = 0u;
    parser->stats.bad_length    = 0u;
    parser->stats.crc_mismatch  = 0u;
    parser->stats.timeouts      = 0u;
    return FWU_OK;
}

fwu_return_e fwu_parser_poll(fwu_parser_t *parser, uint32_t now_ms, fwu_frame_view_t *view)
{
    size_t dropped_this_poll = 0u;

    if((parser == NULL) || (view == NULL) || (parser->source == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }

    /* Remove the frame delivered in the previous poll: view.payload is now invalid. */
    if(parser->delivered > 0u) {
        parser_shift(parser, parser->delivered);
        parser->delivered    = 0u;
        parser->head_time_ms = now_ms;
    }

    for(;;) {
        fwu_frame_view_t candidate;
        fwu_return_e status = parser_fill(parser, now_ms);

        if(status != FWU_OK) {
            return status;
        }
        if(parser->count == 0u) {
            return FWU_ERR_NOT_ENOUGH_DATA;     /* no bytes left to process */
        }

        status = fwu_frame_decode(parser->buffer, parser->count, &candidate);

        if(status == FWU_OK) {
            *view = candidate;
            parser->delivered = candidate.frame_length;
            parser->stats.frames_ok++;
            return FWU_OK;
        }

        if(status == FWU_ERR_NOT_ENOUGH_DATA) {
            /* If a long time has passed since the head started, this frame will never be completed:
             * discard it and retry (with the remaining bytes). Wrap-safe subtraction. */
            if((uint32_t)(now_ms - parser->head_time_ms) > FWU_PARSER_FRAME_TIMEOUT_MS) {
                parser->stats.timeouts++;
                dropped_this_poll += parser_resync(parser, now_ms);
            }
            else {
                return FWU_ERR_NOT_ENOUGH_DATA;  /* wait */
            }
        }
        else {
            /* BAD_FRAME, BAD_LENGTH, CRC_MISMATCH: false alarm, retry from the next SOF. */
            if(status == FWU_ERR_BAD_FRAME) {
                parser->stats.bad_frame++;
            }
            else if(status == FWU_ERR_BAD_LENGTH) {
                parser->stats.bad_length++;
            }
            else {
                parser->stats.crc_mismatch++;
            }
            dropped_this_poll += parser_resync(parser, now_ms);
        }

        /* A poll call must not last forever (even if the interrupt keeps filling in garbage). */
        if(dropped_this_poll >= FWU_PARSER_MAX_DROP_PER_POLL) {
            return FWU_ERR_NOT_ENOUGH_DATA;
        }
    }
}
