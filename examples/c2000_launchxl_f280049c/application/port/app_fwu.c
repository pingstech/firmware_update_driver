#include <stddef.h>
#include "driverlib.h"
#include "device.h"
#include "board.h"
#include "app_fwu.h"
#include "app_fwu_config.h"
#include "app_ble.h"
#include "app_pull.h"
#include "fwu_boot_profile.h"
#include "fwu_boot.h"
#include "fwu_crypto_sw.h"
#include "fwu_flash_adapter.h"
#include "fwu_parser.h"
#include "fwu_ringbuf.h"
#include "fwu_session.h"

/* Receive ring buffer (the ISR writes, the main loop reads through the parser: single producer / single consumer) */
FWU_RINGBUF_DEFINE(app_rx, APP_FWU_RX_RING_SIZE);

static app_rx_t            g_rx;
static fwu_parser_t        g_parser;
static fwu_link_t          g_link;
static fwu_session_t       g_session;
static fwu_transport_t     g_transport;
static fwu_flash_adapter_t g_flash;
static fwu_crypto_sw_t     g_crypto_sw;
static fwu_crypto_t        g_crypto;
static fwu_boot_t          g_boot;
static fwu_boot_config_t   g_boot_config;
static fwu_byte_t          g_verify_work[APP_FWU_WORK_SIZE];
static fwu_byte_t          g_activate_work[APP_FWU_WORK_SIZE];
static fwu_byte_t          g_session_work[APP_FWU_CHUNK_SIZE];
static fwu_return_e        g_status = FWU_ERR_BAD_STATE;     /* "not ready" until init is called */
static bool                g_ready = false;

/* Sets up the session. Flash, activation (commit journal) and verification settings come from fwu_boot: the SAME
 * configuration as the bootloader, so the application writes the commit and the bootloader reads the same journal. */
static fwu_return_e app_fwu_session_setup(void) {
    fwu_session_config_t config;

    config.flash = &g_flash.flash;
    config.activate = fwu_boot_activate(&g_boot);
    config.verify = fwu_boot_verify_config(&g_boot);
    config.auth = NULL;                         /* DEV profile: no authorization */
    config.require_auth = false;
    config.running_image_version = (uint32_t)APP_FWU_RUNNING_VERSION;
    config.chunk_size = APP_FWU_CHUNK_SIZE;
    config.idle_timeout_ms = APP_FWU_IDLE_TIMEOUT_MS;
    config.auth_max_failures = 3u;
    config.auth_lockout_ms = 60000u;
    config.work = g_session_work;
    config.work_length = (uint32_t)APP_FWU_CHUNK_SIZE;
    return fwu_session_init(&g_session, &config);
}

fwu_return_e app_fwu_init(void) {
    fwu_return_e status;

    g_ready = false;

    status = fwu_flash_adapter_init(&g_flash, APP_FWU_FLASH_BASE_WORD, APP_FWU_FLASH_LENGTH_OCTETS);
    if(status == FWU_OK) {
        status = fwu_crypto_sw_init(&g_crypto_sw, &g_crypto);
    }
    if(status == FWU_OK) {
        g_boot_config.policy = fwu_boot_policy_from_profile();
        g_boot_config.layout = fwu_boot_layout_from_board();
        g_boot_config.flash = &g_flash.flash;
        g_boot_config.crypto = &g_crypto;
        g_boot_config.keys = NULL;              /* DEV profile: no signature, SHA-256 digest check only */
        g_boot_config.key_count = 0u;
        g_boot_config.product_id = APP_FWU_PRODUCT_ID;
        g_boot_config.max_trial_boots = APP_FWU_MAX_TRIAL_BOOTS;
        g_boot_config.verify_work = g_verify_work;
        g_boot_config.verify_work_length = APP_FWU_WORK_SIZE;
        g_boot_config.activate_work = g_activate_work;
        g_boot_config.activate_work_length = APP_FWU_WORK_SIZE;
        g_boot_config.app_valid = NULL;
        g_boot_config.watchdog_arm = NULL;
        g_boot_config.context = NULL;
        status = fwu_boot_init(&g_boot, &g_boot_config);
    }
    if(status == FWU_OK) {
        status = app_rx_init(&g_rx);
    }
    if(status == FWU_OK) {
        status = fwu_parser_init(&g_parser, &g_rx.base);
    }
    if(status == FWU_OK) {
        status = app_fwu_session_setup();
    }
    if(status == FWU_OK) {
        fwu_link_config_t link_config;

#if APP_FWU_OVER_BLE
        g_transport.send = app_ble_send;
#else
        g_transport.send = app_sci_send;
#endif
        g_transport.poll = NULL;
        g_transport.context = NULL;
        link_config.session = &g_session;
        link_config.parser = &g_parser;
        link_config.transport = &g_transport;
        status = fwu_link_init(&g_link, &link_config);
    }
#if APP_FWU_PULL
    if(status == FWU_OK) {
        (void)app_pull_init(&g_session, &g_link);   /* if it fails pull is unavailable (g_app_pull_init_status), the BLE update is not affected */
    }
#endif

    g_status = status;
    g_ready = (status == FWU_OK);
    return status;
}

fwu_return_e app_fwu_status(void) {
    return g_status;
}

bool app_fwu_rx_push(fwu_byte_t value) {
    return (fwu_ringbuf_push(&g_rx.base, &value, 1u) == FWU_OK) ? true : false;
}

/* The session asked for a restart (ACTIVATE_OK) and the last response was handed to the link. So that the reply ACTUALLY goes out on the wire, first
 * the SCI TX FIFO and shift register are emptied, then a short time is waited; then reset. Does not return. */
#define APP_FWU_REBOOT_SETTLE_US    20000UL

static void app_fwu_reboot(void) {
#if APP_FWU_OVER_BLE
    app_ble_flush(APP_BLE_FLUSH_TIMEOUT_MS, APP_BLE_FLUSH_SETTLE_MS);   /* until the last response has left the FC41D onto the air (bounded) */
#endif
    while((SCI_getTxFIFOStatus(FW_SCI_BASE) != SCI_FIFO_TX0) || SCI_isTransmitterBusy(FW_SCI_BASE)) {
    }
    DEVICE_DELAY_US(APP_FWU_REBOOT_SETTLE_US);
    DINT;
    SysCtl_resetDevice();
    while(1) {
    }
}

void app_fwu_poll(uint32_t now_ms) {
    fwu_byte_t sink[16];
    size_t used;
    size_t count;

    if(g_ready) {
        (void)fwu_link_poll(&g_link, now_ms);
        if(fwu_link_reboot_ready(&g_link)) {
            app_fwu_reboot();       /* ACTIVATE_OK was delivered to the PC: let the bootloader apply the commit */
        }
        return;
    }
    /* Not ready (setup error): discard incoming bytes so the ring buffer does not fill up. The PC will time out. */
    used = fwu_ringbuf_used_size(&g_rx.base);
    while(used > 0u) {
        count = (used > 16u) ? 16u : used;
        if(fwu_ringbuf_pull(&g_rx.base, sink, count) != FWU_OK) {
            break;
        }
        used -= count;
    }
}

const fwu_link_stats_t *app_fwu_stats(void) {
    return fwu_link_get_stats(&g_link);
}
