#include <stddef.h>
#include "fwu_error.h"
#include "fwu_flash.h"

fwu_return_e fwu_flash_validate(const fwu_flash_t *self) {
    if(self == NULL || self->callback == NULL || self->callback->flash_erase == NULL || self->callback->flash_write == NULL || self->callback->flash_read == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    else if(self->geometry.flash_length == 0u || self->geometry.page_length == 0u || self->geometry.write_granule == 0u) {
        return FWU_ERR_FLASH_GEOMETRY;
    }
    else if(self->geometry.flash_length % self->geometry.page_length != 0u) {
        return FWU_ERR_FLASH_GEOMETRY;
    }
    else if(self->geometry.page_length % self->geometry.write_granule != 0u) {
        return FWU_ERR_FLASH_GEOMETRY;
    }
    else {
        return FWU_OK;
    }
}

fwu_return_e fwu_flash_erase(const fwu_flash_t *self, uint32_t offset, uint32_t length) {
    if(self == NULL || self->callback == NULL || self->callback->flash_erase == NULL || length == 0u) {
        return FWU_ERR_INVALID_ARG;
    }
    else if(offset > self->geometry.flash_length || length > self->geometry.flash_length - offset) {
        return FWU_ERR_OUT_OF_RANGE;
    }
    else if(offset % self->geometry.page_length != 0u || length % self->geometry.page_length != 0u) {
        return FWU_ERR_ERASE_ALIGNMENT;
    }
    else {
        return self->callback->flash_erase(self->context, offset, length);
    }
}

fwu_return_e fwu_flash_write(const fwu_flash_t *self, uint32_t offset, uint32_t length, const fwu_byte_t *data) {
    if(self == NULL || self->callback == NULL || self->callback->flash_write == NULL || length == 0u || data == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    else if(offset > self->geometry.flash_length || length > self->geometry.flash_length - offset) {
        return FWU_ERR_OUT_OF_RANGE;
    }
    else if(offset % self->geometry.write_granule != 0u || length % self->geometry.write_granule != 0u) {
        return FWU_ERR_WRITE_ALIGNMENT;
    }
    else {
        return self->callback->flash_write(self->context, offset, length, data);
    }
}

fwu_return_e fwu_flash_read(const fwu_flash_t *self, uint32_t offset, uint32_t length, fwu_byte_t *data) {
    if(self == NULL || self->callback == NULL || self->callback->flash_read == NULL || length == 0u || data == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    else if(offset > self->geometry.flash_length || length > self->geometry.flash_length - offset) {
        return FWU_ERR_OUT_OF_RANGE;
    }
    else {
        return self->callback->flash_read(self->context, offset, length, data);
    }
}
