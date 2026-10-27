#ifndef FWU_PORT_APP_BLE_H_
#define FWU_PORT_APP_BLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"

/* BLE transport: carries the firmware update protocol over the FC41D's BLE GATT characteristic (FFF1).
 *
 * The FC41D carries only printable ASCII text and a message is at most 95 characters, so the frame bytes are converted to HEX TEXT:
 *   PC -> device : every BLE write is a hex text piece (<= 94 characters = 47 bytes). The pieces are decoded to bytes and collected in a STAGING
 *                  BUFFER; when the frame is complete (according to the LEN in the header) or after APP_BLE_RX_IDLE_MS of silence it is
 *                  handed to the parser with app_fwu_rx_push IN ONE GO. Reason: the pieces arrive 150 ms apart, a frame takes 5-6 pieces,
 *                  but fwu_parser discards a frame whose start has arrived if it is not completed within FWU_PARSER_FRAME_TIMEOUT_MS (500 ms).
 *   device -> PC : transport.send puts the frame into the tx ring buffer; app_ble_poll converts it to hex text in 47-byte pieces, leaving at least
 *                  APP_BLE_TX_GAP_MS between them, and gives it to the FC41D (the driver splits it into 64-character notifications).
 *
 * This module also runs the FC41D driver's 1 ms tick and polling: in BLE mode the main loop calls only app_ble_poll(now).
 *
 * USAGE
 *   At startup  : app_ble_init(app_tick_ms()) after fc41d_api_initialize, before interrupts are enabled.
 *   Main loop   : app_ble_poll(now_ms) in every iteration (does not block). Together with app_fwu_poll(now_ms).
 *   Driver      : app_fwu.c uses app_ble_send as transport.send (APP_FWU_OVER_BLE = 1).
 *   Restart     : app_fwu_reboot calls app_ble_flush beforehand; the reset is not issued until the last response has left for the PC.
 */

/* Counters to watch (CCS watch window): g_app_ble_stats. */
typedef struct {
    uint32_t rx_messages;       /* BLE messages received from the phone/PC */
    uint32_t rx_bytes;          /* bytes decoded from hex and given to app_fwu_rx_push */
    uint32_t rx_ignored;        /* non-hex characters (discarded) */
    uint32_t rx_overflow;       /* app_fwu_rx_push refused (receive ring buffer full): the byte was lost */
    uint32_t rx_other;          /* non-BLE (TCP) messages: discarded */
    uint32_t rx_frames;         /* frames completed in the staging buffer and given to the parser */
    uint32_t rx_idle_flush;     /* idle time expired before the frame was complete: the remaining bytes were given to the parser (incomplete/corrupt frame) */
    uint32_t rx_resync;         /* a half-received hex digit was discarded after silence: byte alignment was re-established (a lost message had broken the alignment) */
    uint32_t tx_pieces;         /* hex text pieces given to the FC41D */
    uint32_t tx_bytes;          /* bytes sent with these pieces */
    uint32_t tx_full;           /* send returned BUFFER_FULL (ring buffer full, the link made it wait) */
    uint32_t tx_refused;        /* the FC41D refused the piece (BLE not connected / queue): will be retried later */
    uint32_t tx_partial;        /* piece was partially accepted (should not happen: queue space is checked beforehand) */
} app_ble_stats_t;

extern volatile app_ble_stats_t g_app_ble_stats;

/* Ring buffers, decoder and counters are cleared; the timer is aligned to now_ms. */
void app_ble_init(uint32_t now_ms);

/* Main loop: FC41D tick + polling, decodes received BLE messages to bytes and pushes them into the receive ring, sends the pending frame in pieces.
 * Does not block. now_ms: millisecond counter (32-bit wrap is supported). */
void app_ble_poll(uint32_t now_ms);

/* Same signature as fwu_transport_send_fn (docs: fwu_transport.h). Copies the frame into the tx ring.
 *   FWU_OK              ALL of it was accepted
 *   FWU_ERR_BUFFER_FULL the ring is full, NOTHING was taken; the link retries later with the same frame
 *   FWU_ERR_BUFFER_TOO_SMALL  the frame can never fit into the ring (setup error): dropped */
fwu_return_e app_ble_send(void *context, const fwu_byte_t *data, size_t length);

/* Are the tx ring, the driver queue and the pending AT command all empty (nothing left to send)? */
bool app_ble_tx_idle(void);

/* BLOCKS (bounded by timeout_ms): returns when tx is empty and has stayed empty for settle_ms. Used right before a restart so that
 * the last response goes from the module onto the air. Runs app_ble_poll internally. If BLE is not connected it returns
 * when timeout_ms expires. */
void app_ble_flush(uint32_t timeout_ms, uint32_t settle_ms);

#endif /* FWU_PORT_APP_BLE_H_ */
