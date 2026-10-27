/**
 * @file    fwu_isotp.c
 * @brief   ISO-TP subset implementation (see fwu_isotp.h for the format and the rules).
 */

#include <stddef.h>
#include "fwu_isotp.h"

#define ISOTP_PCI_MASK      0xF0u
#define ISOTP_PCI_SF        0x00u
#define ISOTP_PCI_FF        0x10u
#define ISOTP_PCI_CF        0x20u
#define ISOTP_PCI_FC        0x30u
#define ISOTP_FS_CTS        0u
#define ISOTP_FS_WAIT       1u
#define ISOTP_FS_OVFLW      2u
#define ISOTP_SF_MAX        7u          /* maximum data in a single frame */
#define ISOTP_FF_DATA       6u          /* data in the first frame */
#define ISOTP_CF_DATA       7u          /* data in a consecutive frame */

#define ISOTP_TX_IDLE       0u
#define ISOTP_TX_FIRST      1u          /* SF or FF not sent yet */
#define ISOTP_TX_WAIT_FC    2u
#define ISOTP_TX_SEND_CF    3u

/* ------------------------------------------------------------------ helpers */

static fwu_byte_t isotp_byte(fwu_byte_t value)
{
    return (fwu_byte_t)(value & FWU_BYTE_MASK);
}

static void isotp_pad(const fwu_isotp_t *iso, fwu_byte_t *frame)
{
    (void)iso;
    for(uint32_t i = 0u; i < FWU_ISOTP_CAN_DLC; i++) {
        frame[i] = (fwu_byte_t)FWU_TRANSPORT_CAN_PAD_BYTE;
    }
}

/** STmin byte -> ms. 0..127 as is; 0xF1..0xF9 (100..900 us) is rounded up to 1 ms; others (reserved) are 127 ms. */
static uint32_t isotp_st_min_decode(fwu_byte_t value)
{
    const uint32_t v = (uint32_t)(value & FWU_BYTE_MASK);

    if(v <= 0x7Fu) {
        return v;
    }
    if((v >= 0xF1u) && (v <= 0xF9u)) {
        return 1u;
    }
    return 0x7Fu;
}

static void isotp_rx_abort(fwu_isotp_t *iso)
{
    if(iso->rx_active) {
        iso->rx_active = false;
        iso->stats.rx_aborts++;
    }
}

static void isotp_tx_abort(fwu_isotp_t *iso)
{
    iso->tx_state = ISOTP_TX_IDLE;
    iso->stats.tx_aborts++;
}

/** Sends an FC; if the mailbox is full (BUFFER_FULL) it is held, on any other error it is dropped. */
static void isotp_send_fc(fwu_isotp_t *iso, uint32_t status)
{
    fwu_byte_t frame[FWU_ISOTP_CAN_DLC];
    fwu_return_e result;

    isotp_pad(iso, frame);
    frame[0] = (fwu_byte_t)(ISOTP_PCI_FC | status);
    frame[1] = (fwu_byte_t)iso->config.block_size;
    frame[2] = (fwu_byte_t)iso->config.st_min_ms;
    result = iso->config.can_send(iso->config.context, frame);
    if(result == FWU_OK) {
        iso->stats.fc_sent++;
        iso->fc_pending = false;
    }
    else if(result == FWU_ERR_BUFFER_FULL) {
        iso->fc_pending = true;
        iso->fc_status = (fwu_byte_t)status;
    }
    else {
        iso->fc_pending = false;                /* driver error: the FC is dropped, the sender gives up on timeout */
    }
}

/** Advances the transmission by one step (first frame, timeout, consecutive frames). Returns only a hard driver error. */
static fwu_return_e isotp_tx_step(fwu_isotp_t *iso)
{
    if(iso->tx_state == ISOTP_TX_FIRST) {
        fwu_byte_t frame[FWU_ISOTP_CAN_DLC];
        fwu_return_e result;

        isotp_pad(iso, frame);
        if(iso->tx_length <= ISOTP_SF_MAX) {
            frame[0] = (fwu_byte_t)(ISOTP_PCI_SF | iso->tx_length);
            for(uint32_t i = 0u; i < iso->tx_length; i++) {
                frame[1u + i] = isotp_byte(iso->tx_buffer[i]);
            }
        }
        else {
            frame[0] = (fwu_byte_t)(ISOTP_PCI_FF | ((iso->tx_length >> 8) & 0x0Fu));
            frame[1] = (fwu_byte_t)(iso->tx_length & 0xFFu);
            for(uint32_t i = 0u; i < ISOTP_FF_DATA; i++) {
                frame[2u + i] = isotp_byte(iso->tx_buffer[i]);
            }
        }
        result = iso->config.can_send(iso->config.context, frame);
        if(result == FWU_ERR_BUFFER_FULL) {
            return FWU_OK;                      /* mailbox full: retried in the next step */
        }
        if(result != FWU_OK) {
            isotp_tx_abort(iso);
            return result;
        }
        if(iso->tx_length <= ISOTP_SF_MAX) {
            iso->tx_state = ISOTP_TX_IDLE;
            iso->stats.tx_messages++;
            return FWU_OK;
        }
        iso->tx_sent = ISOTP_FF_DATA;
        iso->tx_sn = 1u;
        iso->tx_wft = 0u;
        iso->tx_timer_ms = iso->now_ms;
        iso->tx_state = ISOTP_TX_WAIT_FC;
        return FWU_OK;
    }

    if(iso->tx_state == ISOTP_TX_WAIT_FC) {
        if((uint32_t)(iso->now_ms - iso->tx_timer_ms) >= iso->config.timeout_ms) {
            iso->stats.tx_timeouts++;
            isotp_tx_abort(iso);
        }
        return FWU_OK;
    }

    for(uint32_t sent = 0u; (iso->tx_state == ISOTP_TX_SEND_CF) && (sent < FWU_ISOTP_MAX_CF_PER_POLL); sent++) {
        fwu_byte_t frame[FWU_ISOTP_CAN_DLC];
        uint32_t remaining = iso->tx_length - iso->tx_sent;
        uint32_t n = (remaining < ISOTP_CF_DATA) ? remaining : ISOTP_CF_DATA;
        fwu_return_e result;

        if((uint32_t)(iso->now_ms - iso->tx_last_cf_ms) < iso->tx_st_min_ms) {
            return FWU_OK;                      /* STmin has not elapsed */
        }
        isotp_pad(iso, frame);
        frame[0] = (fwu_byte_t)(ISOTP_PCI_CF | iso->tx_sn);
        for(uint32_t i = 0u; i < n; i++) {
            frame[1u + i] = isotp_byte(iso->tx_buffer[iso->tx_sent + i]);
        }
        result = iso->config.can_send(iso->config.context, frame);
        if(result == FWU_ERR_BUFFER_FULL) {
            return FWU_OK;
        }
        if(result != FWU_OK) {
            isotp_tx_abort(iso);
            return result;
        }
        iso->stats.cf_sent++;
        iso->tx_sent += n;
        iso->tx_sn = (iso->tx_sn + 1u) & 0x0Fu;
        iso->tx_last_cf_ms = iso->now_ms;
        if(iso->tx_sent >= iso->tx_length) {
            iso->tx_state = ISOTP_TX_IDLE;
            iso->stats.tx_messages++;
            return FWU_OK;
        }
        if(!iso->tx_unlimited) {
            iso->tx_block_left--;
            if(iso->tx_block_left == 0u) {
                iso->tx_state = ISOTP_TX_WAIT_FC;
                iso->tx_timer_ms = iso->now_ms;
                iso->tx_wft = 0u;
            }
        }
    }
    return FWU_OK;
}

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_isotp_init(fwu_isotp_t *iso, const fwu_isotp_config_t *config)
{
    if((iso == NULL) || (config == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    iso->initialized = false;
    if((config->can_send == NULL) || (config->deliver == NULL) || (config->block_size > 255u) ||
       (config->st_min_ms > 127u) || (config->timeout_ms == 0u)) {
        return FWU_ERR_INVALID_ARG;
    }
    iso->config = *config;
    iso->now_ms = 0u;
    iso->rx_length = 0u;
    iso->rx_received = 0u;
    iso->rx_sn = 0u;
    iso->rx_block = 0u;
    iso->rx_timer_ms = 0u;
    iso->rx_active = false;
    iso->fc_pending = false;
    iso->fc_status = 0u;
    iso->tx_length = 0u;
    iso->tx_sent = 0u;
    iso->tx_sn = 0u;
    iso->tx_state = ISOTP_TX_IDLE;
    iso->tx_block_left = 0u;
    iso->tx_unlimited = false;
    iso->tx_st_min_ms = 0u;
    iso->tx_timer_ms = 0u;
    iso->tx_last_cf_ms = 0u;
    iso->tx_wft = 0u;
    iso->stats.rx_messages = 0u;
    iso->stats.tx_messages = 0u;
    iso->stats.rx_errors = 0u;
    iso->stats.rx_aborts = 0u;
    iso->stats.rx_timeouts = 0u;
    iso->stats.tx_timeouts = 0u;
    iso->stats.tx_aborts = 0u;
    iso->stats.cf_sent = 0u;
    iso->stats.fc_sent = 0u;
    iso->initialized = true;
    return FWU_OK;
}

fwu_return_e fwu_isotp_send(fwu_isotp_t *iso, const fwu_byte_t *data, size_t length, uint32_t now_ms)
{
    if((iso == NULL) || (!iso->initialized) || (data == NULL) || (length == 0u) || (length > FWU_ISOTP_MAX_MESSAGE)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(iso->tx_state != ISOTP_TX_IDLE) {
        return FWU_ERR_BUFFER_FULL;
    }
    for(size_t i = 0u; i < length; i++) {
        iso->tx_buffer[i] = isotp_byte(data[i]);
    }
    iso->tx_length = (uint32_t)length;
    iso->tx_sent = 0u;
    iso->tx_state = ISOTP_TX_FIRST;
    iso->now_ms = now_ms;
    return isotp_tx_step(iso);
}

fwu_return_e fwu_isotp_on_can_frame(fwu_isotp_t *iso, const fwu_byte_t *data, size_t length, uint32_t now_ms)
{
    uint32_t pci;
    uint32_t low;

    if((iso == NULL) || (!iso->initialized) || (data == NULL) || (length == 0u) || (length > FWU_ISOTP_CAN_DLC)) {
        return FWU_ERR_INVALID_ARG;
    }
    iso->now_ms = now_ms;
    pci = (uint32_t)(data[0] & FWU_BYTE_MASK) & ISOTP_PCI_MASK;
    low = (uint32_t)(data[0] & FWU_BYTE_MASK) & 0x0Fu;

    if(pci == ISOTP_PCI_SF) {
        if((low == 0u) || (low > ISOTP_SF_MAX)) {
            iso->stats.rx_errors++;
            return FWU_ERR_BAD_LENGTH;
        }
        if(length < (1u + low)) {
            iso->stats.rx_errors++;
            return FWU_ERR_BAD_FRAME;
        }
        isotp_rx_abort(iso);                    /* aborts a reassembly in progress */
        {
            fwu_byte_t message[ISOTP_SF_MAX];
            for(uint32_t i = 0u; i < low; i++) {
                message[i] = isotp_byte(data[1u + i]);
            }
            iso->stats.rx_messages++;
            iso->config.deliver(iso->config.context, message, low);
        }
        return FWU_OK;
    }

    if(pci == ISOTP_PCI_FF) {
        uint32_t total;

        if(length < FWU_ISOTP_CAN_DLC) {
            iso->stats.rx_errors++;
            return FWU_ERR_BAD_FRAME;
        }
        total = (low << 8) | (uint32_t)(data[1] & FWU_BYTE_MASK);
        if(total <= ISOTP_SF_MAX) {
            iso->stats.rx_errors++;
            return FWU_ERR_BAD_LENGTH;          /* a message of 7 bytes or fewer is not sent with an FF */
        }
        isotp_rx_abort(iso);
        if(total > FWU_ISOTP_MAX_MESSAGE) {
            iso->stats.rx_errors++;
            isotp_send_fc(iso, ISOTP_FS_OVFLW);
            return FWU_ERR_BAD_LENGTH;
        }
        for(uint32_t i = 0u; i < ISOTP_FF_DATA; i++) {
            iso->rx_buffer[i] = isotp_byte(data[2u + i]);
        }
        iso->rx_length = total;
        iso->rx_received = ISOTP_FF_DATA;
        iso->rx_sn = 1u;
        iso->rx_block = 0u;
        iso->rx_timer_ms = now_ms;
        iso->rx_active = true;
        isotp_send_fc(iso, ISOTP_FS_CTS);
        return FWU_OK;
    }

    if(pci == ISOTP_PCI_CF) {
        uint32_t remaining;
        uint32_t n;

        if(!iso->rx_active) {
            iso->stats.rx_errors++;
            return FWU_ERR_BAD_STATE;           /* CF received while no reassembly is active */
        }
        if(low != iso->rx_sn) {
            iso->stats.rx_errors++;
            isotp_rx_abort(iso);
            return FWU_ERR_BAD_STATE;           /* wrong SN: the message was lost */
        }
        remaining = iso->rx_length - iso->rx_received;
        n = (remaining < ISOTP_CF_DATA) ? remaining : ISOTP_CF_DATA;
        if(length < (1u + n)) {
            iso->stats.rx_errors++;
            isotp_rx_abort(iso);
            return FWU_ERR_BAD_FRAME;
        }
        for(uint32_t i = 0u; i < n; i++) {
            iso->rx_buffer[iso->rx_received + i] = isotp_byte(data[1u + i]);
        }
        iso->rx_received += n;
        iso->rx_sn = (iso->rx_sn + 1u) & 0x0Fu;
        iso->rx_timer_ms = now_ms;
        if(iso->rx_received == iso->rx_length) {
            iso->rx_active = false;
            iso->stats.rx_messages++;
            iso->config.deliver(iso->config.context, iso->rx_buffer, iso->rx_length);
            return FWU_OK;
        }
        if(iso->config.block_size > 0u) {
            iso->rx_block++;
            if(iso->rx_block >= iso->config.block_size) {
                iso->rx_block = 0u;
                isotp_send_fc(iso, ISOTP_FS_CTS);
            }
        }
        return FWU_OK;
    }

    if(pci == ISOTP_PCI_FC) {
        if(iso->tx_state != ISOTP_TX_WAIT_FC) {
            iso->stats.rx_errors++;
            return FWU_ERR_BAD_STATE;           /* unexpected FC */
        }
        if(length < 3u) {
            iso->stats.rx_errors++;
            return FWU_ERR_BAD_FRAME;
        }
        if(low == ISOTP_FS_CTS) {
            const uint32_t bs = (uint32_t)(data[1] & FWU_BYTE_MASK);

            iso->tx_unlimited = (bs == 0u);
            iso->tx_block_left = bs;
            iso->tx_st_min_ms = isotp_st_min_decode(data[2]);
            iso->tx_last_cf_ms = now_ms - iso->tx_st_min_ms;     /* the first CF can go immediately */
            iso->tx_state = ISOTP_TX_SEND_CF;
            (void)isotp_tx_step(iso);
            return FWU_OK;
        }
        if(low == ISOTP_FS_WAIT) {
            iso->tx_wft++;
            if(iso->tx_wft > iso->config.wft_max) {
                isotp_tx_abort(iso);
            }
            else {
                iso->tx_timer_ms = now_ms;
            }
            return FWU_OK;
        }
        if(low == ISOTP_FS_OVFLW) {
            isotp_tx_abort(iso);
            return FWU_OK;
        }
        iso->stats.rx_errors++;
        return FWU_ERR_BAD_FRAME;               /* reserved FS value */
    }

    iso->stats.rx_errors++;
    return FWU_ERR_BAD_FRAME;                   /* unknown PCI type */
}

fwu_return_e fwu_isotp_poll(fwu_isotp_t *iso, uint32_t now_ms)
{
    if((iso == NULL) || (!iso->initialized)) {
        return FWU_ERR_INVALID_ARG;
    }
    iso->now_ms = now_ms;
    if(iso->rx_active && ((uint32_t)(now_ms - iso->rx_timer_ms) >= iso->config.timeout_ms)) {
        iso->stats.rx_timeouts++;
        isotp_rx_abort(iso);
    }
    if(iso->fc_pending) {
        if((!iso->rx_active) && ((uint32_t)iso->fc_status == ISOTP_FS_CTS)) {
            iso->fc_pending = false;            /* reassembly was aborted: the old FC is meaningless */
        }
        else {
            isotp_send_fc(iso, (uint32_t)iso->fc_status);
        }
    }
    (void)isotp_tx_step(iso);
    return FWU_OK;
}

bool fwu_isotp_tx_busy(const fwu_isotp_t *iso)
{
    return (iso != NULL) && iso->initialized && (iso->tx_state != ISOTP_TX_IDLE);
}

const fwu_isotp_stats_t *fwu_isotp_get_stats(const fwu_isotp_t *iso)
{
    return (iso != NULL) ? &iso->stats : NULL;
}

fwu_return_e fwu_isotp_transport_send(void *context, const fwu_byte_t *data, size_t length)
{
    fwu_isotp_t *iso = (fwu_isotp_t *)context;

    if(iso == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    return fwu_isotp_send(iso, data, length, iso->now_ms);
}

void fwu_isotp_transport_poll(void *context, uint32_t now_ms)
{
    (void)fwu_isotp_poll((fwu_isotp_t *)context, now_ms);
}
