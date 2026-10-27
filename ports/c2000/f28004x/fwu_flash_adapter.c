#include <stddef.h>
#include "fwu_flash_adapter.h"
#include "flash_port.h"

#define ADAPTER_PAGE_OCTETS     8192u   /* 1 sector = 4096 words = 8192 bytes */
#define ADAPTER_PAGE_WORDS      4096u
#define ADAPTER_GRANULE_OCTETS  16u     /* 8 words = 128 bit */
#define ADAPTER_CHUNK_WORDS     32u     /* stack buffer; must be a multiple of 4 (64-bit group boundary) */
#define ADAPTER_OCTET_MASK      0xFFu

/* Converts a flash_port error to fwu_return_e. The mapping of alignment and hardware errors depends on the operation. */
static fwu_return_e adapter_map_status(flash_status_e status, fwu_return_e align_error, fwu_return_e hardware_error) {
    switch(status) {
        case FLASH_OK:
            return FWU_OK;
        case FLASH_ERR_PARAM:
            return FWU_ERR_INVALID_ARG;
        case FLASH_ERR_RANGE:
            return FWU_ERR_OUT_OF_RANGE;
        case FLASH_ERR_ALIGN:
            return align_error;
        default:                    /* FLASH_ERR_API, FLASH_ERR_FSM, FLASH_ERR_VERIFY */
            return hardware_error;
    }
}

/* Erases page by page. offset and length are already multiples of the page size (fwu_flash_erase verifies this). */
static fwu_return_e adapter_erase(void *context, uint32_t offset, uint32_t length) {
    const fwu_flash_adapter_t *self = (const fwu_flash_adapter_t *)context;
    uint32_t address = self->base_address + (offset / 2u);
    uint32_t pages = length / ADAPTER_PAGE_OCTETS;
    uint32_t index;
    fwu_return_e result;

    for(index = 0u; index < pages; index++) {
        result = adapter_map_status(flash_erase_sector(address + (index * ADAPTER_PAGE_WORDS)),
                                    FWU_ERR_ERASE_ALIGNMENT, FWU_ERR_FLASH_ERASE);
        if(result != FWU_OK) {
            return result;
        }
    }
    return FWU_OK;
}

/* Packs the bytes two by two into words and writes them chunk by chunk. offset and length are multiples of the granule (16 bytes). */
static fwu_return_e adapter_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data) {
    const fwu_flash_adapter_t *self = (const fwu_flash_adapter_t *)context;
    uint16_t chunk[ADAPTER_CHUNK_WORDS];
    uint32_t base_word = self->base_address + (offset / 2u);
    uint32_t words_total = length / 2u;
    uint32_t done = 0u;
    uint32_t count;
    uint32_t index;
    uint16_t low;
    uint16_t high;
    fwu_return_e result;

    while(done < words_total) {
        count = words_total - done;
        if(count > ADAPTER_CHUNK_WORDS) {
            count = ADAPTER_CHUNK_WORDS;
        }
        for(index = 0u; index < count; index++) {
            low = (uint16_t)(data[2u * (done + index)] & ADAPTER_OCTET_MASK);
            high = (uint16_t)(data[(2u * (done + index)) + 1u] & ADAPTER_OCTET_MASK);
            chunk[index] = (uint16_t)(low | (uint16_t)(high << 8));
        }
        result = adapter_map_status(flash_write(base_word + done, chunk, (uint16_t)count),
                                    FWU_ERR_WRITE_ALIGNMENT, FWU_ERR_FLASH_WRITE);
        if(result != FWU_OK) {
            return result;
        }
        done += count;
    }
    return FWU_OK;
}

/* There is no alignment requirement for reads: offset and length may be odd. Reads words and splits them into bytes. */
static fwu_return_e adapter_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data) {
    const fwu_flash_adapter_t *self = (const fwu_flash_adapter_t *)context;
    uint16_t chunk[ADAPTER_CHUNK_WORDS];
    uint32_t done = 0u;
    uint32_t position;
    uint32_t skip;
    uint32_t remaining;
    uint32_t words;
    uint32_t available;
    uint32_t take;
    uint32_t index;
    uint32_t octet_index;
    uint16_t word;
    fwu_return_e result;

    while(done < length) {
        position = offset + done;
        skip = position & 1u;                       /* at an odd offset the first byte is the UPPER half of the word */
        remaining = length - done;
        words = (skip + remaining + 1u) / 2u;
        if(words > ADAPTER_CHUNK_WORDS) {
            words = ADAPTER_CHUNK_WORDS;
        }
        result = adapter_map_status(flash_read(self->base_address + (position / 2u), chunk, (uint16_t)words),
                                    FWU_ERR_INVALID_ARG, FWU_ERR_INVALID_ARG);
        if(result != FWU_OK) {
            return result;
        }
        available = (words * 2u) - skip;
        take = (available < remaining) ? available : remaining;
        for(index = 0u; index < take; index++) {
            octet_index = skip + index;
            word = chunk[octet_index / 2u];
            data[done + index] = (fwu_byte_t)(((octet_index & 1u) != 0u) ? ((word >> 8) & ADAPTER_OCTET_MASK)
                                                                         : (word & ADAPTER_OCTET_MASK));
        }
        done += take;
    }
    return FWU_OK;
}

static const fwu_flash_callback_t g_adapter_callbacks = {
    adapter_erase,
    adapter_write,
    adapter_read
};

fwu_return_e fwu_flash_adapter_init(fwu_flash_adapter_t *self, uint32_t base_address, uint32_t length_octets) {
    if(self == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    self->base_address = base_address;
    self->flash.context = self;
    self->flash.callback = &g_adapter_callbacks;
    self->flash.geometry.page_length = ADAPTER_PAGE_OCTETS;
    self->flash.geometry.flash_length = length_octets;
    self->flash.geometry.write_granule = ADAPTER_GRANULE_OCTETS;
    self->flash.geometry.erased_value = 0xFFu;
    return fwu_flash_validate(&self->flash);
}
