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
#include <string.h>
#include "driverlib.h"
#include "device.h"
#include "board.h"
#include "c2000ware_libraries.h"
#include "flash_port.h"
#include "fwu_flash_adapter.h"
#include "app_fwu.h"
#include "app_tick.h"
#include "app_fwu_config.h"
#include "app_ble.h"
#include "app_pull.h"
#include "FC41D_api.h"
#include "FC41D_bridge.h"



#if defined(ver_1)
#define APP_BLINK_LED LED4
#define LED_BLINK_HALF_PERIOD_MS    500u    /* LED4 toggles every 500 ms (blinks at 1 Hz) */
#else
#define APP_BLINK_LED LED5
#define LED_BLINK_HALF_PERIOD_MS    300u    /* LED4 toggles every 500 ms (blinks at 1 Hz) */
#endif

/* Which UART is the FC41D on? 1: FW UART (SCIB, GPIO13/40) - the FC41D talks here, and UART firmware update CANNOT be used in this mode
 * (same port, FW RX interrupt disabled). 0: FC41D on its own UART (SCIA, GPIO28/29) and firmware update runs on SCIB.
 * The module received no data on the GPIO28/29 lines (the LaunchPad XDS110 back channel is suspected); hence 1 for now. */
#define FC41D_ON_FW_UART            1
#if FC41D_ON_FW_UART
#define FC41D_UART_BASE             FW_SCI_BASE
#else
#define FC41D_UART_BASE             FC41D_SCI_BASE
#endif

static volatile fwu_return_e fwu_init_status;   /* result of app_fwu_init(); read from the debugger, expected 0 (FWU_OK) */

/* FC41D test settings: BLE GATT + Wi-Fi access point (AP) + TCP server. The phone first connects with the password of the "FC41D_AP"
 * network, the TCP client connects to port 8080 at the module's AP address. No EN pin: enable_pin_* is NULL (the module is always powered). */
#if APP_FWU_PULL
/* PULL mode settings: BLE (control) + Wi-Fi station (STA) + TCP client (image download).
 *   - type = starts with BLE ONLY: Wi-Fi / TCP are enabled when PULL_START arrives (app_pull.c: fc41d_api_enable_features). Reason: attempts to
 *     join the network and connect to the server (especially repeatedly while the server is down) keep the FC41D's single AT command line and the radio
 *     busy; a BLE update (including the host-sends mode) then slows down or drops. The Wi-Fi / TCP settings below stay ready and wait until used.
 *   - wi_fi_sta_config : FILL THIS IN YOURSELF (Wi-Fi name / password). If left empty the module cannot join the network; BLE still works.
 *   - tcp_client_config: INITIAL target (placeholder). The host UI sends the real ip/port in the PULL_START frame and the device
 *                        re-establishes the connection to that target; so changing this is not mandatory. It cannot be zero (the driver rejects it).
 *   - config.is_dhcp_enabled: obtain the IP address from the router (DHCP); if false, static_ip_config (zero) would be used.
 *   - config.tcp_hex_send: TCP data is sent as hex text (AT+QICFG="datatype",1): required for the CR/LF in the HTTP request. */
static const fc41d_api_settings_t fc41d_settings = {
    .type = FC41D_BLE_GATT_WITH_WIFI_STA_AND_TCP_CLIENT,
    .setup = {
        .config            = { .is_dhcp_enabled = true, .tcp_hex_send = true },
        .ble_gatt_config   = { .device_name = "FC41D_Dev", .service_uuid = "FFF0", .characteristic_uuid = "FFF1" },
        .wi_fi_sta_config  = { .ssid = "YOUR_WIFI_SSID", .password = "YOUR_WIFI_PASSWORD" },   /* placeholder: set your own credentials */
        .tcp_client_config = { .ip_address = { 172u, 16u, 200u, 59u }, .port = 8080u },
    },
};
#else
/* FC41D test settings: BLE GATT only (firmware update over BLE). */
static const fc41d_api_settings_t fc41d_settings = {
    .type = FC41D_BLE_GATT_ONLY,
    .setup = {
        .ble_gatt_config   = { .device_name = "FC41D_Dev", .service_uuid = "FFF0", .characteristic_uuid = "FFF1" },
        .wi_fi_ap_config   = { .ssid = "FC41D_AP", .password = "CHANGE_ME_AP_PASSWORD" },   /* placeholder */
        .tcp_server_config = { .port = 8080 },
    },
};
#endif
volatile uint32_t g_fc41d_isr_count;            /* number of times the FC41D RX interrupt was entered; watch it in the debugger */
#if !APP_FWU_OVER_BLE
static uint32_t fc41d_tick_last;                /* the tick is given here when there is no BLE transport (app_ble.c keeps its own tick) */
#endif

/* BLE echo test (until TCP connects): the text written by the phone is sent back as a notification "ECHO: <text>".
 * 1: echo enabled, fc41d_bridge_run is not called. 0: the BLE <-> TCP bridge runs. */
#define FC41D_BLE_ECHO_TEST         1

#if FC41D_BLE_ECHO_TEST && !APP_FWU_OVER_BLE
static char     fc41d_echo_in[FC41D_MESSAGE_MAX];       /* static so the stack does not overflow */
static char     fc41d_echo_out[FC41D_MESSAGE_MAX + 8u];
volatile uint32_t g_fc41d_echo_received;        /* number of BLE messages received from the phone */
volatile uint32_t g_fc41d_echo_sent;            /* number of echoes that were queued */
volatile uint32_t g_fc41d_echo_refused;         /* echo could not be queued (BLE off / queue full / empty text) */

static void fc41d_ble_echo(void) {
    uint16_t length = 0u;
    uint16_t index;

    if(fc41d_get_message(&g_fc41d_api, fc41d_echo_in, &length) != FC41D_MESSAGE_BLE) {
        return;                                         /* no message, or not BLE */
    }
    g_fc41d_echo_received++;

    for(index = 0u; index < length; index++) {          /* the BLE text must not contain CR/LF or control characters: cut at the first one */
        if(((uint16_t)fc41d_echo_in[index] < 0x20u) || ((uint16_t)fc41d_echo_in[index] > 0x7Eu)) {
            break;
        }
    }
    length = index;
    if(length == 0u) {
        g_fc41d_echo_refused++;
        return;
    }

    memcpy(fc41d_echo_out, "ECHO: ", 6u);
    memcpy(&fc41d_echo_out[6], fc41d_echo_in, length);
    if(fc41d_api_ble_send(&g_fc41d_api, fc41d_echo_out, (uint16_t)(length + 6u)) == 0u) {
        g_fc41d_echo_refused++;
    }
    else {
        g_fc41d_echo_sent++;
    }
}
#endif                /* last ms given to the driver; ticks are not lost even if the main loop slows down */

//------------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------------
static fwu_flash_adapter_t g_adapter;
static volatile uint16_t adapter_test_status;   /* 0 = OK, otherwise fwu_return_e */
static volatile uint16_t adapter_test_step;

/* Self-test for fwu_flash_adapter (through the driver interface). On failure, adapter_test_step tells
 * which step it stopped at:
 *  1 prepare the bridge | 2 erase the first page | 3 write 32 bytes | 4 read back and compare
 *  5 read the words directly (packing: low byte first) | 6 read from a single offset
 *  7-10 negative cases (unaligned write, unaligned erase, out-of-region read, erasing the application sector)
 *  11 erase the first page again (cleanup) */
static fwu_return_e adapter_selftest(void) {
    fwu_byte_t pattern[32];
    fwu_byte_t readback[32];
    uint16_t words[2];
    fwu_flash_adapter_t other;
    fwu_return_e status;
    uint16_t index;

    for(index = 0u; index < 32u; index++) {
        pattern[index] = (fwu_byte_t)(index * 7u + 3u);     /* each byte stays within 0..255 */
    }

    adapter_test_step = 1u;                               /* prepare the bridge for staging (10 sectors = 81920 bytes, 0x08C000) */
    status = fwu_flash_adapter_init(&g_adapter, 0x08C000UL, 81920u);
    if(status != FWU_OK) {
        return status;
    }

    adapter_test_step = 2u;                               /* erase the first page of staging (8192 bytes) */
    status = fwu_flash_erase(&g_adapter.flash, 0u, 8192u);
    if(status != FWU_OK) {
        return status;
    }

    adapter_test_step = 3u;                               /* write 32 bytes (2 granules) */
    status = fwu_flash_write(&g_adapter.flash, 0u, 32u, pattern);
    if(status != FWU_OK) {
        return status;
    }

    adapter_test_step = 4u;                               /* read back, is it the same as what was written? */
    status = fwu_flash_read(&g_adapter.flash, 0u, 32u, readback);
    if(status != FWU_OK) {
        return status;
    }
    for(index = 0u; index < 32u; index++) {
        if(readback[index] != pattern[index]) {
            return FWU_ERR_BAD_STATE;
        }
    }

    adapter_test_step = 5u;                               /* packing: word = low byte | (high byte << 8) */
    if(flash_read(0x08C000UL, words, 2u) != FLASH_OK) {
        return FWU_ERR_BAD_STATE;
    }
    if((words[0] != (uint16_t)(pattern[0] | (pattern[1] << 8))) ||
       (words[1] != (uint16_t)(pattern[2] | (pattern[3] << 8)))) {
        return FWU_ERR_BAD_STATE;
    }

    adapter_test_step = 6u;                               /* read from a single offset: the first byte is the UPPER half of the word */
    status = fwu_flash_read(&g_adapter.flash, 1u, 31u, readback);
    if(status != FWU_OK) {
        return status;
    }
    for(index = 0u; index < 31u; index++) {
        if(readback[index] != pattern[index + 1u]) {
            return FWU_ERR_BAD_STATE;
        }
    }

    /* Negative cases: the correct error code must be returned, nothing may be written to or erased from flash */
    adapter_test_step = 7u;                               /* write offset is not a multiple of the granule (16 bytes) */
    if(fwu_flash_write(&g_adapter.flash, 8u, 16u, pattern) != FWU_ERR_WRITE_ALIGNMENT) {
        return FWU_ERR_BAD_STATE;
    }
    adapter_test_step = 8u;                               /* erase offset is not a multiple of the page (8192 bytes) */
    if(fwu_flash_erase(&g_adapter.flash, 100u, 8192u) != FWU_ERR_ERASE_ALIGNMENT) {
        return FWU_ERR_BAD_STATE;
    }
    adapter_test_step = 9u;                               /* read that goes past the end of the region */
    if(fwu_flash_read(&g_adapter.flash, 81920u - 4u, 10u, readback) != FWU_ERR_OUT_OF_RANGE) {
        return FWU_ERR_BAD_STATE;
    }
    adapter_test_step = 10u;                              /* the application's sector: flash_port must not allow erasing it */
    status = fwu_flash_adapter_init(&other, 0x082000UL, 8192u);
    if(status != FWU_OK) {
        return status;
    }
    if(fwu_flash_erase(&other.flash, 0u, 8192u) != FWU_ERR_OUT_OF_RANGE) {
        return FWU_ERR_BAD_STATE;
    }

    adapter_test_step = 11u;                              /* cleanup: erase the first page of staging again */
    return fwu_flash_erase(&g_adapter.flash, 0u, 8192u);
}

//------------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------------


#define RX_BUF_SIZE  256u                    /* make it a power of 2: wrap with a mask */
#define RX_ALLIGN_TIMEOUT 5u

typedef struct {
    volatile uint16_t buf[RX_BUF_SIZE];
    volatile uint16_t head;                  /* written only by the ISR */
    volatile uint16_t tail;                  /* written only by main */
    volatile uint16_t overflow;              /* buffer full, byte dropped */
    volatile uint16_t rx_errors;             /* number of SCI errors */
    volatile uint32_t tick;
} rx_ring_buff_t;

static volatile rx_ring_buff_t fw_rx;
static uint16_t rx_data[RX_BUF_SIZE];

static uint16_t sci_fw_ring_buffer_push(const uint16_t *data, uint16_t length) {
    uint16_t written = 0u;

    if(!data || length == 0u) {
        return 0u;
    }
    for(uint16_t index = 0u; index < length; index++) {
        uint16_t next = (fw_rx.head + 1u) & (RX_BUF_SIZE - 1u);

        if(next == fw_rx.tail) {
            break;                                  /* full: do not overwrite unread data */
        }
        fw_rx.buf[fw_rx.head] = data[index];        /* data first */
        fw_rx.head = next;                          /* then head */
        written++;
    }
    return written;
}

static uint16_t sci_fw_ring_buffer_pull(uint16_t *data, uint16_t length) {
    uint16_t count = 0u;

    if(!data || length == 0u) {
        return 0u;
    }
    while(count < length) {
        if(fw_rx.tail == fw_rx.head) {
            break;                                  /* empty */
        }
        data[count] = fw_rx.buf[fw_rx.tail];        /* read first */
        fw_rx.tail = (fw_rx.tail + 1u) & (RX_BUF_SIZE - 1u);   /* then tail */
        count++;
    }
    return count;
}

static bool sci_fw_ring_buffer_is_empty(void) {
    return (fw_rx.tail == fw_rx.head) ? true : false;
}

static uint16_t sci_fw_ring_buffer_used_size(void) {
    return (fw_rx.head >= fw_rx.tail) ? fw_rx.head - fw_rx.tail : (RX_BUF_SIZE - fw_rx.tail) + fw_rx.head;
}

#define FLASH_TEST_ADDRESS  0x08C000UL   /* First staging sector (Bank 0 sector 12) */
#define FLASH_TEST_WORDS    16u

static volatile uint16_t flash_test_status;   /* 0 = FLASH_OK, otherwise a flash_status_e value */
static volatile uint16_t flash_test_step;     /* step at which the test stopped (step numbers below) */

/* Self-test for flash_port. On failure flash_test_step tells which step failed:
 *  1 erase | 2 erased? | 3 write | 4 read back and compare
 *  5-8 negative cases (unaligned erase, erasing the application sector, unaligned write, null parameter)
 *  9-11 6-word write (padding 0xFFFF) | 12 clean the sector after the test */
static flash_status_e flash_selftest(void) {
    uint16_t pattern[FLASH_TEST_WORDS];
    uint16_t readback[FLASH_TEST_WORDS];
    flash_status_e status;
    uint16_t index;

    for(index = 0u; index < FLASH_TEST_WORDS; index++) {
        pattern[index] = index;                       /* 0x0000, 0x0001, ... 0x000F */
    }

    flash_test_step = 1u;                             /* erase the sector */
    status = flash_erase_sector(FLASH_TEST_ADDRESS);
    if(status != FLASH_OK) {
        return status;
    }

    flash_test_step = 2u;                             /* erased? all must be 0xFFFF */
    status = flash_read(FLASH_TEST_ADDRESS, readback, FLASH_TEST_WORDS);
    if(status != FLASH_OK) {
        return status;
    }
    for(index = 0u; index < FLASH_TEST_WORDS; index++) {
        if(readback[index] != 0xFFFFu) {
            return FLASH_ERR_VERIFY;
        }
    }

    flash_test_step = 3u;                             /* write 16 words */
    status = flash_write(FLASH_TEST_ADDRESS, pattern, FLASH_TEST_WORDS);
    if(status != FLASH_OK) {
        return status;
    }

    flash_test_step = 4u;                             /* read back and compare */
    status = flash_read(FLASH_TEST_ADDRESS, readback, FLASH_TEST_WORDS);
    if(status != FLASH_OK) {
        return status;
    }
    for(index = 0u; index < FLASH_TEST_WORDS; index++) {
        if(readback[index] != pattern[index]) {
            return FLASH_ERR_VERIFY;
        }
    }

    /* Negative cases: the functions must return the correct error code. If the expected code does not come back we return FLASH_ERR_PARAM. */
    flash_test_step = 5u;                             /* address not on a sector boundary */
    if(flash_erase_sector(FLASH_TEST_ADDRESS + 1u) != FLASH_ERR_ALIGN) {
        return FLASH_ERR_PARAM;
    }
    flash_test_step = 6u;                             /* the application's sector: erasing must not be allowed */
    if(flash_erase_sector(0x082000UL) != FLASH_ERR_RANGE) {
        return FLASH_ERR_PARAM;
    }
    flash_test_step = 7u;                             /* write address not aligned to 64 bits */
    if(flash_write(FLASH_TEST_ADDRESS + 2u, pattern, 4u) != FLASH_ERR_ALIGN) {
        return FLASH_ERR_PARAM;
    }
    flash_test_step = 8u;                             /* NULL data and zero length */
    if((flash_write(FLASH_TEST_ADDRESS, NULL, 4u) != FLASH_ERR_PARAM) ||
       (flash_write(FLASH_TEST_ADDRESS, pattern, 0u) != FLASH_ERR_PARAM)) {
        return FLASH_ERR_PARAM;
    }

    /* 6-word write: padded up to 8, words 6 and 7 (0-based) must remain 0xFFFF */
    flash_test_step = 9u;
    status = flash_erase_sector(FLASH_TEST_ADDRESS);
    if(status != FLASH_OK) {
        return status;
    }
    flash_test_step = 10u;
    status = flash_write(FLASH_TEST_ADDRESS, pattern, 6u);
    if(status != FLASH_OK) {
        return status;
    }
    flash_test_step = 11u;
    status = flash_read(FLASH_TEST_ADDRESS, readback, 8u);
    if(status != FLASH_OK) {
        return status;
    }
    for(index = 0u; index < 8u; index++) {
        if(readback[index] != ((index < 6u) ? pattern[index] : 0xFFFFu)) {
            return FLASH_ERR_VERIFY;
        }
    }

    flash_test_step = 12u;                            /* leave staging empty */
    return flash_erase_sector(FLASH_TEST_ADDRESS);
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
    // 1 ms time counter and the firmware update layer. Vector table is ready, interrupts still disabled (before EINT).
    //
    app_tick_init();
    fwu_init_status = app_fwu_init();
    (void)fc41d_api_initialize(&g_fc41d_api, FC41D_UART_BASE, &fc41d_settings);  /* 115200 8N1 is set up in Board_init */
#if APP_FWU_OVER_BLE
    app_ble_init(app_tick_ms());                    /* BLE transport: ring buffers and the FC41D tick time */
#endif

    //
    // Enable Global Interrupt (INTM) and real time interrupt (DBGM)
    //
    EINT;
    ERTM;

    GPIO_writePin(LED4, 1);
    GPIO_writePin(LED5, 1);

#if !FC41D_ON_FW_UART
    Interrupt_enable(INT_FW_SCI_RX);                        /* with the FC41D on SCIB the FW RX interrupt is not enabled: the FC41D reads the bytes */
#endif

    fw_rx.tick = 0;
    fw_rx.head = 0;
    fw_rx.tail = 0;

//    flash_test_status = (uint16_t)flash_selftest();
//    if(flash_test_status != (uint16_t)FLASH_OK) {
//        GPIO_writePin(LED5, 0);                       /* Error: light LED5 (LEDs are active-low: 0 = on) */
//    }
//
//    adapter_test_status = (uint16_t)adapter_selftest();
//    if(adapter_test_status != (uint16_t)FWU_OK) {
//        GPIO_writePin(LED5, 0);                       /* Error: light LED5 */
//    }

    uint32_t now;
    uint32_t led_last = app_tick_ms();
#if !APP_FWU_OVER_BLE
    fc41d_tick_last = led_last;
#endif

    while(1)
    {
        now = app_tick_ms();

        app_fwu_poll(now);                                  /* does not block: processes incoming frames, sends replies */

#if APP_FWU_OVER_BLE
        app_ble_poll(now);                                  /* FC41D tick + polling, BLE <-> firmware update, does not block */
#if APP_FWU_PULL
        app_pull_poll(now);                                 /* pull: connection, credit; when the ring has data, heavy work (flash) while the module is quiet */
#endif
#else
        while((uint32_t)(now - fc41d_tick_last) >= 1u) {    /* one tick to the FC41D for each elapsed millisecond */
            (void)fc41d_api_1ms_timer(&g_fc41d_api);
            fc41d_tick_last++;
        }
        (void)fc41d_api_polling(&g_fc41d_api);              /* does not block: UART receive queue -> parser -> state machines */
#if FC41D_BLE_ECHO_TEST
        fc41d_ble_echo();                                   /* BLE echo test */
#else
        fc41d_bridge_run(&g_fc41d_api);                     /* test bridge: BLE <-> TCP */
#endif
#endif

        if((uint32_t)(now - led_last) >= LED_BLINK_HALF_PERIOD_MS) {   /* subtraction: also correct across a 32-bit wrap */
            GPIO_togglePin(APP_BLINK_LED);
            led_last = now;
        }
    }
}


__interrupt void INT_FW_SCI_RX_ISR(void) {
    if(SCI_getRxStatus(FW_SCI_BASE) & SCI_RXSTATUS_ERROR) {
        fw_rx.rx_errors++;
        if(SCI_getOverflowStatus(FW_SCI_BASE)) {   /* ask first */
            fw_rx.overflow++;
            SCI_clearOverflowStatus(FW_SCI_BASE);
        }
        SCI_performSoftwareReset(FW_SCI_BASE);     /* then clear the error flags */
        SCI_resetRxFIFO(FW_SCI_BASE);              /* always */
    }
    else {
        while(SCI_getRxFIFOStatus(FW_SCI_BASE) != SCI_FIFO_RX0) {
            uint16_t byte = SCI_readCharNonBlocking(FW_SCI_BASE);
            if(!app_fwu_rx_push((fwu_byte_t)(byte & 0xFFu))) {
                fw_rx.overflow++;
            }
        }
    }

    SCI_clearInterruptStatus(FW_SCI_BASE, SCI_INT_RXFF);
    Interrupt_clearACKGroup(INT_FW_SCI_RX_INTERRUPT_ACK_GROUP);
}

__interrupt void INT_FC41D_SCI_RX_ISR(void) {
    g_fc41d_isr_count++;
    /* FC41D reception is handled in the main loop (fc41d_api_polling); this interrupt is disabled (Interrupt_enable is not called), it exists only for board.c's registration */
    SCI_clearInterruptStatus(FC41D_SCI_BASE, SCI_INT_RXFF);
    Interrupt_clearACKGroup(INT_FC41D_SCI_RX_INTERRUPT_ACK_GROUP);
}
//
// End of File
//
