/**
 * @file    fwu_client.c
 * @brief   Host-side update client implementation (see fwu_client.h for the flow, the error tolerance and the limits).
 */

#include <stddef.h>
#include "fwu_client.h"

/* ------------------------------------------------------------------ helpers */

static bool client_running(const fwu_client_t *client)
{
    return (client->state != FWU_CLIENT_IDLE) && (client->state != FWU_CLIENT_DONE) && (client->state != FWU_CLIENT_FAILED);
}

static void client_fail(fwu_client_t *client, fwu_client_failure_e failure, fwu_return_e device_error)
{
    client->state = FWU_CLIENT_FAILED;
    client->failure = failure;
    client->device_error = device_error;
    client->awaiting = false;
    client->send_pending = false;
}

/** Sets up a new request (new SEQ); poll does the sending. An encoding error is a PROTOCOL failure. */
static void client_issue(fwu_client_t *client, fwu_byte_t type, const fwu_byte_t *payload, size_t payload_length)
{
    client->req_seq = client->next_seq;
    client->next_seq = (fwu_byte_t)((client->next_seq + 1u) & 0xFFu);
    if(fwu_frame_encode(type, client->req_seq, payload, payload_length, client->req_frame, FWU_FRAME_MAX_SIZE,
                        &client->req_length) != FWU_OK) {
        client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);
        return;
    }
    client->req_type = type;
    client->awaiting = true;
    client->send_pending = true;
    client->retries = 0u;
}

static void client_issue_placeholder(fwu_client_t *client, fwu_byte_t type)
{
    const fwu_byte_t payload[1] = { (fwu_byte_t)FWU_PROTO_PLACEHOLDER };

    client_issue(client, type, payload, 1u);
}

static void client_issue_u32(fwu_client_t *client, fwu_byte_t type, uint32_t value)
{
    fwu_byte_t payload[FWU_PROTO_U32_SIZE];
    size_t length = 0u;

    (void)fwu_proto_encode_u32(value, payload, FWU_PROTO_U32_SIZE, &length);
    client_issue(client, type, payload, length);
}

/** Next transfer step: EXIT if the image is finished, otherwise the next chunk. */
static void client_transfer_step(fwu_client_t *client)
{
    fwu_byte_t payload[FWU_FRAME_MAX_PAYLOAD];
    size_t length = 0u;
    uint32_t chunk;

    if(client->acked >= client->config.image_size) {
        client->state = FWU_CLIENT_EXIT;
        client_issue_u32(client, FWU_MSG_TRANSFER_EXIT, client->config.image_size);
        return;
    }
    chunk = client->config.image_size - client->acked;
    if(chunk > client->chunk_size) {
        chunk = client->chunk_size;
    }
    client->last_offset = client->acked;
    client->last_length = chunk;
    client->state = FWU_CLIENT_TRANSFER;
    if(fwu_proto_encode_data(client->acked, &client->config.image[client->acked], chunk, payload, FWU_FRAME_MAX_PAYLOAD, &length) != FWU_OK) {
        client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);
        return;
    }
    client_issue(client, FWU_MSG_TRANSFER_DATA, payload, length);
}

static void client_begin_hello(fwu_client_t *client)
{
    const fwu_byte_t payload[1] = { (fwu_byte_t)FWU_PROTO_VERSION };

    client->state = FWU_CLIENT_HELLO;
    client_issue(client, FWU_MSG_HELLO, payload, 1u);
}

/** The device session was lost: start from the beginning (HELLO); if the allowance is used up, fail with BAD_STATE. */
static void client_restart(fwu_client_t *client)
{
    if(client->restarts >= client->config.max_restarts) {
        client_fail(client, FWU_CLIENT_FAIL_DEVICE_ERROR, FWU_ERR_BAD_STATE);
        return;
    }
    client->restarts++;
    client->acked = 0u;
    client_begin_hello(client);
}

/** Is pull mode on (the device pulls the image from the server)? */
static bool client_pull_mode(const fwu_client_t *client)
{
    return client->config.pull_target != NULL;
}

/** Sets up the PULL_START request (in pull mode, after DOWNLOAD_ACCEPTED). */
static void client_begin_pull(fwu_client_t *client, uint32_t now_ms)
{
    fwu_byte_t payload[FWU_FRAME_MAX_PAYLOAD];
    size_t length = 0u;

    if(fwu_proto_encode_pull_start(client->config.pull_ip, client->config.pull_port, client->config.pull_target,
                                   client->config.pull_target_length, payload, FWU_FRAME_MAX_PAYLOAD, &length) != FWU_OK) {
        client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);      /* validated in init: must not get here */
        return;
    }
    if(client->state != FWU_CLIENT_PULL_START) {
        client->pull_start_ms = now_ms;
    }
    client->state = FWU_CLIENT_PULL_START;
    client_issue(client, FWU_MSG_PULL_START, payload, length);
}

/** Resends REQUEST_DOWNLOAD: the device reports the offset where it left off (resume / resynchronization). */
static void client_begin_request(fwu_client_t *client)
{
    client->state = FWU_CLIENT_REQUEST;
    client_issue(client, FWU_MSG_REQUEST_DOWNLOAD, client->config.manifest, FWU_MANIFEST_SIZE);
}

static bool client_type_ok(const fwu_client_t *client, fwu_byte_t type)
{
    if(type == FWU_MSG_ERROR) {
        return true;
    }
    if(client->req_type == FWU_MSG_TRANSFER_EXIT) {
        return (type == FWU_MSG_EXIT_PENDING) || (type == FWU_MSG_VERIFY_RESULT);
    }
    return type == (fwu_byte_t)(client->req_type | FWU_PROTO_RESPONSE_FLAG);
}

/** Handling of a PULL_STARTED / PULL_STATUS response (progresses according to the device's pull state). */
static void client_handle_pull_status(fwu_client_t *client, const fwu_frame_view_t *view, uint32_t now_ms)
{
    fwu_proto_pull_status_t status;

    if((fwu_proto_parse_pull_status(view->payload, view->payload_length, &status) != FWU_OK) ||
       (status.next_offset > client->config.image_size)) {
        client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);
        return;
    }
    client->pull_http_status = status.http_status;
    client->acked = status.next_offset;                             /* the offset the device WROTE to flash prevails */
    switch(status.state) {
    case FWU_PROTO_PULL_DONE:
        if(status.next_offset != client->config.image_size) {
            client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);
            return;
        }
        client_transfer_step(client);                               /* acked == image_size: sends TRANSFER_EXIT */
        return;
    case FWU_PROTO_PULL_FAILED:
        client_fail(client, FWU_CLIENT_FAIL_DEVICE_ERROR, (status.error != 0u) ? (fwu_return_e)status.error : FWU_ERR_PULL_PROTOCOL);
        return;
    case FWU_PROTO_PULL_IDLE:
        client_restart(client);                                     /* the device forgot the pull/session (ABORT, reboot): start over */
        return;
    case FWU_PROTO_PULL_REQUESTING:
    case FWU_PROTO_PULL_DOWNLOADING:
    default:
        client->state = FWU_CLIENT_PULL;                            /* the pull is in progress: wait until the next query */
        client->awaiting = false;
        client->send_pending = false;
        client->next_query_ms = now_ms + client->config.pull_poll_ms;
        return;
    }
}

/** ERROR response: depending on the state, continue (resynchronization), start over, or fail finally. */
static void client_handle_error(fwu_client_t *client, const fwu_frame_view_t *view)
{
    fwu_byte_t code = 0u;
    fwu_byte_t request_type = 0u;

    if(fwu_proto_parse_error(view->payload, view->payload_length, &code, &request_type) != FWU_OK) {
        client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);
        return;
    }
    if(client->state == FWU_CLIENT_ABORTING) {
        client_fail(client, FWU_CLIENT_FAIL_CANCELLED, FWU_OK);     /* even if the device rejects the cancel, the cancel ends */
        return;
    }
    if(((client->state == FWU_CLIENT_TRANSFER) || (client->state == FWU_CLIENT_EXIT)) && (code == (fwu_byte_t)FWU_ERR_BAD_BLOCK)) {
        client->resyncs++;
        client_begin_request(client);                               /* the device reports the offset where it left off */
        return;
    }
    if((code == (fwu_byte_t)FWU_ERR_BAD_STATE) && (client->state != FWU_CLIENT_HELLO)) {
        client_restart(client);                                     /* session lost: start over */
        return;
    }
    client_fail(client, FWU_CLIENT_FAIL_DEVICE_ERROR, (fwu_return_e)code);
}

/** Handling in the state machine of a response of the expected type whose SEQ matches. */
static void client_handle_response(fwu_client_t *client, const fwu_frame_view_t *view, uint32_t now_ms)
{
    const fwu_byte_t type = (fwu_byte_t)(view->type & FWU_BYTE_MASK);

    if(type == FWU_MSG_ERROR) {
        client_handle_error(client, view);
        return;
    }

    switch(client->state) {
    case FWU_CLIENT_HELLO: {
        fwu_proto_info_t info;

        if((fwu_proto_parse_info(view->payload, view->payload_length, &info) != FWU_OK) || (info.proto_version != (fwu_byte_t)FWU_PROTO_VERSION) ||
           ((client->config.expected_product != 0u) && (info.product_id != client->config.expected_product))) {
            client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);
            return;
        }
        client->flags = (uint32_t)info.flags;
        if((client->flags & FWU_PROTO_FLAG_AUTH_REQUIRED) != 0u) {
            if(client->config.auth == NULL) {
                client_fail(client, FWU_CLIENT_FAIL_AUTH_UNAVAILABLE, FWU_OK);
                return;
            }
            client->state = FWU_CLIENT_AUTH_REQ;
            client_issue_placeholder(client, FWU_MSG_AUTH_REQ);
            return;
        }
        client_begin_request(client);
        return;
    }
    case FWU_CLIENT_AUTH_REQ: {
        fwu_byte_t response[FWU_PROTO_AUTH_RESPONSE_MAX];
        uint32_t response_length = 0u;

        if((view->payload_length < 1u) || (view->payload_length > FWU_PROTO_AUTH_CHALLENGE_MAX)) {
            client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);
            return;
        }
        if((client->config.auth(client->config.auth_context, view->payload, (uint32_t)view->payload_length, response,
                                FWU_PROTO_AUTH_RESPONSE_MAX, &response_length) != FWU_OK) ||
           (response_length < 1u) || (response_length > FWU_PROTO_AUTH_RESPONSE_MAX)) {
            client_fail(client, FWU_CLIENT_FAIL_AUTH_UNAVAILABLE, FWU_OK);
            return;
        }
        client->state = FWU_CLIENT_AUTH_RESP;
        client_issue(client, FWU_MSG_AUTH_RESP, response, response_length);
        return;
    }
    case FWU_CLIENT_AUTH_RESP:
        client_begin_request(client);
        return;
    case FWU_CLIENT_REQUEST: {
        fwu_proto_accepted_t accepted;

        if((fwu_proto_parse_accepted(view->payload, view->payload_length, &accepted) != FWU_OK) || (accepted.chunk_size == 0u) ||
           ((uint32_t)accepted.chunk_size > FWU_PROTO_MAX_CHUNK) || (accepted.next_offset > client->config.image_size)) {
            client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);
            return;
        }
        client->chunk_size = (uint32_t)accepted.chunk_size;
        client->acked = accepted.next_offset;                       /* the offset reported by the device prevails */
        if(client_pull_mode(client) && (client->acked < client->config.image_size)) {
            client_begin_pull(client, now_ms);                      /* the device pulls the image from the server: no TRANSFER_DATA */
            return;
        }
        client_transfer_step(client);
        return;
    }
    case FWU_CLIENT_PULL_START:
    case FWU_CLIENT_PULL:
        client_handle_pull_status(client, view, now_ms);
        return;
    case FWU_CLIENT_TRANSFER: {
        uint32_t next = 0u;

        if((fwu_proto_parse_u32(view->payload, view->payload_length, &next) != FWU_OK) || (next > client->config.image_size)) {
            client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);
            return;
        }
        if(next != (client->last_offset + client->last_length)) {
            client->resyncs++;                                      /* the device reported a different offset: follow it */
        }
        client->acked = next;
        client_transfer_step(client);
        return;
    }
    case FWU_CLIENT_EXIT:
    case FWU_CLIENT_VERIFY_WAIT:
        if(type == FWU_MSG_EXIT_PENDING) {
            if(client->state == FWU_CLIENT_EXIT) {
                client->state = FWU_CLIENT_VERIFY_WAIT;             /* verification in progress: the result arrives as a separate message */
                client->verify_start_ms = now_ms;
            }
            return;                                                 /* repeat in VERIFY_WAIT: ignored */
        }
        {
            fwu_byte_t code = 0u;

            if(fwu_proto_parse_result(view->payload, view->payload_length, &code) != FWU_OK) {
                client_fail(client, FWU_CLIENT_FAIL_PROTOCOL, FWU_OK);
                return;
            }
            if(code != 0u) {
                client->verify_code = code;
                client_fail(client, FWU_CLIENT_FAIL_REJECTED, FWU_OK);
                return;
            }
            client->state = FWU_CLIENT_ACTIVATE;
            client_issue_u32(client, FWU_MSG_ACTIVATE, client->config.image_size);
        }
        return;
    case FWU_CLIENT_ACTIVATE:
        client->state = FWU_CLIENT_DONE;
        client->awaiting = false;
        client->send_pending = false;
        return;
    case FWU_CLIENT_ABORTING:
        client_fail(client, FWU_CLIENT_FAIL_CANCELLED, FWU_OK);
        return;
    case FWU_CLIENT_IDLE:
    case FWU_CLIENT_DONE:
    case FWU_CLIENT_FAILED:
    default:
        return;
    }
}

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_client_init(fwu_client_t *client, const fwu_client_config_t *config)
{
    fwu_manifest_t manifest;

    if((client == NULL) || (config == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    client->initialized = false;
    if((config->manifest == NULL) || ((config->image == NULL) && (config->pull_target == NULL)) || (config->image_size == 0u) ||
       (config->response_timeout_ms == 0u) || (config->verify_timeout_ms < config->response_timeout_ms)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(config->pull_target != NULL) {
        /* The pull configuration is checked up front: PULL_START must be encodable (valid target, port) and the query interval > 0. */
        fwu_byte_t probe[FWU_FRAME_MAX_PAYLOAD];
        size_t probe_length = 0u;

        if((config->pull_poll_ms == 0u) ||
           (fwu_proto_encode_pull_start(config->pull_ip, config->pull_port, config->pull_target, config->pull_target_length,
                                        probe, FWU_FRAME_MAX_PAYLOAD, &probe_length) != FWU_OK)) {
            return FWU_ERR_INVALID_ARG;
        }
    }
    /* The manifest structure and the size consistency are checked up front (a faulty configuration never goes to the device). */
    if((fwu_manifest_parse(config->manifest, FWU_MANIFEST_SIZE, &manifest) != FWU_OK) || (manifest.image_size != config->image_size)) {
        return FWU_ERR_INVALID_ARG;
    }
    client->config = *config;
    client->state = FWU_CLIENT_IDLE;
    client->failure = FWU_CLIENT_FAIL_NONE;
    client->device_error = FWU_OK;
    client->verify_code = 0u;
    client->chunk_size = 0u;
    client->flags = 0u;
    client->acked = 0u;
    client->last_offset = 0u;
    client->last_length = 0u;
    client->awaiting = false;
    client->send_pending = false;
    client->cancel_requested = false;
    client->req_type = 0u;
    client->req_seq = 0u;
    client->next_seq = 1u;
    client->req_length = 0u;
    client->sent_ms = 0u;
    client->retries = 0u;
    client->verify_start_ms = 0u;
    client->frames_sent = 0u;
    client->retransmits = 0u;
    client->restarts = 0u;
    client->resyncs = 0u;
    client->ignored = 0u;
    client->next_query_ms = 0u;
    client->pull_start_ms = 0u;
    client->pull_http_status = 0u;
    client->initialized = true;
    return FWU_OK;
}

fwu_return_e fwu_client_start(fwu_client_t *client, uint32_t now_ms)
{
    (void)now_ms;
    if((client == NULL) || (!client->initialized)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(client->state != FWU_CLIENT_IDLE) {
        return FWU_ERR_BAD_STATE;
    }
    client_begin_hello(client);
    return FWU_OK;
}

fwu_return_e fwu_client_cancel(fwu_client_t *client)
{
    if((client == NULL) || (!client->initialized)) {
        return FWU_ERR_INVALID_ARG;
    }
    if((!client_running(client)) || (client->state == FWU_CLIENT_ACTIVATE) || (client->state == FWU_CLIENT_ABORTING)) {
        return FWU_ERR_BAD_STATE;
    }
    client->cancel_requested = true;
    return FWU_OK;
}

fwu_return_e fwu_client_poll(fwu_client_t *client, uint32_t now_ms, fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    if((client == NULL) || (!client->initialized) || (out == NULL) || (out_length == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(out_capacity < FWU_FRAME_MAX_SIZE) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    *out_length = 0u;
    if(!client_running(client)) {
        return FWU_OK;
    }

    if(client->cancel_requested && (client->state != FWU_CLIENT_ABORTING)) {
        client->cancel_requested = false;
        client->state = FWU_CLIENT_ABORTING;
        client_issue_placeholder(client, FWU_MSG_ABORT);
    }

    /* Pull mode: total time limit and the next progress query. */
    if((client->state == FWU_CLIENT_PULL_START) || (client->state == FWU_CLIENT_PULL)) {
        if((client->config.pull_timeout_ms != 0u) && ((uint32_t)(now_ms - client->pull_start_ms) >= client->config.pull_timeout_ms)) {
            client_fail(client, FWU_CLIENT_FAIL_TIMEOUT, FWU_OK);
            return FWU_OK;
        }
        if((client->state == FWU_CLIENT_PULL) && (!client->awaiting) && ((uint32_t)(now_ms - client->next_query_ms) < 0x80000000u)) {
            client_issue_placeholder(client, FWU_MSG_PULL_QUERY);
        }
    }

    if(client->awaiting && (!client->send_pending) && ((uint32_t)(now_ms - client->sent_ms) >= client->config.response_timeout_ms)) {
        if(client->state == FWU_CLIENT_VERIFY_WAIT) {
            if((uint32_t)(now_ms - client->verify_start_ms) >= client->config.verify_timeout_ms) {
                client_fail(client, FWU_CLIENT_FAIL_TIMEOUT, FWU_OK);
                return FWU_OK;
            }
            client->retransmits++;
            client->send_pending = true;                            /* EXIT repeat: asks for the result */
        }
        else if(client->retries >= client->config.max_retries) {
            client_fail(client, (client->state == FWU_CLIENT_ABORTING) ? FWU_CLIENT_FAIL_CANCELLED : FWU_CLIENT_FAIL_TIMEOUT, FWU_OK);
            return FWU_OK;
        }
        else {
            client->retries++;
            client->retransmits++;
            client->send_pending = true;
        }
    }

    if(client->awaiting && client->send_pending) {
        for(size_t i = 0u; i < client->req_length; i++) {
            out[i] = client->req_frame[i];
        }
        *out_length = client->req_length;
        client->send_pending = false;
        client->sent_ms = now_ms;
        client->frames_sent++;
    }
    return FWU_OK;
}

fwu_return_e fwu_client_on_frame(fwu_client_t *client, const fwu_frame_view_t *response, uint32_t now_ms)
{
    fwu_byte_t type;

    if((client == NULL) || (!client->initialized) || (response == NULL) || (response->payload == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if((!client_running(client)) || (!client->awaiting)) {
        client->ignored++;
        return FWU_OK;
    }
    type = (fwu_byte_t)(response->type & FWU_BYTE_MASK);
    if(((fwu_byte_t)(response->seq & FWU_BYTE_MASK) != client->req_seq) || (!client_type_ok(client, type))) {
        client->ignored++;                                          /* stale/duplicate/unexpected response */
        return FWU_OK;
    }
    client_handle_response(client, response, now_ms);
    return FWU_OK;
}

bool fwu_client_is_finished(const fwu_client_t *client)
{
    return (client != NULL) && ((client->state == FWU_CLIENT_DONE) || (client->state == FWU_CLIENT_FAILED));
}

fwu_client_state_e fwu_client_get_state(const fwu_client_t *client)
{
    return (client != NULL) ? client->state : FWU_CLIENT_IDLE;
}

fwu_return_e fwu_client_get_result(const fwu_client_t *client, fwu_client_result_t *result)
{
    if((client == NULL) || (result == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    result->state = client->state;
    result->failure = client->failure;
    result->device_error = client->device_error;
    result->verify_code = client->verify_code;
    result->acked_bytes = client->acked;
    result->total_bytes = client->config.image_size;
    result->frames_sent = client->frames_sent;
    result->retransmits = client->retransmits;
    result->restarts = client->restarts;
    result->resyncs = client->resyncs;
    result->ignored = client->ignored;
    result->pull_http_status = client->pull_http_status;
    return FWU_OK;
}
