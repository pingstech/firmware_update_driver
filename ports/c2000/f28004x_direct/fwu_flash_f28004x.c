/**
 * @file    fwu_flash_f28004x.c
 * @brief   TI F28004x flash port (see fwu_flash_f28004x.h: unit, addressing, running from RAM, risks).
 */

#include "fwu_flash_f28004x.h"
#include "F021_F28004x_C28x.h"      /* TI Flash API (in the PC test, the stub under hal_stub/) */

#define PORT_SECTOR_WORDS       FWU_F28004X_SECTOR_WORDS
#define PORT_GRANULE_OCTETS     FWU_F28004X_GRANULE_OCTETS
#define PORT_GRANULE_WORDS      (FWU_F28004X_GRANULE_OCTETS / 2u)
#define PORT_BLANK_WORD         0xFFFFu
#define PORT_MAX_FSM_WAIT       0x7FFFFFFFu     /* Upper bound of the FSM wait loop (guards against hanging; the real time is much shorter) */

/* On the MCU the read window is the flash base (word address 0x080000). The PC test (integration/launchxl_f280049c/host_test) turns this into a fake memory. */
#ifndef FWU_F28004X_MEMORY_WINDOW
#define FWU_F28004X_MEMORY_WINDOW   ((const volatile uint16_t *)(uintptr_t)FWU_F28004X_FLASH_WORD_BASE)
#endif

/* The programming buffer must be 64-bit aligned (TI example: "aligned on 64-bit boundary for verify reasons"). */
#ifdef __TI_COMPILER_VERSION__
#pragma DATA_ALIGN(g_word_buffer, 8)
#endif
/* Type: the Flash API's `uint16` (`unsigned int` in EABI), NOT stdint's uint16_t (`unsigned short` in EABI): that would cause a pointer mismatch. */
static uint16 g_word_buffer[PORT_GRANULE_WORDS];

/* ------------------------------------------------------------------ helpers */

#ifdef __TI_COMPILER_VERSION__
#define PORT_IRQ_SAVE()         __disable_interrupts()
#define PORT_IRQ_RESTORE(s)     __restore_interrupts(s)
typedef uint16_t port_irq_t;
#else
#define PORT_IRQ_SAVE()         0u
#define PORT_IRQ_RESTORE(s)     ((void)(s))
typedef unsigned int port_irq_t;
#endif

static void port_keepalive(const fwu_flash_f28004x_t *port)
{
    if(port->keepalive != NULL) {
        port->keepalive(port->keepalive_context);
    }
}

/** Flash word address from an octet offset (uint32 *: the Flash API's address type). */
static uint32 *port_word_pointer(uint32_t octet_offset)
{
    return (uint32 *)(uintptr_t)(FWU_F28004X_FLASH_WORD_BASE + (octet_offset / 2u));
}

/** Waits until the FSM is ready; returns true at the upper bound (hung). */
static bool port_wait_fsm(bool busy_means_wait)
{
    uint32_t guard = PORT_MAX_FSM_WAIT;

    if(busy_means_wait) {
        while((Fapi_checkFsmForReady() == Fapi_Status_FsmBusy) && (guard > 0u)) {
            guard--;
        }
    }
    else {
        while((Fapi_checkFsmForReady() != Fapi_Status_FsmReady) && (guard > 0u)) {
            guard--;
        }
    }
    return guard == 0u;
}

/* ------------------------------------------------------------------ port functions (run from RAM) */

static fwu_return_e port_erase(void *context, uint32_t offset, uint32_t length);
static fwu_return_e port_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data);

#ifdef __TI_COMPILER_VERSION__
#pragma CODE_SECTION(port_wait_fsm, ".TI.ramfunc")
#pragma CODE_SECTION(port_word_pointer, ".TI.ramfunc")
#pragma CODE_SECTION(port_erase, ".TI.ramfunc")
#pragma CODE_SECTION(port_write, ".TI.ramfunc")
#endif

static fwu_return_e port_erase(void *context, uint32_t offset, uint32_t length)
{
    fwu_flash_f28004x_t *port = (fwu_flash_f28004x_t *)context;
    const uint32_t page_count = length / FWU_F28004X_PAGE_OCTETS;
    fwu_return_e status = FWU_OK;

    for(uint32_t i = 0u; (i < page_count) && (status == FWU_OK); i++) {
        const uint32_t sector_offset = offset + (i * FWU_F28004X_PAGE_OCTETS);
        Fapi_FlashStatusWordType status_word;
        const port_irq_t irq = PORT_IRQ_SAVE();

        port_keepalive(port);
        if((Fapi_issueAsyncCommandWithAddress(Fapi_EraseSector, port_word_pointer(sector_offset)) != Fapi_Status_Success) ||
           port_wait_fsm(false) || (Fapi_getFsmStatus() != 0u)) {
            status = FWU_ERR_FLASH_ERASE;
        }
        else if(Fapi_doBlankCheck(port_word_pointer(sector_offset), PORT_SECTOR_WORDS / 2u, &status_word) != Fapi_Status_Success) {
            status = FWU_ERR_FLASH_ERASE;           /* the erase finished but the sector is not blank */
        }
        else {
            /* done */
        }
        PORT_IRQ_RESTORE(irq);
    }
    return status;
}

static fwu_return_e port_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data)
{
    fwu_flash_f28004x_t *port = (fwu_flash_f28004x_t *)context;
    const uint32_t granule_count = length / PORT_GRANULE_OCTETS;
    fwu_return_e status = FWU_OK;

    /* First check that the TARGET is erased: if not, never call the Flash API (so no hardware error flag is raised). */
    for(uint32_t w = 0u; w < (length / 2u); w++) {
        if(port->memory[(offset / 2u) + w] != PORT_BLANK_WORD) {
            return FWU_ERR_FLASH_NOT_ERASED;
        }
    }

    for(uint32_t g = 0u; (g < granule_count) && (status == FWU_OK); g++) {
        const uint32_t granule_offset = offset + (g * PORT_GRANULE_OCTETS);
        Fapi_FlashStatusWordType status_word;
        port_irq_t irq;

        for(uint32_t k = 0u; k < PORT_GRANULE_WORDS; k++) {     /* octet 2k = low byte, 2k+1 = high byte */
            const uint32_t low = (uint32_t)(data[(g * PORT_GRANULE_OCTETS) + (2u * k)] & FWU_BYTE_MASK);
            const uint32_t high = (uint32_t)(data[(g * PORT_GRANULE_OCTETS) + (2u * k) + 1u] & FWU_BYTE_MASK);

            g_word_buffer[k] = (uint16)(low | (high << 8));
        }
        irq = PORT_IRQ_SAVE();
        port_keepalive(port);
        if(Fapi_issueProgrammingCommand(port_word_pointer(granule_offset), g_word_buffer, (uint16)PORT_GRANULE_WORDS, 0, 0,
                                        Fapi_AutoEccGeneration) != Fapi_Status_Success) {
            status = FWU_ERR_FLASH_WRITE;
        }
        else if(port_wait_fsm(true) || (Fapi_getFsmStatus() != 0u)) {
            status = FWU_ERR_FLASH_WRITE;
        }
        else if(Fapi_doVerifyBy16bits((uint16 *)(uintptr_t)(FWU_F28004X_FLASH_WORD_BASE + (granule_offset / 2u)), PORT_GRANULE_WORDS,
                                      g_word_buffer, &status_word) != Fapi_Status_Success) {
            status = FWU_ERR_FLASH_WRITE;           /* written but could not be verified */
        }
        else {
            /* done */
        }
        PORT_IRQ_RESTORE(irq);
    }
    return status;
}

static fwu_return_e port_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data)
{
    const fwu_flash_f28004x_t *port = (const fwu_flash_f28004x_t *)context;

    for(uint32_t i = 0u; i < length; i++) {
        const uint32_t octet = offset + i;
        const uint16_t word = port->memory[octet / 2u];

        data[i] = (fwu_byte_t)(((octet & 1u) == 0u) ? (word & 0x00FFu) : ((word >> 8) & 0x00FFu));
    }
    return FWU_OK;
}

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_flash_f28004x_init(fwu_flash_f28004x_t *port, uint32_t flash_length, const volatile uint16_t *memory,
                                    fwu_flash_f28004x_keepalive_fn keepalive, void *keepalive_context)
{
    if((port == NULL) || (memory == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if((flash_length == 0u) || ((flash_length % FWU_F28004X_PAGE_OCTETS) != 0u) || (flash_length > FWU_F28004X_FLASH_OCTETS)) {
        return FWU_ERR_FLASH_GEOMETRY;
    }

    port->callback.flash_erase = port_erase;
    port->callback.flash_write = port_write;
    port->callback.flash_read  = port_read;
    port->flash.context  = port;
    port->flash.callback = &port->callback;
    port->flash.geometry.page_length   = FWU_F28004X_PAGE_OCTETS;
    port->flash.geometry.flash_length  = flash_length;
    port->flash.geometry.write_granule = PORT_GRANULE_OCTETS;
    port->flash.geometry.erased_value  = 0xFFu;
    port->memory            = memory;
    port->keepalive         = keepalive;
    port->keepalive_context = keepalive_context;

    return fwu_flash_validate(&port->flash);
}

fwu_return_e fwu_flash_f28004x_hw_init(uint32_t hclk_mhz, fwu_flash_f28004x_prefetch_fn prefetch)
{
    fwu_return_e status = FWU_OK;

    if(Fapi_initializeAPI(F021_CPU0_BASE_ADDRESS, hclk_mhz) != Fapi_Status_Success) {
        return FWU_ERR_FLASH_GEOMETRY;
    }
    if(prefetch != NULL) {
        prefetch(false);                                /* TI example: prefetch is off before setActiveFlashBank */
    }
    if(Fapi_setActiveFlashBank(Fapi_FlashBank0) != Fapi_Status_Success) {
        status = FWU_ERR_FLASH_GEOMETRY;
    }
    if(prefetch != NULL) {
        prefetch(true);                                 /* ... and on afterwards */
    }
    return status;
}

fwu_return_e fwu_flash_f28004x_init_mcu(fwu_flash_f28004x_t *port, uint32_t flash_length, uint32_t hclk_mhz,
                                        fwu_flash_f28004x_prefetch_fn prefetch,
                                        fwu_flash_f28004x_keepalive_fn keepalive, void *keepalive_context)
{
    const fwu_return_e status = fwu_flash_f28004x_hw_init(hclk_mhz, prefetch);

    if(status != FWU_OK) {
        return status;
    }
    /* MISRA C:2012 Rule 11.4 deviation: integer-to-pointer conversion is unavoidable for a hardware address. */
    return fwu_flash_f28004x_init(port, flash_length, FWU_F28004X_MEMORY_WINDOW, keepalive, keepalive_context);
}
