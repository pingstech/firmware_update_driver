/**
 * @file    fwu_session.c
 * @brief   Update session implementation (see fwu_session.h for the states, rules and error codes).
 */

#include <stddef.h>
#include "fwu_session.h"
#include "fwu_bytes.h"

/* Compile time: the manifest also fits the internal buffer when rounded up to the LARGEST supported granule. For that reason it is not
 * checked separately in init (a granule > FWU_SESSION_MAX_GRANULE is already rejected). */
typedef char session_manifest_buffer_fits[(((((uint32_t)FWU_MANIFEST_SIZE + FWU_SESSION_MAX_GRANULE - 1u) /
                                             FWU_SESSION_MAX_GRANULE) * FWU_SESSION_MAX_GRANULE) <= FWU_SESSION_MANIFEST_BUFFER) ? 1 : -1];

/* ------------------------------------------------------------------ helpers */

static uint32_t session_round_up(uint32_t value, uint32_t unit)
{
    return ((value + unit - 1u) / unit) * unit;
}

/** Fits the error code into the protocol's single byte; a code that does not fit (if a hook returns a nonsensical value) becomes CRYPTO_FAILURE. */
static fwu_return_e session_code(fwu_return_e status)
{
    if(((int)status < 1) || ((int)status > 255)) {
        return FWU_ERR_CRYPTO_FAILURE;
    }
    return status;
}

/** Response frame: type, SEQ, payload. The output buffer was found sufficient in the input check, so no overflow can occur. */
static fwu_return_e session_reply(fwu_byte_t type, fwu_byte_t seq, const fwu_byte_t *payload, size_t payload_length,
                                  fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    return fwu_frame_encode(type, seq, payload, payload_length, out, out_capacity, out_length);
}

/** Response with a 1-byte placeholder field (messages that carry no data). */
static fwu_return_e session_reply_byte(fwu_byte_t type, fwu_byte_t seq, fwu_byte_t value,
                                       fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    const fwu_byte_t payload[1] = { value };
    return session_reply(type, seq, payload, 1u, out, out_capacity, out_length);
}

static fwu_return_e session_reply_u32(fwu_byte_t type, fwu_byte_t seq, uint32_t value,
                                      fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_byte_t payload[FWU_PROTO_U32_SIZE];
    size_t length = 0u;
    const fwu_return_e status = fwu_proto_encode_u32(value, payload, FWU_PROTO_U32_SIZE, &length);

    if(status != FWU_OK) {
        return status;
    }
    return session_reply(type, seq, payload, length, out, out_capacity, out_length);
}

/** ERROR response: [code][request type]. Does NOT change the state (changing it is the caller's job). */
static fwu_return_e session_reply_error(fwu_return_e code, const fwu_frame_view_t *request,
                                        fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_byte_t payload[FWU_PROTO_ERROR_SIZE];
    size_t length = 0u;
    const fwu_return_e status = fwu_proto_encode_error(session_code(code), request->type, payload, FWU_PROTO_ERROR_SIZE, &length);

    if(status != FWU_OK) {
        return status;
    }
    return session_reply(FWU_MSG_ERROR, request->seq, payload, length, out, out_capacity, out_length);
}

/** Puts the session into IDLE: download progress and the pending challenge are forgotten (the lockout state is kept). */
static void session_reset(fwu_session_t *session)
{
    session->state = FWU_SESSION_IDLE;
    session->image_size = 0u;
    session->next_offset = 0u;
    session->last_chunk_offset = 0u;
    session->last_chunk_length = 0u;
    session->challenge_length = 0u;
}

/** Are the `length` elements in flash the same as `expected`? Read piece by piece with the `work` buffer. FLASH_WRITE = mismatch. */
static fwu_return_e session_flash_matches(const fwu_session_t *session, uint32_t offset, const fwu_byte_t *expected, uint32_t length)
{
    uint32_t done = 0u;

    while(done < length) {
        uint32_t part = length - done;
        fwu_return_e status;

        if(part > session->config.work_length) {
            part = session->config.work_length;
        }
        status = fwu_flash_read(session->config.flash, offset + done, part, session->config.work);
        if(status != FWU_OK) {
            return status;
        }
        for(uint32_t i = 0u; i < part; i++) {
            if((session->config.work[i] & FWU_BYTE_MASK) != (expected[done + i] & FWU_BYTE_MASK)) {
                return FWU_ERR_FLASH_WRITE;
            }
        }
        done += part;
    }
    return FWU_OK;
}

/** Write and read back (a silent write error is caught immediately as FLASH_WRITE). */
static fwu_return_e session_write_checked(const fwu_session_t *session, uint32_t offset, const fwu_byte_t *data, uint32_t length)
{
    const fwu_return_e status = fwu_flash_write(session->config.flash, offset, length, data);

    if(status != FWU_OK) {
        return status;
    }
    return session_flash_matches(session, offset, data, length);
}

/** Whether the payload is byte-for-byte identical to the accepted manifest in manifest_raw. */
static bool session_same_manifest(const fwu_session_t *session, const fwu_byte_t *payload)
{
    for(uint32_t i = 0u; i < FWU_MANIFEST_SIZE; i++) {
        if((payload[i] & FWU_BYTE_MASK) != (session->manifest_raw[i] & FWU_BYTE_MASK)) {
            return false;
        }
    }
    return true;
}

static bool session_started(const fwu_session_t *session)
{
    return (session->state == FWU_SESSION_AUTH_PENDING) || (session->state == FWU_SESSION_AUTHENTICATED) ||
           (session->state == FWU_SESSION_DOWNLOADING) || (session->state == FWU_SESSION_READY);
}

static uint16_t session_timeout_seconds(const fwu_session_t *session)
{
    const uint32_t seconds = session->config.idle_timeout_ms / 1000u;

    if(seconds > 0xFFFFu) {
        return 0xFFFFu;
    }
    return (uint16_t)seconds;
}

/* ------------------------------------------------------------------ init */

fwu_return_e fwu_session_init(fwu_session_t *session, const fwu_session_config_t *config)
{
    const fwu_verify_config_t *verify;
    uint32_t page_length;
    uint32_t granule;
    uint32_t flash_length;

    if((session == NULL) || (config == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    session->initialized = false;
    if((config->flash == NULL) || (config->activate == NULL) || (config->verify == NULL) ||
       (config->work == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(config->auth != NULL) {
        if((config->auth->begin == NULL) || (config->auth->check == NULL) || (config->auth_max_failures == 0u) ||
           (config->auth_lockout_ms == 0u)) {
            return FWU_ERR_INVALID_ARG;
        }
    }
    else if(config->require_auth) {
        return FWU_ERR_INVALID_ARG;
    }
    if(fwu_flash_validate(config->flash) != FWU_OK) {
        return FWU_ERR_FLASH_GEOMETRY;
    }

    page_length = config->flash->geometry.page_length;
    granule = config->flash->geometry.write_granule;
    flash_length = config->flash->geometry.flash_length;
    verify = config->verify;

    if(granule > FWU_SESSION_MAX_GRANULE) {
        return FWU_ERR_FLASH_GEOMETRY;
    }
    if((config->chunk_size == 0u) || (config->chunk_size > FWU_PROTO_MAX_CHUNK) ||
       ((config->chunk_size % granule) != 0u) || ((page_length % config->chunk_size) != 0u) ||
       (config->work_length < config->chunk_size) || (config->idle_timeout_ms == 0u)) {
        return FWU_ERR_INVALID_ARG;
    }

    /* Manifest page and image region: page aligned, inside flash, not overlapping each other; the manifest rounded up to the granule fits. */
    if(((verify->manifest_offset % page_length) != 0u) || ((verify->image_offset % page_length) != 0u) ||
       (verify->max_image_size == 0u) || ((verify->max_image_size % page_length) != 0u) ||
       (session_round_up(FWU_MANIFEST_SIZE, granule) > page_length)) {
        return FWU_ERR_INVALID_ARG;
    }
    if((verify->manifest_offset > (flash_length - page_length)) ||
       (verify->image_offset > flash_length) || (verify->max_image_size > (flash_length - verify->image_offset))) {
        return FWU_ERR_OUT_OF_RANGE;
    }
    if((verify->manifest_offset >= verify->image_offset) ?
       (verify->manifest_offset < (verify->image_offset + verify->max_image_size)) :
       ((verify->manifest_offset + page_length) > verify->image_offset)) {
        return FWU_ERR_INVALID_ARG;
    }

    session->config = *config;
    session->page_length = page_length;
    session->granule = granule;
    session->last_activity_ms = 0u;
    session->auth_failures = 0u;
    session->auth_seq = 0u;
    session->locked = false;
    session->lock_start_ms = 0u;
    session->pending_seq = 0u;
    session->reboot_requested = false;
    session_reset(session);
    session->initialized = true;
    return FWU_OK;
}

/* ------------------------------------------------------------------ message handlers */

#define SESSION_ARGS const fwu_frame_view_t *request, uint32_t now_ms, fwu_byte_t *out, size_t out_capacity, size_t *out_length

static fwu_return_e handle_hello(fwu_session_t *session, SESSION_ARGS)
{
    fwu_proto_info_t info;
    fwu_byte_t payload[FWU_PROTO_INFO_SIZE];
    size_t length = 0u;
    fwu_return_e status;

    (void)now_ms;
    if(request->payload_length != 1u) {
        return session_reply_error(FWU_ERR_BAD_LENGTH, request, out, out_capacity, out_length);
    }
    info.proto_version = (fwu_byte_t)FWU_PROTO_VERSION;
    info.flags = (fwu_byte_t)(((session->config.auth != NULL) ? FWU_PROTO_FLAG_AUTH_REQUIRED : 0u) |
                              (session->config.verify->require_signature ? FWU_PROTO_FLAG_SIGNED : 0u));
    info.product_id = session->config.verify->product_id;
    info.running_image_version = session->config.running_image_version;
    info.max_chunk = (uint16_t)session->config.chunk_size;
    info.state = (fwu_byte_t)session->state;
    status = fwu_proto_encode_info(&info, payload, FWU_PROTO_INFO_SIZE, &length);
    if(status != FWU_OK) {
        return status;
    }
    return session_reply(FWU_MSG_INFO, request->seq, payload, length, out, out_capacity, out_length);
}

static fwu_return_e handle_keepalive(fwu_session_t *session, SESSION_ARGS)
{
    (void)now_ms;
    if(request->payload_length != 1u) {
        return session_reply_error(FWU_ERR_BAD_LENGTH, request, out, out_capacity, out_length);
    }
    return session_reply_byte(FWU_MSG_KEEPALIVE_OK, request->seq, (fwu_byte_t)session->state, out, out_capacity, out_length);
}

static fwu_return_e handle_auth_req(fwu_session_t *session, SESSION_ARGS)
{
    uint32_t length = 0u;
    fwu_return_e status;

    if((session->config.auth == NULL) ||
       ((session->state != FWU_SESSION_IDLE) && (session->state != FWU_SESSION_AUTH_PENDING))) {
        return session_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);
    }
    if(request->payload_length != 1u) {
        return session_reply_error(FWU_ERR_BAD_LENGTH, request, out, out_capacity, out_length);
    }
    if(session->locked) {
        if((uint32_t)(now_ms - session->lock_start_ms) < session->config.auth_lockout_ms) {
            return session_reply_error(FWU_ERR_LOCKED_OUT, request, out, out_capacity, out_length);
        }
        session->locked = false;                    /* time has elapsed: start clean */
        session->auth_failures = 0u;
    }

    if((session->state == FWU_SESSION_AUTH_PENDING) && ((request->seq & FWU_BYTE_MASK) == session->auth_seq)) {
        /* Repeat of the same AUTH_REQ (challenge lost / delayed copy): the SAME challenge is given again, a new one is not generated. */
        return session_reply(FWU_MSG_AUTH_CHALLENGE, request->seq, session->challenge, session->challenge_length, out, out_capacity, out_length);
    }
    status = session->config.auth->begin(session->config.auth->context, session->challenge, FWU_PROTO_AUTH_CHALLENGE_MAX, &length);
    if((status == FWU_OK) && ((length == 0u) || (length > FWU_PROTO_AUTH_CHALLENGE_MAX))) {
        status = FWU_ERR_INVALID_ARG;               /* the hook violated the contract */
    }
    if(status != FWU_OK) {
        session_reset(session);
        return session_reply_error(status, request, out, out_capacity, out_length);
    }
    session->challenge_length = length;
    session->auth_seq = (fwu_byte_t)(request->seq & FWU_BYTE_MASK);
    session->state = FWU_SESSION_AUTH_PENDING;
    return session_reply(FWU_MSG_AUTH_CHALLENGE, request->seq, session->challenge, length, out, out_capacity, out_length);
}

static fwu_return_e handle_auth_resp(fwu_session_t *session, SESSION_ARGS)
{
    fwu_return_e status;

    if((session->state != FWU_SESSION_AUTH_PENDING) && (session->state != FWU_SESSION_AUTHENTICATED)) {
        return session_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);
    }
    if((request->payload_length < 1u) || (request->payload_length > FWU_PROTO_AUTH_RESPONSE_MAX)) {
        return session_reply_error(FWU_ERR_BAD_LENGTH, request, out, out_capacity, out_length);
    }
    status = session->config.auth->check(session->config.auth->context, session->challenge, session->challenge_length,
                                         request->payload, (uint32_t)request->payload_length);
    if(session->state == FWU_SESSION_AUTHENTICATED) {
        /* AUTH_OK was lost, the host repeated AUTH_RESP: only a CORRECT response is re-acknowledged (state and counter unchanged). */
        if(status == FWU_OK) {
            return session_reply_byte(FWU_MSG_AUTH_OK, request->seq, FWU_PROTO_PLACEHOLDER, out, out_capacity, out_length);
        }
        return session_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);
    }
    if(status == FWU_OK) {
        session->auth_failures = 0u;
        session->state = FWU_SESSION_AUTHENTICATED;      /* the challenge is kept in AUTHENTICATED for re-acknowledgement; it is cleared when the download starts */
        return session_reply_byte(FWU_MSG_AUTH_OK, request->seq, FWU_PROTO_PLACEHOLDER, out, out_capacity, out_length);
    }

    /* Failed: the challenge was single-use; return to IDLE, increment the counter, lock out if necessary. */
    session->auth_failures++;
    if(session->auth_failures >= session->config.auth_max_failures) {
        session->locked = true;
        session->lock_start_ms = now_ms;
    }
    session_reset(session);
    return session_reply_error(status, request, out, out_capacity, out_length);
}

static fwu_return_e handle_request_download(fwu_session_t *session, SESSION_ARGS)
{
    fwu_proto_accepted_t accepted;
    fwu_byte_t payload[FWU_PROTO_ACCEPTED_SIZE];
    size_t length = 0u;
    fwu_return_e status;
    const bool fresh = (session->state == FWU_SESSION_AUTHENTICATED) ||
                       ((session->state == FWU_SESSION_IDLE) && (session->config.auth == NULL));

    (void)now_ms;
    if(!fresh && (session->state != FWU_SESSION_DOWNLOADING)) {
        return session_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);
    }
    if(request->payload_length != FWU_MANIFEST_SIZE) {
        return session_reply_error(FWU_ERR_BAD_LENGTH, request, out, out_capacity, out_length);
    }

    if(!fresh) {
        /* RESUME: if it is the same manifest, report the offset where it left off without touching flash; if different, ABORT is needed first. */
        if(!session_same_manifest(session, request->payload)) {
            return session_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);
        }
    }
    else {
        fwu_manifest_t manifest;
        uint32_t padded;

        /* 1. If an activation is pending, staging is not touched (it may be the source). */
        if(!fwu_activate_staging_writable(session->config.activate)) {
            return session_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);
        }
        /* 2. The offer is verified in RAM; if it does not pass, NOT A SINGLE BYTE of flash is touched. */
        status = fwu_verify_offer(session->config.verify, request->payload, FWU_MANIFEST_SIZE, &manifest);
        if(status != FWU_OK) {
            return session_reply_error(status, request, out, out_capacity, out_length);
        }
        /* 3. Erase the manifest page and write the manifest rounded up to the granule (read back and verified). */
        padded = session_round_up(FWU_MANIFEST_SIZE, session->granule);
        for(uint32_t i = 0u; i < padded; i++) {
            session->manifest_raw[i] = (i < FWU_MANIFEST_SIZE) ? (fwu_byte_t)(request->payload[i] & FWU_BYTE_MASK)
                                                                : (fwu_byte_t)(session->config.flash->geometry.erased_value & FWU_BYTE_MASK);
        }
        status = fwu_flash_erase(session->config.flash, session->config.verify->manifest_offset, session->page_length);
        if(status == FWU_OK) {
            status = session_write_checked(session, session->config.verify->manifest_offset, session->manifest_raw, padded);
        }
        if(status != FWU_OK) {
            return session_reply_error(status, request, out, out_capacity, out_length);
        }
        session->manifest = manifest;
        session->image_size = manifest.image_size;
        session->next_offset = 0u;
        session->last_chunk_offset = 0u;
        session->last_chunk_length = 0u;
        session->challenge_length = 0u;
        session->state = FWU_SESSION_DOWNLOADING;
    }

    accepted.chunk_size = (uint16_t)session->config.chunk_size;
    accepted.mode = (fwu_byte_t)FWU_PROTO_MODE_HANDSHAKE;
    accepted.timeout_s = session_timeout_seconds(session);
    accepted.next_offset = session->next_offset;
    status = fwu_proto_encode_accepted(&accepted, payload, FWU_PROTO_ACCEPTED_SIZE, &length);
    if(status != FWU_OK) {
        return status;
    }
    return session_reply(FWU_MSG_DOWNLOAD_ACCEPTED, request->seq, payload, length, out, out_capacity, out_length);
}

/**
 * Writes a data chunk to staging (the COMMON body of the TRANSFER_DATA handler and fwu_session_push_chunk: both paths
 * apply the same rules, so the "framed" and "frameless" write cannot diverge).
 * If the return is FWU_OK, *next_offset is the current next offset. Error codes: BAD_STATE (not DOWNLOADING), BAD_LENGTH (chunk size
 * or the end-of-image rule), BAD_BLOCK (out-of-order offset or a repeated chunk whose content differs), flash errors (the session drops
 * to IDLE: the host starts over). BAD_STATE/BAD_LENGTH/BAD_BLOCK do NOT change the session.
 */
static fwu_return_e session_store_chunk(fwu_session_t *session, uint32_t offset, const fwu_byte_t *data, size_t data_length,
                                        uint32_t *next_offset)
{
    uint32_t image_offset;
    uint32_t head;
    uint32_t tail;
    fwu_return_e status;

    if(session->state != FWU_SESSION_DOWNLOADING) {
        return FWU_ERR_BAD_STATE;
    }
    if((data == NULL) || (data_length == 0u) || (data_length > session->config.chunk_size)) {
        return FWU_ERR_BAD_LENGTH;
    }
    image_offset = session->config.verify->image_offset;

    /* Exact repeat of the previous chunk (acknowledgement lost): re-acknowledge without writing; BAD_BLOCK if the content differs. */
    if((session->last_chunk_length != 0u) && (offset == session->last_chunk_offset) &&
       (data_length == session->last_chunk_length)) {
        status = session_flash_matches(session, image_offset + offset, data, (uint32_t)data_length);
        if(status == FWU_ERR_FLASH_WRITE) {
            return FWU_ERR_BAD_BLOCK;
        }
        if(status != FWU_OK) {
            session_reset(session);
            return status;
        }
        *next_offset = session->next_offset;
        return FWU_OK;
    }
    if(offset != session->next_offset) {
        return FWU_ERR_BAD_BLOCK;
    }
    /* Overflow-free: offset + length cannot exceed the image size; only the LAST chunk may be short. */
    if((offset > session->image_size) || (data_length > (session->image_size - offset)) ||
       ((data_length != session->config.chunk_size) && ((offset + data_length) != session->image_size))) {
        return FWU_ERR_BAD_LENGTH;
    }

    /* On the first chunk of a page, that page is erased. */
    status = FWU_OK;
    if((offset % session->page_length) == 0u) {
        status = fwu_flash_erase(session->config.flash, image_offset + offset, session->page_length);
    }
    /* Whole granules are written directly; the remaining short tail is written padded to the granule with 0xFF. */
    head = (uint32_t)data_length - ((uint32_t)data_length % session->granule);
    tail = (uint32_t)data_length - head;
    if((status == FWU_OK) && (head > 0u)) {
        status = session_write_checked(session, image_offset + offset, data, head);
    }
    if((status == FWU_OK) && (tail > 0u)) {
        for(uint32_t i = 0u; i < session->granule; i++) {
            session->pad[i] = (i < tail) ? (fwu_byte_t)(data[head + i] & FWU_BYTE_MASK)
                                         : (fwu_byte_t)(session->config.flash->geometry.erased_value & FWU_BYTE_MASK);
        }
        status = session_write_checked(session, image_offset + offset + head, session->pad, session->granule);
    }
    if(status != FWU_OK) {
        session_reset(session);                     /* a flash error ends the session; the host starts over */
        return status;
    }

    session->last_chunk_offset = offset;
    session->last_chunk_length = (uint32_t)data_length;
    session->next_offset = offset + (uint32_t)data_length;
    *next_offset = session->next_offset;
    return FWU_OK;
}

static fwu_return_e handle_transfer_data(fwu_session_t *session, SESSION_ARGS)
{
    fwu_proto_data_t chunk;
    uint32_t next = 0u;
    fwu_return_e status;

    (void)now_ms;
    if(session->state != FWU_SESSION_DOWNLOADING) {
        return session_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);
    }
    status = fwu_proto_parse_data(request->payload, request->payload_length, &chunk);
    if((status != FWU_OK) || (chunk.data_length > session->config.chunk_size)) {
        return session_reply_error(FWU_ERR_BAD_LENGTH, request, out, out_capacity, out_length);
    }
    status = session_store_chunk(session, chunk.offset, chunk.data, chunk.data_length, &next);
    if(status != FWU_OK) {
        return session_reply_error(status, request, out, out_capacity, out_length);
    }
    return session_reply_u32(FWU_MSG_DATA_ACK, request->seq, next, out, out_capacity, out_length);
}

/** Common payload check of TRANSFER_EXIT and ACTIVATE: 4 bytes and the value equals the image size in the manifest. */
static fwu_return_e session_check_size_claim(const fwu_session_t *session, const fwu_frame_view_t *request)
{
    uint32_t claimed = 0u;
    const fwu_return_e status = fwu_proto_parse_u32(request->payload, request->payload_length, &claimed);

    if(status != FWU_OK) {
        return status;
    }
    return (claimed == session->image_size) ? FWU_OK : FWU_ERR_BAD_BLOCK;
}

static fwu_return_e handle_transfer_exit(fwu_session_t *session, SESSION_ARGS)
{
    fwu_return_e status;

    (void)now_ms;
    if((session->state != FWU_SESSION_DOWNLOADING) && (session->state != FWU_SESSION_VERIFYING) &&
       (session->state != FWU_SESSION_READY)) {
        return session_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);
    }
    status = session_check_size_claim(session, request);
    if((status == FWU_OK) && (session->state == FWU_SESSION_DOWNLOADING)) {
        status = (session->next_offset == session->image_size) ? FWU_OK : FWU_ERR_BAD_BLOCK;   /* has the whole image been received */
    }
    if(status != FWU_OK) {
        return session_reply_error(status, request, out, out_capacity, out_length);
    }
    if(session->state == FWU_SESSION_READY) {
        /* REPEAT (response lost): verification already passed; give the result again, nothing is recomputed. */
        return session_reply_byte(FWU_MSG_VERIFY_RESULT, request->seq, 0u, out, out_capacity, out_length);
    }
    session->pending_seq = request->seq;            /* repeat in VERIFYING: the result carries the SEQ of the LATEST request */
    session->state = FWU_SESSION_VERIFYING;
    return session_reply_byte(FWU_MSG_EXIT_PENDING, request->seq, 1u, out, out_capacity, out_length);
}

static fwu_return_e handle_activate(fwu_session_t *session, SESSION_ARGS)
{
    fwu_return_e status;

    (void)now_ms;
    if((session->state != FWU_SESSION_READY) && (session->state != FWU_SESSION_COMMITTED)) {
        return session_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);
    }
    status = session_check_size_claim(session, request);
    if(status != FWU_OK) {
        return session_reply_error(status, request, out, out_capacity, out_length);
    }
    if(session->state == FWU_SESSION_COMMITTED) {
        /* REPEAT (response lost): the commit was already written; it is not written a second time, the same acknowledgement is given. */
        return session_reply_byte(FWU_MSG_ACTIVATE_OK, request->seq, FWU_PROTO_PLACEHOLDER, out, out_capacity, out_length);
    }
    status = fwu_activate_commit(session->config.activate, (session->image_size + session->page_length - 1u) / session->page_length);
    if(status != FWU_OK) {
        session_reset(session);                     /* the activation object may be unreliable: the session ends */
        return session_reply_error(status, request, out, out_capacity, out_length);
    }
    session->state = FWU_SESSION_COMMITTED;
    session->reboot_requested = true;
    return session_reply_byte(FWU_MSG_ACTIVATE_OK, request->seq, FWU_PROTO_PLACEHOLDER, out, out_capacity, out_length);
}

static fwu_return_e handle_abort(fwu_session_t *session, SESSION_ARGS)
{
    (void)now_ms;
    if(session->state == FWU_SESSION_COMMITTED) {
        return session_reply_error(FWU_ERR_BAD_STATE, request, out, out_capacity, out_length);
    }
    if(request->payload_length != 1u) {
        return session_reply_error(FWU_ERR_BAD_LENGTH, request, out, out_capacity, out_length);
    }
    session_reset(session);
    return session_reply_byte(FWU_MSG_ABORT_OK, request->seq, FWU_PROTO_PLACEHOLDER, out, out_capacity, out_length);
}

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_session_on_frame(fwu_session_t *session, const fwu_frame_view_t *request, uint32_t now_ms,
                                  fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    if((session == NULL) || (request == NULL) || (out == NULL) || (out_length == NULL) || (!session->initialized) ||
       (request->payload == NULL) || (request->payload_length == 0u) || (request->payload_length > FWU_FRAME_MAX_PAYLOAD)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(out_capacity < FWU_SESSION_MIN_OUT) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    session->last_activity_ms = now_ms;

    switch(request->type & FWU_BYTE_MASK) {
    case FWU_MSG_HELLO:
        return handle_hello(session, request, now_ms, out, out_capacity, out_length);
    case FWU_MSG_KEEPALIVE:
        return handle_keepalive(session, request, now_ms, out, out_capacity, out_length);
    case FWU_MSG_AUTH_REQ:
        return handle_auth_req(session, request, now_ms, out, out_capacity, out_length);
    case FWU_MSG_AUTH_RESP:
        return handle_auth_resp(session, request, now_ms, out, out_capacity, out_length);
    case FWU_MSG_REQUEST_DOWNLOAD:
        return handle_request_download(session, request, now_ms, out, out_capacity, out_length);
    case FWU_MSG_TRANSFER_DATA:
        return handle_transfer_data(session, request, now_ms, out, out_capacity, out_length);
    case FWU_MSG_TRANSFER_EXIT:
        return handle_transfer_exit(session, request, now_ms, out, out_capacity, out_length);
    case FWU_MSG_ACTIVATE:
        return handle_activate(session, request, now_ms, out, out_capacity, out_length);
    case FWU_MSG_ABORT:
        return handle_abort(session, request, now_ms, out, out_capacity, out_length);
    default:
        return session_reply_error(FWU_ERR_UNKNOWN_MESSAGE, request, out, out_capacity, out_length);
    }
}

fwu_return_e fwu_session_poll(fwu_session_t *session, uint32_t now_ms, fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    if((session == NULL) || (out == NULL) || (out_length == NULL) || (!session->initialized)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(out_capacity < FWU_SESSION_MIN_OUT) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    *out_length = 0u;

    if(session->state == FWU_SESSION_VERIFYING) {
        /* Pending verification: image + manifest + signature + SHA-256 (blocking, may take a long time). */
        const fwu_return_e status = fwu_verify_image(session->config.verify, session->config.flash, NULL);

        if(status == FWU_OK) {
            session->state = FWU_SESSION_READY;
        }
        else {
            session_reset(session);                 /* invalid image or verification could not be performed: the session ends */
        }
        session->last_activity_ms = now_ms;
        return session_reply_byte(FWU_MSG_VERIFY_RESULT, session->pending_seq, (fwu_byte_t)((status == FWU_OK) ? 0u : (unsigned)session_code(status)),
                                  out, out_capacity, out_length);
    }

    if(session_started(session) && ((uint32_t)(now_ms - session->last_activity_ms) >= session->config.idle_timeout_ms)) {
        session_reset(session);                     /* went idle: the session ends, the running version is not affected */
    }
    return FWU_OK;
}

fwu_return_e fwu_session_push_chunk(fwu_session_t *session, uint32_t offset, const fwu_byte_t *data, size_t length,
                                    uint32_t now_ms, uint32_t *next_offset)
{
    if((session == NULL) || (data == NULL) || (next_offset == NULL) || (!session->initialized)) {
        return FWU_ERR_INVALID_ARG;
    }
    session->last_activity_ms = now_ms;             /* so that the idle timeout does not fire during a long pull */
    return session_store_chunk(session, offset, data, length, next_offset);
}

void fwu_session_touch(fwu_session_t *session, uint32_t now_ms)
{
    if((session != NULL) && session->initialized) {
        session->last_activity_ms = now_ms;
    }
}

uint32_t fwu_session_get_image_size(const fwu_session_t *session)
{
    return (session != NULL) ? session->image_size : 0u;
}

uint32_t fwu_session_get_next_offset(const fwu_session_t *session)
{
    return (session != NULL) ? session->next_offset : 0u;
}

uint32_t fwu_session_get_chunk_size(const fwu_session_t *session)
{
    return (session != NULL) ? session->config.chunk_size : 0u;
}

fwu_session_state_e fwu_session_get_state(const fwu_session_t *session)
{
    return (session != NULL) ? session->state : FWU_SESSION_IDLE;
}

bool fwu_session_reboot_requested(const fwu_session_t *session)
{
    return (session != NULL) && session->reboot_requested;
}
