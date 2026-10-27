#include <stdbool.h>
#include <stddef.h>
#include "driverlib.h"
#include "device.h"
#include "F021_F28004x_C28x.h"
#include "flash_port.h"

/* ---- Constants (all are 16-bit WORD addresses / WORD counts) ---------------------------------- */
#define FLASH_START_ADDRESS     0x080000UL   /* First word of flash (Bank 0, sector 0) */
#define FLASH_END_ADDRESS       0x09FFFFUL   /* Last word of flash (Bank 1, sector 15) */

/* Which regions may be written and erased is project specific: the FLASH_PORT_WRITABLE(first, last)
 * macro in flash_port_config.h defines it. This keeps this file the SAME in every project. */
#include "flash_port_config.h"

#define FLASH_SECTOR_WORDS      0x1000u      /* 1 sector = 4K words */
#define FLASH_GROUP_WORDS       4u           /* 64 bit = 4 words: alignment and size unit of a write */
#define FLASH_CHUNK_WORDS       8u           /* Fapi_issueProgrammingCommand takes at most 8 words per call */
#define FLASH_ERASED_WORD       0xFFFFu      /* Every bit is 1 in erased flash */
#define FLASH_SYSCLK_MHZ        (DEVICE_SYSCLK_FREQ / 1000000UL)   /* Fapi_initializeAPI wants the clock in MHz */

static bool g_flash_ready = false;           /* Has the Flash API been prepared once? */

/* ---- Functions that run from RAM ----------------------------------------------------------
 * Flash must not be accessed during erase/write, so these functions go to the .TI.ramfunc section.
 * The linker stores this section in flash, Device_init() copies it to RAM at start-up.
 * The #pragma must be written BEFORE the function definition and AFTER a declaration.
 */
static bool flash_range_allowed(uint32_t address, uint32_t words);
static void flash_disable_prefetch(void);
static void flash_enable_prefetch(void);
static flash_status_e flash_prepare(void);

static flash_status_e flash_erase_sector_core(uint32_t address);
static flash_status_e flash_write_core(uint32_t address, const uint16_t *data, uint16_t words);

#pragma CODE_SECTION(flash_range_allowed, ".TI.ramfunc");
#pragma CODE_SECTION(flash_disable_prefetch, ".TI.ramfunc");
#pragma CODE_SECTION(flash_enable_prefetch, ".TI.ramfunc");
#pragma CODE_SECTION(flash_prepare, ".TI.ramfunc");
#pragma CODE_SECTION(flash_erase_sector_core, ".TI.ramfunc");
#pragma CODE_SECTION(flash_write_core, ".TI.ramfunc");
#pragma CODE_SECTION(flash_erase_sector, ".TI.ramfunc");
#pragma CODE_SECTION(flash_write, ".TI.ramfunc");

/* Is the address range entirely inside a region that is open for writing/erasing? */
static bool flash_range_allowed(uint32_t address, uint32_t words) {
    uint32_t last;

    if(words == 0u) {
        return false;
    }
    last = address + words - 1u;
    if(last < address) {                     /* 32-bit overflow: invalid */
        return false;
    }
    return FLASH_PORT_WRITABLE(address, last) ? true : false;
}

/* Disabling and re-enabling prefetch around the Fapi_setActiveFlashBank() call is TI's workaround in the example
 * (flashapi_ex1_program_autoecc.c, "SW_Workaround" functions). */
static void flash_disable_prefetch(void) {
    Flash_disablePrefetch(FLASH0CTRL_BASE);
    FLASH_DELAY_CONFIG;                      /* Flush the pipeline so that the register takes effect */
}

static void flash_enable_prefetch(void) {
    Flash_enablePrefetch(FLASH0CTRL_BASE);
    FLASH_DELAY_CONFIG;
}

/* Prepares the Flash API on the first call and returns immediately on later ones: no separate init call is needed from outside */
static flash_status_e flash_prepare(void) {
    Fapi_StatusType api_status;

    if(g_flash_ready) {
        return FLASH_OK;
    }
    api_status = Fapi_initializeAPI(F021_CPU0_BASE_ADDRESS, FLASH_SYSCLK_MHZ);
    if(api_status != Fapi_Status_Success) {
        return FLASH_ERR_API;
    }
    flash_disable_prefetch();
    api_status = Fapi_setActiveFlashBank(Fapi_FlashBank0);   /* A single call prepares both banks */
    flash_enable_prefetch();
    if(api_status != Fapi_Status_Success) {
        return FLASH_ERR_API;
    }
    g_flash_ready = true;
    return FLASH_OK;
}

/* Erases one sector. address: first word of the sector (a multiple of 0x1000) */
static flash_status_e flash_erase_sector_core(uint32_t address) {
    Fapi_StatusType api_status;
    Fapi_FlashStatusWordType status_word;
    flash_status_e status;

    if((address % FLASH_SECTOR_WORDS) != 0u) {
        return FLASH_ERR_ALIGN;
    }
    if(!flash_range_allowed(address, FLASH_SECTOR_WORDS)) {
        return FLASH_ERR_RANGE;
    }
    status = flash_prepare();
    if(status != FLASH_OK) {
        return status;
    }

    api_status = Fapi_issueAsyncCommandWithAddress(Fapi_EraseSector, (uint32 *)address);
    while(Fapi_checkFsmForReady() != Fapi_Status_FsmReady) {
        /* The command is asynchronous: wait until the state machine in the flash finishes */
    }
    if(api_status != Fapi_Status_Success) {
        return FLASH_ERR_API;
    }
    if(Fapi_getFsmStatus() != 0u) {          /* FMSTAT: erase error bits */
        return FLASH_ERR_FSM;
    }
    /* The erase performs its own verification, but let us also confirm once by a CPU read that the sector is really blank.
     * Fapi_doBlankCheck wants the length in 32-bit words: 0x1000 (16-bit) words = 0x800 32-bit words. */
    if(Fapi_doBlankCheck((uint32 *)address, FLASH_SECTOR_WORDS / 2u, &status_word) != Fapi_Status_Success) {
        return FLASH_ERR_VERIFY;
    }
    return FLASH_OK;
}

/* Writes the words data[0..words-1] starting at address. address must be a multiple of 4 (64-bit aligned).
 * If the last 64-bit group is not completely filled, the remaining words are padded with 0xFFFF. */
static flash_status_e flash_write_core(uint32_t address, const uint16_t *data, uint16_t words) {
    uint16 chunk[FLASH_CHUNK_WORDS];         /* Fapi wants a modifiable (non-const) buffer; a copy is also needed for padding */
    Fapi_StatusType api_status;
    Fapi_FlashStatusWordType status_word;
    flash_status_e status;
    uint32_t done = 0u;
    uint32_t padded_words;
    uint16_t count;
    uint16_t index;

    if((data == NULL) || (words == 0u)) {
        return FLASH_ERR_PARAM;
    }
    if((address % FLASH_GROUP_WORDS) != 0u) {
        return FLASH_ERR_ALIGN;
    }
    /* The area to be written occupies the length rounded up to a multiple of 4, including padding */
    padded_words = ((uint32_t)words + FLASH_GROUP_WORDS - 1u) / FLASH_GROUP_WORDS * FLASH_GROUP_WORDS;
    if(!flash_range_allowed(address, padded_words)) {
        return FLASH_ERR_RANGE;
    }
    status = flash_prepare();
    if(status != FLASH_OK) {
        return status;
    }

    while(done < words) {
        /* If more than 4 words remain write a chunk of 8, otherwise a chunk of 4 (the API accepts only 4 or 8) */
        count = ((words - done) > FLASH_GROUP_WORDS) ? FLASH_CHUNK_WORDS : FLASH_GROUP_WORDS;
        for(index = 0u; index < count; index++) {
            chunk[index] = ((done + index) < words) ? data[done + index] : FLASH_ERASED_WORD;
        }

        api_status = Fapi_issueProgrammingCommand((uint32 *)(address + done), chunk, count, 0, 0, Fapi_AutoEccGeneration);
        while(Fapi_checkFsmForReady() == Fapi_Status_FsmBusy) {
            /* Wait until the write finishes */
        }
        if(api_status != Fapi_Status_Success) {
            return FLASH_ERR_API;
        }
        if(Fapi_getFsmStatus() != 0u) {      /* FMSTAT: programming error bits */
            return FLASH_ERR_FSM;
        }
        /* Compare what was written with the buffer by a CPU read (16-bit version: causes no alignment problem) */
        if(Fapi_doVerifyBy16bits((uint16 *)(address + done), count, chunk, &status_word) != Fapi_Status_Success) {
            return FLASH_ERR_VERIFY;
        }
        done += count;
    }
    return FLASH_OK;
}

/* INTERRUPTS ARE DISABLED during erase/write. Reason: the application's code and the interrupt handlers (UART ISR) may reside in
 * Bank 0 and the staging area (sectors 12-15) in the same bank, and access to the bank being programmed is forbidden (datasheet
 * 7.3.3). Both of these wrappers run from RAM; the previous INTM state is restored. UART bytes arriving while interrupts are
 * disabled do not fit in the FIFO (16 bytes), so the protocol must be stop-and-wait: the host does not send a new chunk until
 * the acknowledgement arrives. */
flash_status_e flash_erase_sector(uint32_t address) {
    flash_status_e status;
    bool was_disabled = Interrupt_disableGlobal();

    status = flash_erase_sector_core(address);
    if(!was_disabled) {
        (void)Interrupt_enableGlobal();
    }
    return status;
}

flash_status_e flash_write(uint32_t address, const uint16_t *data, uint16_t words) {
    flash_status_e status;
    bool was_disabled = Interrupt_disableGlobal();

    status = flash_write_core(address, data, words);
    if(!was_disabled) {
        (void)Interrupt_enableGlobal();
    }
    return status;
}

/* Reads words words starting at address into data. Flash is read like normal memory, the Flash API is not needed.
 * Note: do not read from a bank that is being written/erased at that moment. */
flash_status_e flash_read(uint32_t address, uint16_t *data, uint16_t words) {
    const volatile uint16_t *source;
    uint16_t index;

    if((data == NULL) || (words == 0u)) {
        return FLASH_ERR_PARAM;
    }
    if((address < FLASH_START_ADDRESS) || ((address + words - 1u) > FLASH_END_ADDRESS)) {
        return FLASH_ERR_RANGE;
    }
    source = (const volatile uint16_t *)address;
    for(index = 0u; index < words; index++) {
        data[index] = source[index];
    }
    return FLASH_OK;
}
