//#############################################################################
//
// FILE:   empty_driverlib_main.c
//
//! \addtogroup driver_example_list
//! <h1>Empty Project Example</h1> 
//!
//! This example is an empty project setup for Driverlib development.
//!
//
//#############################################################################
//
//
// $Copyright:
// Copyright (C) 2026 Texas Instruments Incorporated - http://www.ti.com/
//
// Redistribution and use in source and binary forms, with or without 
// modification, are permitted provided that the following conditions 
// are met:
// 
//   Redistributions of source code must retain the above copyright 
//   notice, this list of conditions and the following disclaimer.
// 
//   Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the 
//   documentation and/or other materials provided with the   
//   distribution.
// 
//   Neither the name of Texas Instruments Incorporated nor the names of
//   its contributors may be used to endorse or promote products derived
//   from this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// $
//#############################################################################

//
// Included Files
//
#include <stdbool.h>
#include "driverlib.h"
#include "device.h"
#include "board.h"
#include "c2000ware_libraries.h"

#include <stddef.h>
#include "boot_fwu_config.h"
#include "fwu_boot_profile.h"
#include "fwu_boot.h"
#include "fwu_crypto_sw.h"
#include "fwu_flash_adapter.h"

#define APP_ENTRY_ADDRESS 0x083000UL // **@Brief: the "APP_BEGIN" address in the linker command file of "fw_update_application"
#define FLASH_ERASED_WORD 0xFFFFu

// **@Brief: Firmware update objects. fwu_boot_t must stay at a FIXED address (the pointers inside refer to its own fields): static.
static fwu_flash_adapter_t g_flash;
static fwu_crypto_sw_t     g_crypto_sw;
static fwu_crypto_t        g_crypto;
static fwu_boot_t          g_boot;
static fwu_boot_config_t   g_boot_config;
static fwu_byte_t          g_verify_work[BOOT_FWU_WORK_SIZE];
static fwu_byte_t          g_activate_work[BOOT_FWU_WORK_SIZE];
static volatile fwu_boot_result_t g_boot_result;  // **@Brief: Look at this in the debugger: reason/status/outcome (reason for staying)
static volatile fwu_return_e      g_boot_setup_status = FWU_OK;

static bool app_is_valid(void) {
    uint16_t first_word = *(volatile uint16_t *)APP_ENTRY_ADDRESS;
    return (first_word != FLASH_ERASED_WORD);
}

static bool app_valid_hook(void *context) {
    (void)context;
    return app_is_valid();
}

// **@Brief: If an update is pending, copies staging -> application (fwu_boot_start), then decides whether to jump.
//           On ANY problem, including a setup error, it returns STAY: no jump to the application.
static fwu_boot_decision_e boot_update_and_decide(void) {
    fwu_return_e status;

    status = fwu_flash_adapter_init(&g_flash, BOOT_FWU_FLASH_BASE_WORD, BOOT_FWU_FLASH_LENGTH_OCTETS);
    if(status == FWU_OK) {
        status = fwu_crypto_sw_init(&g_crypto_sw, &g_crypto);
    }
    g_boot_setup_status = status;
    if(status != FWU_OK) {
        return FWU_BOOT_STAY;
    }

    g_boot_config.policy = fwu_boot_policy_from_profile();
    g_boot_config.layout = fwu_boot_layout_from_board();
    g_boot_config.flash = &g_flash.flash;
    g_boot_config.crypto = &g_crypto;
    g_boot_config.keys = NULL;                      // DEV profile: no signature, SHA-256 digest check only
    g_boot_config.key_count = 0u;
    g_boot_config.product_id = BOOT_FWU_PRODUCT_ID;
    g_boot_config.max_trial_boots = BOOT_FWU_MAX_TRIAL_BOOTS;
    g_boot_config.verify_work = g_verify_work;
    g_boot_config.verify_work_length = BOOT_FWU_WORK_SIZE;
    g_boot_config.activate_work = g_activate_work;
    g_boot_config.activate_work_length = BOOT_FWU_WORK_SIZE;
    g_boot_config.app_valid = app_valid_hook;
    g_boot_config.watchdog_arm = NULL;
    g_boot_config.context = NULL;

    return fwu_boot_start(&g_boot, &g_boot_config, (fwu_boot_result_t *)&g_boot_result);
}

typedef void (*app_entry_t)(void);
static void app_jump(void) {
    DINT;                                   // **@Brief: disable interrupts
    IER = 0x0000;                           // **@Brief: disable all interrupts the CPU listens to; the application side will enable its own
    IFR = 0x0000 ;                          // **@Brief: clear pending interrupt flags; the application side will enable its own
    ((app_entry_t)APP_ENTRY_ADDRESS)();     // **@Brief: jump to the application
}

static void bootloader_fail_animation(void) {
    DEVICE_DELAY_US(30000);
    GPIO_togglePin(LED5);
}

//
// Main
//
void main(void)
{

    //
    // Initialize device clock and peripherals
    //
    Device_init();

    //
    // Disable pin locks and enable internal pull-ups.
    //
    Device_initGPIO();

    //
    // Initialize PIE and clear PIE registers. Disables CPU interrupts.
    //
    Interrupt_initModule();

    //
    // Initialize the PIE vector table with pointers to the shell Interrupt
    // Service Routines (ISR).
    //
    Interrupt_initVectorTable();

    //
    // PinMux and Peripheral Initialization
    //
    Board_init();

    //
    // C2000Ware Library initialization
    //
    C2000Ware_libraries_init();

    //
    // Enable Global Interrupt (INTM) and real time interrupt (DBGM)
    //
    EINT;
    ERTM;

    GPIO_writePin(LED4, 1);
    GPIO_writePin(LED5, 1);
    DEVICE_DELAY_US(100000);

    if(boot_update_and_decide() == FWU_BOOT_JUMP_APP){
        app_jump();
    }

    while(1)
    {
        bootloader_fail_animation();

    }
}

//
// End of File
//
