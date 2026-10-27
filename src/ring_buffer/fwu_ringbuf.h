/**
 * @file    fwu_ringbuf.h
 * @brief   Ring buffer (FIFO) for byte streams. The storage is EMBEDDED in the structure.
 *
 * Used to accumulate bytes arriving from UART/BLE and hand them to the frame handler in order.
 * Replaces the three separate queue copies of the old driver
 * (P14: there was no overflow detection, a full queue looked empty).
 *
 * USAGE (the ONLY VALID WAY in production code: FWU_RINGBUF_DEFINE)
 *   FWU_RINGBUF_DEFINE(uart_rb, 128);        // at file level: type + init, size given by a SINGLE number
 *   static uart_rb_t g_uart_rb;
 *   ...
 *   uart_rb_init(&g_uart_rb);
 *   fwu_ringbuf_push(&g_uart_rb.base, data, length);
 *   fwu_ringbuf_pull(&g_uart_rb.base, out, length);
 *
 * RULE: fwu_ringbuf_init_raw is NOT called in production code. It is an internal function; it is used only
 *       by FWU_RINGBUF_DEFINE and in test code (to exercise runtime parameter validation).
 *       run_tests.sh checks this.
 *
 * DESIGN DECISIONS
 *   1) The size and the array are NEVER written separately: FWU_RINGBUF_DEFINE embeds the storage in
 *      the structure and derives the size from the same number. A wrongly given manual size
 *      (a number larger than the real array) would cause an overflow the compiler cannot catch;
 *      this class of error has been eliminated structurally. There is no generic size macro and no
 *      dynamic memory (malloc); every queue has its own size.
 *   2) CONCURRENCY (ISR <-> main loop), SINGLE PRODUCER / SINGLE CONSUMER (SPSC):
 *        - head is written ONLY by the producer (push), tail ONLY by the consumer (pull, clear).
 *          There is NO jointly written counter (count); so no read-modify-write race
 *          can occur between the two contexts.
 *        - The indices are kept in the range [0, 2*capacity); the full/empty distinction comes from
 *          the difference of the two indices (head == tail means EMPTY only).
 *        - push updates head (publishes) AFTER writing the data; pull updates tail AFTER
 *          reading the data.
 *        - The fields are volatile; this is sufficient for single-core, in-order MCUs
 *          (Cortex-M0+, C28x). Multi-core / weakly ordered memory processors
 *          need an additional memory barrier.
 *        - The indices are uint16_t (read/written in a single access on C28x and ARM).
 *   3) Bulk write/read is "all or nothing": if there is not enough space/data, NOTHING is
 *      written/read and the buffer state does not change.
 *   4) Writing to a full buffer is REJECTED (FWU_ERR_BUFFER_FULL). Unread data is never overwritten.
 *   5) Operations that can fail return fwu_return_e; queries return bool/number.
 *      Invalid or uninitialized (zeroed) buffer: operations return FWU_ERR_INVALID_ARG,
 *      queries return false / 0.
 *
 * CONTEXT CONTRACT (SPSC)
 *   - fwu_ringbuf_push           : PRODUCER context only (e.g. UART RX interrupt).
 *   - fwu_ringbuf_pull / _clear  : CONSUMER context only (e.g. main loop).
 *   - Queries (is_empty, is_full, used_size, free_size): may be called from either context;
 *     since they are snapshots the result is conservative (may show less free space than there is for the
 *     producer, less data than there is for the consumer; NEVER more).
 *   - <name>_init (and the internal fwu_ringbuf_init_raw): must be called while neither context is running yet.
 *
 * MISRA NOTE: FWU_RINGBUF_DEFINE is a DELIBERATE DEVIATION (advisory) from MISRA C:2012 Dir 4.9
 * (function-like macros). Rationale: it structurally prevents a size/array mismatch (silent
 * memory corruption). The deviation must be recorded in the project deviation log.
 */

#ifndef FWU_RINGBUF_H_
#define FWU_RINGBUF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_types.h"
#include "fwu_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum capacity (elements). Since the indices are in the range [0, 2*capacity),
 * 2 * capacity must fit in uint16_t (2 * 32767 = 65534).
 */
#define FWU_RINGBUF_MAX_CAPACITY 32767u

/**
 * @brief Ring buffer structure. Do not modify its fields directly; use only the functions.
 *
 * The caller allocates the structure (usually static). A zeroed (static) structure
 * used without calling init safely returns FWU_ERR_INVALID_ARG.
 */
typedef struct {
    volatile fwu_byte_t *storage;   /**< Storage (capacity elements). Inside the structure when FWU_RINGBUF_DEFINE is used. */
    uint16_t capacity;              /**< Capacity (elements), fixed at init. */
    volatile uint16_t head;         /**< Write index [0, 2*capacity). Written ONLY by the producer. */
    volatile uint16_t tail;         /**< Read index [0, 2*capacity). Written ONLY by the consumer. */
} fwu_ringbuf_t;

/**
 * @brief   INTERNAL: Initializes the buffer with the given storage (makes it empty).
 *
 * NOT called in production code (see RULE). Because the size is given manually, a number larger
 * than the real array causes an overflow the compiler cannot catch.
 * Used by FWU_RINGBUF_DEFINE and by tests.
 *
 * @param[out] self      Buffer to initialize.
 * @param[in]  storage   Storage array (at least capacity elements, must stay valid for the lifetime of the buffer).
 * @param[in]  capacity  Capacity (number of elements), 1 .. FWU_RINGBUF_MAX_CAPACITY.
 * @return  FWU_OK; FWU_ERR_INVALID_ARG if self/storage is NULL or capacity is 0 / too large.
 */
fwu_return_e fwu_ringbuf_init_raw(fwu_ringbuf_t *self, fwu_byte_t *storage, size_t capacity);

/**
 * @brief   Writes bytes to the buffer (all or nothing). From the PRODUCER context only.
 *
 * @param[in,out] self    Target buffer.
 * @param[in]     data    Bytes to write (must not be NULL).
 * @param[in]     length  Number of bytes to write.
 * @return  FWU_OK;
 *          FWU_ERR_INVALID_ARG: self invalid/uninitialized, data NULL or
 *            length > capacity (never fits, permanent);
 *          FWU_ERR_BUFFER_FULL: not enough free space right now (transient, can be retried
 *            once the buffer drains). In this case the buffer does NOT change.
 */
fwu_return_e fwu_ringbuf_push(fwu_ringbuf_t *self, const fwu_byte_t *data, size_t length);

/**
 * @brief   Reads bytes from the buffer in order (FIFO) (all or nothing). From the CONSUMER context only.
 *
 * @param[in,out] self    Source buffer.
 * @param[out]    data    Array to receive the bytes read (at least length elements).
 * @param[in]     length  Number of bytes to read.
 * @return  FWU_OK;
 *          FWU_ERR_INVALID_ARG: self invalid/uninitialized, data NULL or length > capacity;
 *          FWU_ERR_NOT_ENOUGH_DATA: the buffer holds fewer than length bytes (transient, can be
 *            retried when data arrives). In this case the buffer does NOT change.
 */
fwu_return_e fwu_ringbuf_pull(fwu_ringbuf_t *self, fwu_byte_t *data, size_t length);

/**
 * @brief   Empties the buffer (discards pending data). From the CONSUMER context only.
 * @param[in,out] self  Buffer.
 * @return  FWU_OK, or FWU_ERR_INVALID_ARG if the buffer is invalid/uninitialized.
 */
fwu_return_e fwu_ringbuf_clear(fwu_ringbuf_t *self);

/**
 * @brief   Returns whether the buffer is empty.
 * @param[in] self  Buffer. false if invalid/uninitialized.
 */
bool fwu_ringbuf_is_empty(const fwu_ringbuf_t *self);

/**
 * @brief   Returns whether the buffer is full.
 * @param[in] self  Buffer. false if invalid/uninitialized.
 */
bool fwu_ringbuf_is_full(const fwu_ringbuf_t *self);

/**
 * @brief   Returns the number of pending (used) bytes in the buffer.
 * @param[in] self  Buffer. 0 if invalid/uninitialized.
 */
uint16_t fwu_ringbuf_used_size(const fwu_ringbuf_t *self);

/**
 * @brief   Returns the number of free bytes remaining in the buffer.
 * @param[in] self  Buffer. 0 if invalid/uninitialized.
 */
uint16_t fwu_ringbuf_free_size(const fwu_ringbuf_t *self);

/**
 * @brief   Generates a type that EMBEDS the storage in the structure and derives the size from a SINGLE number.
 *          The ONLY valid way to create a queue in production code.
 *
 * Why: writing the size and the array separately causes an overflow the compiler cannot catch if a
 * number larger than the real array is given. Since this macro produces the array and the size from the
 * SAME number, a mismatch is structurally IMPOSSIBLE.
 *
 * Generated:
 *   - <name>_t        : { fwu_ringbuf_t base; fwu_byte_t storage[capacity]; }
 *   - <name>_init(p)  : initializes base with the REAL size of the embedded storage.
 *   - Gives a COMPILE ERROR if the capacity is outside 1 .. FWU_RINGBUF_MAX_CAPACITY.
 *
 * Usage (at file level, a ';' is appended):
 *   FWU_RINGBUF_DEFINE(uart_rb, 128);
 *   static uart_rb_t g_uart_rb;
 *   ...
 *   uart_rb_init(&g_uart_rb);
 *   fwu_ringbuf_push(&g_uart_rb.base, data, length);   // operations go through base
 *
 * @note Deliberate deviation from MISRA C:2012 Dir 4.9 (see the MISRA NOTE at the top of the file).
 */
#define FWU_RINGBUF_DEFINE(name, capacity_value)                                              \
    typedef struct {                                                                          \
        fwu_ringbuf_t base;                                                                   \
        fwu_byte_t storage[(capacity_value)];                                                 \
    } name##_t;                                                                               \
    static inline fwu_return_e name##_init(name##_t *self)                                    \
    {                                                                                         \
        return (self == NULL) ? FWU_ERR_INVALID_ARG                                           \
            : fwu_ringbuf_init_raw(&self->base, self->storage,                                \
                                   sizeof(self->storage) / sizeof(self->storage[0]));         \
    }                                                                                         \
    typedef char name##_capacity_is_valid[                                                    \
        (((capacity_value) >= 1) && ((capacity_value) <= FWU_RINGBUF_MAX_CAPACITY)) ? 1 : -1]

#ifdef __cplusplus
}
#endif

#endif /* FWU_RINGBUF_H_ */
