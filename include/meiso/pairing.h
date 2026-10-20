#ifndef MEISO_PAIRING_H
#define MEISO_PAIRING_H

/* Pairing v0 (docs/pairing-v0.md): how an Edge and a Host first learn each other's PeerId and how to reach each
 * other, outside and before a Session. The Host shows a QR text; the Edge reads it, joins that Host's network and
 * sends one request on the pairing port; the Host answers once. Codecs only: no sockets, no storage, no clock.
 * PeerIds are Session Core's: what pairing records is what OPEN/ACCEPT later expect. */

#include <meiso/core/session.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_PAIRING_VERSION 1U
#define MEISO_PAIRING_DEFAULT_PORT 4444U
#define MEISO_PAIRING_WINDOW_MAX_S 120U
#define MEISO_PAIRING_EDGE_HOSTS_MAX 8U

#define MEISO_PAIRING_PEER_SIZE 16U   /* on the wire: high then low, big-endian, as in Session Core */
#define MEISO_PAIRING_CODE_SIZE 16U   /* 80 bits as RFC 4648 base32 letters A-Z 2-7 */
#define MEISO_PAIRING_NAME_MAX 32U    /* bytes, no terminator */
#define MEISO_PAIRING_SSID_MAX 32U
#define MEISO_PAIRING_PASSPHRASE_MIN 8U
#define MEISO_PAIRING_PASSPHRASE_MAX 63U
#define MEISO_PAIRING_QR_MAX 300U     /* characters of the QR text */

#define MEISO_PAIRING_REQUEST_SIZE 72U
#define MEISO_PAIRING_OP_PAIR 1U      /* request byte 4. MPR1 then another op: REFUSED_VERSION at once, not after 72 bytes */
#define MEISO_PAIRING_REPLY_SIZE 24U

typedef struct MeisoPairingRequest {
    uint8_t code[MEISO_PAIRING_CODE_SIZE];
    MeisoPeerId edge_peer;
    uint8_t edge_name[MEISO_PAIRING_NAME_MAX]; /* UTF-8, edge_name_size bytes used */
    uint8_t edge_name_size;
} MeisoPairingRequest;

typedef enum MeisoPairingReplyKind {
    MEISO_PAIRING_REPLY_PAIRED = 2,
    MEISO_PAIRING_REPLY_REFUSED = 3
} MeisoPairingReplyKind;

typedef enum MeisoPairingRefusal {
    MEISO_PAIRING_REFUSED_NONE = 0,    /* only with PAIRED */
    MEISO_PAIRING_REFUSED_CODE = 1,    /* not this window's code, or already used */
    MEISO_PAIRING_REFUSED_EXPIRED = 2, /* no window open */
    MEISO_PAIRING_REFUSED_BUSY = 3,    /* another pairing exchange in progress (a Session is not "busy") */
    MEISO_PAIRING_REFUSED_VERSION = 4
} MeisoPairingRefusal;

typedef struct MeisoPairingReply {
    uint8_t kind;    /* MeisoPairingReplyKind */
    uint8_t refusal; /* MeisoPairingRefusal: NONE with PAIRED, 1..4 with REFUSED */
    MeisoPeerId host_peer; /* PAIRED: the Host's PeerId; REFUSED: zero */
} MeisoPairingReply;

/* The QR text's fields. Strings are NUL-terminated here; addresses are IPv4 in network order, on one /24. */
typedef struct MeisoPairingQr {
    char host_name[MEISO_PAIRING_NAME_MAX + 1];      /* printable ASCII, no leading or trailing space */
    MeisoPeerId host_peer;
    char ssid[MEISO_PAIRING_SSID_MAX + 1];
    char passphrase[MEISO_PAIRING_PASSPHRASE_MAX + 1]; /* WPA2-PSK, printable ASCII */
    uint8_t channel;                                   /* 2.4 GHz, 1..13 */
    uint8_t host_ip[4];
    uint8_t edge_ip[4]; /* outside the Host's DHCP pool and held by no other paired Edge */
    uint16_t port;
    uint8_t code[MEISO_PAIRING_CODE_SIZE];
    uint16_t expires_s; /* seconds left in the window when shown, 1..120 */
} MeisoPairingQr;

bool meiso_pairing_code_valid(const uint8_t code[MEISO_PAIRING_CODE_SIZE]);
/* Compares all 16 bytes whatever differs first. */
bool meiso_pairing_code_equal(const uint8_t a[MEISO_PAIRING_CODE_SIZE], const uint8_t b[MEISO_PAIRING_CODE_SIZE]);
bool meiso_pairing_host_name_valid(const char *name);
bool meiso_pairing_edge_name_valid(const uint8_t *name, size_t size);

bool meiso_pairing_request_encode(const MeisoPairingRequest *request, uint8_t *out, size_t capacity);
/* False for anything but a well-formed v0 request. The Host reads the first 5 bytes before it waits for 72
 * (docs/pairing-v0.md §5.3): an unknown op is answered from those. */
bool meiso_pairing_request_decode(const uint8_t *in, size_t size, MeisoPairingRequest *out);
bool meiso_pairing_reply_encode(const MeisoPairingReply *reply, uint8_t *out, size_t capacity);
bool meiso_pairing_reply_decode(const uint8_t *in, size_t size, MeisoPairingReply *out);

/* The QR text into out (NUL-terminated); its length, or 0 when a field is invalid or it would pass
 * MEISO_PAIRING_QR_MAX characters or capacity - 1. */
size_t meiso_pairing_qr_encode(const MeisoPairingQr *qr, char *out, size_t capacity);
/* Fields in any order, each once; fields this version does not know (f= for the Host's key, later) are skipped. */
bool meiso_pairing_qr_decode(const char *text, size_t size, MeisoPairingQr *out);

#ifdef __cplusplus
}
#endif

#endif
