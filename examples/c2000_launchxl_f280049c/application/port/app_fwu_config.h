#ifndef FWU_PORT_APP_FWU_CONFIG_H_
#define FWU_PORT_APP_FWU_CONFIG_H_

/* The application's firmware update settings. The bootloader must use the same values (product id, chunk size, policy):
 * the "contract fingerprint" in fwu_boot.h is derived from them. When the bootloader is linked to the driver, these values will be
 * moved to a COMMON file. The profile (FWU_PROFILE_DEV) and board (FWU_BOARD_F280049C) macros come from the project settings (-D). */







#define APP_FWU_PRODUCT_ID          2001u       /* must be the SAME as the "Product ID" in the PC tool and the product_id in the manifest */
#if defined(ver_1)
#define APP_FWU_RUNNING_VERSION     1u          /* Version of the running application (reported in INFO); the new image with LED5 will be 2 */
#else
#define APP_FWU_RUNNING_VERSION     2u          /* Version of the running application (reported in INFO); the new image with LED5 will be 2 */
#endif
#define APP_FWU_CHUNK_SIZE          256u        /* Data chunk (bytes): a multiple of the granule (16), divides the page (8192), <= 508 */
#define APP_FWU_IDLE_TIMEOUT_MS     30000u      /* The session drops if no frame is received for this long */
#define APP_FWU_WORK_SIZE           256u        /* Verification and activation read-back buffers (bytes) */
#define APP_FWU_MAX_TRIAL_BOOTS     3u          /* Used only in swap mode */
#define APP_FWU_RX_RING_SIZE        1024u       /* Receive ring buffer (bytes); the largest frame is 521 bytes, two frames fit */

/* The flash object covers ALL of flash: driver offsets (fwu_layout.h) are counted from the start of flash, address = 0x080000 + offset / 2 */
#define APP_FWU_FLASH_BASE_WORD     0x080000UL
#define APP_FWU_FLASH_LENGTH_OCTETS ((uint32_t)FWU_BOARD_TOTAL_PAGES * (uint32_t)FWU_BOARD_PAGE_SIZE)

/* Transport: 1 = firmware update over BLE (FC41D FFF1, hex text; fwu_port/app_ble.c). 0 = UART (app_sci.c, the FW SCI ISR
 * passes the bytes with app_fwu_rx_push). In BLE mode the main loop calls app_ble_poll, which runs the FC41D driver. */
#define APP_FWU_OVER_BLE            1

/* PULL mode: 1 = the device ITSELF pulls the image from the HTTP server on the PC (or a real one) (FC41D Wi-Fi STA + TCP client). Control
 * (HELLO, manifest, PULL_START, PULL_QUERY, TRANSFER_EXIT, ACTIVATE) is still over BLE, so APP_FWU_OVER_BLE = 1 is required.
 * The server's IP address and port come from the PC with PULL_START (the address of the UI's server mock). The Wi-Fi name/password are written into
 * fc41d_settings.wi_fi_sta_config in main. */
#define APP_FWU_PULL                1
#define APP_PULL_RANGE_BYTES        (APP_FWU_CHUNK_SIZE * 16u)  /* bytes requested by one GET (4096): a multiple of the chunk size AND the size of the range buffer in RAM (RAMGS2, 4096 elements). Large = fewer requests, small = less to repeat on error */
#define APP_PULL_RESPONSE_TIMEOUT_MS 8000u      /* if no byte arrives for this long after a request, the request is renewed */
#define APP_PULL_MAX_RETRIES        5u          /* number of retries for one range; when exceeded the pull becomes FAILED */
/* Wi-Fi and TCP: the FC41D is started with BLE only and they are enabled at the FIRST PULL_START (see the note in main): unless pull is used, the BLE update
 * is not affected by the network at all. */

#if APP_FWU_PULL && !APP_FWU_OVER_BLE
#error "APP_FWU_PULL icin APP_FWU_OVER_BLE = 1 gerekir (kontrol cerceveleri BLE ile gelir)"
#endif

/* BLE transport settings (used only when APP_FWU_OVER_BLE = 1) */
#define APP_BLE_TX_RING_SIZE        640u        /* device -> PC frame buffer (bytes): the largest response frames + margin */
#define APP_BLE_TX_GAP_MS           150u        /* minimum time between consecutive hex pieces (measured: 100 ms lossless, 50 ms lossy) */
#define APP_BLE_RX_STAGE_SIZE       576u        /* PC -> device staging buffer (bytes): largest frame FWU_FRAME_MAX_SIZE (521) + margin */
#define APP_BLE_RX_IDLE_MS          250u        /* after this much silence the staging buffer is given to the parser (even if the frame is not complete) */
#define APP_BLE_RX_ROUNDS           8u          /* maximum number of messages app_ble_poll pulls from the FC41D per round (BLE + TCP) */
#define APP_BLE_FLUSH_TIMEOUT_MS    3000u       /* maximum wait for the last response to go out before reset */
#define APP_BLE_FLUSH_SETTLE_MS     300u        /* extra wait after tx empties for the module to put it on the air */

#endif /* FWU_PORT_APP_FWU_CONFIG_H_ */
