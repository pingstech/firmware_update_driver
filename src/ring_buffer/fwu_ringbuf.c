/**
 * @file    fwu_ringbuf.c
 * @brief   Ring buffer implementation. Interface and design decisions: fwu_ringbuf.h.
 *
 * Because the value range of the indices (head, tail) is [0, 2*capacity):
 *   used = (head - tail) mod (2*capacity)     (0 = empty, capacity = full)
 *   cell position = index, or index - capacity if the index exceeds capacity
 * All intermediate computations are written to stay within the uint16_t range (2*capacity <= 65534);
 * so there is no overflow even with a 16-bit int (C28x).
 */

#include "fwu_ringbuf.h"

/** Is the buffer usable? (NULL, no storage or no capacity = not initialized) */
static bool ringbuf_is_valid(const fwu_ringbuf_t *self)
{
    return (self != NULL) && (self->storage != NULL) && (self->capacity != 0u);
}

/** Size of the index range: 2 * capacity. */
static uint16_t ringbuf_span(const fwu_ringbuf_t *self)
{
    return (uint16_t)(2u * (uint16_t)self->capacity);
}

/**
 * Number of used bytes. Reads head and tail ONCE (snapshot).
 * The other side's index may be stale; this is on the safe side
 * (less free space is seen by the producer, less data by the consumer).
 */
static uint16_t ringbuf_used(const fwu_ringbuf_t *self)
{
    const uint16_t head = self->head;
    const uint16_t tail = self->tail;
    uint16_t used;

    if(head >= tail) {
        used = (uint16_t)(head - tail);
    }
    else {
        used = (uint16_t)((uint16_t)(ringbuf_span(self) - tail) + head);
    }

    return used;
}

/** Advances the index by amount, wrapping within [0, span) (without overflow). */
static uint16_t ringbuf_advance(uint16_t index, uint16_t amount, uint16_t span)
{
    const uint16_t room = (uint16_t)(span - index);    /* 1 .. span */
    uint16_t result;

    if(amount >= room) {
        result = (uint16_t)(amount - room);
    }
    else {
        result = (uint16_t)(index + amount);
    }

    return result;
}

/** Finds the cell position from the index. */
static uint16_t ringbuf_position(uint16_t index, uint16_t capacity)
{
    uint16_t position = index;

    if(index >= capacity) {
        position = (uint16_t)(index - capacity);
    }

    return position;
}

fwu_return_e fwu_ringbuf_init_raw(fwu_ringbuf_t *self, fwu_byte_t *storage, size_t capacity)
{
    if(self == NULL || storage == NULL || capacity == 0u || capacity > FWU_RINGBUF_MAX_CAPACITY) {
        return FWU_ERR_INVALID_ARG;
    }

    self->storage = storage;
    self->capacity = (uint16_t)capacity;
    self->head = 0u;
    self->tail = 0u;

    return FWU_OK;
}

fwu_return_e fwu_ringbuf_push(fwu_ringbuf_t *self, const fwu_byte_t *data, size_t length)
{
    /* length > capacity: never fits, hence a permanent error (INVALID_ARG). */
    if(!ringbuf_is_valid(self) || data == NULL || length > self->capacity) {
        return FWU_ERR_INVALID_ARG;
    }

    /* Free space = capacity - used. If it does not fit, write nothing (all or nothing). */
    const uint16_t free_space = (uint16_t)(self->capacity - ringbuf_used(self));
    if(free_space < length) {
        return FWU_ERR_BUFFER_FULL;
    }

    /* Write the data. head is NOT touched yet: the consumer must not see an unfinished write. */
    const uint16_t head = self->head;
    uint16_t position = ringbuf_position(head, self->capacity);
    for(size_t index = 0; index < length; index++) {
        self->storage[position] = data[index];
        position++;
        if(position == self->capacity) {
            position = 0u;
        }
    }

    /* Publish head AFTER the data is written (single write). */
    self->head = ringbuf_advance(head, (uint16_t)length, ringbuf_span(self));

    return FWU_OK;
}

fwu_return_e fwu_ringbuf_pull(fwu_ringbuf_t *self, fwu_byte_t *data, size_t length)
{
    if(!ringbuf_is_valid(self) || data == NULL || length > self->capacity) {
        return FWU_ERR_INVALID_ARG;
    }

    /* If there is not enough data, read nothing (all or nothing). Not an integrity error:
     * NOT_ENOUGH_DATA instead of BAD_LENGTH, because the upper layer must not request retransmission. */
    if(ringbuf_used(self) < length) {
        return FWU_ERR_NOT_ENOUGH_DATA;
    }

    /* Read the data. tail is NOT touched yet: the producer must still see these cells as full. */
    const uint16_t tail = self->tail;
    uint16_t position = ringbuf_position(tail, self->capacity);
    for(size_t index = 0; index < length; index++) {
        data[index] = self->storage[position];
        position++;
        if(position == self->capacity) {
            position = 0u;
        }
    }

    /* Publish tail AFTER the data is read (single write). */
    self->tail = ringbuf_advance(tail, (uint16_t)length, ringbuf_span(self));

    return FWU_OK;
}

fwu_return_e fwu_ringbuf_clear(fwu_ringbuf_t *self)
{
    if(!ringbuf_is_valid(self)) {
        return FWU_ERR_INVALID_ARG;
    }

    /* Consumer side: discards everything unread (tail = head). The data is not erased, no need. */
    self->tail = self->head;

    return FWU_OK;
}

bool fwu_ringbuf_is_empty(const fwu_ringbuf_t *self)
{
    return ringbuf_is_valid(self) && (ringbuf_used(self) == 0u);
}

bool fwu_ringbuf_is_full(const fwu_ringbuf_t *self)
{
    return ringbuf_is_valid(self) && (ringbuf_used(self) == self->capacity);
}

uint16_t fwu_ringbuf_used_size(const fwu_ringbuf_t *self)
{
    return ringbuf_is_valid(self) ? ringbuf_used(self) : 0u;
}

uint16_t fwu_ringbuf_free_size(const fwu_ringbuf_t *self)
{
    return ringbuf_is_valid(self) ? (uint16_t)(self->capacity - ringbuf_used(self)) : 0u;
}
