/**
 * @file    fwu_http.h
 * @brief   MINIMAL HTTP/1.1 CLIENT LAYER (independent of transport and MCU): (1) builds a range request (GET + Range),
 *          (2) decodes the server's response BYTE BY BYTE, piece by piece. fwu_pull uses it when pulling the image from the server in pieces.
 *
 * WHY A SEPARATE MODULE: the HTTP parsing of the legacy code worked with fixed offsets (`+9`, `+16`, `+21`) and
 * assumed the response arrived in a SINGLE TCP segment; it copied without bound into a 2000-byte buffer. TCP is a byte stream: a response
 * can be split anywhere. Here the response is a STATE MACHINE: it accumulates header lines, uses
 * Content-Length to count the body, never writes to any buffer without bound, and checks every length and number.
 *
 * SCOPE (deliberately narrow): only the "GET + Range -> 206 Partial Content" flow. Unsupported (treated as errors):
 *   - Transfer-Encoding: chunked (the body is counted with Content-Length),
 *   - a response without Content-Length ("read until the connection closes" is not safe in this flow),
 *   - HTTPS/TLS (this layer carries bytes, it does not encrypt; there is no TLS on the MCU/FC41D side).
 * Header names are read CASE-INSENSITIVELY (HTTP rule). Line endings may be CRLF or LF only. Unrecognized headers
 * are ignored. If the total of the header section exceeds FWU_HTTP_HEADER_MAX_BYTES, the response is rejected (overflow defence).
 *
 * USAGE
 *   // Request:
 *   fwu_http_build_range_request(ip, port, target, target_length, first, last, buf, sizeof(buf)/sizeof(buf[0]), &n);   // then send n bytes
 *   // Response (before each new request):
 *   fwu_http_parser_init(&p);
 *   // For each group of received bytes:
 *   while(length > 0u) {
 *       size_t used = 0u;  const fwu_byte_t *body = NULL;  size_t body_length = 0u;
 *       switch(fwu_http_feed(&p, data, length, &used, &body, &body_length)) {
 *       case FWU_HTTP_NEED_MORE:    break;                          // all input consumed, wait for the next group
 *       case FWU_HTTP_HEADERS_DONE: ... fwu_http_response(&p) ...   // status code, Content-Length, Content-Range ready
 *       case FWU_HTTP_BODY:         ... body[0..body_length-1] ...  // body piece (points into the input, no copy)
 *       case FWU_HTTP_ERROR:        ... fwu_http_error(&p) ...
 *       }
 *       data += used;  length -= used;
 *   }
 *   When fwu_http_complete(&p) becomes true, the ENTIRE response has been received.
 *   Each call consumes at least 1 byte or returns NEED_MORE (input finished); the loop does not spin forever.
 *
 * TIME: this layer does not know about time; the timeout is fwu_pull's job.
 */

#ifndef FWU_HTTP_H_
#define FWU_HTTP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_proto.h"       /* FWU_PROTO_PULL_TARGET_MAX: maximum length of the request target (client and device use the same limit) */

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum total length of the header section (status line + headers + blank line). */
#define FWU_HTTP_HEADER_MAX_BYTES   2048u
/** Maximum retained length of a parsed line; for longer lines only the beginning is kept (the headers looked for are short). */
#define FWU_HTTP_LINE_MAX           128u
/** Safe upper bound for the maximum length of the fwu_http_build_range_request output (target 96 + fixed parts). */
#define FWU_HTTP_REQUEST_MAX        256u

/** Result of fwu_http_feed. */
typedef enum {
    FWU_HTTP_NEED_MORE = 0,     /**< Input consumed; nothing to report. */
    FWU_HTTP_HEADERS_DONE,      /**< Headers finished: fwu_http_response() is valid. If there is no body, fwu_http_complete() is true immediately. */
    FWU_HTTP_BODY,              /**< A body piece was returned (body, body_length). */
    FWU_HTTP_ERROR              /**< Response invalid: fwu_http_error() gives the code. The parser stays in the error state until init. */
} fwu_http_event_e;

/** Summary of the parsed headers. Meaningful only after FWU_HTTP_HEADERS_DONE. */
typedef struct {
    uint16_t status;                /**< HTTP status code (200, 206, 404 ...). */
    bool     has_content_length;    /**< The Content-Length header is present and the number is valid. */
    uint32_t content_length;        /**< Body length. */
    bool     has_content_range;     /**< In the form "bytes a-b/total", all three are numbers. */
    uint32_t range_first;           /**< a. */
    uint32_t range_last;            /**< b (inclusive). */
    uint32_t range_total;           /**< total file size. */
    bool     connection_close;      /**< "Connection: close": the server will close the connection after this response. */
} fwu_http_response_t;

/** Parser state. Do not touch the fields directly. */
typedef struct {
    uint16_t            phase;               /* C28x: no uint8_t */
    fwu_return_e        error;
    fwu_byte_t          line[FWU_HTTP_LINE_MAX];
    uint16_t            line_length;
    bool                line_overflow;
    uint32_t            header_bytes;
    bool                chunked;
    uint32_t            body_remaining;
    fwu_http_response_t response;
} fwu_http_parser_t;

/**
 * @brief  Builds the GET + Range request. Output (with CRLF, ASCII):
 *           GET <target> HTTP/1.1  /  Host: a.b.c.d:port  /  Range: bytes=<first>-<last>  /  Connection: keep-alive  /  blank line
 *         Connection: keep-alive makes it possible to request the next range on the SAME connection (no reconnect for each range).
 * @param  ip       IPv4 address of the server (4 elements).
 * @param  port     TCP port (1..65535).
 * @param  target   Request target: path + query ("/api/...?deviceType=fan&version=12"); starts with '/', printable ASCII,
 *                  no space/CR/LF (request-line injection defence).
 * @param  first    First byte (inclusive). last: last byte (inclusive), must have last >= first.
 * @param  out      Output buffer; out_capacity is the number of ELEMENTS (FWU_HTTP_REQUEST_MAX is enough).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for invalid parameters (NULL, port 0, invalid target, last < first); FWU_ERR_BUFFER_TOO_SMALL if the
 *         capacity is insufficient. The output is written only on FWU_OK (including out_length).
 */
fwu_return_e fwu_http_build_range_request(const fwu_byte_t ip[4], uint16_t port, const fwu_byte_t *target, size_t target_length,
                                          uint32_t first, uint32_t last, fwu_byte_t *out, size_t out_capacity, size_t *out_length);

/** Resets the parser for a new response (called before each new request). */
void fwu_http_parser_init(fwu_http_parser_t *parser);

/**
 * @brief  Processes incoming bytes (see the top of the file for details and the loop).
 * @param  data/length   Newly received bytes (length may be 0: NEED_MORE).
 * @param  consumed      Number of bytes consumed in this call (<= length; on HEADERS_DONE, BODY and ERROR the remaining input stays with the caller).
 * @param  body          On a BODY event, the start of the body piece (points into the input); NULL for other events.
 * @param  body_length   On a BODY event, the length of the piece; 0 for others.
 * @return Event. Error codes: FWU_ERR_PULL_PROTOCOL (status line/header format, number overflow, conflicting Content-Length, chunked,
 *         no Content-Length, header section too large). For a NULL parameter: FWU_HTTP_ERROR + FWU_ERR_INVALID_ARG.
 *         Extra bytes arriving AFTER the response is complete (old/duplicate response) are silently discarded (counted as consumed).
 */
fwu_http_event_e fwu_http_feed(fwu_http_parser_t *parser, const fwu_byte_t *data, size_t length, size_t *consumed,
                               const fwu_byte_t **body, size_t *body_length);

/** Summary of the parsed headers (a QUERY). The content is meaningless before HEADERS_DONE. NULL if parser is NULL. */
const fwu_http_response_t *fwu_http_response(const fwu_http_parser_t *parser);

/** Has the whole response (headers + Content-Length bytes of body) been received? (a QUERY) */
bool fwu_http_complete(const fwu_http_parser_t *parser);

/** The code if in the error state, otherwise FWU_OK (a QUERY). */
fwu_return_e fwu_http_error(const fwu_http_parser_t *parser);

/** Have the headers been parsed (has the HEADERS_DONE event passed)? (a QUERY) */
bool fwu_http_headers_done(const fwu_http_parser_t *parser);

#ifdef __cplusplus
}
#endif

#endif /* FWU_HTTP_H_ */
