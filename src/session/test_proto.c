/**
 * @file    test_proto.c
 * @brief   fwu_proto (oturum protokolü sabitleri, kodlama, ayrıştırma) için kendi kendini kontrol eden test.
 *          Beklenen baytlar ELLE yazılmıştır (kodlayıcının kendisinden türetilmez).
 *
 * Kapsam: sabitler (numaralar kalıcı), her mesajın bilinen baytları, gidiş-dönüş, uzunluk sınırları, NULL ve
 * kapasite hataları, hatada çıkışın değişmemesi, kirli üst bitler (C28x).
 *
 * Derleme (session klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../frame fwu_proto.c test_proto.c -o test_proto && ./test_proto
 */

#include <stdio.h>
#include "fwu_proto.h"

/* C28x'te fwu_byte_t 16 bittir: sizeof BAYT degil ELEMAN sayisi vermez, bu yuzden eleman sayisi ayri hesaplanir. */
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

static int bytes_are(const fwu_byte_t *got, const unsigned *want, size_t n)
{
    for(size_t i = 0u; i < n; i++) {
        if(got[i] != (fwu_byte_t)want[i]) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ sabitler */

static void test_constants(void)
{
    const unsigned requests[] = { FWU_MSG_HELLO, FWU_MSG_AUTH_REQ, FWU_MSG_AUTH_RESP, FWU_MSG_REQUEST_DOWNLOAD,
                                  FWU_MSG_TRANSFER_DATA, FWU_MSG_TRANSFER_EXIT, FWU_MSG_ACTIVATE, FWU_MSG_ABORT,
                                  FWU_MSG_KEEPALIVE };
    const unsigned responses[] = { FWU_MSG_INFO, FWU_MSG_AUTH_CHALLENGE, FWU_MSG_AUTH_OK, FWU_MSG_DOWNLOAD_ACCEPTED,
                                   FWU_MSG_DATA_ACK, FWU_MSG_EXIT_PENDING, FWU_MSG_ACTIVATE_OK, FWU_MSG_VERIFY_RESULT,
                                   FWU_MSG_ABORT_OK, FWU_MSG_KEEPALIVE_OK };
    int unique = 1;
    int requests_low = 1;
    int responses_high = 1;
    int pairs = 1;

    printf("\n--- sabitler: mesaj numaralari KALICI ---\n");
    check("istek turleri: 0x01 0x02 0x03 0x10 0x11 0x12 0x13 0x1F 0x3E",
          FWU_MSG_HELLO == 0x01u && FWU_MSG_AUTH_REQ == 0x02u && FWU_MSG_AUTH_RESP == 0x03u &&
          FWU_MSG_REQUEST_DOWNLOAD == 0x10u && FWU_MSG_TRANSFER_DATA == 0x11u && FWU_MSG_TRANSFER_EXIT == 0x12u &&
          FWU_MSG_ACTIVATE == 0x13u && FWU_MSG_ABORT == 0x1Fu && FWU_MSG_KEEPALIVE == 0x3Eu);
    check("yanit turleri: INFO 0x81, AUTH_CHALLENGE 0x82, AUTH_OK 0x83, ACCEPTED 0x90, DATA_ACK 0x91",
          FWU_MSG_INFO == 0x81u && FWU_MSG_AUTH_CHALLENGE == 0x82u && FWU_MSG_AUTH_OK == 0x83u &&
          FWU_MSG_DOWNLOAD_ACCEPTED == 0x90u && FWU_MSG_DATA_ACK == 0x91u);
    check("EXIT_PENDING 0x92, ACTIVATE_OK 0x93, VERIFY_RESULT 0x94, ABORT_OK 0x9F, KEEPALIVE_OK 0xBE, ERROR 0x7F",
          FWU_MSG_EXIT_PENDING == 0x92u && FWU_MSG_ACTIVATE_OK == 0x93u && FWU_MSG_VERIFY_RESULT == 0x94u &&
          FWU_MSG_ABORT_OK == 0x9Fu && FWU_MSG_KEEPALIVE_OK == 0xBEu && FWU_MSG_ERROR == 0x7Fu);
    check("durumlar: IDLE 0, AUTH_PENDING 1, AUTHENTICATED 2, DOWNLOADING 3, VERIFYING 4, READY 5, COMMITTED 6",
          FWU_PROTO_STATE_IDLE == 0u && FWU_PROTO_STATE_AUTH_PENDING == 1u && FWU_PROTO_STATE_AUTHENTICATED == 2u &&
          FWU_PROTO_STATE_DOWNLOADING == 3u && FWU_PROTO_STATE_VERIFYING == 4u && FWU_PROTO_STATE_READY == 5u &&
          FWU_PROTO_STATE_COMMITTED == 6u);
    check("bayraklar ve mod: AUTH_REQUIRED 1, SIGNED 2, HANDSHAKE 0; surum 1",
          FWU_PROTO_FLAG_AUTH_REQUIRED == 1u && FWU_PROTO_FLAG_SIGNED == 2u && FWU_PROTO_MODE_HANDSHAKE == 0u &&
          FWU_PROTO_VERSION == 1u);
    check("boyutlar: INFO 13, ACCEPTED 9, u32 4, veri basligi 4, ERROR 2, parca 508, meydan okuma 64, yanit 128",
          FWU_PROTO_INFO_SIZE == 13u && FWU_PROTO_ACCEPTED_SIZE == 9u && FWU_PROTO_U32_SIZE == 4u &&
          FWU_PROTO_DATA_HEADER_SIZE == 4u && FWU_PROTO_ERROR_SIZE == 2u && FWU_PROTO_MAX_CHUNK == 508u &&
          FWU_PROTO_AUTH_CHALLENGE_MAX == 64u && FWU_PROTO_AUTH_RESPONSE_MAX == 128u);
    check("en buyuk TRANSFER_DATA payload'i tam 512 (cerceve siniri)", (FWU_PROTO_DATA_HEADER_SIZE + FWU_PROTO_MAX_CHUNK) == FWU_FRAME_MAX_PAYLOAD);

    for(unsigned i = 0u; i < 9u; i++) {
        requests_low = requests_low && (requests[i] < 0x80u);
        for(unsigned j = i + 1u; j < 9u; j++) {
            unique = unique && (requests[i] != requests[j]);
        }
    }
    for(unsigned i = 0u; i < 10u; i++) {
        responses_high = responses_high && (responses[i] >= 0x80u);
        for(unsigned j = i + 1u; j < 10u; j++) {
            unique = unique && (responses[i] != responses[j]);
        }
    }
    check("istek turleri < 0x80, yanit turleri >= 0x80, hicbiri tekrar etmiyor", requests_low && responses_high && unique);
    check("ERROR 0x7F hicbir istek ve yanitla cakismaz", FWU_MSG_ERROR != FWU_MSG_KEEPALIVE && FWU_MSG_ERROR < 0x80u);
    pairs = pairs && ((FWU_MSG_HELLO | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_INFO);
    pairs = pairs && ((FWU_MSG_AUTH_REQ | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_AUTH_CHALLENGE);
    pairs = pairs && ((FWU_MSG_AUTH_RESP | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_AUTH_OK);
    pairs = pairs && ((FWU_MSG_REQUEST_DOWNLOAD | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_DOWNLOAD_ACCEPTED);
    pairs = pairs && ((FWU_MSG_TRANSFER_DATA | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_DATA_ACK);
    pairs = pairs && ((FWU_MSG_TRANSFER_EXIT | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_EXIT_PENDING);
    pairs = pairs && ((FWU_MSG_ACTIVATE | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_ACTIVATE_OK);
    pairs = pairs && ((FWU_MSG_ABORT | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_ABORT_OK);
    pairs = pairs && ((FWU_MSG_KEEPALIVE | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_KEEPALIVE_OK);
    check("yanit = istek | 0x80 kurali (VERIFY_RESULT ve ERROR ayri, eslesmesiz)", pairs && FWU_PROTO_RESPONSE_FLAG == 0x80u);
}

/* ------------------------------------------------------------------ kodlama: bilinen baytlar */

static void test_encode_known(void)
{
    fwu_byte_t out[600];
    size_t length = 0u;
    fwu_proto_info_t info;
    fwu_proto_accepted_t acc;
    const fwu_byte_t data[3] = { 0xAAu, 0xBBu, 0xCCu };
    const unsigned want_info[13] = { 0x01u, 0x03u, 0x00u, 0x00u, 0x30u, 0x39u, 0x00u, 0x00u, 0x00u, 0x19u, 0x01u, 0xFCu, 0x03u };
    const unsigned want_acc[9] = { 0x01u, 0x00u, 0x00u, 0x00u, 0x1Eu, 0x00u, 0x00u, 0x01u, 0x00u };
    const unsigned want_u32[4] = { 0x01u, 0x02u, 0x03u, 0x04u };
    const unsigned want_data[7] = { 0x00u, 0x00u, 0x02u, 0x00u, 0xAAu, 0xBBu, 0xCCu };

    printf("\n--- kodlama: ELLE yazilmis baytlar ---\n");
    info.proto_version = 1u; info.flags = 0x03u; info.product_id = 12345u; info.running_image_version = 25u;
    info.max_chunk = 508u; info.state = 3u;
    expect("INFO", fwu_proto_encode_info(&info, out, sizeof(out) / sizeof(out[0]), &length), FWU_OK);
    check("... 13 bayt: 01 03 | 00 00 30 39 | 00 00 00 19 | 01 FC | 03", length == 13u && bytes_are(out, want_info, 13u));

    acc.chunk_size = 256u; acc.mode = 0u; acc.timeout_s = 30u; acc.next_offset = 256u;
    expect("DOWNLOAD_ACCEPTED", fwu_proto_encode_accepted(&acc, out, sizeof(out) / sizeof(out[0]), &length), FWU_OK);
    check("... 9 bayt: 01 00 | 00 | 00 1E | 00 00 01 00", length == 9u && bytes_are(out, want_acc, 9u));

    expect("u32 0x01020304", fwu_proto_encode_u32(0x01020304u, out, sizeof(out) / sizeof(out[0]), &length), FWU_OK);
    check("... 01 02 03 04 (buyuk endian)", length == 4u && bytes_are(out, want_u32, 4u));

    expect("TRANSFER_DATA ofset 0x200, 3 bayt", fwu_proto_encode_data(0x200u, data, 3u, out, sizeof(out) / sizeof(out[0]), &length), FWU_OK);
    check("... 00 00 02 00 AA BB CC", length == 7u && bytes_are(out, want_data, 7u));

    expect("VERIFY_RESULT kod 50", fwu_proto_encode_result(FWU_ERR_BAD_SIGNATURE, out, sizeof(out) / sizeof(out[0]), &length), FWU_OK);
    check("... tek bayt 0x32", length == 1u && out[0] == 0x32u);
    expect("VERIFY_RESULT kod 0 (gecerli)", fwu_proto_encode_result(FWU_OK, out, sizeof(out) / sizeof(out[0]), &length), FWU_OK);
    check("... tek bayt 0x00", length == 1u && out[0] == 0x00u);
    expect("ERROR kod 50, istek 0x11", fwu_proto_encode_error(FWU_ERR_BAD_SIGNATURE, 0x11u, out, sizeof(out) / sizeof(out[0]), &length), FWU_OK);
    check("... 32 11", length == 2u && out[0] == 0x32u && out[1] == 0x11u);
    expect("ERROR en kucuk kod (INVALID_ARG = 10)", fwu_proto_encode_error(FWU_ERR_INVALID_ARG, 0xFFu, out, sizeof(out) / sizeof(out[0]), &length), FWU_OK);
    check("... 0A FF", out[0] == 0x0Au && out[1] == 0xFFu);

    /* en buyuk parca */
    {
        static fwu_byte_t big[FWU_PROTO_MAX_CHUNK];
        static fwu_byte_t frame_payload[FWU_FRAME_MAX_PAYLOAD];
        int ok = 1;

        for(size_t i = 0u; i < FWU_PROTO_MAX_CHUNK; i++) {
            big[i] = (fwu_byte_t)(i & 0xFFu);
        }
        expect("TRANSFER_DATA en buyuk parca (508)", fwu_proto_encode_data(0xFFFFFFFFu, big, FWU_PROTO_MAX_CHUNK, frame_payload, FWU_FRAME_MAX_PAYLOAD, &length), FWU_OK);
        check("... payload 512, ofset FF FF FF FF", length == 512u && frame_payload[0] == 0xFFu && frame_payload[3] == 0xFFu);
        for(size_t i = 0u; i < FWU_PROTO_MAX_CHUNK; i++) {
            if(frame_payload[4u + i] != (fwu_byte_t)(i & 0xFFu)) {
                ok = 0;
            }
        }
        check("... 508 veri baytinin hepsi yerinde", ok);
    }
}

/* ------------------------------------------------------------------ ayrıştırma ve gidiş-dönüş */

static void test_parse(void)
{
    fwu_byte_t buf[600];
    size_t length = 0u;
    fwu_proto_info_t info;
    fwu_proto_info_t info2;
    fwu_proto_accepted_t acc;
    fwu_proto_accepted_t acc2;
    fwu_proto_data_t data;
    uint32_t u = 0u;
    fwu_byte_t code = 0u;
    fwu_byte_t type = 0u;
    const fwu_byte_t chunk[5] = { 1u, 2u, 3u, 4u, 5u };

    printf("\n--- ayristirma ve gidis-donus ---\n");
    info.proto_version = 1u; info.flags = 0x02u; info.product_id = 0xDEADBEEFu; info.running_image_version = 0xFFFFFFFFu;
    info.max_chunk = 0xFFFFu; info.state = 6u;
    (void)fwu_proto_encode_info(&info, buf, sizeof(buf) / sizeof(buf[0]), &length);
    expect("INFO gidis-donus", fwu_proto_parse_info(buf, length, &info2), FWU_OK);
    check("... tum alanlar (uc degerler dahil)", info2.proto_version == 1u && info2.flags == 0x02u && info2.product_id == 0xDEADBEEFu &&
          info2.running_image_version == 0xFFFFFFFFu && info2.max_chunk == 0xFFFFu && info2.state == 6u);

    acc.chunk_size = 0xFFFFu; acc.mode = 7u; acc.timeout_s = 0xFFFFu; acc.next_offset = 0xFFFFFFFFu;
    (void)fwu_proto_encode_accepted(&acc, buf, sizeof(buf) / sizeof(buf[0]), &length);
    expect("ACCEPTED gidis-donus", fwu_proto_parse_accepted(buf, length, &acc2), FWU_OK);
    check("... tum alanlar", acc2.chunk_size == 0xFFFFu && acc2.mode == 7u && acc2.timeout_s == 0xFFFFu && acc2.next_offset == 0xFFFFFFFFu);

    (void)fwu_proto_encode_u32(0x80000001u, buf, sizeof(buf) / sizeof(buf[0]), &length);
    expect("u32 gidis-donus", fwu_proto_parse_u32(buf, length, &u), FWU_OK);
    check("... 0x80000001", u == 0x80000001u);

    (void)fwu_proto_encode_data(0x12345678u, chunk, 5u, buf, sizeof(buf) / sizeof(buf[0]), &length);
    expect("TRANSFER_DATA gidis-donus", fwu_proto_parse_data(buf, length, &data), FWU_OK);
    check("... ofset, 5 bayt veri, isaretci payload'in icinde (kopya yok)",
          data.offset == 0x12345678u && data.data_length == 5u && data.data == &buf[4] && data.data[0] == 1u && data.data[4] == 5u);

    /* ELLE yazilmis baytlardan ayristirma (gidis-donus tum degerleri esit secerse konum hatasini gizler) */
    {
        const fwu_byte_t acc_bytes[9] = { 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x09u };
        const fwu_byte_t info_bytes[13] = { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u, 0x99u, 0xAAu, 0xBBu, 0xCCu, 0xDDu };
        const fwu_byte_t data_bytes[7] = { 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0xE1u, 0xE2u, 0xE3u };

        expect("ACCEPTED, 01..09 baytlari", fwu_proto_parse_accepted(acc_bytes, 9u, &acc2), FWU_OK);
        check("... parca 0x0102, mod 3, zaman asimi 0x0405, sonraki ofset 0x06070809",
              acc2.chunk_size == 0x0102u && acc2.mode == 3u && acc2.timeout_s == 0x0405u && acc2.next_offset == 0x06070809u);
        expect("INFO, 11..DD baytlari", fwu_proto_parse_info(info_bytes, 13u, &info2), FWU_OK);
        check("... surum 0x11, bayrak 0x22, urun 0x33445566, calisan surum 0x778899AA, parca 0xBBCC, durum 0xDD",
              info2.proto_version == 0x11u && info2.flags == 0x22u && info2.product_id == 0x33445566u &&
              info2.running_image_version == 0x778899AAu && info2.max_chunk == 0xBBCCu && info2.state == 0xDDu);
        expect("u32, 0A 0B 0C 0D", fwu_proto_parse_u32(data_bytes, 4u, &u), FWU_OK);
        check("... 0x0A0B0C0D", u == 0x0A0B0C0Du);
        expect("DATA, 0A 0B 0C 0D E1 E2 E3", fwu_proto_parse_data(data_bytes, 7u, &data), FWU_OK);
        check("... ofset 0x0A0B0C0D, 3 bayt veri (E1 E2 E3)", data.offset == 0x0A0B0C0Du && data.data_length == 3u &&
              data.data[0] == 0xE1u && data.data[2] == 0xE3u);
        expect("ERROR, 0A 0B", fwu_proto_parse_error(data_bytes, 2u, &code, &type), FWU_OK);
        check("... kod 0x0A, istek turu 0x0B", code == 0x0Au && type == 0x0Bu);
    }

    /* veri uzunluk sinirlari */
    {
        static fwu_byte_t p[FWU_FRAME_MAX_PAYLOAD + 1u];
        for(size_t i = 0u; i < sizeof(p) / sizeof(p[0]); i++) {
            p[i] = 0x11u;
        }
        data.data_length = 99u;
        expect("TRANSFER_DATA payload 4 (veri yok)", fwu_proto_parse_data(p, 4u, &data), FWU_ERR_BAD_LENGTH);
        expect("TRANSFER_DATA payload 0", fwu_proto_parse_data(p, 0u, &data), FWU_ERR_BAD_LENGTH);
        check("... hatada cikis degismedi", data.data_length == 99u);
        expect("TRANSFER_DATA payload 5 (1 bayt veri)", fwu_proto_parse_data(p, 5u, &data), FWU_OK);
        check("... 1 bayt", data.data_length == 1u);
        expect("TRANSFER_DATA payload 512 (508 bayt veri)", fwu_proto_parse_data(p, 512u, &data), FWU_OK);
        check("... 508 bayt", data.data_length == 508u);
        expect("TRANSFER_DATA payload 513", fwu_proto_parse_data(p, 513u, &data), FWU_ERR_BAD_LENGTH);
    }

    (void)fwu_proto_encode_result(FWU_ERR_HASH_MISMATCH, buf, sizeof(buf) / sizeof(buf[0]), &length);
    expect("RESULT gidis-donus", fwu_proto_parse_result(buf, length, &code), FWU_OK);
    check("... kod 35", code == 35u);
    (void)fwu_proto_encode_error(FWU_ERR_LOCKED_OUT, 0x02u, buf, sizeof(buf) / sizeof(buf[0]), &length);
    expect("ERROR gidis-donus", fwu_proto_parse_error(buf, length, &code, &type), FWU_OK);
    check("... kod 62, istek 0x02", code == 62u && type == 0x02u);

    /* uzunluk uyusmazliklari: tam beklenen uzunluk, 1 eksik ve 1 fazla reddedilir */
    {
        fwu_byte_t pad[20];
        for(size_t i = 0u; i < sizeof(pad) / sizeof(pad[0]); i++) {
            pad[i] = 0u;
        }
        expect("INFO 12 bayt", fwu_proto_parse_info(pad, 12u, &info2), FWU_ERR_BAD_LENGTH);
        expect("INFO 14 bayt", fwu_proto_parse_info(pad, 14u, &info2), FWU_ERR_BAD_LENGTH);
        expect("ACCEPTED 8 bayt", fwu_proto_parse_accepted(pad, 8u, &acc2), FWU_ERR_BAD_LENGTH);
        expect("ACCEPTED 10 bayt", fwu_proto_parse_accepted(pad, 10u, &acc2), FWU_ERR_BAD_LENGTH);
        expect("u32 3 bayt", fwu_proto_parse_u32(pad, 3u, &u), FWU_ERR_BAD_LENGTH);
        expect("u32 5 bayt", fwu_proto_parse_u32(pad, 5u, &u), FWU_ERR_BAD_LENGTH);
        expect("RESULT 0 bayt", fwu_proto_parse_result(pad, 0u, &code), FWU_ERR_BAD_LENGTH);
        expect("RESULT 2 bayt", fwu_proto_parse_result(pad, 2u, &code), FWU_ERR_BAD_LENGTH);
        expect("ERROR 1 bayt", fwu_proto_parse_error(pad, 1u, &code, &type), FWU_ERR_BAD_LENGTH);
        expect("ERROR 3 bayt", fwu_proto_parse_error(pad, 3u, &code, &type), FWU_ERR_BAD_LENGTH);
    }

    /* kirli ust bitler (C28x: eleman 16 bit, ust 8 bit cop olabilir) */
    {
        fwu_byte_t dirty[13];
        const unsigned clean[13] = { 0x01u, 0x03u, 0x00u, 0x00u, 0x30u, 0x39u, 0x00u, 0x00u, 0x00u, 0x19u, 0x01u, 0xFCu, 0x03u };
        for(size_t i = 0u; i < 13u; i++) {
            dirty[i] = (fwu_byte_t)(clean[i] | 0xAB00u);
        }
        expect("INFO, her elemana | 0xAB00", fwu_proto_parse_info(dirty, 13u, &info2), FWU_OK);
        check("... kirli ve temiz ayni sayilari verir", info2.proto_version == 1u && info2.flags == 3u && info2.product_id == 12345u &&
              info2.running_image_version == 25u && info2.max_chunk == 508u && info2.state == 3u);
        dirty[0] = (fwu_byte_t)(0x32u | 0xAB00u);
        dirty[1] = (fwu_byte_t)(0x11u | 0x7700u);
        expect("ERROR, kirli", fwu_proto_parse_error(dirty, 2u, &code, &type), FWU_OK);
        check("... kod 50, istek 0x11", code == 50u && type == 0x11u);
        expect("RESULT, kirli", fwu_proto_parse_result(dirty, 1u, &code), FWU_OK);
        check("... kod 50", code == 50u);
    }
}

/* ------------------------------------------------------------------ hatalar: NULL, kapasite, aralik */

static void test_errors(void)
{
    fwu_byte_t out[600];
    fwu_byte_t small[16];
    size_t length = 77u;
    fwu_proto_info_t info;
    fwu_proto_accepted_t acc;
    const fwu_byte_t data[2] = { 1u, 2u };
    fwu_byte_t code = 0u;
    fwu_byte_t type = 0u;
    uint32_t u = 0u;
    fwu_proto_info_t pinfo;
    fwu_proto_accepted_t pacc;
    fwu_proto_data_t pdata;

    printf("\n--- hatalar: NULL, kapasite, aralik; hatada cikis degismez ---\n");
    info.proto_version = 1u; info.flags = 0u; info.product_id = 1u; info.running_image_version = 1u; info.max_chunk = 1u; info.state = 0u;
    acc.chunk_size = 1u; acc.mode = 0u; acc.timeout_s = 1u; acc.next_offset = 0u;
    for(size_t i = 0u; i < sizeof(out) / sizeof(out[0]); i++) {
        out[i] = 0x55u;
    }

    expect("INFO info NULL", fwu_proto_encode_info(NULL, out, 600u, &length), FWU_ERR_INVALID_ARG);
    expect("INFO out NULL", fwu_proto_encode_info(&info, NULL, 600u, &length), FWU_ERR_INVALID_ARG);
    expect("INFO out_length NULL", fwu_proto_encode_info(&info, out, 600u, NULL), FWU_ERR_INVALID_ARG);
    expect("INFO kapasite 12 (1 eksik)", fwu_proto_encode_info(&info, out, 12u, &length), FWU_ERR_BUFFER_TOO_SMALL);
    check("... out ve out_length DEGISMEDI", out[0] == 0x55u && out[12] == 0x55u && length == 77u);
    expect("INFO kapasite 13 (tam)", fwu_proto_encode_info(&info, out, 13u, &length), FWU_OK);

    expect("ACCEPTED NULL", fwu_proto_encode_accepted(NULL, out, 600u, &length), FWU_ERR_INVALID_ARG);
    expect("ACCEPTED kapasite 8", fwu_proto_encode_accepted(&acc, small, 8u, &length), FWU_ERR_BUFFER_TOO_SMALL);
    expect("ACCEPTED kapasite 9", fwu_proto_encode_accepted(&acc, small, 9u, &length), FWU_OK);
    expect("u32 kapasite 3", fwu_proto_encode_u32(1u, small, 3u, &length), FWU_ERR_BUFFER_TOO_SMALL);
    expect("u32 kapasite 4", fwu_proto_encode_u32(1u, small, 4u, &length), FWU_OK);
    expect("u32 out NULL", fwu_proto_encode_u32(1u, NULL, 4u, &length), FWU_ERR_INVALID_ARG);

    expect("DATA veri NULL", fwu_proto_encode_data(0u, NULL, 2u, out, 600u, &length), FWU_ERR_INVALID_ARG);
    expect("DATA veri 0 bayt", fwu_proto_encode_data(0u, data, 0u, out, 600u, &length), FWU_ERR_INVALID_ARG);
    expect("DATA veri 509 bayt (siniri asar)", fwu_proto_encode_data(0u, out, 509u, out + 0, 600u, &length), FWU_ERR_INVALID_ARG);
    expect("DATA kapasite 5 (6 gerekir)", fwu_proto_encode_data(0u, data, 2u, small, 5u, &length), FWU_ERR_BUFFER_TOO_SMALL);
    expect("DATA kapasite 6", fwu_proto_encode_data(0u, data, 2u, small, 6u, &length), FWU_OK);

    expect("RESULT kod 256 (1 bayta sigmaz)", fwu_proto_encode_result((fwu_return_e)256, small, 16u, &length), FWU_ERR_INVALID_ARG);
    expect("RESULT kod 255", fwu_proto_encode_result((fwu_return_e)255, small, 16u, &length), FWU_OK);
    check("... bayt 0xFF", small[0] == 0xFFu);
    expect("RESULT kapasite 0", fwu_proto_encode_result(FWU_OK, small, 0u, &length), FWU_ERR_BUFFER_TOO_SMALL);
    expect("ERROR kod 0 (basari hata yaniti olamaz)", fwu_proto_encode_error(FWU_OK, 1u, small, 16u, &length), FWU_ERR_INVALID_ARG);
    expect("ERROR kod 256", fwu_proto_encode_error((fwu_return_e)256, 1u, small, 16u, &length), FWU_ERR_INVALID_ARG);
    expect("ERROR kod 255", fwu_proto_encode_error((fwu_return_e)255, 1u, small, 16u, &length), FWU_OK);
    expect("ERROR kapasite 1", fwu_proto_encode_error(FWU_ERR_BAD_STATE, 1u, small, 1u, &length), FWU_ERR_BUFFER_TOO_SMALL);

    /* parse: NULL */
    expect("parse INFO payload NULL", fwu_proto_parse_info(NULL, 13u, &pinfo), FWU_ERR_INVALID_ARG);
    expect("parse INFO cikis NULL", fwu_proto_parse_info(out, 13u, NULL), FWU_ERR_INVALID_ARG);
    expect("parse ACCEPTED payload NULL", fwu_proto_parse_accepted(NULL, 9u, &pacc), FWU_ERR_INVALID_ARG);
    expect("parse ACCEPTED cikis NULL", fwu_proto_parse_accepted(out, 9u, NULL), FWU_ERR_INVALID_ARG);
    expect("parse u32 payload NULL", fwu_proto_parse_u32(NULL, 4u, &u), FWU_ERR_INVALID_ARG);
    expect("parse u32 cikis NULL", fwu_proto_parse_u32(out, 4u, NULL), FWU_ERR_INVALID_ARG);
    expect("parse DATA payload NULL", fwu_proto_parse_data(NULL, 8u, &pdata), FWU_ERR_INVALID_ARG);
    expect("parse DATA cikis NULL", fwu_proto_parse_data(out, 8u, NULL), FWU_ERR_INVALID_ARG);
    expect("parse RESULT payload NULL", fwu_proto_parse_result(NULL, 1u, &code), FWU_ERR_INVALID_ARG);
    expect("parse RESULT cikis NULL", fwu_proto_parse_result(out, 1u, NULL), FWU_ERR_INVALID_ARG);
    expect("parse ERROR payload NULL", fwu_proto_parse_error(NULL, 2u, &code, &type), FWU_ERR_INVALID_ARG);
    expect("parse ERROR kod NULL", fwu_proto_parse_error(out, 2u, NULL, &type), FWU_ERR_INVALID_ARG);
    expect("parse ERROR istek NULL", fwu_proto_parse_error(out, 2u, &code, NULL), FWU_ERR_INVALID_ARG);
}


/* ------------------------------------------------------------------ PULL_START / PULL_STATUS */

static void test_pull(void)
{
    const fwu_byte_t ip[4] = { 192u, 168u, 1u, 20u };
    const char *text = "/getFwUpdateFile?deviceType=fan&version=12";
    fwu_byte_t target[FWU_PROTO_PULL_TARGET_MAX + 4u];
    fwu_byte_t out[128];
    fwu_byte_t canary[128];
    size_t length = 0u;
    size_t target_length = 0u;
    fwu_proto_pull_start_t start;
    fwu_proto_pull_status_t st;
    fwu_proto_pull_status_t back;

    while(text[target_length] != '\0') {
        target[target_length] = (fwu_byte_t)text[target_length];
        target_length++;
    }

    /* numaralar kalici, yanit = istek | 0x80, 0x94 (VERIFY_RESULT) ile cakismaz */
    check("PULL numaralari: START 0x15, QUERY 0x16, STARTED 0x95, STATUS 0x96",
          FWU_MSG_PULL_START == 0x15u && FWU_MSG_PULL_QUERY == 0x16u && FWU_MSG_PULL_STARTED == 0x95u && FWU_MSG_PULL_STATUS == 0x96u);
    check("PULL yanitlari = istek | 0x80 ve VERIFY_RESULT'a (0x94) carpmaz",
          (FWU_MSG_PULL_START | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_PULL_STARTED &&
          (FWU_MSG_PULL_QUERY | FWU_PROTO_RESPONSE_FLAG) == FWU_MSG_PULL_STATUS &&
          FWU_MSG_PULL_STARTED != FWU_MSG_VERIFY_RESULT && FWU_MSG_PULL_STATUS != FWU_MSG_VERIFY_RESULT);
    check("PULL durumlari 0..4, PULL_STATUS 8 bayt, hedef en cok 96, sabit alan 7",
          FWU_PROTO_PULL_IDLE == 0u && FWU_PROTO_PULL_REQUESTING == 1u && FWU_PROTO_PULL_DOWNLOADING == 2u &&
          FWU_PROTO_PULL_DONE == 3u && FWU_PROTO_PULL_FAILED == 4u && FWU_PROTO_PULL_STATUS_SIZE == 8u &&
          FWU_PROTO_PULL_TARGET_MAX == 96u && FWU_PROTO_PULL_START_FIXED == 7u);
    check("en buyuk PULL_START payload'i cerceveye sigar", (FWU_PROTO_PULL_START_FIXED + FWU_PROTO_PULL_TARGET_MAX) <= FWU_FRAME_MAX_PAYLOAD);

    /* PULL_START: bilinen baytlar (ELLE): ip, port 8080 = 0x1F90, uzunluk, hedef */
    expect("PULL_START encode", fwu_proto_encode_pull_start(ip, 8080u, target, target_length, out, ELEMENTS(out), &length), FWU_OK);
    {
        const unsigned want_head[] = { 192u, 168u, 1u, 20u, 0x1Fu, 0x90u, 42u, (unsigned)'/', (unsigned)'g' };
        check("PULL_START bilinen baytlar (ip, port BE, uzunluk 42, hedef)",
              length == 7u + 42u && bytes_are(out, want_head, 9u) && out[7u + 41u] == (fwu_byte_t)'2');
    }
    expect("PULL_START parse", fwu_proto_parse_pull_start(out, length, &start), FWU_OK);
    check("PULL_START gidis-donus (ip, port, hedef)",
          start.ip[0] == 192u && start.ip[3] == 20u && start.port == 8080u && start.target_length == 42u &&
          start.target == &out[7] && start.target[0] == (fwu_byte_t)'/');

    /* encode: gecersiz girdiler, cikis degismez */
    for(size_t i = 0u; i < ELEMENTS(canary); i++) { canary[i] = 0xEEu; out[i] = 0xEEu; }
    expect("PULL_START ip NULL", fwu_proto_encode_pull_start(NULL, 80u, target, target_length, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    expect("PULL_START port 0", fwu_proto_encode_pull_start(ip, 0u, target, target_length, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    expect("PULL_START hedef NULL", fwu_proto_encode_pull_start(ip, 80u, NULL, 3u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    expect("PULL_START hedef bos", fwu_proto_encode_pull_start(ip, 80u, target, 0u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    {
        const fwu_byte_t no_slash[] = { 'a', 'b' };
        const fwu_byte_t space[] = { '/', 'a', ' ', 'b' };
        const fwu_byte_t crlf[] = { '/', 'a', '\r', '\n' };
        const fwu_byte_t high[] = { '/', 0xC3u };
        expect("PULL_START '/' ile baslamiyor", fwu_proto_encode_pull_start(ip, 80u, no_slash, 2u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
        expect("PULL_START hedefte bosluk", fwu_proto_encode_pull_start(ip, 80u, space, 4u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
        expect("PULL_START hedefte CR/LF (istek satiri enjeksiyonu)", fwu_proto_encode_pull_start(ip, 80u, crlf, 4u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
        expect("PULL_START hedefte ASCII disi bayt", fwu_proto_encode_pull_start(ip, 80u, high, 2u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    }
    {
        int untouched = 1;
        for(size_t i = 0u; i < ELEMENTS(out); i++) {
            if(out[i] != canary[i]) {
                untouched = 0;
            }
        }
        check("gecersiz PULL_START encode'u cikisa yazmaz", untouched);
    }
    {
        fwu_byte_t t2[FWU_PROTO_PULL_TARGET_MAX + 1u];
        for(size_t i = 0u; i < ELEMENTS(t2); i++) { t2[i] = (fwu_byte_t)'a'; }
        t2[0] = (fwu_byte_t)'/';
        expect("PULL_START hedef 96 (sinir) gecer", fwu_proto_encode_pull_start(ip, 80u, t2, 96u, out, ELEMENTS(out), &length), FWU_OK);
        expect("PULL_START hedef 97 reddedilir", fwu_proto_encode_pull_start(ip, 80u, t2, 97u, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    }
    expect("PULL_START kapasite yetmez", fwu_proto_encode_pull_start(ip, 80u, target, target_length, out, 48u, &length), FWU_ERR_BUFFER_TOO_SMALL);

    /* parse: uzunluk ve icerik hatalari */
    expect("PULL_START encode (parse testleri icin)", fwu_proto_encode_pull_start(ip, 8080u, target, target_length, out, ELEMENTS(out), &length), FWU_OK);
    expect("parse PULL_START payload NULL", fwu_proto_parse_pull_start(NULL, 20u, &start), FWU_ERR_INVALID_ARG);
    expect("parse PULL_START cikis NULL", fwu_proto_parse_pull_start(out, length, NULL), FWU_ERR_INVALID_ARG);
    expect("parse PULL_START yalniz sabit alan (hedef yok)", fwu_proto_parse_pull_start(out, 7u, &start), FWU_ERR_BAD_LENGTH);
    expect("parse PULL_START bir bayt eksik", fwu_proto_parse_pull_start(out, length - 1u, &start), FWU_ERR_BAD_LENGTH);
    expect("parse PULL_START bir bayt fazla", fwu_proto_parse_pull_start(out, length + 1u, &start), FWU_ERR_BAD_LENGTH);
    {
        fwu_byte_t bad[128];
        for(size_t i = 0u; i < ELEMENTS(bad); i++) { bad[i] = 'x'; }
        for(size_t i = 0u; i < 7u; i++) { bad[i] = out[i]; }
        bad[6] = 0u;
        expect("parse PULL_START uzunluk alani 0", fwu_proto_parse_pull_start(bad, 8u, &start), FWU_ERR_BAD_LENGTH);
        bad[6] = 97u;
        expect("parse PULL_START uzunluk alani 97", fwu_proto_parse_pull_start(bad, 7u + 97u, &start), FWU_ERR_BAD_LENGTH);
        bad[6] = 3u; bad[4] = 0u; bad[5] = 0u; bad[7] = (fwu_byte_t)'/'; bad[8] = (fwu_byte_t)'a'; bad[9] = (fwu_byte_t)'b';
        expect("parse PULL_START port 0", fwu_proto_parse_pull_start(bad, 10u, &start), FWU_ERR_INVALID_ARG);
        bad[5] = 80u; bad[8] = 0x20u;
        expect("parse PULL_START hedefte bosluk", fwu_proto_parse_pull_start(bad, 10u, &start), FWU_ERR_INVALID_ARG);
        bad[8] = (fwu_byte_t)'a'; bad[7] = (fwu_byte_t)'x';
        expect("parse PULL_START '/' ile baslamiyor", fwu_proto_parse_pull_start(bad, 10u, &start), FWU_ERR_INVALID_ARG);
        bad[7] = (fwu_byte_t)'/';
        expect("parse PULL_START gecerli (port 80)", fwu_proto_parse_pull_start(bad, 10u, &start), FWU_OK);
#if defined(C2000)
        bad[7] = (fwu_byte_t)(0x100u | (unsigned)'/');   /* C28x: ust bit cop, maskelenmeli */
        bad[0] = (fwu_byte_t)(0x300u | 10u);
        expect("parse PULL_START kirli ust bitler maskelenir (C28x)", fwu_proto_parse_pull_start(bad, 10u, &start), FWU_OK);
        check("kirli ust bitli ip temiz okunur", start.ip[0] == 10u);
#endif
    }

    /* PULL_STATUS: bilinen baytlar: durum 2, ofset 0x0001E240 = 123456, hata 0, http 206 */
    st.state = FWU_PROTO_PULL_DOWNLOADING; st.next_offset = 123456u; st.error = 0u; st.http_status = 206u;
    expect("PULL_STATUS encode", fwu_proto_encode_pull_status(&st, out, ELEMENTS(out), &length), FWU_OK);
    {
        const unsigned want[] = { 2u, 0x00u, 0x01u, 0xE2u, 0x40u, 0u, 0x00u, 0xCEu };
        check("PULL_STATUS bilinen baytlar", length == 8u && bytes_are(out, want, 8u));
    }
    expect("PULL_STATUS parse", fwu_proto_parse_pull_status(out, length, &back), FWU_OK);
    check("PULL_STATUS gidis-donus", back.state == 2u && back.next_offset == 123456u && back.error == 0u && back.http_status == 206u);
    st.state = FWU_PROTO_PULL_FAILED; st.error = (fwu_byte_t)FWU_ERR_PULL_HTTP_STATUS; st.http_status = 404u; st.next_offset = 0xFFFFFFFFu;
    expect("PULL_STATUS FAILED encode", fwu_proto_encode_pull_status(&st, out, ELEMENTS(out), &length), FWU_OK);
    expect("PULL_STATUS FAILED parse", fwu_proto_parse_pull_status(out, length, &back), FWU_OK);
    check("FAILED: hata 81, http 404, ofset 0xFFFFFFFF", back.state == 4u && back.error == 81u && back.http_status == 404u && back.next_offset == 0xFFFFFFFFu);
    st.state = 5u;
    expect("PULL_STATUS bilinmeyen durum (encode)", fwu_proto_encode_pull_status(&st, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    expect("PULL_STATUS NULL", fwu_proto_encode_pull_status(NULL, out, ELEMENTS(out), &length), FWU_ERR_INVALID_ARG);
    st.state = 0u;
    expect("PULL_STATUS kapasite 7", fwu_proto_encode_pull_status(&st, out, 7u, &length), FWU_ERR_BUFFER_TOO_SMALL);
    out[0] = 9u;
    expect("parse PULL_STATUS bilinmeyen durum", fwu_proto_parse_pull_status(out, 8u, &back), FWU_ERR_INVALID_ARG);
    expect("parse PULL_STATUS 7 bayt", fwu_proto_parse_pull_status(out, 7u, &back), FWU_ERR_BAD_LENGTH);
    expect("parse PULL_STATUS 9 bayt", fwu_proto_parse_pull_status(out, 9u, &back), FWU_ERR_BAD_LENGTH);
    expect("parse PULL_STATUS payload NULL", fwu_proto_parse_pull_status(NULL, 8u, &back), FWU_ERR_INVALID_ARG);
    expect("parse PULL_STATUS cikis NULL", fwu_proto_parse_pull_status(out, 8u, NULL), FWU_ERR_INVALID_ARG);
}

int main(void)
{
    test_constants();
    test_encode_known();
    test_parse();
    test_errors();
    test_pull();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
