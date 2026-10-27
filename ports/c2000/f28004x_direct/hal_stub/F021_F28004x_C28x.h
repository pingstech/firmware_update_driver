/**
 * @file    F021_F28004x_C28x.h  (PC TEST STUB, not vendor code: stands in for the TI header of the same name)
 * @brief   Type and function declarations for the subset of the Flash API used by fwu_flash_f28004x.c. Same name and same
 *          signatures as the real header; the behaviour is in hal_stub.c (fake flash). Used only in tests, never built for
 *          a target (not part of the MCU build).
 */
#ifndef HAL_STUB_F021_F28004X_C28X_H_
#define HAL_STUB_F021_F28004X_C28X_H_

#include <stdint.h>

typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint32_t Fapi_FlashStatusType;
typedef struct { uint32_t dummy; } Fapi_FlashStatusWordType;
typedef struct { uint32_t dummy; } Fapi_FmcRegistersType;

typedef enum { Fapi_Status_Success = 0, Fapi_Status_FsmBusy, Fapi_Status_FsmReady, Fapi_Error_Fail } Fapi_StatusType;
typedef enum { Fapi_FlashBank0, Fapi_FlashBank1 } Fapi_FlashBankType;
typedef enum { Fapi_EraseSector = 0x0006 } Fapi_FlashStateCommandsType;
typedef enum { Fapi_AutoEccGeneration, Fapi_DataOnly } Fapi_FlashProgrammingCommandsType;

#define F021_CPU0_BASE_ADDRESS ((Fapi_FmcRegistersType *)0)

Fapi_StatusType Fapi_initializeAPI(Fapi_FmcRegistersType *poFlashControlRegister, uint32 u32HclkFrequency);
Fapi_StatusType Fapi_setActiveFlashBank(Fapi_FlashBankType oFlashBank);
Fapi_StatusType Fapi_issueAsyncCommandWithAddress(Fapi_FlashStateCommandsType oCommand, uint32 *pu32StartAddress);
Fapi_StatusType Fapi_checkFsmForReady(void);
Fapi_FlashStatusType Fapi_getFsmStatus(void);
Fapi_StatusType Fapi_doBlankCheck(uint32 *pu32StartAddress, uint32 u32Length, Fapi_FlashStatusWordType *poFlashStatusWord);
Fapi_StatusType Fapi_issueProgrammingCommand(uint32 *pu32StartAddress, uint16 *pu16DataBuffer, uint16 u16DataBufferSizeInWords,
                                             uint16 *pu16EccBuffer, uint16 u16EccBufferSizeInBytes,
                                             Fapi_FlashProgrammingCommandsType oMode);
Fapi_StatusType Fapi_doVerifyBy16bits(uint16 *pu16StartAddress, uint32 u16Length, uint16 *pu16CheckValueBuffer,
                                      Fapi_FlashStatusWordType *poFlashStatusWord);

/* ---- test control ---- */
#define HAL_STUB_FLASH_WORDS    131072u         /* 256 KB */

typedef struct {
    uint32_t erase_calls;
    uint32_t program_calls;
    uint32_t init_calls;
    uint32_t bank_calls;
    uint32_t last_erase_address;                /* word address */
    uint32_t last_program_address;
    uint32_t last_program_words;
    uint32_t erase_log[64];                     /* word address of the first 64 erases */
    uint32_t fail_erase_at;                     /* >0: the erase with this (1-based) sequence number fails */
    uint32_t fail_program_at;                   /* >0: the program operation with this sequence number fails (no word is written) */
    uint32_t fmstat_erase_at;                   /* >0: the erase with this sequence number IS PERFORMED but the FMSTAT error bit stays set (data correct, status bad) */
    uint32_t fmstat_program_at;                 /* >0: the program operation with this sequence number IS PERFORMED but the FMSTAT error bit stays set */
    uint32_t hclk_mhz;
} hal_stub_stats_t;

void hal_stub_reset(void);                      /* flash fully erased (0xFFFF), counters zero, power on */
uint16_t *hal_stub_memory(void);                /* fake flash of 131072 words; address = 0x080000 + index */
#define HAL_STUB_GROUPS (HAL_STUB_FLASH_WORDS / 4u)
uint8_t *hal_stub_group_flags(void);            /* per 64-bit group, a 'programmed since this erase' flag (must be saved TOGETHER with the memory when taking a snapshot) */
const hal_stub_stats_t *hal_stub_stats(void);
hal_stub_stats_t *hal_stub_stats_rw(void);
uint32_t hal_stub_operations(void);             /* number of erases + programs (number of power-cut points) */
void hal_stub_cut_power_after(uint32_t operations);   /* power is CUT after this many operations: the next operation is left HALF done */
void hal_stub_power_on(void);
int hal_stub_power_is_cut(void);

#endif
