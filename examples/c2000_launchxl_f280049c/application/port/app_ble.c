#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "app_ble.h"
#include "app_fwu.h"
#include "app_fwu_config.h"
#include "app_tick.h"
#if APP_FWU_PULL
#include "app_pull.h"
#endif
#include "fwu_frame.h"
#include "FC41D_api.h"

#define APP_BLE_PIECE_BYTES     47u                         /* bytes in one BLE message (94 hex characters); the FC41D receive limit is 95 characters */
#define APP_BLE_PIECE_HEX       (APP_BLE_PIECE_BYTES * 2u)

volatile app_ble_stats_t g_app_ble_stats;

/* ---- tx ring buffer (used only by the main loop: no lock needed) */
static fwu_byte_t g_tx_ring[APP_BLE_TX_RING_SIZE];
static uint16_t   g_tx_head;                                /* next place to write */
static uint16_t   g_tx_tail;                                /* next place to read */
static uint16_t   g_tx_count;
static uint32_t   g_tx_last_ms;                             /* time of the last piece (or refusal) */
static char       g_hex_out[APP_BLE_PIECE_HEX + 1u];

/* ---- rx staging buffer: decoded bytes wait here until the frame is complete (why: app_ble.h) */
static fwu_byte_t g_rx_stage[APP_BLE_RX_STAGE_SIZE];
static uint16_t   g_rx_stage_count;
static uint32_t   g_rx_stage_last_ms;                       /* time the last byte was added to the buffer */

/* ---- rx decoder: produces a byte when the number of hex digits becomes even; message boundaries do not break byte alignment */
static char       g_rx_text[FC41D_MESSAGE_MAX];
static bool       g_rx_half_valid;
static uint32_t   g_rx_half_ms;                             /* time the half hex digit (g_rx_half) started waiting */
static uint16_t   g_rx_half;

/* ---- FC41D 1 ms tick: if the main loop slows down (flash erase), the missed milliseconds are given back-to-back */
static uint32_t   g_tick_last;

static int16_t hex_value(char c) {
    if((c >= '0') && (c <= '9')) {
        return (int16_t)(c - '0');
    }
    if((c >= 'A') && (c <= 'F')) {
        return (int16_t)(c - 'A' + 10);
    }
    if((c >= 'a') && (c <= 'f')) {
        return (int16_t)(c - 'a' + 10);
    }
    return -1;
}

void app_ble_init(uint32_t now_ms) {
    g_tx_head = 0u;
    g_tx_tail = 0u;
    g_tx_count = 0u;
    g_tx_last_ms = now_ms - APP_BLE_TX_GAP_MS;              /* the first piece can go without waiting */
    g_rx_half_valid = false;
    g_rx_half_ms = now_ms;
    g_rx_half = 0u;
    g_rx_stage_count = 0u;
    g_rx_stage_last_ms = now_ms;
    g_tick_last = now_ms;
    g_app_ble_stats.rx_messages = 0u;
    g_app_ble_stats.rx_bytes = 0u;
    g_app_ble_stats.rx_ignored = 0u;
    g_app_ble_stats.rx_overflow = 0u;
    g_app_ble_stats.rx_other = 0u;
    g_app_ble_stats.rx_frames = 0u;
    g_app_ble_stats.rx_idle_flush = 0u;
    g_app_ble_stats.rx_resync = 0u;
    g_app_ble_stats.tx_pieces = 0u;
    g_app_ble_stats.tx_bytes = 0u;
    g_app_ble_stats.tx_full = 0u;
    g_app_ble_stats.tx_refused = 0u;
    g_app_ble_stats.tx_partial = 0u;
}

fwu_return_e app_ble_send(void *context, const fwu_byte_t *data, size_t length) {
    size_t index;

    (void)context;
    if((data == NULL) || (length == 0u)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(length > (size_t)APP_BLE_TX_RING_SIZE) {
        return FWU_ERR_BUFFER_TOO_SMALL;                    /* never fits: waiting changes nothing */
    }
    if(length > (size_t)(APP_BLE_TX_RING_SIZE - g_tx_count)) {
        g_app_ble_stats.tx_full++;
        return FWU_ERR_BUFFER_FULL;                         /* nothing was taken: the link retries later with the same frame */
    }
    for(index = 0u; index < length; index++) {
        g_tx_ring[g_tx_head] = (fwu_byte_t)(data[index] & 0xFFu);
        g_tx_head++;
        if(g_tx_head >= APP_BLE_TX_RING_SIZE) {
            g_tx_head = 0u;
        }
    }
    g_tx_count = (uint16_t)(g_tx_count + length);
    return FWU_OK;
}

/* Gives all bytes in the staging buffer to the parser's receive ring. */
static void rx_stage_flush(void) {
    uint16_t index;

    for(index = 0u; index < g_rx_stage_count; index++) {
        if(app_fwu_rx_push(g_rx_stage[index])) {
            g_app_ble_stats.rx_bytes++;
        }
        else {
            g_app_ble_stats.rx_overflow++;
        }
    }
    g_rx_stage_count = 0u;
}

/* true if the buffer starts with a SOF and as many bytes as the LEN in the header indicates have been collected (or the LEN is invalidly large; the parser rejects it). */
static bool rx_stage_frame_complete(void) {
    uint16_t length;

    if((g_rx_stage_count < FWU_FRAME_HEADER_SIZE) || (g_rx_stage[0] != FWU_FRAME_SOF)) {
        return false;
    }
    length = (uint16_t)(((g_rx_stage[FWU_FRAME_OFFSET_LEN] & 0xFFu) << 8) | (g_rx_stage[FWU_FRAME_OFFSET_LEN + 1u] & 0xFFu));
    if(length > FWU_FRAME_MAX_PAYLOAD) {
        return true;
    }
    return g_rx_stage_count >= (uint16_t)(FWU_FRAME_OVERHEAD + length);
}

static void rx_stage_add(fwu_byte_t byte, uint32_t now_ms) {
    if(g_rx_stage_count >= APP_BLE_RX_STAGE_SIZE) {
        rx_stage_flush();                                   /* full: hand over what we have, make room */
    }
    g_rx_stage[g_rx_stage_count] = (fwu_byte_t)(byte & 0xFFu);
    g_rx_stage_count++;
    g_rx_stage_last_ms = now_ms;
}

/* Gives the buffer to the parser if the frame is complete or the idle time has expired.
 *
 * RE-ALIGNMENT: the byte alignment of the hex text depends on the "even/odd digit" state (g_rx_half_valid). The FC41D splits a message at 95 characters (an ODD number);
 * if one of these pieces is lost (queue full, UART overflow) the digit alignment is shifted by one digit PERMANENTLY and EVERY following frame is garbage:
 * the board does not answer, only a reset recovers it. A frame always carries an EVEN number of digits and the gaps between its pieces are shorter than APP_BLE_RX_IDLE_MS;
 * so if a half digit has waited longer than this time it no longer belongs to any frame: it is discarded and the next message starts aligned. */
static void rx_stage_service(uint32_t now_ms) {
    if(g_rx_half_valid && ((uint32_t)(now_ms - g_rx_half_ms) >= (uint32_t)APP_BLE_RX_IDLE_MS)) {
        g_rx_half_valid = false;
        g_app_ble_stats.rx_resync++;
    }
    if(g_rx_stage_count == 0u) {
        return;
    }
    if(rx_stage_frame_complete()) {
        g_app_ble_stats.rx_frames++;
        rx_stage_flush();
    }
    else if((uint32_t)(now_ms - g_rx_stage_last_ms) >= (uint32_t)APP_BLE_RX_IDLE_MS) {
        g_app_ble_stats.rx_idle_flush++;
        rx_stage_flush();
    }
}

/* Decodes received BLE messages to bytes; TCP messages (pull mode) go to app_pull. At most APP_BLE_RX_ROUNDS messages per round: the main loop
 * also does other work. A TCP message is at most 95 bytes and an AT+QIRD response is split into 3-6 messages: the round count is high to drain the queue quickly. */
static void rx_service(uint32_t now_ms) {
    uint16_t rounds;

    for(rounds = 0u; rounds < APP_BLE_RX_ROUNDS; rounds++) {
        uint16_t length = 0u;
        uint16_t index;
        fc41d_message_source_e source = fc41d_get_message(&g_fc41d_api, g_rx_text, &length);

        if(source == FC41D_MESSAGE_NONE) {
            return;
        }
        if(source != FC41D_MESSAGE_BLE) {
#if APP_FWU_PULL
            if(source == FC41D_MESSAGE_TCP_CLIENT) {
                app_pull_on_tcp(g_rx_text, length, now_ms); /* HTTP response from the server: to the pull ring */
                continue;
            }
#endif
            g_app_ble_stats.rx_other++;                     /* other non-BLE message: this transport does not use it */
            continue;
        }
        g_app_ble_stats.rx_messages++;
        for(index = 0u; index < length; index++) {
            int16_t value = hex_value(g_rx_text[index]);

            if(value < 0) {
                g_app_ble_stats.rx_ignored++;
            }
            else if(!g_rx_half_valid) {
                g_rx_half = (uint16_t)value;
                g_rx_half_valid = true;
                g_rx_half_ms = now_ms;
            }
            else {
                fwu_byte_t byte = (fwu_byte_t)((g_rx_half << 4) | (uint16_t)value);

                g_rx_half_valid = false;
                rx_stage_add(byte, now_ms);
            }
        }
    }
}

/* Converts the first APP_BLE_PIECE_BYTES-byte piece in the ring to hex text and gives it to the FC41D. A byte leaves the ring ONLY when accepted. */
static void tx_service(uint32_t now_ms) {
    static const char digits[] = "0123456789ABCDEF";
    uint16_t pieces_bytes;
    uint16_t hex_length;
    uint16_t needed;
    uint16_t index;
    uint16_t position;
    uint16_t sent;

    if(g_tx_count == 0u) {
        return;
    }
    if((uint32_t)(now_ms - g_tx_last_ms) < (uint32_t)APP_BLE_TX_GAP_MS) {
        return;
    }

    pieces_bytes = (g_tx_count < APP_BLE_PIECE_BYTES) ? g_tx_count : (uint16_t)APP_BLE_PIECE_BYTES;
    hex_length = (uint16_t)(pieces_bytes * 2u);

    /* The driver queue splits into 64-character notifications and keeps a 3-byte header for each. If there is no room we try later (partial
     * acceptance could leave a half hex pair, so the space is checked first). */
    needed = (uint16_t)(hex_length + 3u * ((hex_length + FC41D_TX_CHUNK - 1u) / FC41D_TX_CHUNK));
    if(fc41d_queue_space(&g_fc41d_api.core.transmit_queue) < needed) {
        return;
    }

    position = g_tx_tail;
    for(index = 0u; index < pieces_bytes; index++) {
        uint16_t byte = (uint16_t)(g_tx_ring[position] & 0xFFu);

        g_hex_out[2u * index] = digits[byte >> 4];
        g_hex_out[2u * index + 1u] = digits[byte & 0x0Fu];
        position++;
        if(position >= APP_BLE_TX_RING_SIZE) {
            position = 0u;
        }
    }
    g_hex_out[hex_length] = '\0';

    sent = fc41d_api_ble_send(&g_fc41d_api, g_hex_out, hex_length);
    g_tx_last_ms = now_ms;                                  /* acceptance or refusal both impose a wait interval: on refusal we do not push the driver */

    if(sent == 0u) {
        g_app_ble_stats.tx_refused++;                       /* BLE not connected or queue: the bytes stay in the ring and are retried later */
        return;
    }
    if(sent != hex_length) {
        g_app_ble_stats.tx_partial++;                       /* should not happen; as many bytes as were accepted are consumed, the rest goes in the next piece */
        pieces_bytes = (uint16_t)(sent / 2u);
        if(pieces_bytes == 0u) {
            return;
        }
    }

    g_tx_tail = (uint16_t)(g_tx_tail + pieces_bytes);
    if(g_tx_tail >= APP_BLE_TX_RING_SIZE) {
        g_tx_tail = (uint16_t)(g_tx_tail - APP_BLE_TX_RING_SIZE);
    }
    g_tx_count = (uint16_t)(g_tx_count - pieces_bytes);
    g_app_ble_stats.tx_pieces++;
    g_app_ble_stats.tx_bytes += pieces_bytes;
}

void app_ble_poll(uint32_t now_ms) {
    while((uint32_t)(now_ms - g_tick_last) >= 1u) {         /* one tick for every elapsed millisecond */
        (void)fc41d_api_1ms_timer(&g_fc41d_api);
        g_tick_last++;
    }
    (void)fc41d_api_polling(&g_fc41d_api);
    rx_service(now_ms);
    rx_stage_service(now_ms);
    tx_service(now_ms);
}

bool app_ble_tx_idle(void) {
    return (g_tx_count == 0u) &&
           (fc41d_queue_count(&g_fc41d_api.core.transmit_queue) == 0u) &&
           (!g_fc41d_api.core.tx_job.active);
}

void app_ble_flush(uint32_t timeout_ms, uint32_t settle_ms) {
    uint32_t start = app_tick_ms();
    uint32_t idle_since = start;
    bool was_idle = false;

    for(;;) {
        uint32_t now = app_tick_ms();

        app_ble_poll(now);
        if(app_ble_tx_idle()) {
            if(!was_idle) {
                was_idle = true;
                idle_since = now;
            }
            else if((uint32_t)(now - idle_since) >= settle_ms) {
                return;
            }
        }
        else {
            was_idle = false;
        }
        if((uint32_t)(now - start) >= timeout_ms) {
            return;
        }
    }
}
