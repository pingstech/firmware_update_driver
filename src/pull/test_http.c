/**
 * @file    test_http.c
 * @brief   fwu_http (aralık isteği kurma + cevap ayrıştırma) için kendi kendini kontrol eden test.
 *          Beklenen baytlar ELLE yazılmıştır (kodlayıcıdan türetilmez).
 *
 * Kapsam: isteğin tam baytları, sınır değerleri, geçersiz girdide çıkışın değişmemesi; cevap ayrıştırma: tek seferde, BAYT BAYT ve
 * rastgele parçalı besleme sonuçları AYNI; başlık adlarında büyük/küçük harf, yalnız LF, tanınmayan ve çok uzun başlıklar,
 * Content-Range biçimleri, hata durumları (durum satırı, Content-Length yok/çelişkili/taşan, chunked, başlık bölümü sınırı),
 * tamamlandıktan sonra gelen fazla baytlar, düşman (rastgele) girdi.
 *
 * Derleme (pull klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../session -I../frame -I../bytes -I../crc fwu_http.c test_http.c -o test_http && ./test_http
 */

#include <stdio.h>
#include <string.h>
#include "fwu_http.h"

#define ELEMENTS(a) (sizeof(a) / sizeof((a)[0]))

static int g_failures = 0;

static void check(const char *name, int condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if(!condition) {
        g_failures++;
    }
}

static void expect(const char *name, fwu_return_e got, fwu_return_e want)
{
    if(got == want) {
        printf("[PASS] %s\n", name);
    } else {
        printf("[FAIL] %s  (got %d, expected %d)\n", name, (int)got, (int)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------ yardımcılar */

/** C dizesini fwu_byte_t dizisine çevirir (NUL hariç). Dönüş: uzunluk. */
static size_t to_bytes(const char *text, fwu_byte_t *out)
{
    size_t n = 0u;

    while(text[n] != '\0') {
        out[n] = (fwu_byte_t)(unsigned char)text[n];
        n++;
    }
    return n;
}

static int same_text(const fwu_byte_t *got, size_t got_length, const char *want)
{
    const size_t n = strlen(want);

    if(got_length != n) {
        return 0;
    }
    for(size_t i = 0u; i < n; i++) {
        if(got[i] != (fwu_byte_t)(unsigned char)want[i]) {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    int          headers_events;        /* HEADERS_DONE olay sayısı */
    int          error_events;
    size_t       body_total;            /* toplanan gövde baytı */
    fwu_byte_t   body[4096];
    fwu_return_e error;
    uint32_t     calls;
} run_t;

/**
 * Baytları `pattern` boyutlu parçalarla besler (pattern 0 = hepsi birden). Kural denetimi: her çağrı en az 1 bayt tüketir ya da
 * girdi bitmiştir; döngü asla sonsuz dönmez. Hata olayında durur (çağıran init'e kadar bekler).
 */
static void run_feed(fwu_http_parser_t *p, const fwu_byte_t *data, size_t length, size_t pattern, run_t *r)
{
    size_t pos = 0u;

    memset(r, 0, sizeof(*r));
    r->error = FWU_OK;
    while((pos < length) && (r->error_events == 0)) {
        size_t piece = (pattern == 0u) ? (length - pos) : pattern;
        size_t offset = 0u;

        if(piece > (length - pos)) {
            piece = length - pos;
        }
        while(offset < piece) {
            size_t used = 0u;
            const fwu_byte_t *body = NULL;
            size_t body_length = 0u;
            const fwu_http_event_e ev = fwu_http_feed(p, &data[pos + offset], piece - offset, &used, &body, &body_length);

            r->calls++;
            if(ev == FWU_HTTP_ERROR) {
                r->error_events++;
                r->error = fwu_http_error(p);
                return;
            }
            if(ev == FWU_HTTP_HEADERS_DONE) {
                r->headers_events++;
            }
            if(ev == FWU_HTTP_BODY) {
                for(size_t k = 0u; (k < body_length) && ((r->body_total + k) < ELEMENTS(r->body)); k++) {
                    r->body[r->body_total + k] = body[k];
                }
                r->body_total += body_length;
            }
            if((used == 0u) && (ev != FWU_HTTP_NEED_MORE)) {
                check("her olay en az 1 bayt tuketir (sonsuz dongu yok)", 0);
                return;
            }
            offset += used;
            if(ev == FWU_HTTP_NEED_MORE) {
                break;                                          /* girdi tuketildi (used == piece - offset) */
            }
        }
        pos += piece;
    }
}

/* örnek cevap: 10 baytlık gövde "0123456789" */
static const char *RESPONSE_OK =
    "HTTP/1.1 206 Partial Content\r\n"
    "Server: nginx/1.25.3\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: 10\r\n"
    "Content-Range: bytes 512-521/57408\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "0123456789";

/* ------------------------------------------------------------------ istek kurma */

static void test_request(void)
{
    const fwu_byte_t ip[4] = { 192u, 168u, 1u, 20u };
    fwu_byte_t target[FWU_PROTO_PULL_TARGET_MAX + 8u];
    fwu_byte_t out[FWU_HTTP_REQUEST_MAX];
    fwu_byte_t canary[FWU_HTTP_REQUEST_MAX];
    size_t length = 0u;
    size_t tl;

    tl = to_bytes("/getFwUpdateFile?deviceType=fan&version=12", target);

    expect("istek kurulur", fwu_http_build_range_request(ip, 8080u, target, tl, 0u, 511u, out, ELEMENTS(out), &length), FWU_OK);
    check("istegin TAM baytlari (elle yazilmis)",
          same_text(out, length,
                    "GET /getFwUpdateFile?deviceType=fan&version=12 HTTP/1.1\r\n"
                    "Host: 192.168.1.20:8080\r\n"
                    "Range: bytes=0-511\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"));

    expect("sinir degerleri: ip 255.255.255.255, port 65535, 0-4294967295",
           fwu_http_build_range_request((const fwu_byte_t[4]){ 255u, 255u, 255u, 255u }, 65535u, target, tl, 0u, 4294967295u, out, ELEMENTS(out), &length), FWU_OK);
    check("sinir degerlerinin metni",
          same_text(out, length,
                    "GET /getFwUpdateFile?deviceType=fan&version=12 HTTP/1.1\r\n"
                    "Host: 255.255.255.255:65535\r\n"
                    "Range: bytes=0-4294967295\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"));
    expect("tek baytlik aralik (first == last == 0)", fwu_http_build_range_request(ip, 80u, target, tl, 0u, 0u, out, ELEMENTS(out), &length), FWU_OK);
    expect("ip sifirlar", fwu_http_build_range_request((const fwu_byte_t[4]){ 0u, 0u, 0u, 0u }, 1u, target, tl, 100u, 100u, out, ELEMENTS(out), &length), FWU_OK);
    check("sifirlar dogru yazilir: Host: 0.0.0.0:1, Range: bytes=100-100",
          same_text(out, length,
                    "GET /getFwUpdateFile?deviceType=fan&version=12 HTTP/1.1\r\n"
                    "Host: 0.0.0.0:1\r\n"
                    "Range: bytes=100-100\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"));
    {
        fwu_byte_t t96[FWU_PROTO_PULL_TARGET_MAX + 1u];
        for(size_t i = 0u; i < ELEMENTS(t96); i++) { t96[i] = (fwu_byte_t)'a'; }
        t96[0] = (fwu_byte_t)'/';
        expect("en uzun hedef (96) en uzun alanlarla bile tampona sigar",
               fwu_http_build_range_request((const fwu_byte_t[4]){ 255u, 255u, 255u, 255u }, 65535u, t96, 96u, 4294967294u, 4294967295u, out, ELEMENTS(out), &length), FWU_OK);
        check("96 baytlik hedefli istek FWU_HTTP_REQUEST_MAX'tan kisa", length < FWU_HTTP_REQUEST_MAX);
        expect("hedef 97 reddedilir", fwu_http_build_range_request(ip, 80u, t96, 97u, 0u, 1u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    }

    /* gecersiz girdiler: INVALID_ARG ve cikis degismez */
    for(size_t i = 0u; i < ELEMENTS(out); i++) { out[i] = 0xEEu; canary[i] = 0xEEu; }
    length = 777u;
    expect("ip NULL", fwu_http_build_range_request(NULL, 80u, target, tl, 0u, 1u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    expect("port 0", fwu_http_build_range_request(ip, 0u, target, tl, 0u, 1u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    expect("hedef NULL", fwu_http_build_range_request(ip, 80u, NULL, 5u, 0u, 1u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    expect("hedef bos", fwu_http_build_range_request(ip, 80u, target, 0u, 0u, 1u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    expect("last < first", fwu_http_build_range_request(ip, 80u, target, tl, 10u, 9u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    expect("cikis NULL", fwu_http_build_range_request(ip, 80u, target, tl, 0u, 1u, NULL, 100u, &length), FWU_ERR_INVALID_ARG);
    expect("out_length NULL", fwu_http_build_range_request(ip, 80u, target, tl, 0u, 1u, out, ELEMENTS(out), NULL), FWU_ERR_INVALID_ARG);
    {
        const fwu_byte_t no_slash[] = { 'a', 'b' };
        const fwu_byte_t space[] = { '/', 'a', ' ', 'b' };
        const fwu_byte_t crlf[] = { '/', 'a', '\r', '\n', 'X', ':', '1' };
        const fwu_byte_t high[] = { '/', 0xC3u, 0xA9u };
        expect("hedef '/' ile baslamiyor", fwu_http_build_range_request(ip, 80u, no_slash, 2u, 0u, 1u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
        expect("hedefte bosluk", fwu_http_build_range_request(ip, 80u, space, 4u, 0u, 1u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
        expect("hedefte CR/LF (baslik enjeksiyonu)", fwu_http_build_range_request(ip, 80u, crlf, 7u, 0u, 1u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
        expect("hedefte ASCII disi bayt", fwu_http_build_range_request(ip, 80u, high, 3u, 0u, 1u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    }
    {
        int untouched = (length == 777u);
        for(size_t i = 0u; i < ELEMENTS(out); i++) {
            if(out[i] != canary[i]) {
                untouched = 0;
            }
        }
        check("hatali istek cikisa ve out_length'e yazmaz", untouched);
    }
    expect("kapasite yetmez", fwu_http_build_range_request(ip, 80u, target, tl, 0u, 1u, out, 20u, &length), FWU_ERR_BUFFER_TOO_SMALL);
    {
        int untouched = (length == 777u);
        for(size_t i = 0u; i < ELEMENTS(out); i++) {
            if(out[i] != canary[i]) {
                untouched = 0;
            }
        }
        check("yetmeyen kapasitede cikis degismez", untouched);
    }
}

/* ------------------------------------------------------------------ cevap ayrıştırma */

static void test_parse_ok(void)
{
    static fwu_byte_t resp[1024];
    static run_t r;
    fwu_http_parser_t p;
    const fwu_http_response_t *info;
    size_t n = to_bytes(RESPONSE_OK, resp);
    const size_t patterns[] = { 0u, 1u, 2u, 3u, 7u, 13u, 64u };

    for(size_t k = 0u; k < ELEMENTS(patterns); k++) {
        char name[96];

        fwu_http_parser_init(&p);
        run_feed(&p, resp, n, patterns[k], &r);
        info = fwu_http_response(&p);
        (void)snprintf(name, sizeof(name), "206 cevabi, %u baytlik parcalarla: baslik, govde, tamam", (unsigned)patterns[k]);
        check(name, r.headers_events == 1 && r.error_events == 0 && fwu_http_complete(&p) && fwu_http_headers_done(&p) &&
                    same_text(r.body, r.body_total, "0123456789") && info->status == 206u && info->has_content_length &&
                    info->content_length == 10u && info->has_content_range && info->range_first == 512u &&
                    info->range_last == 521u && info->range_total == 57408u && !info->connection_close);
    }
    fwu_http_parser_init(&p);
    run_feed(&p, resp, n, 0u, &r);
    check("tek seferde: yalniz 2 olay (HEADERS_DONE, BODY)", r.calls == 2u);
}

static void test_parse_variants(void)
{
    static fwu_byte_t buf[1024];
    static run_t r;
    fwu_http_parser_t p;
    const fwu_http_response_t *info;
    size_t n;

    /* baslik adlari buyuk/kucuk harfe duyarsiz; yalniz LF satir sonu */
    n = to_bytes("HTTP/1.1 206 Partial Content\nCONTENT-LENGTH: 3\ncontent-range:bytes 0-2/10\nCoNnEcTiOn: Close\n\nabc", buf);
    fwu_http_parser_init(&p);
    run_feed(&p, buf, n, 1u, &r);
    info = fwu_http_response(&p);
    check("yalniz LF + karisik harfli baslik adlari + 'bytes' bosluksuz + Connection: Close",
          r.error_events == 0 && fwu_http_complete(&p) && same_text(r.body, r.body_total, "abc") && info->content_length == 3u &&
              info->has_content_range && info->range_first == 0u && info->range_last == 2u && info->range_total == 10u &&
              info->connection_close);

    /* tanınmayan basliklar, ':' olmayan satir, deger bosluklari */
    n = to_bytes("HTTP/1.0 206 OK\r\nX-Foo: bar\r\nbirsey\r\nContent-Length:   5   \r\nContent-Range: bytes  7-11/100 \r\n\r\nhello", buf);
    fwu_http_parser_init(&p);
    run_feed(&p, buf, n, 5u, &r);
    info = fwu_http_response(&p);
    check("HTTP/1.0, bilinmeyen/biçimsiz satirlar yok sayilir, deger bosluklari kirpilir",
          r.error_events == 0 && fwu_http_complete(&p) && same_text(r.body, r.body_total, "hello") && info->content_length == 5u &&
              info->range_first == 7u && info->range_last == 11u && info->range_total == 100u);

    /* durum satirinda aciklama yok */
    n = to_bytes("HTTP/1.1 206\r\nContent-Length: 1\r\n\r\nz", buf);
    fwu_http_parser_init(&p);
    run_feed(&p, buf, n, 0u, &r);
    check("durum satirinda aciklama metni olmayabilir", r.error_events == 0 && fwu_http_response(&p)->status == 206u && fwu_http_complete(&p));

    /* 404 + govde: durum kodu okunur, govde yine sayilir */
    n = to_bytes("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\n\r\nnot found", buf);
    fwu_http_parser_init(&p);
    run_feed(&p, buf, n, 4u, &r);
    check("404: durum kodu, govde sayilir (karar fwu_pull'un)", r.error_events == 0 && fwu_http_response(&p)->status == 404u &&
                                                               fwu_http_complete(&p) && same_text(r.body, r.body_total, "not found"));

    /* 200 (Range yok sayildi) */
    n = to_bytes("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nfull", buf);
    fwu_http_parser_init(&p);
    run_feed(&p, buf, n, 0u, &r);
    check("200: durum 200, Content-Range yok", fwu_http_response(&p)->status == 200u && !fwu_http_response(&p)->has_content_range);

    /* Content-Length: 0 -> baslikla birlikte tamam */
    n = to_bytes("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nContent-Range: bytes */100\r\n\r\n", buf);
    fwu_http_parser_init(&p);
    run_feed(&p, buf, n, 0u, &r);
    check("Content-Length 0: HEADERS_DONE'da tamam, 'bytes */100' Content-Range sayilmaz",
          r.error_events == 0 && r.headers_events == 1 && fwu_http_complete(&p) && r.body_total == 0u &&
              !fwu_http_response(&p)->has_content_range && fwu_http_response(&p)->status == 416u);

    /* Content-Range bicimleri: gecersiz olanlar has_content_range = false */
    {
        static const char *bad_ranges[] = {
            "bytes 5-3/10", "bytes a-b/c", "bytes 0-9", "items 0-9/10", "bytes -5/10", "bytes 0-9/", "bytes 0-9/x", "", "bytes 99999999999-1/2"
        };
        for(size_t i = 0u; i < ELEMENTS(bad_ranges); i++) {
            char line[200];
            char name[128];
            (void)snprintf(line, sizeof(line), "HTTP/1.1 206 P\r\nContent-Length: 1\r\nContent-Range: %s\r\n\r\nx", bad_ranges[i]);
            n = to_bytes(line, buf);
            fwu_http_parser_init(&p);
            run_feed(&p, buf, n, 0u, &r);
            (void)snprintf(name, sizeof(name), "gecersiz Content-Range '%s' kabul edilmez (hata degil, has_content_range false)", bad_ranges[i]);
            check(name, r.error_events == 0 && !fwu_http_response(&p)->has_content_range);
        }
    }

    /* ayni Content-Length iki kez: sorun degil */
    n = to_bytes("HTTP/1.1 206 P\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\nab", buf);
    fwu_http_parser_init(&p);
    run_feed(&p, buf, n, 0u, &r);
    check("ayni Content-Length iki kez gelirse kabul", r.error_events == 0 && fwu_http_complete(&p));

    /* Gövde sonrasi fazla baytlar (eski/yinelenen cevap) sessizce atilir */
    n = to_bytes("HTTP/1.1 206 P\r\nContent-Length: 2\r\n\r\nabEXTRA-BYTES-OF-A-STALE-RESPONSE", buf);
    fwu_http_parser_init(&p);
    run_feed(&p, buf, n, 0u, &r);
    check("govdeden sonraki fazla baytlar atilir, hata degil, govde 'ab'", r.error_events == 0 && fwu_http_complete(&p) && same_text(r.body, r.body_total, "ab"));
}

static void test_parse_errors(void)
{
    static fwu_byte_t buf[4096];
    static run_t r;
    fwu_http_parser_t p;
    size_t n;
    size_t used = 0u;
    const fwu_byte_t *body = NULL;
    size_t body_length = 0u;

    struct { const char *name; const char *text; } cases[] = {
        { "durum satiri: HTTP degil", "FTP/1.1 206 P\r\n\r\n" },
        { "durum satiri: HTTP/2", "HTTP/2 206 P\r\n\r\n" },
        { "durum satiri: surum 1.2", "HTTP/1.2 206 P\r\n\r\n" },
        { "durum satiri: iki haneli kod", "HTTP/1.1 20 P\r\n\r\n" },
        { "durum satiri: dort haneli kod", "HTTP/1.1 2060 P\r\n\r\n" },
        { "durum satiri: kod rakam degil", "HTTP/1.1 2x6 P\r\n\r\n" },
        { "durum satiri: kod 099", "HTTP/1.1 099 P\r\n\r\n" },
        { "durum satiri: kod 600", "HTTP/1.1 600 P\r\n\r\n" },
        { "durum satiri: kodtan sonra bosluk yok", "HTTP/1.1 206X\r\n\r\n" },
        { "durum satiri: bos satir", "\r\n\r\n" },
        { "Content-Length yok", "HTTP/1.1 206 P\r\nContent-Range: bytes 0-1/2\r\n\r\nab" },
        { "Content-Length rakam degil", "HTTP/1.1 206 P\r\nContent-Length: 1x\r\n\r\na" },
        { "Content-Length eksi", "HTTP/1.1 206 P\r\nContent-Length: -1\r\n\r\na" },
        { "Content-Length bos", "HTTP/1.1 206 P\r\nContent-Length:\r\n\r\n" },
        { "Content-Length 4294967296 (tasar)", "HTTP/1.1 206 P\r\nContent-Length: 4294967296\r\n\r\n" },
        { "Content-Length 99999999999999999999", "HTTP/1.1 206 P\r\nContent-Length: 99999999999999999999\r\n\r\n" },
        { "celisen iki Content-Length", "HTTP/1.1 206 P\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nabc" },
        { "chunked", "HTTP/1.1 206 P\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n2\r\nab\r\n0\r\n\r\n" },
        { "chunked (karisik harf)", "HTTP/1.1 206 P\r\ntransfer-encoding: gzip, ChUnKeD\r\n\r\n" },
    };

    for(size_t i = 0u; i < ELEMENTS(cases); i++) {
        for(size_t pattern = 0u; pattern < 2u; pattern++) {
            char name[160];
            n = to_bytes(cases[i].text, buf);
            fwu_http_parser_init(&p);
            run_feed(&p, buf, n, pattern, &r);
            (void)snprintf(name, sizeof(name), "%s: FWU_ERR_PULL_PROTOCOL (%s)", cases[i].name, pattern == 0u ? "tek seferde" : "bayt bayt");
            check(name, r.error_events == 1 && r.error == FWU_ERR_PULL_PROTOCOL && !fwu_http_complete(&p) && r.body_total == 0u);
        }
    }

    /* hatadan sonra: tuketmez, hata durumunda kalir; init temizler */
    n = to_bytes("garbage\r\n", buf);
    fwu_http_parser_init(&p);
    (void)fwu_http_feed(&p, buf, n, &used, &body, &body_length);
    check("hatali ayristirici sonraki beslemede ERROR doner ve tuketmez",
          fwu_http_feed(&p, buf, 4u, &used, &body, &body_length) == FWU_HTTP_ERROR && used == 0u && fwu_http_error(&p) == FWU_ERR_PULL_PROTOCOL);
    fwu_http_parser_init(&p);
    n = to_bytes(RESPONSE_OK, buf);
    run_feed(&p, buf, n, 0u, &r);
    check("init hatadan sonra ayristiriciyi yeniden kullanilabilir yapar", r.error_events == 0 && fwu_http_complete(&p) && fwu_http_error(&p) == FWU_OK);

    /* baslik bolumu siniri */
    {
        size_t pos = to_bytes("HTTP/1.1 206 P\r\n", buf);
        int ok_below = 0;
        int err_above = 0;

        /* sinirin hemen altinda: bir cok kisa baslik + Content-Length: 1 */
        while(pos < (FWU_HTTP_HEADER_MAX_BYTES - 40u)) {
            pos += to_bytes("X-Padding: 0123456789\r\n", &buf[pos]);
        }
        pos += to_bytes("Content-Length: 1\r\n\r\nz", &buf[pos]);
        fwu_http_parser_init(&p);
        run_feed(&p, buf, pos, 0u, &r);
        ok_below = (r.error_events == 0) && fwu_http_complete(&p);
        check("baslik bolumu sinirin altinda: kabul", ok_below);

        pos = to_bytes("HTTP/1.1 206 P\r\n", buf);
        while(pos < (FWU_HTTP_HEADER_MAX_BYTES + 100u)) {
            pos += to_bytes("X-Padding: 0123456789\r\n", &buf[pos]);
        }
        pos += to_bytes("Content-Length: 1\r\n\r\nz", &buf[pos]);
        fwu_http_parser_init(&p);
        run_feed(&p, buf, pos, 0u, &r);
        err_above = (r.error_events == 1) && (r.error == FWU_ERR_PULL_PROTOCOL);
        check("baslik bolumu FWU_HTTP_HEADER_MAX_BYTES'i asarsa reddedilir (tasma savunmasi)", err_above);
    }

    /* cok uzun TEK satir: yok sayilir ama toplam sinir hala gecerli; aranan baslik sonra gelirse okunur */
    {
        size_t pos = to_bytes("HTTP/1.1 206 P\r\nX-Long: ", buf);
        for(size_t i = 0u; i < 300u; i++) {
            buf[pos++] = (fwu_byte_t)'a';
        }
        pos += to_bytes("\r\nContent-Length: 2\r\n\r\nok", &buf[pos]);
        fwu_http_parser_init(&p);
        run_feed(&p, buf, pos, 0u, &r);
        check("300 karakterlik tanimsiz baslik satiri (LINE_MAX'i asar) atlanir, sonraki baslik okunur",
              r.error_events == 0 && fwu_http_complete(&p) && same_text(r.body, r.body_total, "ok"));
    }

    /* NULL ve bos girdi */
    fwu_http_parser_init(&p);
    check("NULL ayristirici ile feed ERROR", fwu_http_feed(NULL, buf, 1u, &used, &body, &body_length) == FWU_HTTP_ERROR);
    check("NULL consumed ile feed ERROR", fwu_http_feed(&p, buf, 1u, NULL, &body, &body_length) == FWU_HTTP_ERROR);
    fwu_http_parser_init(&p);
    check("NULL veri + 0 uzunluk: NEED_MORE (hata degil)", fwu_http_feed(&p, NULL, 0u, &used, &body, &body_length) == FWU_HTTP_NEED_MORE && used == 0u);
    fwu_http_parser_init(&p);
    check("NULL veri + uzunluk > 0: ERROR", fwu_http_feed(&p, NULL, 5u, &used, &body, &body_length) == FWU_HTTP_ERROR);
    check("NULL ayristiriciya sorgular guvenli", fwu_http_response(NULL) == NULL && !fwu_http_complete(NULL) && !fwu_http_headers_done(NULL) &&
                                                  fwu_http_error(NULL) == FWU_ERR_INVALID_ARG);
    fwu_http_parser_init(NULL);
    check("init(NULL) cokmez", 1);
}

/* ------------------------------------------------------------------ düşman akış */

static uint32_t g_rng = 12345u;

static uint32_t rnd(void)
{
    g_rng = (g_rng * 1664525u) + 1013904223u;
    return g_rng >> 8;
}

static void test_fuzz(void)
{
    static fwu_byte_t buf[2048];
    static run_t r;
    fwu_http_parser_t p;
    int sane = 1;
    int partial_consistent = 1;

    /* 1) tamamen rastgele baytlar, rastgele parcalarla: cokmez, sonsuz donmez */
    for(int round = 0; round < 400; round++) {
        const size_t n = 1u + (size_t)(rnd() % 600u);

        for(size_t i = 0u; i < n; i++) {
            buf[i] = (fwu_byte_t)(rnd() & 0xFFu);
        }
        fwu_http_parser_init(&p);
        run_feed(&p, buf, n, 1u + (size_t)(rnd() % 40u), &r);
        if(r.body_total > n) {
            sane = 0;
        }
    }
    check("400 rastgele akis: cokme/sonsuz dongu yok, govde girdiden buyuk olamaz", sane);

    /* 2) yari gecerli: gecerli cevabin baytlari rastgele bozulur; govde asla Content-Length'i asmaz */
    for(int round = 0; round < 600; round++) {
        size_t n = to_bytes(RESPONSE_OK, buf);
        const uint32_t flips = 1u + (rnd() % 3u);

        for(uint32_t f = 0u; f < flips; f++) {
            buf[rnd() % n] = (fwu_byte_t)(rnd() & 0xFFu);
        }
        fwu_http_parser_init(&p);
        run_feed(&p, buf, n, 1u + (size_t)(rnd() % 9u), &r);
        if(fwu_http_headers_done(&p) && (r.body_total > fwu_http_response(&p)->content_length)) {
            partial_consistent = 0;
        }
    }
    check("600 bozulmus cevap: govde hicbir zaman bildirilen Content-Length'i asmaz", partial_consistent);

    /* 3) parcalama bagimsizligi: ayni gecerli cevap, rastgele parcalarla her zaman ayni sonucu verir */
    {
        int all_same = 1;
        for(int round = 0; round < 300; round++) {
            size_t n = to_bytes(RESPONSE_OK, buf);

            fwu_http_parser_init(&p);
            run_feed(&p, buf, n, 1u + (size_t)(rnd() % 30u), &r);
            if(!(r.error_events == 0 && fwu_http_complete(&p) && same_text(r.body, r.body_total, "0123456789") &&
                 fwu_http_response(&p)->range_first == 512u)) {
                all_same = 0;
            }
        }
        check("300 rastgele parcalama: sonuc her zaman ayni", all_same);
    }
}

int main(void)
{
    test_request();
    test_parse_ok();
    test_parse_variants();
    test_parse_errors();
    test_fuzz();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
