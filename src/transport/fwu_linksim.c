/**
 * @file    fwu_linksim.c
 * @brief   Lossy link simulator implementation (see fwu_linksim.h for the faults). TEST ONLY.
 */

#include <stdbool.h>
#include <stddef.h>
#include "fwu_linksim.h"

static uint32_t sim_rnd(fwu_linksim_t *sim)
{
    sim->rng = (sim->rng * 1664525u) + 1013904223u;
    return sim->rng >> 8;
}

/** Did the probability (per-mille) hit? */
static bool sim_roll(fwu_linksim_t *sim, uint32_t per_mille)
{
    return (per_mille > 0u) && ((sim_rnd(sim) % 1000u) < per_mille);
}

static fwu_return_e sim_check_profile(const fwu_linksim_profile_t *p)
{
    if(p == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    if((p->drop_pm > 1000u) || (p->duplicate_pm > 1000u) || (p->corrupt_pm > 1000u) || (p->truncate_pm > 1000u) ||
       (p->delay_pm > 1000u) || (p->garbage_pm > 1000u) || ((p->delay_pm > 0u) && (p->delay_max_ms == 0u))) {
        return FWU_ERR_INVALID_ARG;
    }
    return FWU_OK;
}

fwu_return_e fwu_linksim_init(fwu_linksim_t *sim, const fwu_linksim_profile_t *profile, uint32_t seed)
{
    if((sim == NULL) || (sim_check_profile(profile) != FWU_OK)) {
        return FWU_ERR_INVALID_ARG;
    }
    sim->profile = *profile;
    sim->rng = seed;
    sim->count = 0u;
    sim->next_order = 0u;
    sim->stats.sent = 0u;
    sim->stats.dropped = 0u;
    sim->stats.duplicated = 0u;
    sim->stats.corrupted = 0u;
    sim->stats.truncated = 0u;
    sim->stats.delayed = 0u;
    sim->stats.garbage = 0u;
    sim->stats.delivered = 0u;
    sim->stats.queue_overflow = 0u;
    return FWU_OK;
}

fwu_return_e fwu_linksim_set_profile(fwu_linksim_t *sim, const fwu_linksim_profile_t *profile)
{
    if((sim == NULL) || (sim_check_profile(profile) != FWU_OK)) {
        return FWU_ERR_INVALID_ARG;
    }
    sim->profile = *profile;
    return FWU_OK;
}

/** Puts a copy of a chunk into the queue (with delay and insertion order). */
static void sim_enqueue(fwu_linksim_t *sim, const fwu_byte_t *data, size_t length, uint32_t now_ms)
{
    uint32_t delay = 1u;

    if(sim->count >= FWU_LINKSIM_QUEUE) {
        sim->stats.queue_overflow++;
        sim->stats.dropped++;
        return;
    }
    if(sim_roll(sim, sim->profile.delay_pm)) {
        delay += sim_rnd(sim) % sim->profile.delay_max_ms;
        sim->stats.delayed++;
    }
    for(size_t i = 0u; i < length; i++) {
        sim->queue[sim->count].data[i] = (fwu_byte_t)(data[i] & FWU_BYTE_MASK);
    }
    sim->queue[sim->count].length = length;
    sim->queue[sim->count].deliver_ms = now_ms + delay;
    sim->order[sim->count] = sim->next_order;
    sim->next_order++;
    sim->count++;
}

fwu_return_e fwu_linksim_send(fwu_linksim_t *sim, const fwu_byte_t *data, size_t length, uint32_t now_ms)
{
    fwu_byte_t work[FWU_LINKSIM_CHUNK_MAX];
    size_t n;

    if((sim == NULL) || (data == NULL) || (length == 0u) || (length > (FWU_LINKSIM_CHUNK_MAX - 8u))) {
        return FWU_ERR_INVALID_ARG;
    }
    sim->stats.sent++;
    if(sim_roll(sim, sim->profile.drop_pm)) {
        sim->stats.dropped++;
        return FWU_OK;
    }

    n = 0u;
    if(sim_roll(sim, sim->profile.garbage_pm)) {
        const size_t junk = 1u + (size_t)(sim_rnd(sim) % 8u);
        for(size_t i = 0u; i < junk; i++) {
            work[n++] = (fwu_byte_t)(sim_rnd(sim) & 0xFFu);
        }
        sim->stats.garbage++;
    }
    for(size_t i = 0u; i < length; i++) {
        work[n++] = (fwu_byte_t)(data[i] & FWU_BYTE_MASK);
    }
    if(sim_roll(sim, sim->profile.corrupt_pm)) {
        const uint32_t flips = 1u + (sim_rnd(sim) % 3u);
        for(uint32_t k = 0u; k < flips; k++) {
            work[sim_rnd(sim) % n] ^= (fwu_byte_t)(1u << (sim_rnd(sim) % 8u));
        }
        sim->stats.corrupted++;
    }
    if((n > 1u) && sim_roll(sim, sim->profile.truncate_pm)) {
        n = 1u + (size_t)(sim_rnd(sim) % (uint32_t)(n - 1u));
        sim->stats.truncated++;
    }

    sim_enqueue(sim, work, n, now_ms);
    if(sim_roll(sim, sim->profile.duplicate_pm)) {
        sim_enqueue(sim, work, n, now_ms);
        sim->stats.duplicated++;
    }
    return FWU_OK;
}

size_t fwu_linksim_receive(fwu_linksim_t *sim, uint32_t now_ms, fwu_byte_t *out, size_t capacity)
{
    uint32_t best = FWU_LINKSIM_QUEUE;
    size_t length;

    if((sim == NULL) || (out == NULL)) {
        return 0u;
    }
    for(uint32_t i = 0u; i < sim->count; i++) {
        if((uint32_t)(now_ms - sim->queue[i].deliver_ms) < 0x80000000u) {          /* delivery time has arrived (wrap-safe) */
            if((best == FWU_LINKSIM_QUEUE) || (sim->queue[i].deliver_ms < sim->queue[best].deliver_ms) ||
               ((sim->queue[i].deliver_ms == sim->queue[best].deliver_ms) && (sim->order[i] < sim->order[best]))) {
                best = i;
            }
        }
    }
    if(best == FWU_LINKSIM_QUEUE) {
        return 0u;
    }
    length = sim->queue[best].length;
    if(length <= capacity) {
        for(size_t i = 0u; i < length; i++) {
            out[i] = sim->queue[best].data[i];
        }
        sim->stats.delivered++;
    }
    else {
        length = 0u;
    }
    sim->count--;
    if(best != sim->count) {
        sim->queue[best] = sim->queue[sim->count];
        sim->order[best] = sim->order[sim->count];
    }
    return length;
}

uint32_t fwu_linksim_in_flight(const fwu_linksim_t *sim)
{
    return (sim != NULL) ? sim->count : 0u;
}
