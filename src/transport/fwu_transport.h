/**
 * @file    fwu_transport.h
 * @brief   TRANSPORT CONTRACT: (1) the interface through which the device side talks to a transport (fwu_transport_t),
 *          (2) the PROFILE of each transport: a single source of truth so that host and device use the same values
 *          (UART, TCP, BLE, CAN). The host application (fwu_app) takes these values from here; they are not reinvented
 *          on the Python side.
 *
 * PRINCIPLE (decision D16): the frame (fwu_frame.h) and the messages (fwu_proto.h) are IDENTICAL on EVERY transport. A
 * transport only carries bytes; none of them is considered trustworthy (trust lies in the image signature). The frame
 * layer extracts frames from the byte stream (resynchronization, CRC), so the transport does not have to be contiguous,
 * ordered or complete; loss, duplication and corruption are handled in the upper layer (host retransmission).
 *
 * DEVICE-SIDE FLOW (fwu_link.h):
 *   RX: transport driver (ISR or main loop) pushes received bytes into a ring buffer  ->  fwu_parser  ->  fwu_session
 *   TX: fwu_session response  ->  fwu_transport_t.send  ->  transport driver
 *   On CAN, ISO-TP segmentation sits between RX and TX (fwu_isotp.h); the others are byte streams.
 *
 * PROFILES
 *   UART (USB-TTL, also BLE): a BLE bridge module connects BLE to the MCU as a UART (phone -> bridge -> UART -> MCU), so
 *     on the MCU side BLE is also a UART. 115200 8N1, no flow control; the baud rate is configurable (the driver supplies
 *     a default).
 *   TCP: byte stream, single client. The port is configurable, there is NO default (the integrator chooses).
 *   BLE: a GATT service plus two characteristics (the UUIDs below were generated RANDOMLY for this project). Host -> device
 *     write type is "write without response"; each write carries at most (ATT_MTU - 3) bytes (default MTU 23 -> 20
 *     bytes). The bridge forwards the bytes to the UART UNCHANGED; frame boundaries are not preserved (the parser
 *     extracts frames from the byte stream).
 *   CAN: classic CAN (8 data bytes), 11-bit identifier, normal addressing, ISO-TP subset (SF/FF/CF/FC) [UNVERIFIED: not
 *     checked against the official text of ISO 15765-2]. One fwu frame (at most 521 bytes) is one ISO-TP message.
 *
 * UNIT: byte = the low 8 bits of fwu_byte_t (on C28x an element is 16 bits, only the low 8 bits are significant).
 */

#ifndef FWU_TRANSPORT_H_
#define FWU_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ device-side interface */

/**
 * Hands the COMPLETE frame (response) to the transport.
 *   FWU_OK              the transport accepted the COMPLETE frame (copied it into its own buffer); the caller may reuse the buffer.
 *   FWU_ERR_BUFFER_FULL the transport is busy right now (TX buffer full / previous message in progress): NOTHING was accepted;
 *                       the caller retries later with the same frame.
 *   other errors        the send failed (no connection, driver error): the frame is dropped; the host retries after its timeout.
 */
typedef fwu_return_e (*fwu_transport_send_fn)(void *context, const fwu_byte_t *data, size_t length);

/** Optional: the transport's time-dependent work (e.g. ISO-TP timers, consecutive-frame transmission). May be NULL. */
typedef void (*fwu_transport_poll_fn)(void *context, uint32_t now_ms);

/** Interface through which the device side uses a transport. The RX direction is NOT in this structure: the driver pushes bytes into the ring buffer. */
typedef struct {
    fwu_transport_send_fn send;     /**< Mandatory. */
    fwu_transport_poll_fn poll;     /**< Optional (NULL = none). The link calls it at the start of every poll. */
    void                 *context;  /**< Passed to both functions. */
} fwu_transport_t;

/* ------------------------------------------------------------------ UART profile */

#define FWU_TRANSPORT_UART_BAUD_DEFAULT     115200u     /**< Default baud rate (configurable). */
#define FWU_TRANSPORT_UART_DATA_BITS        8u
#define FWU_TRANSPORT_UART_PARITY_NONE      1           /**< No parity. */
#define FWU_TRANSPORT_UART_STOP_BITS        1u
/* There is no flow control: because the host works in stop-and-wait mode, the device does not receive new bytes while a request is in progress. */

/* ------------------------------------------------------------------ TCP profile */
/* Byte stream, single client. There is NO default port (the integrator chooses); the device-side TCP stack is not part of this repository. */

/* ------------------------------------------------------------------ BLE profile (GATT) */

/** Service and characteristic UUIDs: generated randomly for this project (the probability of a clash with another service is negligible). */
#define FWU_TRANSPORT_BLE_SERVICE_UUID      "795c2cc5-f830-41e4-b8db-3f18978bb05f"
/** Host -> device: written with "write without response" (the write type is fixed). */
#define FWU_TRANSPORT_BLE_RX_CHAR_UUID      "f956ae27-636f-4988-aa5a-670216e418ba"
/** Device -> host: delivered with "notify" (the host must enable the subscription). */
#define FWU_TRANSPORT_BLE_TX_CHAR_UUID      "7dd31ef7-1bb3-46c5-a253-47fd51e0e5bd"
/** Default maximum write chunk: 23 - 3 = 20 bytes for ATT_MTU 23. Grows with MTU negotiation (at most MTU - 3). */
#define FWU_TRANSPORT_BLE_DEFAULT_CHUNK     20u

/* ------------------------------------------------------------------ CAN profile (ISO-TP subset, fwu_isotp.h) */

/* The identifiers are PLACEHOLDERS [UNVERIFIED]: only the network owner knows which identifiers are unused on the real target network. They can be overridden with -D.
 * (OBD/UDS diagnostic identifiers such as 0x7E0/0x7E8 were deliberately NOT used.) */
#ifndef FWU_TRANSPORT_CAN_REQUEST_ID
#define FWU_TRANSPORT_CAN_REQUEST_ID        0x5F0u      /**< Host -> device (11 bit). */
#endif
#ifndef FWU_TRANSPORT_CAN_RESPONSE_ID
#define FWU_TRANSPORT_CAN_RESPONSE_ID       0x5F1u      /**< Device -> host (11 bit). */
#endif
#define FWU_TRANSPORT_CAN_DLC               8u          /**< Every CAN frame is 8 bytes (short content is padded with 0xCC). */
#define FWU_TRANSPORT_CAN_PAD_BYTE          0xCCu
#define FWU_TRANSPORT_CAN_BLOCK_SIZE        8u          /**< Block size the receiver announces in every flow control (0 = unlimited). */
#define FWU_TRANSPORT_CAN_ST_MIN_MS         2u          /**< Consecutive-frame spacing requested by the receiver (ms, 0..127). */
#define FWU_TRANSPORT_CAN_TIMEOUT_MS        1000u       /**< N_Bs / N_Cr: time to wait for flow control or for a consecutive frame. */

#if defined(FWU_TRANSPORT_USE_CAN) && defined(FWU_PRODUCTION) && !defined(FWU_TRANSPORT_CAN_IDS_VERIFIED)
#error "FWU transport (CAN): the identifiers are PLACEHOLDERS; choose free identifiers on your network and build with -DFWU_TRANSPORT_CAN_IDS_VERIFIED for production"
#endif

#ifdef __cplusplus
}
#endif

#endif /* FWU_TRANSPORT_H_ */
