#ifndef FWU_PORT_APP_PULL_H_
#define FWU_PORT_APP_PULL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_link.h"
#include "fwu_session.h"

/* Pull glue: connects the fwu_pull controller to the FC41D's TCP client.
 *
 * WHY A SEPARATE FILE: fwu_pull (driver) is independent of the MCU and the module; only the "send bytes" hook, the "give received bytes" call,
 * the "is the socket connected" information and the decision "when may heavy work be done" come from outside. This file implements those four things for the FC41D.
 *
 * FLOW
 *   PC (UI) --BLE--> PULL_START(ip, port, target) --> device: sets the TCP client target to (ip, port), connects
 *   device --TCP--> GET <target> + Range --> server mock on the PC --TCP--> image piece (HTTP 206)
 *   received bytes -> fwu_pull ring -> flash (staging) ; the status is queried over BLE with PULL_QUERY.
 *
 * FOUR TASKS (all in the main loop, none blocks; only fwu_pull_poll blocks briefly while writing flash, see 4)
 *   1. SENDING    : puts fwu_pull's request into the FC41D's TCP send queue (in HEX mode: CR/LF is allowed). BUFFER_FULL if there is no room.
 *   2. RECEIVING  : gives TCP messages (<= 95 bytes) to fwu_pull_feed. With the FC41D's "credit window" the module does NOT read
 *                   data for which the ring has no room: no bytes are lost (why: FC41D_api.h "Fast TCP receive").
 *   3. CONNECTION : is the FC41D's TCP client connected? On a change, fwu_pull_set_link.
 *   4. HEAVY WORK : if the ring has bytes, the FC41D is first told to HOLD new reads; when the module is no longer busy
 *                   fwu_pull_poll is run (flash erase/write takes milliseconds, the UART FIFO is only 16 bytes), then the hold is released.
 *
 * USAGE
 *   app_fwu_init (app_fwu.c) calls app_pull_init(session, link); this also adds the PULL_START / PULL_QUERY routing to the link.
 *   app_ble.c gives the TCP messages from the FC41D to app_pull_on_tcp. The main loop calls app_pull_poll AFTER app_ble_poll.
 */

/* Counters to watch (CCS watch window): g_app_pull_stats. */
typedef struct {
    uint32_t rx_messages;       /* TCP messages from the FC41D */
    uint32_t rx_bytes;          /* bytes given to fwu_pull_feed */
    uint32_t rx_dropped;        /* feed said the ring is full: bytes were lost (stays 0 if the credit is correct) */
    uint32_t tx_requests;       /* HTTP requests given to TCP */
    uint32_t tx_full;           /* no room in the send queue: fwu_pull retries later */
    uint32_t tx_failed;         /* the FC41D did not accept the request (TCP not connected): fwu_pull retries later */
    uint32_t link_ups;          /* TCP client became connected */
    uint32_t link_downs;        /* TCP client dropped / was retargeted */
    uint32_t retargets;         /* TCP target changed with PULL_START (the connection is being re-established) */
    uint32_t retarget_error;    /* result of the last fc41d_customize_tcp_settings: 0 = OK */
    uint32_t credit_given;      /* total read credit given to the FC41D (bytes) */
    uint32_t heavy_runs;        /* fwu_pull_poll ran while the ring had bytes (flash writing happens here) */
    uint32_t heavy_waited;      /* ... waited this many rounds because the module was busy */
} app_pull_stats_t;

extern volatile app_pull_stats_t g_app_pull_stats;
extern volatile fwu_return_e     g_app_pull_init_status;    /* result of app_pull_init (read from the debugger, expected 0 = FWU_OK) */

/* Sets up fwu_pull, opens the FC41D read credit and adds the extension (PULL_START / PULL_QUERY) to the link. Called from app_fwu_init AFTER the session
 * and link are set up. If not FWU_OK, pull is unusable (the BLE update is not affected). */
fwu_return_e app_pull_init(fwu_session_t *session, fwu_link_t *link);

/* Gives a TCP message (raw byte text) from the FC41D to the pull ring. If no pull is in progress, fwu_pull discards it. */
void app_pull_on_tcp(const char *data, uint16_t length, uint32_t now_ms);

/* Main loop: connection tracking, credit, heavy work. After app_ble_poll, in every iteration. */
void app_pull_poll(uint32_t now_ms);

/* Is a pull in progress? (a query) */
bool app_pull_active(void);

#endif /* FWU_PORT_APP_PULL_H_ */
