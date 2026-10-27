/**
 * @file    fwu_pull.c
 * @brief   Implementation of the pull-image-from-server controller (for the flow, error model and integration rules see fwu_pull.h).
 */

#include <stddef.h>
#include "fwu_pull.h"

/** Request backoff: wait this many ms per retry (at most FWU_PULL_BACKOFF_MAX_MS). */
#define FWU_PULL_BACKOFF_STEP_MS    100u
#define FWU_PULL_BACKOFF_MAX_MS     1000u

/* ------------------------------------------------------------------ helpers */

static bool pull_state_is_active(fwu_byte_t state)
{
    return (state == FWU_PROTO_PULL_REQUESTING) || (state == FWU_PROTO_PULL_DOWNLOADING);
}

/** Has now reached or passed target? (correct across 32-bit wraparound: reached if the difference is below half the range) */
static bool pull_time_reached(uint32_t now_ms, uint32_t target_ms)
{
    return (uint32_t)(now_ms - target_ms) < 0x80000000u;
}

static uint32_t pull_elapsed(uint32_t now_ms, uint32_t since_ms)
{
    return (uint32_t)(now_ms - since_ms);
}

/** Empties the input ring (consumer context: poll). */
static void pull_discard_input(fwu_pull_t *pull)
{
    (void)fwu_ringbuf_clear(&pull->input.base);
}

/** Stops pulling: no pending/ongoing request, the partial chunk and the input are discarded. The error field is not changed (the caller sets it). */
static void pull_stop(fwu_pull_t *pull, fwu_byte_t state)
{
    pull->state = state;
    pull->request_outstanding = false;
    pull->send_pending = false;
    pull->wait_reconnect = false;
    pull->input_overflowed = false;
    pull->chunk_fill = 0u;
    pull->stale_response = false;
    pull_discard_input(pull);
}

/** PERMANENT error: the pull becomes FAILED, the host reads the reason with PULL_STATUS. */
static void pull_fail(fwu_pull_t *pull, fwu_return_e code)
{
    pull->error = code;
    pull_stop(pull, FWU_PROTO_PULL_FAILED);
}

/**
 * Determines the range of the next GET: starts from the last offset WRITTEN to flash by the session (always on a chunk boundary), requests range_bytes,
 * and truncates at the end of the file. The partial chunk and the ongoing response are discarded: the restart point is always the last written chunk.
 */
static void pull_prepare_range(fwu_pull_t *pull)
{
    const uint32_t size = fwu_session_get_image_size(pull->config.session);
    const uint32_t next = fwu_session_get_next_offset(pull->config.session);
    const uint32_t remaining = (size > next) ? (size - next) : 0u;

    pull->range_first = next;
    pull->range_last = (remaining <= pull->config.range_bytes) ? (size - 1u) : (next + pull->config.range_bytes - 1u);
    pull->send_pending = true;
    pull->request_outstanding = false;
    pull->headers_checked = false;
    pull->chunk_fill = 0u;
    pull->chunk_offset = next;
    pull->input_overflowed = false;
    pull->stale_response = false;
    fwu_http_parser_init(&pull->parser);
}

/** Retries the same range (with backoff) or, if the allowance is used up, gives up with `code`. */
static void pull_retry(fwu_pull_t *pull, uint32_t now_ms, fwu_return_e code)
{
    uint32_t backoff;

    pull->retry_count++;
    pull->stats.retries++;
    if(pull->retry_count > pull->config.max_retries) {
        pull_fail(pull, code);
        return;
    }
    pull->state = FWU_PROTO_PULL_REQUESTING;
    pull_prepare_range(pull);
    backoff = pull->retry_count * FWU_PULL_BACKOFF_STEP_MS;
    if(backoff > FWU_PULL_BACKOFF_MAX_MS) {
        backoff = FWU_PULL_BACKOFF_MAX_MS;
    }
    pull->not_before_ms = now_ms + backoff;
}

/** Sends the range request. BUFFER_FULL = transport busy: the request waits and is retried on the next poll. */
static void pull_send_request(fwu_pull_t *pull, uint32_t now_ms)
{
    fwu_byte_t request[FWU_HTTP_REQUEST_MAX];
    size_t length = 0u;
    fwu_return_e status;

    status = fwu_http_build_range_request(pull->ip, pull->port, pull->target, pull->target_length,
                                          pull->range_first, pull->range_last, request, FWU_HTTP_REQUEST_MAX, &length);
    if(status != FWU_OK) {
        pull_fail(pull, status);                    /* validated in PULL_START: must not get here */
        return;
    }
    status = pull->config.send(pull->config.send_context, request, length);
    if(status == FWU_OK) {
        pull->request_outstanding = true;
        pull->send_pending = false;
        pull->last_rx_ms = now_ms;
        pull->stats.requests++;
        pull_discard_input(pull);                   /* leftover (old) bytes from the previous one must not mix into the new response */
        fwu_http_parser_init(&pull->parser);
        pull->headers_checked = false;
        pull->chunk_fill = 0u;
        pull->chunk_offset = pull->range_first;
        pull->input_overflowed = false;
        pull->stale_response = false;
    }
    else if(status != FWU_ERR_BUFFER_FULL) {
        pull_retry(pull, now_ms, FWU_ERR_PULL_TIMEOUT);     /* the transport could not send (no connection...): treated like a timeout */
    }
    else {
        /* busy: stays pending */
    }
}

/**
 * Checks the headers. true = start processing the body. false = the caller changed the state (fail or retry), stop processing.
 * The order matters: PERMANENT problems first (status code, total size), then transient problems (wrong/old range).
 */
static bool pull_check_headers(fwu_pull_t *pull, uint32_t now_ms)
{
    const fwu_http_response_t *r = fwu_http_response(&pull->parser);
    const uint32_t image_size = fwu_session_get_image_size(pull->config.session);

    pull->http_status = r->status;
    if(r->status != 206u) {
        pull_fail(pull, FWU_ERR_PULL_HTTP_STATUS);
        return false;
    }
    if(!r->has_content_range || !r->has_content_length) {
        pull_retry(pull, now_ms, FWU_ERR_PULL_PROTOCOL);
        return false;
    }
    if(r->range_total != image_size) {
        pull_fail(pull, FWU_ERR_PULL_SIZE_MISMATCH);        /* wrong file: caught before a single byte was written */
        return false;
    }
    if((r->range_last < r->range_first) || (r->range_last >= image_size) ||
       (r->content_length != ((r->range_last - r->range_first) + 1u))) {
        pull_retry(pull, now_ms, FWU_ERR_PULL_PROTOCOL);    /* internally inconsistent header */
        return false;
    }
    if((r->range_first != pull->range_first) || (r->range_last != pull->range_last)) {
        /* Consistent but not the range we asked for: a late or duplicate response to a previous request. Its body is discarded, the retry allowance is
         * NOT spent; we keep waiting for our own response (otherwise every stale response would trigger a new request and use up the allowance). */
        pull->stale_response = true;
        pull->headers_checked = true;
        return true;
    }
    pull->headers_checked = true;
    pull->state = FWU_PROTO_PULL_DOWNLOADING;
    return true;
}

/**
 * Accumulates body bytes in the RANGE buffer. Nothing is written to flash: writing before the response is complete and its length verified would mean
 * that after a byte lost in the transport layer, everything following would be stamped into staging SHIFTED (since the re-request
 * continues from the written offset, the corruption would not be repaired). false = buffer overflowed (the caller did a retry).
 */
static bool pull_store_body(fwu_pull_t *pull, const fwu_byte_t *body, size_t length, uint32_t now_ms)
{
    if(pull->stale_response) {
        return true;                                /* response of another range: the body is discarded */
    }
    if((length > pull->config.chunk_buffer_length) || (pull->chunk_fill > (pull->config.chunk_buffer_length - (uint32_t)length))) {
        pull_retry(pull, now_ms, FWU_ERR_PULL_PROTOCOL);    /* must not happen: the header check made Content-Length equal to the range */
        return false;
    }
    for(size_t i = 0u; i < length; i++) {
        pull->config.chunk_buffer[pull->chunk_fill + (uint32_t)i] = (fwu_byte_t)(body[i] & FWU_BYTE_MASK);
    }
    pull->chunk_fill += (uint32_t)length;
    return true;
}

/** Writes the completed range to the session chunk by chunk. false = write failed (the pull became FAILED). */
static bool pull_commit_range(fwu_pull_t *pull, uint32_t now_ms)
{
    const uint32_t chunk_size = fwu_session_get_chunk_size(pull->config.session);
    uint32_t done = 0u;

    while(done < pull->chunk_fill) {
        uint32_t n = pull->chunk_fill - done;
        uint32_t next = 0u;
        fwu_return_e status;

        if(n > chunk_size) {
            n = chunk_size;
        }
        status = fwu_session_push_chunk(pull->config.session, pull->range_first + done, &pull->config.chunk_buffer[done], n, now_ms, &next);
        if(status != FWU_OK) {
            pull_fail(pull, status);
            return false;
        }
        pull->stats.chunks_written++;
        done += n;
    }
    pull->chunk_offset = pull->range_first + done;
    pull->chunk_fill = 0u;
    return true;
}

/** A response is COMPLETE: verify the length, write; is it finished, if not prepare the next range. */
static void pull_response_complete(fwu_pull_t *pull, uint32_t now_ms)
{
    const bool close = fwu_http_response(&pull->parser)->connection_close;

    if(pull->chunk_fill != ((pull->range_last - pull->range_first) + 1u)) {
        pull_retry(pull, now_ms, FWU_ERR_PULL_PROTOCOL);    /* body short/long: re-requested without writing anything */
        return;
    }
    if(!pull_commit_range(pull, now_ms)) {
        return;
    }
    pull->stats.responses++;
    pull->request_outstanding = false;
    pull->retry_count = 0u;                         /* progress was made: the retry allowance is renewed */
    if(fwu_session_get_next_offset(pull->config.session) >= fwu_session_get_image_size(pull->config.session)) {
        pull->error = FWU_OK;
        pull_stop(pull, FWU_PROTO_PULL_DONE);
        return;
    }
    pull_prepare_range(pull);
    pull->not_before_ms = now_ms;
    if(close) {
        pull->wait_reconnect = true;                /* the server will close this connection: wait for the new connection */
        pull->wait_start_ms = now_ms;
    }
}

/** From within poll: gives the bytes pulled from the ring to the HTTP parser and the body to the session. */
static void pull_consume(fwu_pull_t *pull, const fwu_byte_t *data, size_t length, uint32_t now_ms)
{
    pull->last_rx_ms = now_ms;
    pull->stats.bytes_received += (uint32_t)length;
    while((length > 0u) && pull_state_is_active(pull->state) && pull->request_outstanding) {
        size_t used = 0u;
        const fwu_byte_t *body = NULL;
        size_t body_length = 0u;
        const fwu_http_event_e event = fwu_http_feed(&pull->parser, data, length, &used, &body, &body_length);

        data += used;
        length -= used;
        switch(event) {
        case FWU_HTTP_HEADERS_DONE:
            if(!pull_check_headers(pull, now_ms)) {
                return;
            }
            break;
        case FWU_HTTP_BODY:
            if(!pull_store_body(pull, body, body_length, now_ms)) {
                return;
            }
            break;
        case FWU_HTTP_ERROR:
            pull_retry(pull, now_ms, FWU_ERR_PULL_PROTOCOL);
            return;
        case FWU_HTTP_NEED_MORE:
            break;
        default:
            break;
        }
        if(pull->headers_checked && fwu_http_complete(&pull->parser)) {
            if(pull->stale_response) {
                pull->stale_response = false;       /* the stale response was discarded entirely: ours keeps being awaited */
                pull->headers_checked = false;
                pull->chunk_fill = 0u;
                fwu_http_parser_init(&pull->parser);
                continue;
            }
            pull_response_complete(pull, now_ms);   /* remaining bytes (if any) are an old/duplicate response: discarded */
            return;
        }
        if((event == FWU_HTTP_NEED_MORE) && (used == 0u)) {
            return;                                 /* no progress: safety */
        }
    }
}

/* ------------------------------------------------------------------ response frames */

static fwu_return_e pull_reply_error(fwu_return_e code, const fwu_frame_view_t *request, fwu_byte_t *out, size_t out_capacity,
                                     size_t *out_length)
{
    fwu_byte_t payload[FWU_PROTO_ERROR_SIZE];
    size_t length = 0u;
    const fwu_return_e status = fwu_proto_encode_error(code, request->type, payload, FWU_PROTO_ERROR_SIZE, &length);

    if(status != FWU_OK) {
        return status;
    }
    return fwu_frame_encode(FWU_MSG_ERROR, request->seq, payload, length, out, out_capacity, out_length);
}

static fwu_return_e pull_reply_status(const fwu_pull_t *pull, fwu_byte_t type, fwu_byte_t seq, fwu_byte_t *out, size_t out_capacity,
                                      size_t *out_length)
{
    fwu_proto_pull_status_t status;
    fwu_byte_t payload[FWU_PROTO_PULL_STATUS_SIZE];
    size_t length = 0u;
    fwu_return_e result;

    fwu_pull_get_status(pull, &status);
    result = fwu_proto_encode_pull_status(&status, payload, FWU_PROTO_PULL_STATUS_SIZE, &length);
    if(result != FWU_OK) {
        return result;
    }
    return fwu_frame_encode(type, seq, payload, length, out, out_capacity, out_length);
}

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_pull_init(fwu_pull_t *pull, const fwu_pull_config_t *config)
{
    uint32_t chunk_size;

    if((pull == NULL) || (config == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    pull->initialized = false;
    if((config->session == NULL) || (config->send == NULL) || (config->chunk_buffer == NULL) ||
       (config->response_timeout_ms == 0u) || (config->range_bytes == 0u)) {
        return FWU_ERR_INVALID_ARG;
    }
    chunk_size = fwu_session_get_chunk_size(config->session);
    if(chunk_size == 0u) {
        return FWU_ERR_BAD_STATE;                   /* session not initialized */
    }
    if((config->chunk_buffer_length < config->range_bytes) || ((config->range_bytes % chunk_size) != 0u)) {
        return FWU_ERR_INVALID_ARG;
    }

    pull->config = *config;
    pull->state = FWU_PROTO_PULL_IDLE;
    pull->error = FWU_OK;
    pull->http_status = 0u;
    for(size_t i = 0u; i < 4u; i++) {
        pull->ip[i] = 0u;
    }
    pull->port = 0u;
    pull->target_length = 0u;
    pull->link_up = false;
    pull->request_outstanding = false;
    pull->send_pending = false;
    pull->wait_reconnect = false;
    pull->input_overflowed = false;
    pull->range_first = 0u;
    pull->range_last = 0u;
    pull->retry_count = 0u;
    pull->not_before_ms = 0u;
    pull->last_rx_ms = 0u;
    pull->wait_start_ms = 0u;
    pull->headers_checked = false;
    pull->chunk_fill = 0u;
    pull->chunk_offset = 0u;
    pull->stale_response = false;
    fwu_http_parser_init(&pull->parser);
    pull->stats.requests = 0u;
    pull->stats.responses = 0u;
    pull->stats.retries = 0u;
    pull->stats.chunks_written = 0u;
    pull->stats.bytes_received = 0u;
    pull->stats.rx_overflows = 0u;
    pull->stats.link_drops = 0u;
    if(fwu_pull_rb_init(&pull->input) != FWU_OK) {
        return FWU_ERR_INVALID_ARG;
    }
    pull->initialized = true;
    return FWU_OK;
}

bool fwu_pull_handles(fwu_byte_t type)
{
    const fwu_byte_t t = (fwu_byte_t)(type & FWU_BYTE_MASK);

    return (t == FWU_MSG_PULL_START) || (t == FWU_MSG_PULL_QUERY);
}

/** Is the payload the same server/target (a repeat of PULL_START)? */
static bool pull_same_target(const fwu_pull_t *pull, const fwu_proto_pull_start_t *start)
{
    if((pull->port != start->port) || (pull->target_length != start->target_length)) {
        return false;
    }
    for(size_t i = 0u; i < 4u; i++) {
        if((pull->ip[i] & FWU_BYTE_MASK) != (start->ip[i] & FWU_BYTE_MASK)) {
            return false;
        }
    }
    for(size_t i = 0u; i < start->target_length; i++) {
        if((pull->target[i] & FWU_BYTE_MASK) != (start->target[i] & FWU_BYTE_MASK)) {
            return false;
        }
    }
    return true;
}

fwu_return_e fwu_pull_on_frame(fwu_pull_t *pull, const fwu_frame_view_t *request, uint32_t now_ms,
                               fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_proto_pull_start_t start;
    fwu_return_e status;

    if((pull == NULL) || (request == NULL) || (out == NULL) || (out_length == NULL) || (!pull->initialized) ||
       (request->payload == NULL) || (request->payload_length == 0u) || (request->payload_length > FWU_FRAME_MAX_PAYLOAD)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(!fwu_pull_handles(request->type)) {
        return FWU_ERR_UNKNOWN_MESSAGE;
    }
    if(out_capacity < FWU_PULL_MIN_OUT) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    fwu_session_touch(pull->config.session, now_ms);    /* the host is talking: the session is alive */

    if((request->type & FWU_BYTE_MASK) == FWU_MSG_PULL_QUERY) {
        if(request->payload_length != 1u) {
            return pull_reply_error(FWU_ERR_BAD_LENGTH, request, out, out_capacity, out_length);
        }
        return pull_reply_status(pull, FWU_MSG_PULL_STATUS, request->seq, out, out_capacity, out_length);
    }

    /* PULL_START */
    if(fwu_session_get_state(pull->config.session) != FWU_SESSION_DOWNLOADING) {
        return pull_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);     /* REQUEST_DOWNLOAD first */
    }
    status = fwu_proto_parse_pull_start(request->payload, request->payload_length, &start);
    if(status != FWU_OK) {
        return pull_reply_error(status, request, out, out_capacity, out_length);
    }
    if(pull_state_is_active(pull->state)) {
        if(pull_same_target(pull, &start)) {
            /* Repeat with the SAME target: the response was lost or the host is re-reading the state. The ongoing pull is not affected. */
            return pull_reply_status(pull, FWU_MSG_PULL_STARTED, request->seq, out, out_capacity, out_length);
        }
        return pull_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);     /* switching to another target: ABORT first */
    }
    if((pull->state == FWU_PROTO_PULL_DONE) && pull_same_target(pull, &start) &&
       (fwu_session_get_next_offset(pull->config.session) >= fwu_session_get_image_size(pull->config.session))) {
        /* Repeat for a finished pull with the SAME target: the response was lost. A finished pull stays finished. DONE is valid only if the image was actually
         * written in this session: a DONE left over from an old session (e.g. one whose verification failed) does not block the new session's pull. */
        return pull_reply_status(pull, FWU_MSG_PULL_STARTED, request->seq, out, out_capacity, out_length);
    }

    /* (Re)start: IDLE, FAILED or DONE with a different target. */
    for(size_t i = 0u; i < 4u; i++) {
        pull->ip[i] = (fwu_byte_t)(start.ip[i] & FWU_BYTE_MASK);
    }
    pull->port = start.port;
    pull->target_length = start.target_length;
    for(size_t i = 0u; i < start.target_length; i++) {
        pull->target[i] = (fwu_byte_t)(start.target[i] & FWU_BYTE_MASK);
    }
    pull->error = FWU_OK;
    pull->http_status = 0u;
    pull->retry_count = 0u;
    pull->wait_reconnect = false;
    pull->not_before_ms = now_ms;
    pull->last_rx_ms = now_ms;
    if(fwu_session_get_next_offset(pull->config.session) >= fwu_session_get_image_size(pull->config.session)) {
        pull_stop(pull, FWU_PROTO_PULL_DONE);       /* everything is already written (e.g. the host restarted) */
    }
    else {
        pull->state = FWU_PROTO_PULL_REQUESTING;
        pull_prepare_range(pull);
        pull_discard_input(pull);
    }
    return pull_reply_status(pull, FWU_MSG_PULL_STARTED, request->seq, out, out_capacity, out_length);
}

void fwu_pull_set_link(fwu_pull_t *pull, bool up, uint32_t now_ms)
{
    if((pull == NULL) || (!pull->initialized)) {
        return;
    }
    if(!up && pull->link_up) {
        pull->stats.link_drops++;
        if(pull_state_is_active(pull->state)) {
            /* The connection died: the ongoing response is lost, the partial chunk is deleted; after reconnecting, the request is made from the last written chunk.
             * A connection drop is not counted as the server's fault: the retry allowance is not spent. */
            pull_prepare_range(pull);
            pull->not_before_ms = now_ms;
            pull->wait_reconnect = false;
            pull->last_rx_ms = now_ms;
        }
    }
    pull->link_up = up;
}

fwu_return_e fwu_pull_feed(fwu_pull_t *pull, const fwu_byte_t *data, size_t length, uint32_t now_ms)
{
    fwu_return_e status;

    (void)now_ms;
    if((pull == NULL) || (!pull->initialized) || ((data == NULL) && (length > 0u))) {
        return FWU_ERR_INVALID_ARG;
    }
    if(length == 0u) {
        return FWU_OK;
    }
    if(!pull->request_outstanding) {
        return FWU_OK;                              /* bytes arriving while there is no request are leftovers of an old response: discarded */
    }
    if(pull->input_overflowed) {
        /* The response has a HOLE (a chunk was dropped). Bytes arriving afterwards would be shifted, and poll would write them into the chunk buffer and stamp them into flash;
         * since the re-request continues from the written offset, this corruption would not be repaired. All are discarded until the re-request (prepare_range) arrives. */
        return FWU_ERR_BUFFER_FULL;
    }
    status = fwu_ringbuf_push(&pull->input.base, data, length);
    if(status != FWU_OK) {
        pull->stats.rx_overflows++;
        pull->input_overflowed = true;              /* the response is now incomplete: poll will re-request */
        return FWU_ERR_BUFFER_FULL;
    }
    return FWU_OK;
}

void fwu_pull_poll(fwu_pull_t *pull, uint32_t now_ms)
{
    fwu_byte_t scratch[FWU_PULL_SCRATCH_SIZE];

    if((pull == NULL) || (!pull->initialized)) {
        return;
    }
    if(!pull_state_is_active(pull->state)) {
        pull_discard_input(pull);                   /* bytes arriving while no pull is running are leftovers */
        if((pull->state == FWU_PROTO_PULL_DONE) && (fwu_session_get_state(pull->config.session) == FWU_SESSION_IDLE)) {
            pull->error = FWU_OK;                   /* the session dropped (verification failure, ABORT...): the old session's DONE lost its validity */
            pull->state = FWU_PROTO_PULL_IDLE;
        }
        return;
    }
    if(fwu_session_get_state(pull->config.session) != FWU_SESSION_DOWNLOADING) {
        pull->error = FWU_OK;
        pull_stop(pull, FWU_PROTO_PULL_IDLE);       /* the session dropped through ABORT/timeout/flash error: pulling is meaningless */
        return;
    }
    fwu_session_touch(pull->config.session, now_ms);    /* while pulling the host sends no frames: the session must not time out */

    /* 1. Process the bytes in the ring. */
    while(pull_state_is_active(pull->state) && (fwu_ringbuf_used_size(&pull->input.base) > 0u)) {
        uint16_t n = fwu_ringbuf_used_size(&pull->input.base);

        if(!pull->request_outstanding) {
            pull_discard_input(pull);               /* no pending request: what arrived is a leftover of an old response */
            break;
        }
        if(n > FWU_PULL_SCRATCH_SIZE) {
            n = FWU_PULL_SCRATCH_SIZE;
        }
        if(fwu_ringbuf_pull(&pull->input.base, scratch, n) != FWU_OK) {
            break;                                  /* must not happen: single consumer, no more than used_size was requested */
        }
        pull_consume(pull, scratch, n, now_ms);
    }
    if(!pull_state_is_active(pull->state)) {
        return;
    }

    /* 2. Overflowed input: the ongoing response is incomplete, request it again immediately. */
    if(pull->input_overflowed && pull->request_outstanding) {
        pull_retry(pull, now_ms, FWU_ERR_PULL_PROTOCOL);
        if(!pull_state_is_active(pull->state)) {
            return;
        }
    }

    /* 3. Timeout. */
    if(pull->request_outstanding && (pull_elapsed(now_ms, pull->last_rx_ms) >= pull->config.response_timeout_ms)) {
        pull_retry(pull, now_ms, FWU_ERR_PULL_TIMEOUT);
        if(!pull_state_is_active(pull->state)) {
            return;
        }
    }

    /* 4. If the wait for a new connection after "Connection: close" expires (the connection never dropped), continue. */
    if(pull->wait_reconnect && (pull_elapsed(now_ms, pull->wait_start_ms) >= pull->config.response_timeout_ms)) {
        pull->wait_reconnect = false;
    }

    /* 5. Send the next request. */
    if(pull->send_pending && !pull->request_outstanding && pull->link_up && !pull->wait_reconnect &&
       pull_time_reached(now_ms, pull->not_before_ms)) {
        pull_send_request(pull, now_ms);
    }
}

void fwu_pull_abort(fwu_pull_t *pull)
{
    if((pull == NULL) || (!pull->initialized)) {
        return;
    }
    pull->error = FWU_OK;
    pull_stop(pull, FWU_PROTO_PULL_IDLE);
}

void fwu_pull_get_status(const fwu_pull_t *pull, fwu_proto_pull_status_t *status)
{
    if(status == NULL) {
        return;
    }
    status->state = FWU_PROTO_PULL_IDLE;
    status->next_offset = 0u;
    status->error = 0u;
    status->http_status = 0u;
    if((pull == NULL) || (!pull->initialized)) {
        return;
    }
    status->state = pull->state;
    status->next_offset = fwu_session_get_next_offset(pull->config.session);
    status->error = (pull->state == FWU_PROTO_PULL_FAILED) ? (fwu_byte_t)pull->error : 0u;
    status->http_status = pull->http_status;
}

void fwu_pull_get_stats(const fwu_pull_t *pull, fwu_pull_stats_t *stats)
{
    if(stats == NULL) {
        return;
    }
    if((pull == NULL) || (!pull->initialized)) {
        stats->requests = 0u;
        stats->responses = 0u;
        stats->retries = 0u;
        stats->chunks_written = 0u;
        stats->bytes_received = 0u;
        stats->rx_overflows = 0u;
        stats->link_drops = 0u;
        return;
    }
    *stats = pull->stats;
}

bool fwu_pull_active(const fwu_pull_t *pull)
{
    return (pull != NULL) && pull->initialized && pull_state_is_active(pull->state);
}

uint16_t fwu_pull_input_free(const fwu_pull_t *pull)
{
    return ((pull != NULL) && pull->initialized) ? fwu_ringbuf_free_size(&pull->input.base) : 0u;
}

uint16_t fwu_pull_input_used(const fwu_pull_t *pull)
{
    return ((pull != NULL) && pull->initialized) ? fwu_ringbuf_used_size(&pull->input.base) : 0u;
}
