#ifndef FWU_PORT_APP_FWU_H_
#define FWU_PORT_APP_FWU_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_link.h"

/* The application's firmware update layer: sets up the driver's receive ring buffer, parser, session and link.
 *
 * USAGE
 *   At startup      : call app_fwu_init(). If the return is not FWU_OK the setup failed and app_fwu_poll does nothing
 *                     (app_fwu_status() returns the same code).
 *   SCI receive ISR : passes every received byte with app_fwu_rx_push(byte).
 *   Main loop       : call app_fwu_poll(current_time_ms) often (does not block).
 *
 * PORT-SPECIFIC FUNCTION (app_sci.c): same signature as the driver's transport.send function. Hands a frame to SCI TX:
 *   FWU_OK              the frame was accepted COMPLETELY (copied into the TX buffer)
 *   FWU_ERR_BUFFER_FULL TX is busy right now, NOTHING was taken; the link retries later with the same frame
 *   other error         the send failed, the frame is dropped (the PC retries on timeout)
 */
fwu_return_e app_sci_send(void *context, const fwu_byte_t *data, size_t length);

fwu_return_e app_fwu_init(void);
fwu_return_e app_fwu_status(void);

/* Called from the ISR: pushes one byte into the ring buffer (the low 8 bits are significant). Returns false if the buffer is full (the byte is dropped). */
bool app_fwu_rx_push(fwu_byte_t value);

/* Main loop: processes incoming frames, sends responses. now_ms: millisecond counter (32-bit wrap is supported). */
void app_fwu_poll(uint32_t now_ms);

/* Diagnostic counters (read-only): frames processed, responses sent, TX busy/error counts. Never returns NULL. */
const fwu_link_stats_t *app_fwu_stats(void);

#endif /* FWU_PORT_APP_FWU_H_ */
