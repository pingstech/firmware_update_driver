#include "driverlib.h"
#include "board.h"
#include "app_fwu.h"

fwu_return_e app_sci_send(void *context, const fwu_byte_t *data, size_t length) {
    if(data == NULL || length == 0u) {
        return FWU_ERR_INVALID_ARG;
    }

    while(length) {
        SCI_writeCharBlockingFIFO(FW_SCI_BASE, *data & 0xFF);
        data++;
        length--;
    }

    return FWU_OK;
}
