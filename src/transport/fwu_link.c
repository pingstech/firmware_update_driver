/**
 * @file    fwu_link.c
 * @brief   Link layer implementation (see fwu_link.h for the behavior).
 */

#include <stddef.h>
#include "fwu_link.h"

/** Tries to send the pending response. BUFFER_FULL: held. Any other error: dropped. */
static void link_flush(fwu_link_t *link)
{
    fwu_return_e status;

    if(link->pending_length == 0u) {
        return;
    }
    status = link->config.transport->send(link->config.transport->context, link->pending, link->pending_length);
    if(status == FWU_OK) {
        link->stats.frames_out++;
        link->pending_length = 0u;
    }
    else if(status == FWU_ERR_BUFFER_FULL) {
        link->stats.tx_busy++;                  /* held, retried with the same bytes */
    }
    else {
        link->stats.tx_errors++;                /* send failed: the response is dropped, the host retries */
        link->pending_length = 0u;
    }
}

fwu_return_e fwu_link_init(fwu_link_t *link, const fwu_link_config_t *config)
{
    if((link == NULL) || (config == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    link->initialized = false;
    if((config->session == NULL) || (config->parser == NULL) || (config->transport == NULL) ||
       (config->transport->send == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    link->config = *config;
    link->pending_length = 0u;
    link->stats.frames_in = 0u;
    link->stats.frames_out = 0u;
    link->stats.tx_busy = 0u;
    link->stats.tx_errors = 0u;
    link->ext_handles = NULL;
    link->ext_on_frame = NULL;
    link->ext_context = NULL;
    link->initialized = true;
    return FWU_OK;
}

fwu_return_e fwu_link_set_extension(fwu_link_t *link, fwu_link_ext_handles_fn handles, fwu_link_ext_on_frame_fn on_frame, void *context)
{
    if((link == NULL) || (!link->initialized) || ((handles == NULL) != (on_frame == NULL))) {
        return FWU_ERR_INVALID_ARG;
    }
    link->ext_handles = handles;
    link->ext_on_frame = on_frame;
    link->ext_context = (handles != NULL) ? context : NULL;
    return FWU_OK;
}

fwu_return_e fwu_link_poll(fwu_link_t *link, uint32_t now_ms)
{
    fwu_return_e status;

    if((link == NULL) || (!link->initialized)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(link->config.transport->poll != NULL) {
        link->config.transport->poll(link->config.transport->context, now_ms);
    }

    link_flush(link);
    if(link->pending_length != 0u) {
        return FWU_OK;                          /* transport still busy: no new request is processed (stop-and-wait order) */
    }

    for(uint32_t i = 0u; i < FWU_LINK_MAX_FRAMES_PER_POLL; i++) {
        fwu_frame_view_t view;
        size_t length = 0u;

        status = fwu_parser_poll(link->config.parser, now_ms, &view);
        if(status == FWU_ERR_NOT_ENOUGH_DATA) {
            break;                              /* no frame to deliver right now */
        }
        if(status != FWU_OK) {
            return status;
        }
        link->stats.frames_in++;
        if((link->ext_handles != NULL) && link->ext_handles(view.type)) {
            status = link->ext_on_frame(link->ext_context, &view, now_ms, link->pending, FWU_FRAME_MAX_SIZE, &length);
        }
        else {
            status = fwu_session_on_frame(link->config.session, &view, now_ms, link->pending, FWU_FRAME_MAX_SIZE, &length);
        }
        if(status != FWU_OK) {
            return status;
        }
        link->pending_length = length;
        link_flush(link);
        if(link->pending_length != 0u) {
            return FWU_OK;                      /* could not be sent: held, remaining frames on the next poll */
        }
    }

    {
        size_t length = 0u;

        status = fwu_session_poll(link->config.session, now_ms, link->pending, FWU_FRAME_MAX_SIZE, &length);
        if(status != FWU_OK) {
            return status;
        }
        link->pending_length = length;
        link_flush(link);
    }
    return FWU_OK;
}

bool fwu_link_reboot_ready(const fwu_link_t *link)
{
    return (link != NULL) && link->initialized && fwu_session_reboot_requested(link->config.session) &&
           (link->pending_length == 0u);
}

bool fwu_link_tx_pending(const fwu_link_t *link)
{
    return (link != NULL) && (link->pending_length != 0u);
}

const fwu_link_stats_t *fwu_link_get_stats(const fwu_link_t *link)
{
    return (link != NULL) ? &link->stats : NULL;
}
