#ifndef FWU_PORT_BOOT_FWU_CONFIG_H_
#define FWU_PORT_BOOT_FWU_CONFIG_H_

/* The bootloader's firmware update settings. The values must be the SAME as fwu_port/app_fwu_config.h in the application:
 * the "contract fingerprint" in fwu_boot.h is derived from the product id, trial boot count, policy, flash layout and geometry.
 * If the application and the bootloader are built with different values, the commit cannot be read / is misinterpreted. After it works, these values
 * will be moved to a header shared by the two projects. The profile (FWU_PROFILE_DEV) and board (FWU_BOARD_F280049C) come from the project settings (-D). */

#define BOOT_FWU_PRODUCT_ID          2001u       /* SAME as APP_FWU_PRODUCT_ID in the application */
#define BOOT_FWU_MAX_TRIAL_BOOTS     3u          /* SAME as APP_FWU_MAX_TRIAL_BOOTS in the application (used only in swap mode) */
#define BOOT_FWU_WORK_SIZE           256u        /* verification and activation read-back buffers (bytes) */

/* The flash object covers ALL of flash: driver offsets are counted from the start of flash, address = 0x080000 + offset / 2 */
#define BOOT_FWU_FLASH_BASE_WORD     0x080000UL
#define BOOT_FWU_FLASH_LENGTH_OCTETS ((uint32_t)FWU_BOARD_TOTAL_PAGES * (uint32_t)FWU_BOARD_PAGE_SIZE)

#endif /* FWU_PORT_BOOT_FWU_CONFIG_H_ */
