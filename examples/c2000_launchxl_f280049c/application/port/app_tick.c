#include "driverlib.h"
#include "device.h"
#include "app_tick.h"

#define APP_TICK_TIMER_BASE     CPUTIMER0_BASE
#define APP_TICK_PERIOD_COUNTS  (DEVICE_SYSCLK_FREQ / 1000UL)     /* 1 ms = SYSCLK / 1000 counts (100 000 at 100 MHz) */

static volatile uint32_t g_tick_ms;

/* Runs every 1 ms. The timer clears its own period flag when the interrupt is taken; we only acknowledge the PIE group
 * (INT_TIMER0 is in PIE group 1). */
static __interrupt void app_tick_isr(void) {
    g_tick_ms++;
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
}

void app_tick_init(void) {
    g_tick_ms = 0u;

    Interrupt_register(INT_TIMER0, &app_tick_isr);

    CPUTimer_stopTimer(APP_TICK_TIMER_BASE);                        /* stop first, then configure: do not change settings while running */
    CPUTimer_setPeriod(APP_TICK_TIMER_BASE, APP_TICK_PERIOD_COUNTS - 1UL);   /* for N counts write N-1 */
    CPUTimer_setPreScaler(APP_TICK_TIMER_BASE, 0u);
    CPUTimer_reloadTimerCounter(APP_TICK_TIMER_BASE);
    CPUTimer_setEmulationMode(APP_TICK_TIMER_BASE, CPUTIMER_EMULATIONMODE_RUNFREE);   /* keep counting when the debugger halts */
    CPUTimer_enableInterrupt(APP_TICK_TIMER_BASE);
    Interrupt_enable(INT_TIMER0);
    CPUTimer_startTimer(APP_TICK_TIMER_BASE);
}

uint32_t app_tick_ms(void) {
    return g_tick_ms;               /* a 32-bit read is a single instruction on C28x (MOVL): no interrupt can occur in between */
}
