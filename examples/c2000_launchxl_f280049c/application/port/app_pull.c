#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "app_pull.h"
#include "app_fwu_config.h"
#include "fwu_http.h"
#include "fwu_proto.h"
#include "fwu_pull.h"
#include "FC41D_api.h"

#define APP_PULL_TX_QUEUE_CAPACITY  256u    /* FC41D send queue (DECLARE_QUEUE(char, 256)); one request (+ frame headers) must fit in it */

volatile app_pull_stats_t g_app_pull_stats;
volatile fwu_return_e     g_app_pull_init_status = FWU_ERR_BAD_STATE;

static fwu_pull_t  g_pull;
/* fwu_pull's RANGE buffer: the response to one GET (APP_PULL_RANGE_BYTES) is not written to flash until it has arrived COMPLETELY and its length is verified; a
 * byte lost in the transport layer therefore never enters staging. 4096 elements do not fit in .bss (RAMGS0, ~600 elements free): separate section
 * (28004x_app_flash_lnk.cmd: ramgs2 -> RAMGS2). */
#pragma DATA_SECTION(g_chunk, "ramgs2")
static fwu_byte_t  g_chunk[APP_PULL_RANGE_BYTES];
static fwu_byte_t  g_feed[FC41D_MESSAGE_MAX];           /* TCP message -> fwu_byte_t conversion (static: the stack is small) */
static char        g_request[FWU_HTTP_REQUEST_MAX];     /* fwu_byte_t request -> text given to the FC41D */
static bool        g_ready;                             /* app_pull_init succeeded */
static bool        g_link_up;                           /* last TCP state reported to fwu_pull */
static uint32_t    g_credit_given;                      /* total credit given to the FC41D (bytes, wraps) */
static uint32_t    g_fed_total;                         /* total bytes received from TCP and given to fwu_pull (wraps) */

/* Is the TCP client connected (and able to send)? If the target was changed (tcp_changed) the connection will be re-established anyway: "not connected". */
static bool tcp_is_up(void) {
    const fc41d_core_t *core = &g_fc41d_api.core;

    return core->is_module_ready &&
           core->communication_type.tcp_client &&
           !core->net.tcp_changed &&
           ((core->net.tcp_open_mask & (uint16_t)(1u << FC41D_TCP_CLIENT_SOCKET_ID)) != 0u);
}

/* fwu_pull's "send request" hook. Contract (fwu_transport.h): FWU_OK = ALL of it was accepted; BUFFER_FULL = nothing was taken, try later.
 * So we first check whether there is FULL room in the queue (the FC41D splits the request into 64-byte pieces and keeps a 3-byte header for each);
 * a half-accepted HTTP request would leave the server waiting on a malformed request. */
static fwu_return_e pull_send(void *context, const fwu_byte_t *data, size_t length) {
    uint16_t needed;
    uint16_t index;
    uint16_t sent;

    (void)context;
    if((data == NULL) || (length == 0u) || (length > (size_t)FWU_HTTP_REQUEST_MAX)) {
        return FWU_ERR_INVALID_ARG;
    }
    needed = (uint16_t)(length + 3u * ((length + FC41D_TX_CHUNK - 1u) / FC41D_TX_CHUNK));
    if(needed > APP_PULL_TX_QUEUE_CAPACITY) {
        return FWU_ERR_BUFFER_TOO_SMALL;                    /* does not fit in the queue at all: waiting changes nothing */
    }
    if(!tcp_is_up()) {
        g_app_pull_stats.tx_failed++;
        return FWU_ERR_BUFFER_FULL;                         /* fwu_pull does not send while there is no connection; still safe: it retries later */
    }
    if(fc41d_queue_space(&g_fc41d_api.core.transmit_queue) < needed) {
        g_app_pull_stats.tx_full++;
        return FWU_ERR_BUFFER_FULL;
    }
    for(index = 0u; index < (uint16_t)length; index++) {
        g_request[index] = (char)(data[index] & 0xFFu);     /* every byte, including CR/LF: allowed in FC41D HEX mode (tcp_hex_send) */
    }
    sent = fc41d_api_tcp_send(&g_fc41d_api, FC41D_TCP_CLIENT_SOCKET_ID, g_request, (uint16_t)length);
    if(sent == 0u) {
        g_app_pull_stats.tx_failed++;
        return FWU_ERR_BUFFER_FULL;                         /* not accepted (TCP just dropped): nothing is in the queue */
    }
    g_app_pull_stats.tx_requests++;
    return FWU_OK;                                          /* space was checked beforehand: sent == length is expected */
}

/* The (ip, port) from PULL_START becomes the TCP client's target. Why here: the device learns the server's address only from this frame.
 * If the same target arrives again (the reply was lost) the FC41D does nothing; if it differs the socket is closed and the new target is connected
 * (tcp_changed -> tcp_is_up is false, so fwu_pull sends no request until the connection is established). */
static void pull_retarget(const fwu_byte_t *payload, size_t payload_length) {
    fwu_proto_pull_start_t start;
    uint8_t                ip[4];
    uint16_t               index;
    fc41d_api_return_e     result;
    bool                   was_changing = g_fc41d_api.core.net.tcp_changed;

    if(fwu_proto_parse_pull_start(payload, payload_length, &start) != FWU_OK) {
        return;                                             /* malformed frame: fwu_pull_on_frame will produce an ERROR response */
    }
    for(index = 0u; index < 4u; index++) {
        ip[index] = (uint8_t)(start.ip[index] & 0xFFu);
    }
    /* Because the driver starts with BLE only, Wi-Fi / TCP are enabled at the FIRST PULL_START (does nothing if the same request arrives again). Changing the
     * target can be done only AFTER the TCP client feature is enabled. */
    (void)fc41d_api_enable_features(&g_fc41d_api, (uint16_t)(FEAT_WIFI_STA | FEAT_TCP_CLIENT));
    result = fc41d_customize_tcp_settings(&g_fc41d_api, ip, start.port);
    g_app_pull_stats.retarget_error = (uint32_t)result;
    if((result == FC41D_API_OK) && g_fc41d_api.core.net.tcp_changed && !was_changing) {
        g_app_pull_stats.retargets++;
    }
}

/* Link extension: only pull messages come here (fwu_pull_handles). */
static bool pull_handles(fwu_byte_t type) {
    return fwu_pull_handles(type);
}

static fwu_return_e pull_on_frame(void *context, const fwu_frame_view_t *view, uint32_t now_ms,
                                  fwu_byte_t *out, size_t out_capacity, size_t *out_length) {
    (void)context;
    if(view->type == FWU_MSG_PULL_START) {
        pull_retarget(view->payload, view->payload_length);   /* target first, then fwu_pull: when it starts the connection already looks "down" */
    }
    return fwu_pull_on_frame(&g_pull, view, now_ms, out, out_capacity, out_length);
}

fwu_return_e app_pull_init(fwu_session_t *session, fwu_link_t *link) {
    fwu_pull_config_t config;
    fwu_return_e      status;

    g_ready = false;
    g_link_up = false;
    g_credit_given = 0u;
    g_fed_total = 0u;

    config.session = session;
    config.send = pull_send;
    config.send_context = NULL;
    config.chunk_buffer = g_chunk;
    config.chunk_buffer_length = APP_PULL_RANGE_BYTES;
    config.range_bytes = APP_PULL_RANGE_BYTES;
    config.response_timeout_ms = APP_PULL_RESPONSE_TIMEOUT_MS;
    config.max_retries = APP_PULL_MAX_RETRIES;
    status = fwu_pull_init(&g_pull, &config);
    if(status == FWU_OK) {
        status = fwu_link_set_extension(link, pull_handles, pull_on_frame, NULL);
    }
    g_ready = (status == FWU_OK);
    g_app_pull_init_status = status;
    return status;
}

void app_pull_on_tcp(const char *data, uint16_t length, uint32_t now_ms) {
    uint16_t     index;
    fwu_return_e status;

    if((!g_ready) || (data == NULL) || (length == 0u) || (length > FC41D_MESSAGE_MAX)) {
        return;
    }
    for(index = 0u; index < length; index++) {
        g_feed[index] = (fwu_byte_t)((uint16_t)data[index] & 0xFFu);   /* char is 16 bit: the upper bits are masked */
    }
    g_fed_total += length;                                  /* credit accounting: these bytes are no longer in the FC41D queue, they are ours */
    g_app_pull_stats.rx_messages++;
    status = fwu_pull_feed(&g_pull, g_feed, length, now_ms);
    if(status == FWU_OK) {
        g_app_pull_stats.rx_bytes += length;
    }
    else {
        g_app_pull_stats.rx_dropped += length;              /* ring full: cannot happen if the credit was given correctly */
    }
}

/* Gives the FC41D read credit as room opens up in the ring. Credit that was given but has not yet reached fwu_pull (bytes in the FC41D queue or in flight)
 * is SUBTRACTED from the ring's free space: so the ring never overflows. There is a single thread of execution, so there is no race. */
static void grant_credit(void) {
    uint16_t free_space = fwu_pull_input_free(&g_pull);
    uint32_t outstanding = (uint32_t)(g_credit_given - g_fed_total);
    uint32_t grant;

    if(outstanding > 0x7FFFFFFFUL) {                        /* fed > given (leftover messages from an old connection): count as zero */
        outstanding = 0u;
    }
    if((uint32_t)free_space <= outstanding) {
        return;
    }
    grant = (uint32_t)free_space - outstanding;
    if(grant > 0xFFFFUL) {
        grant = 0xFFFFUL;
    }
    fc41d_api_tcp_read_credit(&g_fc41d_api, (uint16_t)grant);
    g_credit_given += grant;
    g_app_pull_stats.credit_given += grant;
}

void app_pull_poll(uint32_t now_ms) {
    bool up;

    if(!g_ready) {
        return;
    }

    /* 1. Credit window: fc41d_api_initialize (in main, AFTER app_fwu_init) resets the instance, so it sets itself up here. */
    if(!g_fc41d_api.core.net.tcp_window_on) {
        fc41d_api_tcp_read_window_enable(&g_fc41d_api, true);   /* window starts from 0 */
        g_credit_given = g_fed_total;                           /* accounting is reset too (no pending credit) */
    }

    /* 2. Connection tracking: on a change fwu_pull is informed (when it drops the half chunk is discarded, when it connects the request goes out). */
    up = tcp_is_up();
    if(up != g_link_up) {
        g_link_up = up;
        if(up) {
            g_app_pull_stats.link_ups++;
        }
        else {
            g_app_pull_stats.link_downs++;
            fc41d_api_tcp_read_window_enable(&g_fc41d_api, true);   /* credit left from the old connection is invalid: the window is reset */
            g_credit_given = g_fed_total;
        }
        fwu_pull_set_link(&g_pull, up, now_ms);
    }

    /* 3. Credit. */
    grant_credit();

    /* 4. Heavy work. If the ring has bytes (parsing + flash writing to come): the FC41D is stopped from starting a new read; when the module is no longer
     *    busy (the AT+QIRD response in progress has finished) fwu_pull_poll runs. When the ring empties the reads are released.
     *    If the ring is empty, fwu_pull_poll only does timeouts/sending requests (does not touch flash): no need to hold. */
    if(fwu_pull_input_used(&g_pull) > 0u) {
        fc41d_api_tcp_read_hold(&g_fc41d_api, true);
        if(fc41d_api_busy(&g_fc41d_api)) {
            g_app_pull_stats.heavy_waited++;
            return;
        }
        g_app_pull_stats.heavy_runs++;
        fwu_pull_poll(&g_pull, now_ms);
        if(fwu_pull_input_used(&g_pull) == 0u) {
            fc41d_api_tcp_read_hold(&g_fc41d_api, false);
        }
    }
    else {
        fc41d_api_tcp_read_hold(&g_fc41d_api, false);
        fwu_pull_poll(&g_pull, now_ms);
    }
}

bool app_pull_active(void) {
    return g_ready && fwu_pull_active(&g_pull);
}
