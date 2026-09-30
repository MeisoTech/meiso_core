#include <meiso/profiles/display_ingress/audio.h>
#include "display_ingress_profile_private.h"
#include <string.h>
#define put32 meiso_display_ingress_private_put_u32
#define put64 meiso_display_ingress_private_put_u64
#define get32 meiso_display_ingress_private_get_u32
#define get64 meiso_display_ingress_private_get_u64
#define zero meiso_display_ingress_private_bytes_zero
#define overlap meiso_display_ingress_private_ranges_overlap

/* planes-v12.md §15: DISPLAY_/AUDI_END. */
static const MeisoEndpointId endpoint = {UINT64_C(0x444953504c41595f), UINT64_C(0x415544495f454e44)};

const MeisoEndpointId *meiso_display_audio_endpoint(void) { return &endpoint; }

static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *b) { return (uint16_t)(b[0] * 256U + b[1]); }

static bool cap_valid(const MeisoDisplayAudioCapability *c)
{
    return c && c->sample_rate_hz == MEISO_DISPLAY_AUDIO_SAMPLE_RATE_HZ && c->channels == 1 &&
        c->format == MEISO_DISPLAY_AUDIO_FORMAT_S16LE && c->max_utterance_ms >= MEISO_DISPLAY_AUDIO_MIN_UTTERANCE_MS &&
        c->max_utterance_ms <= MEISO_DISPLAY_AUDIO_MAX_UTTERANCE_MS;
}

bool meiso_display_audio_capability_encode(const MeisoDisplayAudioCapability *c, MeisoMutableBuffer out)
{
    if (!cap_valid(c) || !out.data || out.capacity < 16 || overlap(c, sizeof(*c), out.data, 16)) return false;
    uint8_t b[16] = {0};
    put32(b, c->sample_rate_hz); b[4] = c->channels; b[5] = c->format; put16(b + 6, c->max_utterance_ms);
    memcpy(out.data, b, 16); return true;
}

bool meiso_display_audio_capability_decode(MeisoByteView in, MeisoDisplayAudioCapability *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) || !zero(in.data + 8, 8))
        return false;
    const MeisoDisplayAudioCapability c = {get32(in.data), in.data[4], in.data[5], get16(in.data + 6)};
    if (!cap_valid(&c)) return false;
    *out = c; return true;
}

bool meiso_display_audio_desired_encode(const MeisoDisplayAudioDesired *d, MeisoMutableBuffer out)
{
    if (!d || !out.data || out.capacity < 16 || overlap(d, sizeof(*d), out.data, 16)) return false;
    memset(out.data, 0, 16);
    out.data[0] = d->allowed;
    return true;
}

bool meiso_display_audio_desired_decode(MeisoByteView in, MeisoDisplayAudioDesired *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) || in.data[0] > 1 ||
        !zero(in.data + 1, 15)) return false;
    *out = (MeisoDisplayAudioDesired){in.data[0] == 1};
    return true;
}

bool meiso_display_audio_actual_encode(const MeisoDisplayAudioActual *a, MeisoMutableBuffer out)
{
    if (!a || a->state > MEISO_DISPLAY_AUDIO_FAILED || !out.data || out.capacity < 16 ||
        overlap(a, sizeof(*a), out.data, 16)) return false;
    memset(out.data, 0, 16);
    out.data[0] = a->state;
    return true;
}

bool meiso_display_audio_actual_decode(MeisoByteView in, MeisoDisplayAudioActual *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) ||
        in.data[0] > MEISO_DISPLAY_AUDIO_FAILED || !zero(in.data + 1, 15)) return false;
    *out = (MeisoDisplayAudioActual){in.data[0]};
    return true;
}

enum { ALL_FLAGS = MEISO_DISPLAY_AUDIO_FIRST | MEISO_DISPLAY_AUDIO_LAST | MEISO_DISPLAY_AUDIO_TRUNCATED |
    MEISO_DISPLAY_AUDIO_ABORTED };

static bool record_valid(const MeisoDisplayAudioRecord *r)
{
    const uint8_t f = r->flags, end = MEISO_DISPLAY_AUDIO_TRUNCATED | MEISO_DISPLAY_AUDIO_ABORTED;
    return r->time_ns && r->utterance && !(f & ~ALL_FLAGS) && (!(f & MEISO_DISPLAY_AUDIO_FIRST) == !!r->index) &&
        (!(f & end) || ((f & MEISO_DISPLAY_AUDIO_LAST) && (f & end) != end)) &&
        r->count <= MEISO_DISPLAY_AUDIO_MAX_RECORD_SAMPLES && (r->count || (f & MEISO_DISPLAY_AUDIO_LAST)) &&
        (!r->count || r->samples);
}

bool meiso_display_audio_record_encode(const MeisoDisplayAudioRecord *r, MeisoMutableBuffer out, size_t *written)
{
    if (!r || !record_valid(r) || !out.data || !written) return false;
    const size_t n = meiso_display_audio_record_bytes(r->count);
    if (out.capacity < n || overlap(r, sizeof(*r), out.data, n) || overlap(written, sizeof(*written), out.data, n) ||
        (r->count && overlap(r->samples, 2U * r->count, out.data, n))) return false;
    uint8_t b[MEISO_DISPLAY_AUDIO_RECORD_HEADER_BYTES] = {0};
    put64(b, r->time_ns); put32(b + 8, r->utterance); put32(b + 12, r->index); b[16] = r->flags; put16(b + 18, r->count);
    memcpy(out.data, b, sizeof(b));
    if (r->count) memcpy(out.data + sizeof(b), r->samples, 2U * r->count);
    *written = n; return true;
}

bool meiso_display_audio_record_decode(MeisoByteView in, MeisoDisplayAudioRecord *out)
{
    if (!in.data || in.size < MEISO_DISPLAY_AUDIO_RECORD_HEADER_BYTES || !out ||
        overlap(in.data, in.size, out, sizeof(*out)) || in.data[17] || !zero(in.data + 20, 4)) return false;
    const uint8_t *b = in.data;
    const MeisoDisplayAudioRecord r = {get64(b), get32(b + 8), get32(b + 12), b[16], get16(b + 18),
        b + MEISO_DISPLAY_AUDIO_RECORD_HEADER_BYTES};
    if (in.size != meiso_display_audio_record_bytes(r.count) || !record_valid(&r)) return false;
    *out = r; return true;
}
