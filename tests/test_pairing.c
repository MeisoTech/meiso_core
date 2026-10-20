/* Pairing v0 (docs/pairing-v0.md): golden bytes of the two messages and the QR text, and what each refuses.
 * All values are made up: no real network, passphrase or PeerId. */
#include <meiso/pairing.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int nibble(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static size_t unhex(const char *h, uint8_t *out)
{
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        if (nibble(h[0]) < 0 || nibble(h[1]) < 0) return 0;
        out[n++] = (uint8_t)(nibble(h[0]) << 4 | nibble(h[1]));
    }
    return n;
}

#define SET(field, literal) memcpy(field, literal, sizeof literal) /* with its NUL */

static const MeisoPeerId edge_peer = {0x0102030405060708u, 0x090a0b0c0d0e0f10u};
static const MeisoPeerId host_peer = {0xa0a1a2a3a4a5a6a7u, 0xa8a9aaabacadaeafu};

/* Field by field: the structs have padding. */
static bool request_equal(const MeisoPairingRequest *a, const MeisoPairingRequest *b)
{
    return !memcmp(a->code, b->code, sizeof a->code) && meiso_peer_id_equal(a->edge_peer, b->edge_peer) &&
           a->edge_name_size == b->edge_name_size && !memcmp(a->edge_name, b->edge_name, a->edge_name_size);
}

static bool reply_equal(const MeisoPairingReply *a, const MeisoPairingReply *b)
{
    return a->kind == b->kind && a->refusal == b->refusal && a->host_peer.high == b->host_peer.high &&
           a->host_peer.low == b->host_peer.low;
}

static bool qr_equal(const MeisoPairingQr *a, const MeisoPairingQr *b)
{
    return !strcmp(a->host_name, b->host_name) && meiso_peer_id_equal(a->host_peer, b->host_peer) &&
           !strcmp(a->ssid, b->ssid) && !strcmp(a->passphrase, b->passphrase) && a->channel == b->channel &&
           !memcmp(a->host_ip, b->host_ip, 4) && !memcmp(a->edge_ip, b->edge_ip, 4) && a->port == b->port &&
           !memcmp(a->code, b->code, sizeof a->code) && a->expires_s == b->expires_s;
}

static const char request_hex[] =
    "4d50523101"                                /* MPR1, op 1 = pair */
    "4142434445464748494a4b4c4d4e4f50"          /* code ABCDEFGHIJKLMNOP */
    "0102030405060708090a0b0c0d0e0f10"          /* Edge PeerId, high then low, big-endian */
    "476c61737365732d41" "0000000000000000000000000000000000000000000000" /* "Glasses-A", zero padded to 32 */
    "000000";
static const char paired_hex[] = "4d505231" "0200" "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf" "0000";
static const char busy_hex[] = "4d505231" "0303" "00000000000000000000000000000000" "0000";

static MeisoPairingRequest request(void)
{
    MeisoPairingRequest r;
    memset(&r, 0, sizeof r);
    memcpy(r.code, "ABCDEFGHIJKLMNOP", 16);
    r.edge_peer = edge_peer;
    memcpy(r.edge_name, "Glasses-A", 9); r.edge_name_size = 9;
    return r;
}

static int requests(void)
{
    uint8_t want[MEISO_PAIRING_REQUEST_SIZE], got[MEISO_PAIRING_REQUEST_SIZE], b[MEISO_PAIRING_REQUEST_SIZE];
    CHECK(unhex(request_hex, want) == MEISO_PAIRING_REQUEST_SIZE);
    MeisoPairingRequest r = request(), back;
    CHECK(meiso_pairing_request_encode(&r, got, sizeof got) && !memcmp(got, want, sizeof want));
    CHECK(meiso_pairing_request_decode(want, sizeof want, &back) && request_equal(&back, &r));
    CHECK(!meiso_pairing_request_encode(&r, got, sizeof got - 1));
    CHECK(!meiso_pairing_request_decode(want, sizeof want - 1, &back));
    /* the Edge's name is UTF-8: drawn by the Host, which has the fonts */
    static const uint8_t utf8[] = {0xe7, 0x9c, 0xbc, 0xe9, 0x95, 0x9c, 'A'};
    MeisoPairingRequest u = r;
    memcpy(u.edge_name, utf8, sizeof utf8); u.edge_name_size = sizeof utf8;
    CHECK(meiso_pairing_request_encode(&u, got, sizeof got) && meiso_pairing_request_decode(got, sizeof got, &back) &&
          back.edge_name_size == sizeof utf8 && !memcmp(back.edge_name, utf8, sizeof utf8));
    /* each byte the decoder must refuse */
    static const struct { size_t at; uint8_t value; } bad[] = {
        {0, 'm'}, {3, '2'}, {4, 2}, {5, '1'}, {5, 'a'}, {20, '8'}, {69, 1}, {71, 1},
        {37, 0x07}, {37, 0x80}, {38, 0}, /* a control, a lone continuation, "G\0asses-A": only zero after the end */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        memcpy(b, want, sizeof b); b[bad[i].at] = bad[i].value;
        CHECK(!meiso_pairing_request_decode(b, sizeof b, &back));
    }
    memcpy(b, want, sizeof b); memset(b + 21, 0, 16); CHECK(!meiso_pairing_request_decode(b, sizeof b, &back));
    memcpy(b, want, sizeof b); memset(b + 37, 0, 32); CHECK(!meiso_pairing_request_decode(b, sizeof b, &back));
    static const uint8_t not_utf8[][3] = {{0xc0, 0x80, 0}, {0xed, 0xa0, 0x80}, {0xc2, 0x85, 0}}; /* overlong, surrogate, C1 */
    for (size_t i = 0; i < 3; ++i) {
        MeisoPairingRequest x = r;
        memcpy(x.edge_name, not_utf8[i], 3); x.edge_name_size = not_utf8[i][2] ? 3 : 2;
        CHECK(!meiso_pairing_request_encode(&x, got, sizeof got));
    }
    return 0;
}

static int replies(void)
{
    uint8_t want[MEISO_PAIRING_REPLY_SIZE], got[MEISO_PAIRING_REPLY_SIZE], b[MEISO_PAIRING_REPLY_SIZE];
    MeisoPairingReply paired = {MEISO_PAIRING_REPLY_PAIRED, MEISO_PAIRING_REFUSED_NONE, {0, 0}}, back;
    paired.host_peer = host_peer;
    CHECK(unhex(paired_hex, want) == MEISO_PAIRING_REPLY_SIZE);
    CHECK(meiso_pairing_reply_encode(&paired, got, sizeof got) && !memcmp(got, want, sizeof want));
    CHECK(meiso_pairing_reply_decode(want, sizeof want, &back) && reply_equal(&back, &paired));
    const MeisoPairingReply busy = {MEISO_PAIRING_REPLY_REFUSED, MEISO_PAIRING_REFUSED_BUSY, {0, 0}};
    CHECK(unhex(busy_hex, want) == MEISO_PAIRING_REPLY_SIZE);
    CHECK(meiso_pairing_reply_encode(&busy, got, sizeof got) && !memcmp(got, want, sizeof want));
    CHECK(meiso_pairing_reply_decode(want, sizeof want, &back) && reply_equal(&back, &busy));
    MeisoPairingReply x = paired; x.refusal = MEISO_PAIRING_REFUSED_CODE; CHECK(!meiso_pairing_reply_encode(&x, got, sizeof got));
    x = paired; x.host_peer.high = x.host_peer.low = 0; CHECK(!meiso_pairing_reply_encode(&x, got, sizeof got));
    x = busy; x.refusal = 0; CHECK(!meiso_pairing_reply_encode(&x, got, sizeof got));
    x = busy; x.refusal = 5; CHECK(!meiso_pairing_reply_encode(&x, got, sizeof got));
    x = busy; x.host_peer.low = 1; CHECK(!meiso_pairing_reply_encode(&x, got, sizeof got));
    x = busy; x.kind = 1; CHECK(!meiso_pairing_reply_encode(&x, got, sizeof got));
    CHECK(unhex(paired_hex, want) == MEISO_PAIRING_REPLY_SIZE);
    static const size_t at[] = {0, 4, 5, 22, 23};
    for (size_t i = 0; i < 5; ++i) { memcpy(b, want, sizeof b); b[at[i]] ^= 0x40; CHECK(!meiso_pairing_reply_decode(b, sizeof b, &back)); }
    CHECK(!meiso_pairing_reply_decode(want, sizeof want + 0 - 1, &back));
    return 0;
}

static int names_and_codes(void)
{
    CHECK(meiso_pairing_host_name_valid("Host-B") && meiso_pairing_host_name_valid("Desk PC"));
    CHECK(!meiso_pairing_host_name_valid("") && !meiso_pairing_host_name_valid(" Desk") && !meiso_pairing_host_name_valid("Desk "));
    CHECK(!meiso_pairing_host_name_valid("\xe7\x9c\xbc")); /* v0: printable ASCII only, the glasses draw it locally */
    CHECK(meiso_pairing_host_name_valid("0123456789abcdef0123456789abcdef"));
    CHECK(!meiso_pairing_host_name_valid("0123456789abcdef0123456789abcdefX"));
    uint8_t a[16], c[16], lower[16], one[16];
    memcpy(a, "ABCDEFGHIJKLMNOP", 16); memcpy(c, "ABCDEFGHIJKLMNOQ", 16);
    memcpy(lower, "abcdefghijklmnop", 16); memcpy(one, "ABCDEFGHIJKLMNO1", 16);
    CHECK(meiso_pairing_code_valid(a) && meiso_pairing_code_equal(a, a) && !meiso_pairing_code_equal(a, c));
    CHECK(meiso_pairing_code_equal(c, c) && !meiso_pairing_code_equal(c, a)); /* differing in the last byte only */
    CHECK(!meiso_pairing_code_valid(lower) && !meiso_pairing_code_valid(one));
    return 0;
}

static const char qr_text[] = "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;"
                              "c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;";

static MeisoPairingQr qr(void)
{
    MeisoPairingQr q;
    memset(&q, 0, sizeof q);
    SET(q.host_name, "Host-A"); SET(q.ssid, "Meiso-Example"); SET(q.passphrase, "pa;ss:word");
    q.host_peer = host_peer;
    q.channel = 1; q.port = 4444; q.expires_s = 120;
    q.host_ip[0] = q.edge_ip[0] = 10; q.host_ip[2] = q.edge_ip[2] = 7; q.host_ip[3] = 1; q.edge_ip[3] = 34;
    memcpy(q.code, "ABCDEFGHIJKLMNOP", 16);
    return q;
}

static bool decodes(const char *text)
{
    MeisoPairingQr out;
    return meiso_pairing_qr_decode(text, strlen(text), &out);
}

static int qr_texts(void)
{
    char text[MEISO_PAIRING_QR_MAX + 1];
    MeisoPairingQr q = qr(), back;
    CHECK(meiso_pairing_qr_encode(&q, text, sizeof text) == strlen(qr_text) && !strcmp(text, qr_text));
    CHECK(meiso_pairing_qr_decode(qr_text, strlen(qr_text), &back) && qr_equal(&back, &q));
    CHECK(!meiso_pairing_qr_encode(&q, text, strlen(qr_text))); /* no room for the terminator */
    /* any order; a later version's field (the Host's key, f=) is skipped */
    CHECK(decodes("MEISO-PAIR:1;x=120;k=ABCDEFGHIJKLMNOP;f=00ff;o=4444;e=10.0.7.34;a=10.0.7.1;c=1;p=pa\\;ss\\:word;"
                  "s=Meiso-Example;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;n=Host-A;;"));
    static const char *const bad[] = {
        "MEISO-PAIR:2;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;n=Host-B;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss:word;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=14;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=01;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.8.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.1;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=121;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.34;o=0;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=A0A1A2A3A4A5A6A7A8A9AAABACADAEAF;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=abcdefghijklmnop;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=short;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n= Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;Z",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;c=1;a=10.0.7.1;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;p=pass\\;word\\;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7;e=10.0.7.34;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
        "MEISO-PAIR:1;n=Host-A;h=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf;s=Meiso-Example;p=pa\\;ss\\:word;c=1;a=10.0.7.1;e=10.0.7.255;o=4444;k=ABCDEFGHIJKLMNOP;x=120;;",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        if (decodes(bad[i])) { fprintf(stderr, "bad[%zu] decoded\n", i); return 1; }
    }
    /* a NUL inside the text; and the text's length bound */
    char with_nul[sizeof qr_text];
    memcpy(with_nul, qr_text, sizeof qr_text); with_nul[17] = 0; /* inside n= */
    CHECK(!meiso_pairing_qr_decode(with_nul, sizeof qr_text - 1, &back));
    char longer[MEISO_PAIRING_QR_MAX + 2];
    memset(longer, 'a', sizeof longer);
    CHECK(!meiso_pairing_qr_decode(longer, sizeof longer, &back));
    /* encode refuses what decode would */
    MeisoPairingQr x = q; SET(x.passphrase, "short"); CHECK(!meiso_pairing_qr_encode(&x, text, sizeof text));
    x = q; x.edge_ip[2] = 8; CHECK(!meiso_pairing_qr_encode(&x, text, sizeof text));
    x = q; SET(x.host_name, "Host-A "); CHECK(!meiso_pairing_qr_encode(&x, text, sizeof text));
    /* the longest fields with everything escaped do not fit in 300: the encoder says so, it does not cut */
    x = q; memset(x.ssid, ';', 32); x.ssid[32] = 0; memset(x.passphrase, ';', 63); x.passphrase[63] = 0;
    CHECK(!meiso_pairing_qr_encode(&x, text, sizeof text));
    return 0;
}

int main(void)
{
    if (requests() || replies() || names_and_codes() || qr_texts()) return 1;
    puts("pairing v0: ok");
    return 0;
}
