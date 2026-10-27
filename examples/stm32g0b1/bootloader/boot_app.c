/**
 * @file    boot_app.c
 * @brief   Bootloader uygulama katmani (see boot_app.h for the overall flow and limits).
 */

#include "fwu_platform.h"           /* profile and board macros: BEFORE fwu_boot_profile.h */
#include "boot_app.h"
#include "fwu_boot_profile.h"
#include "fwu_crypto_sw.h"
#include "fwu_flash_stm32g0.h"

volatile fwu_boot_result_t g_boot_result;

static fwu_flash_stm32g0_t g_flash_port;
static fwu_crypto_sw_t     g_crypto_sw;
static fwu_crypto_t        g_crypto;
static fwu_boot_t          g_boot;
static fwu_boot_config_t   g_config;
static fwu_byte_t          g_verify_work[FWU_PLATFORM_WORK_SIZE];
static fwu_byte_t          g_activate_work[FWU_PLATFORM_WORK_SIZE];

/* ------------------------------------------------------------------ hooks */

/** Can the application be jumped to: the stack in the vector table is in RAM, the reset vector is in the application region and odd (Thumb). */
static bool app_valid(void *context)
{
    const volatile uint32_t *vectors = (const volatile uint32_t *)FWU_PLATFORM_APP_ADDRESS;
    const uint32_t stack = vectors[0];
    const uint32_t reset = vectors[1];

    (void)context;
    if((stack <= FWU_PLATFORM_RAM_BASE) || (stack > FWU_PLATFORM_RAM_END) || ((stack & 3u) != 0u)) {
        return false;                                   /* empty flash (0xFFFFFFFF) or invalid stack */
    }
    if(((reset & 1u) == 0u) || ((reset - 1u) < FWU_PLATFORM_APP_ADDRESS) || ((reset - 1u) >= (FWU_PLATFORM_APP_ADDRESS + FWU_PLATFORM_APP_MAX_SIZE))) {
        return false;
    }
    return true;
}

/** Trial boot (swap mode only): if the new application hangs, the independent watchdog resets, so that a rollback happens at the next start. */
static void watchdog_arm(void *context)
{
    (void)context;
    IWDG->KR  = 0x0000CCCCu;            /* start (the LSI is enabled by hardware) */
    IWDG->KR  = 0x00005555u;            /* unlock PR / RLR */
    IWDG->PR  = 4u;                     /* LSI / 64 = ~500 Hz */
    IWDG->RLR = 2000u;                  /* ~4 s (same as the application's own watchdog) */
    while(IWDG->SR != 0u) {
    }
    IWDG->KR  = 0x0000AAAAu;            /* reload the counter */
}

/* ------------------------------------------------------------------ jump */

static void jump_to_app(void)
{
    const volatile uint32_t *vectors = (const volatile uint32_t *)FWU_PLATFORM_APP_ADDRESS;
    const uint32_t stack = vectors[0];
    const uint32_t reset = vectors[1];

    __disable_irq();
    SysTick->CTRL = 0u;                 /* the HAL's 1 ms interrupt stops */
    SysTick->LOAD = 0u;
    SysTick->VAL = 0u;
    NVIC->ICER[0] = 0xFFFFFFFFu;        /* Cortex-M0+: 32 interrupt lines */
    NVIC->ICPR[0] = 0xFFFFFFFFu;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;   /* leave no pending SysTick / PendSV */
    (void)HAL_RCC_DeInit();             /* return the clock to its reset state (the application sets up its own clock) */
    SCB->VTOR = FWU_PLATFORM_APP_ADDRESS;
    __set_MSP(stack);
    __enable_irq();                     /* PRIMASK is 0 after a real reset; if left at 1 the application's SysTick never fires
                                         * (HAL_Delay waits forever). NVIC and SysTick were disabled above: the transition is safe */
    ((void (*)(void))reset)();
    for(;;) {
    }
}

/* ------------------------------------------------------------------ main flow */

void boot_app_run(void)
{
    fwu_boot_decision_e decision = FWU_BOOT_STAY;
    fwu_boot_result_t result;

    result.decision = FWU_BOOT_STAY;
    result.reason = FWU_BOOT_REASON_CONFIG;
    result.status = FWU_ERR_BAD_STATE;
    result.outcome = FWU_ACT_OUTCOME_NONE;
    result.phase = FWU_ACT_PHASE_IDLE;
    result.trial = false;

    if((fwu_flash_stm32g0_init_mcu(&g_flash_port, FWU_BOARD_TOTAL_PAGES * FWU_BOARD_PAGE_SIZE, NULL, NULL) == FWU_OK) &&
       (fwu_crypto_sw_init(&g_crypto_sw, &g_crypto) == FWU_OK)) {
        g_config.policy = fwu_boot_policy_from_profile();
        g_config.layout = fwu_platform_boot_layout();     /* board layout + 78-page image limit (SAME as the application) */
        g_config.flash = &g_flash_port.flash;
        g_config.crypto = &g_crypto;
        g_config.keys = NULL;                           /* DEV profile: no signature */
        g_config.key_count = 0u;
        g_config.product_id = FWU_PLATFORM_PRODUCT_ID;
        g_config.max_trial_boots = FWU_PLATFORM_MAX_TRIAL_BOOTS;
        g_config.verify_work = g_verify_work;
        g_config.verify_work_length = FWU_PLATFORM_WORK_SIZE;
        g_config.activate_work = g_activate_work;
        g_config.activate_work_length = FWU_PLATFORM_WORK_SIZE;
        g_config.app_valid = app_valid;
        g_config.watchdog_arm = watchdog_arm;
        g_config.context = NULL;
        decision = fwu_boot_start(&g_boot, &g_config, &result);

        /* Contract fingerprint: the application compares it with its own configuration; if they do not match it rejects the update. */
        fwu_byte_t record[FWU_BOOT_INFO_SIZE];
        size_t record_length = 0u;
        if(fwu_boot_info_encode(&g_config, record, sizeof(record), &record_length) == FWU_OK) {
            fwu_bootinfo_put_contract(record);
        }
    }
    g_boot_result = result;
    fwu_bootinfo_put((uint32_t)result.outcome, (uint32_t)result.reason);

    if(decision == FWU_BOOT_JUMP_APP) {
        jump_to_app();
    }
    for(;;) {                           /* STAY: do not jump to the application; the cause is in g_boot_result.reason. NO sleep: a debugger cannot attach to a sleeping chip */
    }
}

void boot_app_nmi(void)
{
    if(fwu_flash_stm32g0_handle_ecc_nmi(&g_flash_port)) {
        return;                         /* flash ECC double error detection: flags cleared, NO reset */
    }
    NVIC_SystemReset();
}
