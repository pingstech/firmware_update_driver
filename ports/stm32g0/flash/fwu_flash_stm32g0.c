/**
 * @file    fwu_flash_stm32g0.c
 * @brief   STM32G0B1 flash port implementation (see fwu_flash_stm32g0.h for behaviour, errors and HARDWARE RISKS).
 */

#include <stddef.h>
#include "stm32g0xx_hal.h"
#include "fwu_flash_stm32g0.h"

#define PORT_GRANULE    8u          /* double word: 64 bit */

/* ------------------------------------------------------------------ helpers */

static void port_keepalive(const fwu_flash_stm32g0_t *port)
{
    if(port->keepalive != NULL) {
        port->keepalive(port->keepalive_context);
    }
}

/** Unlocks the flash and clears the option byte validation error flag (the earlier project's flash_api.c does the same). */
static bool port_unlock(void)
{
    if(HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR);
    return true;
}

/** Clears the ECC flags (without incrementing the counters). */
static void port_ecc_clear(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCC1);
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCD1);
#if defined(FLASH_DBANK_SUPPORT)
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCC2);
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCD2);
#endif
}

/** Counts and clears the ECC flags that are set. Returns true if a double error was detected. */
static bool port_ecc_account(fwu_flash_stm32g0_t *port)
{
    bool detected = false;

    if(__HAL_FLASH_GET_FLAG(FLASH_FLAG_ECCD1)) {
        detected = true;
    }
    if(__HAL_FLASH_GET_FLAG(FLASH_FLAG_ECCC1)) {
        if(port != NULL) {
            port->ecc_corrected++;
        }
    }
#if defined(FLASH_DBANK_SUPPORT)
    if(__HAL_FLASH_GET_FLAG(FLASH_FLAG_ECCD2)) {
        detected = true;
    }
    if(__HAL_FLASH_GET_FLAG(FLASH_FLAG_ECCC2)) {
        if(port != NULL) {
            port->ecc_corrected++;
        }
    }
#endif
    if(detected && (port != NULL)) {
        port->ecc_detected++;
    }
    port_ecc_clear();
    return detected;
}

/* ------------------------------------------------------------------ port functions */

static fwu_return_e port_erase(void *context, uint32_t offset, uint32_t length)
{
    fwu_flash_stm32g0_t *port = (fwu_flash_stm32g0_t *)context;
    const uint32_t flash_base = (uint32_t)FLASH_BASE;
    const uint32_t page_size = (uint32_t)FLASH_PAGE_SIZE;
    const uint32_t first_page = offset / page_size;
    const uint32_t page_count = length / page_size;
    fwu_return_e status = FWU_OK;

    if(!port_unlock()) {
        return FWU_ERR_FLASH_ERASE;
    }
    for(uint32_t i = 0u; i < page_count; i++) {
        const uint32_t address = flash_base + ((first_page + i) * page_size);
        const uint32_t bank2_base = flash_base + (uint32_t)FLASH_BANK_SIZE;     /* the HAL reads this at run time */
        FLASH_EraseInitTypeDef erase;
        uint32_t page_error = 0u;

        port_keepalive(port);
        erase.TypeErase = FLASH_TYPEERASE_PAGES;
        if(address >= bank2_base) {
            erase.Banks = FLASH_BANK_2;
            erase.Page = (address - bank2_base) / page_size;        /* page number WITHIN the bank */
        }
        else {
            erase.Banks = FLASH_BANK_1;
            erase.Page = (address - flash_base) / page_size;
        }
        erase.NbPages = 1u;
        if(HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
            status = FWU_ERR_FLASH_ERASE;
            break;
        }
    }
    (void)HAL_FLASH_Lock();                             /* lock in EVERY case */
    return status;
}

static fwu_return_e port_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data)
{
    fwu_flash_stm32g0_t *port = (fwu_flash_stm32g0_t *)context;
    const uint32_t granule_count = length / PORT_GRANULE;
    fwu_return_e status = FWU_OK;

    /* First check that the TARGET is erased: if not, never call the HAL (so no hardware error flag is raised). */
    for(uint32_t i = 0u; i < length; i++) {
        if((port->memory[offset + i] & FWU_BYTE_MASK) != 0xFFu) {
            return FWU_ERR_FLASH_NOT_ERASED;
        }
    }

    if(!port_unlock()) {
        return FWU_ERR_FLASH_WRITE;
    }
    for(uint32_t g = 0u; g < granule_count; g++) {
        uint64_t value = 0u;

        port_keepalive(port);
        for(uint32_t b = PORT_GRANULE; b > 0u; b--) {   /* little endian: byte 0 is the least significant byte */
            value = (value << 8) | (uint64_t)(data[(g * PORT_GRANULE) + (b - 1u)] & FWU_BYTE_MASK);
        }
        if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, (uint32_t)FLASH_BASE + offset + (g * PORT_GRANULE), value) != HAL_OK) {
            status = FWU_ERR_FLASH_WRITE;
            break;
        }
    }
    (void)HAL_FLASH_Lock();                             /* lock in EVERY case */
    return status;
}

static fwu_return_e port_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data)
{
    fwu_flash_stm32g0_t *port = (fwu_flash_stm32g0_t *)context;

    port_ecc_clear();                                   /* so the counters belong to THIS read only */
    for(uint32_t i = 0u; i < length; i++) {
        data[i] = (fwu_byte_t)(port->memory[offset + i] & FWU_BYTE_MASK);
    }
    (void)port_ecc_account(port);                       /* counts and clears; the read returns no error (consumers verify via CRC/signature) */
    return FWU_OK;
}

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_flash_stm32g0_init(fwu_flash_stm32g0_t *port, uint32_t flash_length, const volatile fwu_byte_t *memory,
                                    fwu_flash_stm32g0_keepalive_fn keepalive, void *keepalive_context)
{
    if((port == NULL) || (memory == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    /* If the layout is larger than the chip (e.g. a 512 KB layout on a 256 KB part), the regions would extend beyond the chip. */
    if((flash_length == 0u) || ((flash_length % (uint32_t)FLASH_PAGE_SIZE) != 0u) || (flash_length > (uint32_t)FLASH_SIZE)) {
        return FWU_ERR_FLASH_GEOMETRY;
    }

    port->callback.flash_erase = port_erase;
    port->callback.flash_write = port_write;
    port->callback.flash_read  = port_read;
    port->flash.context  = port;
    port->flash.callback = &port->callback;
    port->flash.geometry.page_length   = (uint32_t)FLASH_PAGE_SIZE;
    port->flash.geometry.flash_length  = flash_length;
    port->flash.geometry.write_granule = PORT_GRANULE;
    port->flash.geometry.erased_value  = 0xFFu;
    port->memory            = memory;
    port->keepalive         = keepalive;
    port->keepalive_context = keepalive_context;
    port->ecc_corrected     = 0u;
    port->ecc_detected      = 0u;

    return fwu_flash_validate(&port->flash);
}

fwu_return_e fwu_flash_stm32g0_init_mcu(fwu_flash_stm32g0_t *port, uint32_t flash_length,
                                        fwu_flash_stm32g0_keepalive_fn keepalive, void *keepalive_context)
{
    /* MISRA C:2012 Rule 11.4 deviation: integer-to-pointer conversion is unavoidable for a hardware address. */
    return fwu_flash_stm32g0_init(port, flash_length, (const volatile fwu_byte_t *)(uintptr_t)FLASH_BASE, keepalive,
                                  keepalive_context);
}

bool fwu_flash_stm32g0_handle_ecc_nmi(fwu_flash_stm32g0_t *port)
{
    return port_ecc_account(port);
}
