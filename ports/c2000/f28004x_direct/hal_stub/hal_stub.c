/**
 * @file    hal_stub.c
 * @brief   TI Flash API TEST STUB (PC test, not vendor code, never built for a target): the part of the F28004x behaviour
 *          needed by the tests, on a 256 KB fake flash.
 *          Modelled: sector erase (4 KW), 64-bit aligned programming of at most 8 words, each 64-bit group once per erase,
 *          programming only turns 1 -> 0, fault injection (FMSTAT), and POWER LOSS (half erase: the first half of the sector
 *          is erased; half write: the first half of the words is written).
 *          Not modelled: ECC, timing, bank races (to be tried on a real chip).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "F021_F28004x_C28x.h"

#define BASE        0x00080000u
#define SECTOR      0x1000u
#define GROUPS      (HAL_STUB_FLASH_WORDS / 4u)

static uint16_t g_mem[HAL_STUB_FLASH_WORDS];
static uint8_t  g_group_programmed[GROUPS];
static hal_stub_stats_t g_stats;
static uint32_t g_ops;
static uint32_t g_cut_after;
static int g_cut_armed;
static int g_power_cut;
static uint32_t g_fmstat;

static int in_range(uint32_t address, uint32_t words)
{
    return (address >= BASE) && ((address - BASE) + words <= HAL_STUB_FLASH_WORDS);
}

void hal_stub_reset(void)
{
    memset(g_mem, 0xFF, sizeof(g_mem));
    memset(g_group_programmed, 0, sizeof(g_group_programmed));
    memset(&g_stats, 0, sizeof(g_stats));
    g_ops = 0u;
    g_cut_armed = 0;
    g_power_cut = 0;
    g_fmstat = 0u;
}

uint16_t *hal_stub_memory(void) { return g_mem; }
uint8_t *hal_stub_group_flags(void) { return g_group_programmed; }
const hal_stub_stats_t *hal_stub_stats(void) { return &g_stats; }
hal_stub_stats_t *hal_stub_stats_rw(void) { return &g_stats; }
uint32_t hal_stub_operations(void) { return g_ops; }
void hal_stub_cut_power_after(uint32_t operations) { g_cut_after = g_ops + operations; g_cut_armed = 1; }
void hal_stub_power_on(void) { g_power_cut = 0; g_cut_armed = 0; g_fmstat = 0u; }
int hal_stub_power_is_cut(void) { return g_power_cut; }

/* Power-loss check. 0: normal, 1: power already off (no operation is performed), 2: THIS operation is left half done. */
static int power_check(void)
{
    if(g_power_cut) {
        return 1;
    }
    if(g_cut_armed && (g_ops >= g_cut_after)) {
        g_power_cut = 1;
        return 2;
    }
    return 0;
}

Fapi_StatusType Fapi_initializeAPI(Fapi_FmcRegistersType *poFlashControlRegister, uint32 u32HclkFrequency)
{
    (void)poFlashControlRegister;
    g_stats.init_calls++;
    g_stats.hclk_mhz = u32HclkFrequency;
    return (u32HclkFrequency == 0u) ? Fapi_Error_Fail : Fapi_Status_Success;
}

Fapi_StatusType Fapi_setActiveFlashBank(Fapi_FlashBankType oFlashBank)
{
    (void)oFlashBank;
    g_stats.bank_calls++;
    return Fapi_Status_Success;
}

Fapi_StatusType Fapi_issueAsyncCommandWithAddress(Fapi_FlashStateCommandsType oCommand, uint32 *pu32StartAddress)
{
    const uint32_t address = (uint32_t)(uintptr_t)pu32StartAddress;
    int cut;

    g_fmstat = 0u;
    if((oCommand != Fapi_EraseSector) || !in_range(address, SECTOR) || (((address - BASE) % SECTOR) != 0u)) {
        return Fapi_Error_Fail;
    }
    cut = power_check();
    if(cut == 1) {
        return Fapi_Status_Success;                 /* no power: device is off, nothing happens */
    }
    g_ops++;
    if(getenv("HAL_STUB_TRACE") != NULL) {
        fprintf(stderr, "[hal] ERASE 0x%06lX%s\n", (unsigned long)address, (cut == 2) ? "  (POWER CUT: partial)" : "");
    }
    g_stats.erase_calls++;
    g_stats.last_erase_address = address;
    if(g_stats.erase_calls <= 64u) {
        g_stats.erase_log[g_stats.erase_calls - 1u] = address;
    }
    if((g_stats.fail_erase_at != 0u) && (g_stats.erase_calls == g_stats.fail_erase_at)) {
        g_fmstat = 0x20u;                           /* erase error (FMSTAT) */
        return Fapi_Status_Success;
    }
    if(cut == 2) {
        memset(&g_mem[address - BASE], 0xFF, (SECTOR / 2u) * sizeof(uint16_t));         /* half erase */
        memset(&g_group_programmed[(address - BASE) / 4u], 0, (SECTOR / 2u) / 4u);
        return Fapi_Status_Success;
    }
    memset(&g_mem[address - BASE], 0xFF, SECTOR * sizeof(uint16_t));
    memset(&g_group_programmed[(address - BASE) / 4u], 0, SECTOR / 4u);
    if((g_stats.fmstat_erase_at != 0u) && (g_stats.erase_calls == g_stats.fmstat_erase_at)) {
        g_fmstat = 0x20u;                           /* erase performed but the FMSTAT error bit stayed set */
    }
    return Fapi_Status_Success;
}

Fapi_StatusType Fapi_checkFsmForReady(void)
{
    return Fapi_Status_FsmReady;                    /* stub: the command finishes immediately */
}

Fapi_FlashStatusType Fapi_getFsmStatus(void)
{
    return g_fmstat;
}

Fapi_StatusType Fapi_doBlankCheck(uint32 *pu32StartAddress, uint32 u32Length, Fapi_FlashStatusWordType *poFlashStatusWord)
{
    const uint32_t address = (uint32_t)(uintptr_t)pu32StartAddress;

    (void)poFlashStatusWord;
    if(!in_range(address, u32Length * 2u)) {
        return Fapi_Error_Fail;
    }
    for(uint32_t i = 0u; i < (u32Length * 2u); i++) {
        if(g_mem[(address - BASE) + i] != 0xFFFFu) {
            return Fapi_Error_Fail;
        }
    }
    return Fapi_Status_Success;
}

Fapi_StatusType Fapi_issueProgrammingCommand(uint32 *pu32StartAddress, uint16 *pu16DataBuffer, uint16 u16DataBufferSizeInWords,
                                             uint16 *pu16EccBuffer, uint16 u16EccBufferSizeInBytes,
                                             Fapi_FlashProgrammingCommandsType oMode)
{
    const uint32_t address = (uint32_t)(uintptr_t)pu32StartAddress;
    int cut;

    g_fmstat = 0u;
    if((oMode != Fapi_AutoEccGeneration) || (pu16EccBuffer != 0) || (u16EccBufferSizeInBytes != 0u) ||
       ((u16DataBufferSizeInWords != 4u) && (u16DataBufferSizeInWords != 8u)) || (((address - BASE) % 4u) != 0u) ||
       !in_range(address, u16DataBufferSizeInWords)) {
        return Fapi_Error_Fail;                     /* TI: 4 or 8 words per call, 64-bit aligned */
    }
    cut = power_check();
    if(cut == 1) {
        return Fapi_Status_Success;
    }
    g_ops++;
    if(getenv("HAL_STUB_TRACE") != NULL) {
        fprintf(stderr, "[hal] WRITE 0x%06lX  %u words%s\n", (unsigned long)address, (unsigned)u16DataBufferSizeInWords,
                (cut == 2) ? "  (POWER CUT: partial)" : "");
    }
    g_stats.program_calls++;
    g_stats.last_program_address = address;
    g_stats.last_program_words = u16DataBufferSizeInWords;
    if((g_stats.fail_program_at != 0u) && (g_stats.program_calls == g_stats.fail_program_at)) {
        g_fmstat = 0x10u;                           /* program error (FMSTAT), nothing is written */
        return Fapi_Status_Success;
    }
    for(uint32_t g = 0u; g < (u16DataBufferSizeInWords / 4u); g++) {
        if(g_group_programmed[((address - BASE) / 4u) + g] != 0u) {
            g_fmstat = 0x10u;                       /* the 64-bit group was ALREADY programmed since this erase */
            return Fapi_Status_Success;
        }
    }
    {
        const uint32_t count = (cut == 2) ? (u16DataBufferSizeInWords / 2u) : u16DataBufferSizeInWords;     /* half write */

        for(uint32_t i = 0u; i < count; i++) {
            g_mem[(address - BASE) + i] = (uint16_t)(g_mem[(address - BASE) + i] & pu16DataBuffer[i]);       /* only 1 -> 0 */
        }
        for(uint32_t g = 0u; g < ((count + 3u) / 4u); g++) {
            g_group_programmed[((address - BASE) / 4u) + g] = 1u;
        }
    }
    if((g_stats.fmstat_program_at != 0u) && (g_stats.program_calls == g_stats.fmstat_program_at)) {
        g_fmstat = 0x10u;                           /* write performed but the FMSTAT error bit stayed set */
    }
    return Fapi_Status_Success;
}

Fapi_StatusType Fapi_doVerifyBy16bits(uint16 *pu16StartAddress, uint32 u16Length, uint16 *pu16CheckValueBuffer,
                                      Fapi_FlashStatusWordType *poFlashStatusWord)
{
    const uint32_t address = (uint32_t)(uintptr_t)pu16StartAddress;

    (void)poFlashStatusWord;
    if(!in_range(address, u16Length)) {
        return Fapi_Error_Fail;
    }
    for(uint32_t i = 0u; i < u16Length; i++) {
        if(g_mem[(address - BASE) + i] != pu16CheckValueBuffer[i]) {
            return Fapi_Error_Fail;
        }
    }
    return Fapi_Status_Success;
}
