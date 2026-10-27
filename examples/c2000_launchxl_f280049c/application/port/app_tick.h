#ifndef FWU_PORT_APP_TICK_H_
#define FWU_PORT_APP_TICK_H_

#include <stdint.h>

/* 1 ms time counter (CPU Timer 0). app_fwu_poll(now_ms) uses it for the driver's timeouts.
 *
 * USAGE: app_tick_init() is called once. It must come AFTER Interrupt_initVectorTable(), because it registers the interrupt
 * handler in the vector table. The counter runs once global interrupts (EINT) are enabled.
 */
void     app_tick_init(void);

/* Milliseconds since power-up. 32 bit, wraps around on overflow (the driver supports this wrap). */
uint32_t app_tick_ms(void);

#endif /* FWU_PORT_APP_TICK_H_ */
