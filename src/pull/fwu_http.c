/**
 * @file    fwu_http.c
 * @brief   Minimal HTTP/1.1 client layer implementation (for behaviour and limits see fwu_http.h).
 */

#include <stddef.h>
#include "fwu_http.h"
#include "fwu_proto.h"

/* Parser phases. */
#define PHASE_STATUS_LINE   0u      /* waiting for the "HTTP/1.1 206 ..." line */
#define PHASE_HEADERS       1u      /* header lines, up to the blank line */
#define PHASE_BODY          2u      /* Content-Length bytes of body */
#define PHASE_DONE          3u      /* response complete; extra bytes are discarded */
#define PHASE_FAILED        4u      /* error; stays in this state until fwu_http_parser_init */

/* ------------------------------------------------------------------ small helpers */

static fwu_byte_t http_byte(const fwu_byte_t *p)
{
    return (fwu_byte_t)(*p & FWU_BYTE_MASK);
}

/** Converts to ASCII lower case (A-Z only); other bytes are left unchanged. */
static fwu_byte_t http_lower(fwu_byte_t c)
{
    return ((c >= (fwu_byte_t)'A') && (c <= (fwu_byte_t)'Z')) ? (fwu_byte_t)(c + 32u) : c;
}

/** Does the range buffer[start..end) BEGIN with `text`, case-insensitively? `text` must be lower-case ASCII. */
static bool http_starts_with(const fwu_byte_t *buffer, size_t start, size_t end, const char *text)
{
    size_t i = 0u;

    while(text[i] != '\0') {
        if(((start + i) >= end) || (http_lower(http_byte(&buffer[start + i])) != (fwu_byte_t)text[i])) {
            return false;
        }
        i++;
    }
    return true;
}

/** Does the range buffer[start..end) CONTAIN `text`, case-insensitively? */
static bool http_contains(const fwu_byte_t *buffer, size_t start, size_t end, const char *text)
{
    for(size_t i = start; i < end; i++) {
        if(http_starts_with(buffer, i, end, text)) {
            return true;
        }
    }
    return false;
}

/**
 * Is buffer[start..end) a non-overflowing uint32 consisting only of decimal digits? An empty range is invalid.
 * The output is written only when true is returned.
 */
static bool http_parse_u32(const fwu_byte_t *buffer, size_t start, size_t end, uint32_t *value)
{
    uint32_t result = 0u;

    if(start >= end) {
        return false;
    }
    for(size_t i = start; i < end; i++) {
        const fwu_byte_t c = http_byte(&buffer[i]);
        uint32_t digit;

        if((c < (fwu_byte_t)'0') || (c > (fwu_byte_t)'9')) {
            return false;
        }
        digit = (uint32_t)(c - (fwu_byte_t)'0');
        if(result > ((0xFFFFFFFFu - digit) / 10u)) {
            return false;                           /* would overflow */
        }
        result = (result * 10u) + digit;
    }
    *value = result;
    return true;
}

/* ------------------------------------------------------------------ request building */

/** Small output-writing helper: on overflow `ok` becomes false, nothing more is written. */
typedef struct {
    fwu_byte_t *buffer;
    size_t      capacity;
    size_t      position;
    bool        ok;
} http_writer_t;

static void http_put_char(http_writer_t *w, fwu_byte_t c)
{
    if(w->ok && (w->position < w->capacity)) {
        w->buffer[w->position] = c;
        w->position++;
    }
    else {
        w->ok = false;
    }
}

static void http_put_text(http_writer_t *w, const char *text)
{
    for(size_t i = 0u; text[i] != '\0'; i++) {
        http_put_char(w, (fwu_byte_t)text[i]);
    }
}

/** Writes a uint32 in decimal (no leading zeros; 0 -> "0"). */
static void http_put_u32(http_writer_t *w, uint32_t value)
{
    fwu_byte_t digits[10];
    size_t count = 0u;

    do {
        digits[count] = (fwu_byte_t)((uint32_t)'0' + (value % 10u));
        count++;
        value /= 10u;
    } while(value != 0u);
    while(count > 0u) {
        count--;
        http_put_char(w, digits[count]);
    }
}

fwu_return_e fwu_http_build_range_request(const fwu_byte_t ip[4], uint16_t port, const fwu_byte_t *target, size_t target_length,
                                          uint32_t first, uint32_t last, fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    fwu_byte_t temp[FWU_HTTP_REQUEST_MAX];
    http_writer_t w;

    if((ip == NULL) || (port == 0u) || (out == NULL) || (out_length == NULL) || (last < first)) {
        return FWU_ERR_INVALID_ARG;
    }
    if((target == NULL) || (target_length == 0u) || (target_length > FWU_PROTO_PULL_TARGET_MAX) ||
       ((target[0] & FWU_BYTE_MASK) != (fwu_byte_t)'/')) {
        return FWU_ERR_INVALID_ARG;
    }
    for(size_t i = 0u; i < target_length; i++) {
        const fwu_byte_t c = http_byte(&target[i]);

        if((c < 0x21u) || (c > 0x7Eu)) {
            return FWU_ERR_INVALID_ARG;             /* space, CR, LF, non-ASCII: would corrupt the request line */
        }
    }

    w.buffer = temp;
    w.capacity = FWU_HTTP_REQUEST_MAX;
    w.position = 0u;
    w.ok = true;
    http_put_text(&w, "GET ");
    for(size_t i = 0u; i < target_length; i++) {
        http_put_char(&w, http_byte(&target[i]));
    }
    http_put_text(&w, " HTTP/1.1\r\nHost: ");
    for(size_t i = 0u; i < 4u; i++) {
        http_put_u32(&w, (uint32_t)http_byte(&ip[i]));
        if(i < 3u) {
            http_put_char(&w, (fwu_byte_t)'.');
        }
    }
    http_put_char(&w, (fwu_byte_t)':');
    http_put_u32(&w, (uint32_t)port);
    http_put_text(&w, "\r\nRange: bytes=");
    http_put_u32(&w, first);
    http_put_char(&w, (fwu_byte_t)'-');
    http_put_u32(&w, last);
    http_put_text(&w, "\r\nConnection: keep-alive\r\n\r\n");
    if(!w.ok) {
        return FWU_ERR_BUFFER_TOO_SMALL;            /* did not fit in the fixed buffer (must not happen under the target limit) */
    }
    if(out_capacity < w.position) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    for(size_t i = 0u; i < w.position; i++) {
        out[i] = temp[i];
    }
    *out_length = w.position;
    return FWU_OK;
}

/* ------------------------------------------------------------------ response parsing */

void fwu_http_parser_init(fwu_http_parser_t *parser)
{
    if(parser == NULL) {
        return;
    }
    parser->phase = PHASE_STATUS_LINE;
    parser->error = FWU_OK;
    parser->line_length = 0u;
    parser->line_overflow = false;
    parser->header_bytes = 0u;
    parser->chunked = false;
    parser->body_remaining = 0u;
    parser->response.status = 0u;
    parser->response.has_content_length = false;
    parser->response.content_length = 0u;
    parser->response.has_content_range = false;
    parser->response.range_first = 0u;
    parser->response.range_last = 0u;
    parser->response.range_total = 0u;
    parser->response.connection_close = false;
}

static fwu_http_event_e http_fail(fwu_http_parser_t *parser, fwu_return_e code)
{
    parser->phase = PHASE_FAILED;
    parser->error = code;
    return FWU_HTTP_ERROR;
}

/** "HTTP/1.x SSS ..." : version 1.0 or 1.1, then a space and EXACTLY 3 digits, then end of line or a space. */
static bool http_parse_status_line(fwu_http_parser_t *parser)
{
    const fwu_byte_t *l = parser->line;
    const size_t n = parser->line_length;
    uint32_t code = 0u;

    if(parser->line_overflow || (n < 12u) || !http_starts_with(l, 0u, n, "http/1.")) {
        return false;
    }
    if((http_byte(&l[7]) != (fwu_byte_t)'0') && (http_byte(&l[7]) != (fwu_byte_t)'1')) {
        return false;
    }
    if(http_byte(&l[8]) != (fwu_byte_t)' ') {
        return false;
    }
    if(!http_parse_u32(l, 9u, 12u, &code) || (code < 100u) || (code > 599u)) {
        return false;
    }
    if((n > 12u) && (http_byte(&l[12]) != (fwu_byte_t)' ')) {
        return false;
    }
    parser->response.status = (uint16_t)code;
    return true;
}

/** Processes one header line. Return false = response invalid (e.g. conflicting Content-Length). Unrecognized ones are ignored. */
static bool http_parse_header_line(fwu_http_parser_t *parser)
{
    const fwu_byte_t *l = parser->line;
    size_t n = parser->line_length;
    size_t colon = 0u;
    size_t vs;
    size_t ve;

    if(parser->line_overflow) {
        return true;                                /* line too long: the headers we look for are short, skip */
    }
    while((colon < n) && (http_byte(&l[colon]) != (fwu_byte_t)':')) {
        colon++;
    }
    if(colon >= n) {
        return true;                                /* no ':': not a header, ignore */
    }
    vs = colon + 1u;
    while((vs < n) && ((http_byte(&l[vs]) == (fwu_byte_t)' ') || (http_byte(&l[vs]) == (fwu_byte_t)'\t'))) {
        vs++;
    }
    ve = n;
    while((ve > vs) && ((http_byte(&l[ve - 1u]) == (fwu_byte_t)' ') || (http_byte(&l[ve - 1u]) == (fwu_byte_t)'\t'))) {
        ve--;
    }
    n = colon;                                       /* n is now the length of the header name */

    if((n == 14u) && http_starts_with(l, 0u, n, "content-length")) {
        uint32_t value = 0u;

        if(!http_parse_u32(l, vs, ve, &value)) {
            return false;
        }
        if(parser->response.has_content_length && (parser->response.content_length != value)) {
            return false;                           /* two conflicting Content-Length headers: sign of request smuggling */
        }
        parser->response.has_content_length = true;
        parser->response.content_length = value;
    }
    else if((n == 13u) && http_starts_with(l, 0u, n, "content-range")) {
        /* "bytes <a>-<b>/<total>"; if the format does not match, has_content_range stays false (for a 206 this counts as an error). */
        size_t p = vs;
        size_t q;
        uint32_t first = 0u;
        uint32_t last = 0u;
        uint32_t total = 0u;

        if(http_starts_with(l, p, ve, "bytes")) {
            p += 5u;
            while((p < ve) && (http_byte(&l[p]) == (fwu_byte_t)' ')) {
                p++;
            }
            q = p;
            while((q < ve) && (http_byte(&l[q]) != (fwu_byte_t)'-')) {
                q++;
            }
            if(http_parse_u32(l, p, q, &first) && (q < ve)) {
                p = q + 1u;
                q = p;
                while((q < ve) && (http_byte(&l[q]) != (fwu_byte_t)'/')) {
                    q++;
                }
                if(http_parse_u32(l, p, q, &last) && (q < ve) && http_parse_u32(l, q + 1u, ve, &total) && (last >= first)) {
                    parser->response.has_content_range = true;
                    parser->response.range_first = first;
                    parser->response.range_last = last;
                    parser->response.range_total = total;
                }
            }
        }
    }
    else if((n == 17u) && http_starts_with(l, 0u, n, "transfer-encoding")) {
        if(http_contains(l, vs, ve, "chunked")) {
            parser->chunked = true;
        }
    }
    else if((n == 10u) && http_starts_with(l, 0u, n, "connection")) {
        if(http_contains(l, vs, ve, "close")) {
            parser->response.connection_close = true;
        }
    }
    else {
        /* unrecognized header: ignored */
    }
    return true;
}

fwu_http_event_e fwu_http_feed(fwu_http_parser_t *parser, const fwu_byte_t *data, size_t length, size_t *consumed,
                               const fwu_byte_t **body, size_t *body_length)
{
    size_t i = 0u;

    if((consumed == NULL) || (body == NULL) || (body_length == NULL) || (parser == NULL) || ((data == NULL) && (length > 0u))) {
        if(parser != NULL) {
            parser->phase = PHASE_FAILED;
            parser->error = FWU_ERR_INVALID_ARG;
        }
        if(consumed != NULL) {
            *consumed = 0u;
        }
        return FWU_HTTP_ERROR;
    }
    *consumed = 0u;
    *body = NULL;
    *body_length = 0u;

    switch(parser->phase) {
    case PHASE_FAILED:
        return FWU_HTTP_ERROR;                      /* consumes nothing: the caller stops until init */

    case PHASE_DONE:
        *consumed = length;                         /* response complete: extra (old/duplicate) bytes are discarded */
        return FWU_HTTP_NEED_MORE;

    case PHASE_BODY: {
        size_t take = length;

        if(take > (size_t)parser->body_remaining) {
            take = (size_t)parser->body_remaining;
        }
        if(take == 0u) {
            return FWU_HTTP_NEED_MORE;
        }
        parser->body_remaining -= (uint32_t)take;
        if(parser->body_remaining == 0u) {
            parser->phase = PHASE_DONE;
        }
        *body = data;
        *body_length = take;
        *consumed = take;
        return FWU_HTTP_BODY;
    }

    default:
        break;                                      /* PHASE_STATUS_LINE and PHASE_HEADERS below */
    }

    while(i < length) {
        const fwu_byte_t c = http_byte(&data[i]);

        i++;
        parser->header_bytes++;
        if(parser->header_bytes > FWU_HTTP_HEADER_MAX_BYTES) {
            *consumed = i;
            return http_fail(parser, FWU_ERR_PULL_PROTOCOL);
        }
        if(c == (fwu_byte_t)'\r') {
            continue;                               /* CR is dropped: the end of line is determined by LF (CRLF and LF only are accepted) */
        }
        if(c != (fwu_byte_t)'\n') {
            if(parser->line_length < FWU_HTTP_LINE_MAX) {
                parser->line[parser->line_length] = c;
                parser->line_length++;
            }
            else {
                parser->line_overflow = true;
            }
            continue;
        }

        /* A line is complete. */
        if(parser->phase == PHASE_STATUS_LINE) {
            if(!http_parse_status_line(parser)) {
                *consumed = i;
                return http_fail(parser, FWU_ERR_PULL_PROTOCOL);
            }
            parser->phase = PHASE_HEADERS;
        }
        else if((parser->line_length == 0u) && !parser->line_overflow) {
            /* Blank line: headers finished. */
            *consumed = i;
            if(parser->chunked || !parser->response.has_content_length) {
                return http_fail(parser, FWU_ERR_PULL_PROTOCOL);
            }
            parser->body_remaining = parser->response.content_length;
            parser->phase = (parser->body_remaining == 0u) ? PHASE_DONE : PHASE_BODY;
            return FWU_HTTP_HEADERS_DONE;
        }
        else if(!http_parse_header_line(parser)) {
            *consumed = i;
            return http_fail(parser, FWU_ERR_PULL_PROTOCOL);
        }
        parser->line_length = 0u;
        parser->line_overflow = false;
    }
    *consumed = i;
    return FWU_HTTP_NEED_MORE;
}

const fwu_http_response_t *fwu_http_response(const fwu_http_parser_t *parser)
{
    return (parser != NULL) ? &parser->response : NULL;
}

bool fwu_http_complete(const fwu_http_parser_t *parser)
{
    return (parser != NULL) && (parser->phase == PHASE_DONE);
}

fwu_return_e fwu_http_error(const fwu_http_parser_t *parser)
{
    return (parser != NULL) ? parser->error : FWU_ERR_INVALID_ARG;
}

bool fwu_http_headers_done(const fwu_http_parser_t *parser)
{
    return (parser != NULL) && ((parser->phase == PHASE_BODY) || (parser->phase == PHASE_DONE));
}
