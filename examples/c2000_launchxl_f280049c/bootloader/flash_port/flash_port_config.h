#ifndef FLASH_PORT_FLASH_PORT_CONFIG_H_
#define FLASH_PORT_FLASH_PORT_CONFIG_H_

/* This file is PROJECT-SPECIFIC. flash_port.c does not change in any project, only this file changes.
 *
 * FLASH_PORT_WRITABLE(first, last): yields a non-zero value if the range [first, last] (word addresses, both inclusive) lies
 * entirely inside a region that may be written and erased.
 *
 * Flash layout (fwu_driver, sector = 4096 words):
 *   sector  0..2    bootloader      0x080000 .. 0x082FFF
 *   sector  3..12   application     0x083000 .. 0x08CFFF
 *   sector 13..22   staging         0x08D000 .. 0x096FFF  (first sector is the manifest, image capacity 9 sectors)
 *   sector 23..26   log/config      0x097000 .. 0x09AFFF
 *   sector 27..28   counter A/B     0x09B000 .. 0x09CFFF
 *   sector 29..31   spare           0x09D000 .. 0x09FFEF  (journal A: 29, journal B: 30, scratch: 31)
 *
 * The BOOTLOADER project may write to the application region (copy), the counters and the journal. It only READS staging
 * (there is no restriction for reads). The bootloader's own region (sectors 0-2) must never be in the writable list.
 *   Application: 0x083000 .. 0x08CFFF
 *   Counters   : 0x09B000 .. 0x09CFFF (A and B)
 *   Journal    : 0x09D000 .. 0x09EFFF (A and B)
 */
#define FLASH_PORT_WRITABLE(first, last) \
    ((((first) >= 0x083000UL) && ((last) <= 0x08CFFFUL)) || \
     (((first) >= 0x09B000UL) && ((last) <= 0x09CFFFUL)) || \
     (((first) >= 0x09D000UL) && ((last) <= 0x09EFFFUL)))

#endif /* FLASH_PORT_FLASH_PORT_CONFIG_H_ */
