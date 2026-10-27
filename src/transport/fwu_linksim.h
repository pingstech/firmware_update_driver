/**
 * @file    fwu_linksim.h
 * @brief   LOSSY LINK SIMULATOR (TEST/PC only): imitates a one-directional channel and applies deterministic, seed-repeatable
 *          faults to every chunk of bytes sent (usually one frame). It is the counterpart of fwu_flash_sim.h on the flash side
 *          and fwu_crypto_sim.h on the crypto side: it is meant for exercising the client + transport + session chain under
 *          bad conditions without a real bad line (BLE, CAN, a long UART cable).
 *
 * FAULTS (all in per-mille, 0..1000; independent on every send)
 *   drop      the chunk never arrives.
 *   duplicate the chunk arrives twice (the second copy with its own delay).
 *   corrupt   1..3 random bits are flipped (the frame CRC should catch this).
 *   truncate  the chunk is CUT to a random length (a half frame; the parser recovers via its timeout).
 *   delay     the chunk arrives 1..delay_max_ms late; different delays cause REORDERING.
 *   garbage   1..8 random bytes (garbage) are prepended to the chunk.
 *
 * If the queue is full (FWU_LINKSIM_QUEUE) the new chunk is dropped (counted as a drop). There is no dynamic memory. It is NOT
 * secure; the randomness is only for repeatable tests.
 */

#ifndef FWU_LINKSIM_H_
#define FWU_LINKSIM_H_

#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FWU_LINKSIM_QUEUE       128u        /**< Maximum number of chunks in flight at the same time. */
#define FWU_LINKSIM_CHUNK_MAX   640u        /**< Maximum size of a chunk (largest frame 521 + garbage). */

/** Fault probabilities (per-mille). All 0 = perfect line. */
typedef struct {
    uint32_t drop_pm;
    uint32_t duplicate_pm;
    uint32_t corrupt_pm;
    uint32_t truncate_pm;
    uint32_t delay_pm;
    uint32_t garbage_pm;
    uint32_t delay_max_ms;      /**< >= 1 if delay_pm > 0. */
} fwu_linksim_profile_t;

typedef struct {
    uint32_t sent;
    uint32_t dropped;
    uint32_t duplicated;
    uint32_t corrupted;
    uint32_t truncated;
    uint32_t delayed;
    uint32_t garbage;
    uint32_t delivered;
    uint32_t queue_overflow;
} fwu_linksim_stats_t;

typedef struct {
    uint32_t   deliver_ms;
    size_t     length;
    fwu_byte_t data[FWU_LINKSIM_CHUNK_MAX];
} fwu_linksim_chunk_t;

typedef struct {
    fwu_linksim_profile_t profile;
    uint32_t              rng;
    fwu_linksim_chunk_t   queue[FWU_LINKSIM_QUEUE];
    uint32_t              count;
    uint32_t              order[FWU_LINKSIM_QUEUE];   /**< Insertion order (for FIFO at the same delivery time). */
    uint32_t              next_order;
    fwu_linksim_stats_t   stats;
} fwu_linksim_t;

/** Initializes. @return FWU_OK; FWU_ERR_INVALID_ARG for NULL, a probability > 1000, or delay_max_ms 0 while delay_pm > 0. */
fwu_return_e fwu_linksim_init(fwu_linksim_t *sim, const fwu_linksim_profile_t *profile, uint32_t seed);

/** Changes the profile (e.g. to degrade or repair the line later). Validation is the same as init. */
fwu_return_e fwu_linksim_set_profile(fwu_linksim_t *sim, const fwu_linksim_profile_t *profile);

/**
 * @brief  Puts a chunk on the line (faults are applied). length 1..FWU_LINKSIM_CHUNK_MAX - 8.
 * @return FWU_OK (even if the chunk is dropped: the line loses silently); FWU_ERR_INVALID_ARG for NULL or an invalid length.
 */
fwu_return_e fwu_linksim_send(fwu_linksim_t *sim, const fwu_byte_t *data, size_t length, uint32_t now_ms);

/**
 * @brief  Returns the oldest chunk whose delivery time has arrived as of now_ms (also in insertion order).
 * @return number of bytes delivered (0 = no chunk to deliver right now); if `capacity` is not enough the chunk is DROPPED and 0 is returned.
 */
size_t fwu_linksim_receive(fwu_linksim_t *sim, uint32_t now_ms, fwu_byte_t *out, size_t capacity);

/** Number of chunks in flight (a QUERY). */
uint32_t fwu_linksim_in_flight(const fwu_linksim_t *sim);

#ifdef __cplusplus
}
#endif

#endif /* FWU_LINKSIM_H_ */
