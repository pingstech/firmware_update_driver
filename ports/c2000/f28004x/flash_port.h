#ifndef FLASH_PORT_FLASH_PORT_H_
#define FLASH_PORT_FLASH_PORT_H_

#include <stdint.h>

typedef enum {
    FLASH_OK = 0,
    FLASH_ERR_PARAM,     /* invalid parameter (NULL, length 0, ...) */
    FLASH_ERR_RANGE,     /* outside the permitted region */
    FLASH_ERR_ALIGN,     /* alignment rule violated */
    FLASH_ERR_API,       /* a Fapi_ function returned an error */
    FLASH_ERR_FSM,       /* Fapi_getFsmStatus() is not zero */
    FLASH_ERR_VERIFY     /* written and read-back data differ */
} flash_status_e;

flash_status_e flash_erase_sector(uint32_t address);                              /* address: word address */
flash_status_e flash_write(uint32_t address, const uint16_t *data, uint16_t words);
flash_status_e flash_read(uint32_t address, uint16_t *data, uint16_t words);

#endif /* FLASH_PORT_FLASH_PORT_H_ */
