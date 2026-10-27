
MEMORY
{
PAGE 0 :
   /* Application entry point: the bootloader jumps here (0x083000 = Bank0 sector 3) */
   APP_BEGIN       : origin = 0x083000, length = 0x000002
   /* Rest of the application: 0x083002 .. 0x08CFFF (sectors 3..12, 10 sectors) */
   APP_FLASH       : origin = 0x083002, length = 0x009FFE

   RAMM0           	: origin = 0x0000F6, length = 0x00030A

   RAMLS03          : origin = 0x008000, length = 0x002000
   RAMLS4      		: origin = 0x00A000, length = 0x000800
   RESET           	: origin = 0x3FFFC0, length = 0x000002

PAGE 1 :

   BOOT_RSVD       : origin = 0x000002, length = 0x0000F1     /* Part of M0, BOOT rom will use this for stack */
   RAMM1           : origin = 0x000400, length = 0x0003F8     /* on-chip RAM block M1 */
//   RAMM1_RSVD      : origin = 0x0007F8, length = 0x000008     /* Reserve and do not use for code as per the errata advisory "Memory: Prefetching Beyond Valid Memory" */

   RAMLS5      : origin = 0x00A800, length = 0x000800
   RAMLS6      : origin = 0x00B000, length = 0x000800
   RAMLS7      : origin = 0x00B800, length = 0x000800

   RAMGS0      : origin = 0x00C000, length = 0x002000
   RAMGS1      : origin = 0x00E000, length = 0x002000
   RAMGS2      : origin = 0x010000, length = 0x002000
   RAMGS3      : origin = 0x012000, length = 0x001FF8
//   RAMGS3_RSVD : origin = 0x013FF8, length = 0x000008     /* Reserve and do not use for code as per the errata advisory "Memory: Prefetching Beyond Valid Memory" */
}


SECTIONS
{
   codestart        : > APP_BEGIN, PAGE = 0, ALIGN(4)
   .text            : > APP_FLASH, PAGE = 0, ALIGN(4)
   .cinit           : > APP_FLASH, PAGE = 0, ALIGN(4)
   .switch          : > APP_FLASH, PAGE = 0, ALIGN(4)
   .reset           : > RESET,     PAGE = 0, TYPE = DSECT /* not used, */

   .stack           : > RAMM1,     PAGE = 1

#if defined(__TI_EABI__)
   .init_array      : > APP_FLASH,       PAGE = 0,       ALIGN(4)
   .bss             : > RAMGS0,       PAGE = 1        /* fwu_driver + receive buffers do not fit into RAMLS5 (2 KW) */
   .bss:output      : > RAMLS03,      PAGE = 0
   .bss:cio         : > RAMLS03,      PAGE = 0
   .data            : > RAMLS5,       PAGE = 1
   .sysmem          : > RAMLS5,       PAGE = 1
   /* Initalized sections go in Flash */
   .const           : > APP_FLASH,       PAGE = 0,       ALIGN(4)
#else
   .pinit           : > APP_FLASH,       PAGE = 0,       ALIGN(4)
   .ebss            : > RAMLS5,       PAGE = 1
   .esysmem         : > RAMLS5,       PAGE = 1
   .cio             : > RAMLS0,       PAGE = 0
   .econst          : > APP_FLASH,    PAGE = 0, ALIGN(4)
#endif

   ramgs0           : > RAMGS0,    PAGE = 1
   ramgs1           : > RAMGS1,    PAGE = 1
   ramgs2           : > RAMGS2,    PAGE = 1    /* fwu_pull range buffer (g_chunk in app_pull.c, 4096 elements) */
   debug_data       : > RAMGS1,    PAGE = 1    /* FC41D driver object (~1.8 KW); not cleared at startup, fc41d_api_initialize clears it */

 
#if defined(__TI_EABI__)
   GROUP
   {
       .TI.ramfunc
       { -l F021_API_F28004x_FPU32_EABI.lib }
   } LOAD = APP_FLASH,
     RUN  = RAMLS03,
     LOAD_START(RamfuncsLoadStart),
     LOAD_SIZE(RamfuncsLoadSize),
     LOAD_END(RamfuncsLoadEnd),
     RUN_START(RamfuncsRunStart),
     RUN_SIZE(RamfuncsRunSize),
     RUN_END(RamfuncsRunEnd),
     PAGE = 0, ALIGN(4)
#else					  
   .TI.ramfunc      : LOAD = APP_FLASH,
                      RUN = RAMLS0,
                      LOAD_START(_RamfuncsLoadStart),
                      LOAD_SIZE(_RamfuncsLoadSize),
                      LOAD_END(_RamfuncsLoadEnd),
                      RUN_START(_RamfuncsRunStart),
                      RUN_SIZE(_RamfuncsRunSize),
                      RUN_END(_RamfuncsRunEnd),
                      PAGE = 0, ALIGN(4)
#endif

}

/*
//===========================================================================
// End of file.
//===========================================================================
*/
