/**
 * @file    fwu_isotp.h
 * @brief   SUBSET of ISO-TP (ISO 15765-2) for CAN: splits an fwu frame (at most 521 bytes) that does not fit into the 8-byte
 *          classic CAN frame into pieces and sends them, and reassembles incoming pieces and delivers them as a complete
 *          message. It is BIDIRECTIONAL: the same module is used on the device and on the host (the DLL of fwu_app). It is
 *          hardware independent: it connects to the CAN driver only through a "send an 8-byte frame" hook and receives
 *          incoming frames through on_can_frame.
 *
 * [UNVERIFIED] THIS IS A SUBSET AND HAS NOT BEEN VERIFIED AGAINST THE OFFICIAL TEXT OF ISO 15765-2. The format follows the
 * commonly known implementation.
 *
 * FRAME FORMATS (classic CAN, normal addressing, all 8 bytes; short content is padded with FWU_TRANSPORT_CAN_PAD_BYTE)
 *   SF  single frame       [0x0 | length(1..7)] [data...]
 *   FF  first frame        [0x1 | length>>8] [length & 0xFF] [6 data bytes]        length 8..4095 (here <= 521)
 *   CF  consecutive frame  [0x2 | SN] [7 data bytes]                              SN: starts at 1, wraps to 0 after 15
 *   FC  flow control       [0x3 | FS] [BS] [STmin]                                FS: 0 continue (CTS), 1 wait (WAIT), 2 overflow
 *   STmin: 0..127 = ms; 0xF1..0xF9 = 100..900 microseconds (rounded up to 1 ms here); others are treated as 127 ms.
 *   BS: the receiver requests a new FC after this many CFs (0 = unlimited).
 *
 * FLOW: the sender sends an SF or FF; after the FF it waits for the receiver's FC (CTS), then sends the CFs with STmin spacing
 * (when BS is reached it waits for an FC again). On an FF the receiver sends an FC, collects the CFs in SN order and delivers
 * the message when the length is reached.
 *
 * RULES
 *   - Only ONE transmission can be in progress at a time (stop-and-wait; the fwu protocol is that way anyway): while one is
 *     in progress fwu_isotp_send returns BUFFER_FULL.
 *   - On the receiver side, if a reassembly is in progress, a new SF delivers its message and ABORTS the old reassembly; a new
 *     FF aborts the old reassembly and starts a new one (ISO behavior). A wrong SN, an unexpected CF/FC, or a timeout aborts
 *     the reassembly.
 *   - Timeout (timeout_ms): if the sender does not receive the FC, or the receiver does not receive the next CF, within this time
 *     it aborts; the lost message is compensated in the upper layer (host retransmission).
 *   - FC(WAIT) is accepted at most wft_max times; if exceeded the transmission is aborted. FC(OVFLW) aborts the transmission.
 *   - The delivery hook gives the message from the reassembly buffer; it is valid only for the duration of the call (must be copied).
 *   - Time is given via now_ms (32-bit wrap-around is supported). There is no dynamic memory; the two buffers (transmit, receive) are 521 bytes.
 *
 * on_can_frame RETURNS: FWU_OK (frame processed). Protocol violations are DROPPED and counted (an ISR may ignore them):
 *   FWU_ERR_BAD_FRAME (unknown PCI type or the content does not fit in the frame), FWU_ERR_BAD_LENGTH (invalid length or overflow:
 *   FF > 521), FWU_ERR_BAD_STATE (out of sequence: unexpected CF/FC, wrong SN). Usage errors are FWU_ERR_INVALID_ARG.
 */

#ifndef FWU_ISOTP_H_
#define FWU_ISOTP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_frame.h"
#include "fwu_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FWU_ISOTP_MAX_MESSAGE       FWU_FRAME_MAX_SIZE      /**< Maximum size of a message (521). */
#define FWU_ISOTP_CAN_DLC           8u                      /**< Data length of every CAN frame. */
#define FWU_ISOTP_MAX_CF_PER_POLL   8u                      /**< Maximum number of consecutive frames sent in one step. */
#define FWU_ISOTP_DEFAULT_WFT_MAX   10u                     /**< Maximum number of consecutive FC(WAIT) accepted. */

/** Sends ONE 8-byte frame to the CAN driver (with the TX identifier). BUFFER_FULL: mailbox full, retried later. */
typedef fwu_return_e (*fwu_isotp_can_send_fn)(void *context, const fwu_byte_t *data);
/** Called when a complete message has been reassembled (`message` is valid only for the duration of the call). */
typedef void (*fwu_isotp_deliver_fn)(void *context, const fwu_byte_t *message, size_t length);

/** Diagnostic counters (32 bit, wrap around on overflow). */
typedef struct {
    uint32_t rx_messages;       /**< Complete messages delivered. */
    uint32_t tx_messages;       /**< Messages sent completely. */
    uint32_t rx_errors;         /**< Invalid / out-of-sequence frames dropped. */
    uint32_t rx_aborts;         /**< Reassemblies aborted (wrong SN, new SF/FF, timeout). */
    uint32_t rx_timeouts;       /**< Receiver timeouts. */
    uint32_t tx_timeouts;       /**< Sender timeouts (FC did not arrive). */
    uint32_t tx_aborts;         /**< Transmissions aborted (timeout, OVFLW, WAIT limit, driver error). */
    uint32_t cf_sent;           /**< Consecutive frames sent. */
    uint32_t fc_sent;           /**< Flow-control frames sent. */
} fwu_isotp_stats_t;

typedef struct {
    fwu_isotp_can_send_fn can_send;     /**< Mandatory. */
    fwu_isotp_deliver_fn  deliver;      /**< Mandatory. */
    void                 *context;      /**< Passed to both hooks. */
    uint32_t              block_size;   /**< BS announced as receiver (0..255; 0 = unlimited). */
    uint32_t              st_min_ms;    /**< STmin announced as receiver (0..127 ms). */
    uint32_t              timeout_ms;   /**< N_Bs / N_Cr timeout (>= 1). */
    uint32_t              wft_max;      /**< Maximum number of consecutive FC(WAIT) accepted. */
} fwu_isotp_config_t;

/** State. Do not touch the fields directly (except stats, which is readable). */
typedef struct {
    fwu_isotp_config_t config;
    bool               initialized;
    uint32_t           now_ms;                          /**< Last known time (updated by poll / on_can_frame / send). */
    /* receive */
    fwu_byte_t         rx_buffer[FWU_ISOTP_MAX_MESSAGE];
    uint32_t           rx_length;                       /**< Expected total length. */
    uint32_t           rx_received;
    uint32_t           rx_sn;                           /**< Next expected SN (0..15). */
    uint32_t           rx_block;                        /**< CFs received in this block. */
    uint32_t           rx_timer_ms;
    bool               rx_active;
    bool               fc_pending;                      /**< There is an FC that could not be sent. */
    fwu_byte_t         fc_status;
    /* transmit */
    fwu_byte_t         tx_buffer[FWU_ISOTP_MAX_MESSAGE];
    uint32_t           tx_length;
    uint32_t           tx_sent;
    uint32_t           tx_sn;
    uint32_t           tx_state;                        /**< 0 idle, 1 waiting for first frame, 2 waiting for FC, 3 sending CFs. */
    uint32_t           tx_block_left;                   /**< CFs left in the block granted by the FC (ignored if tx_unlimited). */
    bool               tx_unlimited;
    uint32_t           tx_st_min_ms;
    uint32_t           tx_timer_ms;
    uint32_t           tx_last_cf_ms;
    uint32_t           tx_wft;
    fwu_isotp_stats_t  stats;
} fwu_isotp_t;

/**
 * @brief  Initializes.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL, can_send/deliver NULL, block_size > 255, st_min_ms > 127, timeout_ms 0.
 */
fwu_return_e fwu_isotp_init(fwu_isotp_t *iso, const fwu_isotp_config_t *config);

/**
 * @brief  Starts sending a message (an attempt is made to send the first frame immediately). The message is COPIED into the internal buffer.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL or length 0 / > 521; FWU_ERR_BUFFER_FULL if a transmission is in progress.
 */
fwu_return_e fwu_isotp_send(fwu_isotp_t *iso, const fwu_byte_t *data, size_t length, uint32_t now_ms);

/**
 * @brief  Processes a frame coming from the CAN driver (from an ISR or the main loop; must be called from the SAME context as poll).
 * @param  length  Data length of the frame (1..8).
 * @return see "on_can_frame RETURNS" in the header.
 */
fwu_return_e fwu_isotp_on_can_frame(fwu_isotp_t *iso, const fwu_byte_t *data, size_t length, uint32_t now_ms);

/** Time-dependent work: timeouts, retrying an FC that could not be sent, sending consecutive frames. Called often. */
fwu_return_e fwu_isotp_poll(fwu_isotp_t *iso, uint32_t now_ms);

/** Whether a transmission is in progress (a QUERY). false if NULL. */
bool fwu_isotp_tx_busy(const fwu_isotp_t *iso);

/** Counters (NULL if NULL). */
const fwu_isotp_stats_t *fwu_isotp_get_stats(const fwu_isotp_t *iso);

/** fwu_transport_t adapters: connect ISO-TP to the link's transport interface (context = fwu_isotp_t *). */
fwu_return_e fwu_isotp_transport_send(void *context, const fwu_byte_t *data, size_t length);
void fwu_isotp_transport_poll(void *context, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FWU_ISOTP_H_ */
