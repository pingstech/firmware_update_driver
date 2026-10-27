/**
 * @file    boot_app.h
 * @brief   Bootloader application layer: connects the fwu_driver bootloader core (fwu_boot) to this device.
 *
 * Flow (once, from main): the flash port + SHA-256 port are set up -> fwu_boot_start (verify/apply the pending update) -> decision:
 *   JUMP : jump to the application (vector table at 0x08009000), the result is written to the RAM mailbox (the application reads it for FwuResult)
 *   STAY : do NOT jump to the application; g_boot_result.reason tells the cause (watch it in the debugger); wait forever.
 *
 * Hardware: only flash, RCC and SysTick are used; there is NO UART/ESP/GPIO (downloading the update is the application's job).
 */

#ifndef BOOT_APP_H_
#define BOOT_APP_H_

#include "main.h"
#include "fwu_boot.h"

/** The bootloader's whole job: does not return (either jumps to the application or waits in the STAY loop). */
void boot_app_run(void);

/** Called from NMI_Handler: if it was a flash ECC double error detection, clears the flags and returns; otherwise resets the system. */
void boot_app_nmi(void);

/** Last decision and its reason for the debugger (result of fwu_boot_start). */
extern volatile fwu_boot_result_t g_boot_result;

#endif /* BOOT_APP_H_ */
