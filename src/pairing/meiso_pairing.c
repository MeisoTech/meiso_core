/* Pairing v0 codecs (docs/pairing-v0.md, include/meiso/pairing.h). */
#include <meiso/pairing.h>

#include <stdio.h>
#include <string.h>

static const uint8_t magic[4] = {'M', 'P', 'R', '1'};

static bool zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (p[i]) return false;
    return true;
}

/* PeerId bytes as Session Core puts them on its wire. */
static void put_peer(uint8_t *b, MeisoPeerId v)
{
    for (int i = 0; i < 8; ++i) {
        b[i] = (uint8_t)(v.high >> (56 - 8 * i));
        b[8 + i] = (uint8_t)(v.low >> (56 - 8 * i));
    }
}

static MeisoPeerId get_peer(const uint8_t *b)
{
    MeisoPeerId v = {0, 0};
    for (int i = 0; i < 8; ++i) {
        v.high = v.high << 8 | b[i];
        v.low = v.low << 8 | b[8 + i];
    }
    return v;
}

static bool peer_zero(MeisoPeerId v) { return !v.high && !v.low; }

static bool base32(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'); }

bool meiso_pairing_code_valid(const uint8_t code[MEISO_PAIRING_CODE_SIZE])
{
    if (!code) return false;
    for (size_t i = 0; i < MEISO_PAIRING_CODE_SIZE; ++i) if (!base32(code[i])) return false;
    return true;
}

bool meiso_pairing_code_equal(const uint8_t a[MEISO_PAIRING_CODE_SIZE], const uint8_t b[MEISO_PAIRING_CODE_SIZE])
{
    if (!a || !b) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < MEISO_PAIRING_CODE_SIZE; ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* strnlen is POSIX, not C11. */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) ++n;
    return n;
}

static bool printable(const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return false;
    return true;
}

bool meiso_pairing_host_name_valid(const char *name)
{
    if (!name) return false;
    const size_t n = bounded_len(name, MEISO_PAIRING_NAME_MAX + 1);
    return n >= 1 && n <= MEISO_PAIRING_NAME_MAX && printable(name, n) && name[0] != ' ' && name[n - 1] != ' ';
}

/* Well-formed UTF-8 (no overlong forms, no surrogates, at most U+10FFFF), no C0 or C1 controls, no DEL. */
bool meiso_pairing_edge_name_valid(const uint8_t *s, size_t n)
{
    if (!s || n < 1 || n > MEISO_PAIRING_NAME_MAX) return false;
    for (size_t i = 0; i < n;) {
        const uint8_t c = s[i];
        uint32_t cp; size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if (c >= 0xc2 && c <= 0xdf) { cp = c & 0x1fu; len = 2; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 0x0fu; len = 3; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 0x07u; len = 4; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            if ((s[i + k] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3fu);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp < 0x20 || (cp >= 0x7f && cp <= 0x9f)) return false;
        i += len;
    }
    return true;
}

bool meiso_pairing_request_encode(const MeisoPairingRequest *r, uint8_t *out, size_t capacity)
{
    if (!r || !out || capacity < MEISO_PAIRING_REQUEST_SIZE || !meiso_pairing_code_valid(r->code) ||
        !meiso_peer_id_valid(r->edge_peer) || !meiso_pairing_edge_name_valid(r->edge_name, r->edge_name_size))
        return false;
    uint8_t b[MEISO_PAIRING_REQUEST_SIZE] = {0};
    memcpy(b, magic, 4); b[4] = MEISO_PAIRING_OP_PAIR;
    memcpy(b + 5, r->code, MEISO_PAIRING_CODE_SIZE);
    put_peer(b + 21, r->edge_peer);
    memcpy(b + 37, r->edge_name, r->edge_name_size);
    memcpy(out, b, sizeof b); return true;
}

bool meiso_pairing_request_decode(const uint8_t *in, size_t size, MeisoPairingRequest *out)
{
    if (!in || size != MEISO_PAIRING_REQUEST_SIZE || !out || memcmp(in, magic, 4) || in[4] != MEISO_PAIRING_OP_PAIR ||
        !zero(in + 69, 3)) return false;
    MeisoPairingRequest r;
    memset(&r, 0, sizeof r);
    memcpy(r.code, in + 5, MEISO_PAIRING_CODE_SIZE);
    r.edge_peer = get_peer(in + 21);
    size_t n = 0;
    while (n < MEISO_PAIRING_NAME_MAX && in[37 + n]) ++n;
    if (!zero(in + 37 + n, MEISO_PAIRING_NAME_MAX - n)) return false; /* zero padding only after the name */
    memcpy(r.edge_name, in + 37, n); r.edge_name_size = (uint8_t)n;
    if (!meiso_pairing_code_valid(r.code) || !meiso_peer_id_valid(r.edge_peer) ||
        !meiso_pairing_edge_name_valid(r.edge_name, r.edge_name_size)) return false;
    *out = r; return true;
}

static bool reply_valid(const MeisoPairingReply *r)
{
    if (r->kind == MEISO_PAIRING_REPLY_PAIRED)
        return r->refusal == MEISO_PAIRING_REFUSED_NONE && meiso_peer_id_valid(r->host_peer);
    return r->kind == MEISO_PAIRING_REPLY_REFUSED && r->refusal >= MEISO_PAIRING_REFUSED_CODE &&
           r->refusal <= MEISO_PAIRING_REFUSED_VERSION && peer_zero(r->host_peer);
}

bool meiso_pairing_reply_encode(const MeisoPairingReply *r, uint8_t *out, size_t capacity)
{
    if (!r || !out || capacity < MEISO_PAIRING_REPLY_SIZE || !reply_valid(r)) return false;
    uint8_t b[MEISO_PAIRING_REPLY_SIZE] = {0};
    memcpy(b, magic, 4); b[4] = r->kind; b[5] = r->refusal;
    put_peer(b + 6, r->host_peer);
    memcpy(out, b, sizeof b); return true;
}

bool meiso_pairing_reply_decode(const uint8_t *in, size_t size, MeisoPairingReply *out)
{
    if (!in || size != MEISO_PAIRING_REPLY_SIZE || !out || memcmp(in, magic, 4) || !zero(in + 22, 2)) return false;
    MeisoPairingReply r;
    memset(&r, 0, sizeof r);
    r.kind = in[4]; r.refusal = in[5]; r.host_peer = get_peer(in + 6);
    if (!reply_valid(&r)) return false;
    *out = r; return true;
}

/* ---- QR text: MEISO-PAIR:1;n=..;h=..;s=..;p=..;c=..;a=..;e=..;o=..;k=..;x=..;; ---------------------------------
 * As the Wi-Fi QR ("WIFI:"): a value escapes \ ; , : " with a backslash. */

static const char prefix[] = "MEISO-PAIR:1;";

typedef struct { char *p; size_t n, cap; bool ok; } Out;

static void put(Out *o, const char *s, size_t n)
{
    if (!o->ok || o->n + n >= o->cap) { o->ok = false; return; }
    memcpy(o->p + o->n, s, n); o->n += n;
}

static void put_escaped(Out *o, const char *s)
{
    for (; *s; ++s) {
        if (strchr("\\;,:\"", *s)) put(o, "\\", 1);
        put(o, s, 1);
    }
}

static void put_field(Out *o, char key, const char *value, bool escape)
{
    const char head[2] = {key, '='};
    put(o, head, 2);
    if (escape) put_escaped(o, value); else put(o, value, strlen(value));
    put(o, ";", 1);
}

static void hex(const uint8_t *b, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[2 * i] = digits[b[i] >> 4]; out[2 * i + 1] = digits[b[i] & 15]; }
    out[2 * n] = 0;
}

static bool ssid_valid(const char *s)
{
    const size_t n = bounded_len(s, MEISO_PAIRING_SSID_MAX + 1);
    return n >= 1 && n <= MEISO_PAIRING_SSID_MAX && printable(s, n);
}

static bool passphrase_valid(const char *s)
{
    const size_t n = bounded_len(s, MEISO_PAIRING_PASSPHRASE_MAX + 1);
    return n >= MEISO_PAIRING_PASSPHRASE_MIN && n <= MEISO_PAIRING_PASSPHRASE_MAX && printable(s, n);
}

static bool ip_usable(const uint8_t a[4]) { return a[0] && a[0] != 127 && a[0] < 224 && a[3] && a[3] != 255; }

static bool qr_valid(const MeisoPairingQr *q)
{
    return meiso_pairing_host_name_valid(q->host_name) && meiso_peer_id_valid(q->host_peer) &&
           ssid_valid(q->ssid) && passphrase_valid(q->passphrase) && q->channel >= 1 && q->channel <= 13 &&
           ip_usable(q->host_ip) && ip_usable(q->edge_ip) && !memcmp(q->host_ip, q->edge_ip, 3) &&
           q->host_ip[3] != q->edge_ip[3] && q->port && meiso_pairing_code_valid(q->code) && q->expires_s >= 1 &&
           q->expires_s <= MEISO_PAIRING_WINDOW_MAX_S;
}

size_t meiso_pairing_qr_encode(const MeisoPairingQr *q, char *out, size_t capacity)
{
    if (!q || !out || !capacity || !qr_valid(q)) return 0;
    char peer[2 * MEISO_PAIRING_PEER_SIZE + 1], code[MEISO_PAIRING_CODE_SIZE + 1], num[24];
    uint8_t peer_bytes[MEISO_PAIRING_PEER_SIZE];
    const size_t cap = capacity < MEISO_PAIRING_QR_MAX + 1 ? capacity : MEISO_PAIRING_QR_MAX + 1;
    Out o = {out, 0, cap, true};
    put(&o, prefix, sizeof prefix - 1);
    put_field(&o, 'n', q->host_name, true);
    put_peer(peer_bytes, q->host_peer); hex(peer_bytes, MEISO_PAIRING_PEER_SIZE, peer); put_field(&o, 'h', peer, false);
    put_field(&o, 's', q->ssid, true);
    put_field(&o, 'p', q->passphrase, true);
    snprintf(num, sizeof num, "%u", (unsigned)q->channel); put_field(&o, 'c', num, false);
    snprintf(num, sizeof num, "%u.%u.%u.%u", (unsigned)q->host_ip[0], (unsigned)q->host_ip[1], (unsigned)q->host_ip[2],
             (unsigned)q->host_ip[3]);
    put_field(&o, 'a', num, false);
    snprintf(num, sizeof num, "%u.%u.%u.%u", (unsigned)q->edge_ip[0], (unsigned)q->edge_ip[1], (unsigned)q->edge_ip[2],
             (unsigned)q->edge_ip[3]);
    put_field(&o, 'e', num, false);
    snprintf(num, sizeof num, "%u", (unsigned)q->port); put_field(&o, 'o', num, false);
    memcpy(code, q->code, MEISO_PAIRING_CODE_SIZE); code[MEISO_PAIRING_CODE_SIZE] = 0; put_field(&o, 'k', code, false);
    snprintf(num, sizeof num, "%u", (unsigned)q->expires_s); put_field(&o, 'x', num, false);
    put(&o, ";", 1);
    if (!o.ok) { out[0] = 0; return 0; }
    out[o.n] = 0; return o.n;
}

static bool unsigned_value(const char *s, size_t n, unsigned max, unsigned *out)
{
    if (!n || n > 5 || (n > 1 && s[0] == '0')) return false;
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    if (v > max) return false;
    *out = v; return true;
}

static bool ip_value(const char *s, size_t n, uint8_t ip[4])
{
    size_t start = 0;
    for (int part = 0; part < 4; ++part) {
        size_t end = start;
        while (end < n && s[end] != '.') ++end;
        unsigned v;
        if (!unsigned_value(s + start, end - start, 255, &v) || (part < 3 ? end >= n : end != n)) return false;
        ip[part] = (uint8_t)v; start = end + 1;
    }
    return true;
}

static bool hex_value(const char *s, size_t n, uint8_t *out, size_t bytes)
{
    if (n != 2 * bytes) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        out[i / 2] = (uint8_t)(i % 2 ? (out[i / 2] | d) : d << 4);
    }
    return true;
}

bool meiso_pairing_qr_decode(const char *text, size_t size, MeisoPairingQr *out)
{
    if (!text || !out || size > MEISO_PAIRING_QR_MAX || size < sizeof prefix + 1 ||
        memcmp(text, prefix, sizeof prefix - 1) || memchr(text, 0, size)) return false; /* no NUL anywhere */
    MeisoPairingQr q;
    memset(&q, 0, sizeof q);
    unsigned seen = 0, v;
    uint8_t peer[MEISO_PAIRING_PEER_SIZE];
    size_t i = sizeof prefix - 1;
    while (i < size - 1) { /* each field ends at an unescaped ';'; the text ends with an empty one */
        if (i + 2 > size || text[i + 1] != '=') return false;
        const char key = text[i];
        char value[2 * MEISO_PAIRING_PASSPHRASE_MAX + 1];
        size_t n = 0;
        for (i += 2; i < size && text[i] != ';'; ++i) {
            if (text[i] == '\\') { if (++i >= size) return false; }
            else if (strchr(",:\"", text[i])) return false; /* these come escaped */
            if (n + 1 >= sizeof value) return false;
            value[n++] = text[i];
        }
        if (i >= size) return false;
        value[n] = 0; ++i;
        const char *keys = "nhspcaeokx", *at = strchr(keys, key);
        if (!at) continue; /* a field of a later version */
        const unsigned bit = 1u << (unsigned)(at - keys);
        if (seen & bit) return false;
        seen |= bit;
        switch (key) {
        case 'n': if (n > MEISO_PAIRING_NAME_MAX) return false; memcpy(q.host_name, value, n + 1); break;
        case 'h': if (!hex_value(value, n, peer, MEISO_PAIRING_PEER_SIZE)) return false; q.host_peer = get_peer(peer); break;
        case 's': if (n > MEISO_PAIRING_SSID_MAX) return false; memcpy(q.ssid, value, n + 1); break;
        case 'p': if (n > MEISO_PAIRING_PASSPHRASE_MAX) return false; memcpy(q.passphrase, value, n + 1); break;
        case 'c': if (!unsigned_value(value, n, 13, &v)) return false; q.channel = (uint8_t)v; break;
        case 'a': if (!ip_value(value, n, q.host_ip)) return false; break;
        case 'e': if (!ip_value(value, n, q.edge_ip)) return false; break;
        case 'o': if (!unsigned_value(value, n, 65535, &v)) return false; q.port = (uint16_t)v; break;
        case 'k': if (n != MEISO_PAIRING_CODE_SIZE) return false; memcpy(q.code, value, n); break;
        case 'x': if (!unsigned_value(value, n, MEISO_PAIRING_WINDOW_MAX_S, &v)) return false; q.expires_s = (uint16_t)v; break;
        }
    }
    /* the loop stops before the closing empty field, which is one unescaped ';' (an escaped one ended a value) */
    if (i != size - 1 || text[i] != ';' || seen != (1u << 10) - 1 || !qr_valid(&q)) return false;
    *out = q; return true;
}
