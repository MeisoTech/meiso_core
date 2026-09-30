#ifndef MEISO_DISPLAY_INGRESS_AUDIO_H
#define MEISO_DISPLAY_INGRESS_AUDIO_H
#include <meiso/core/resource.h>
#include <meiso/runtime/runtime.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Display Ingress v12 audio endpoint (planes-v12.md §15, meiso_host #93): push-to-talk. The Edge
 * records its microphone only while the SYSTEM key (§11) is held, and sends each hold as one
 * utterance of 16 kHz mono PCM, timed on the Edge clock (clock epoch 4). The Host can forbid
 * recording, never start it. Headers big-endian; samples little-endian (S16LE). */
enum {
    MEISO_DISPLAY_AUDIO_CAPABILITY_BYTES = 16,
    MEISO_DISPLAY_AUDIO_DESIRED_BYTES = 16,
    MEISO_DISPLAY_AUDIO_ACTUAL_BYTES = 16,
    MEISO_DISPLAY_AUDIO_RECORD_HEADER_BYTES = 24,
    MEISO_DISPLAY_AUDIO_MAX_RECORD_SAMPLES = 960,   /* 60 ms at 16 kHz */
    MEISO_DISPLAY_AUDIO_MAX_RECORD_BYTES = 24 + 2 * 960,
    MEISO_DISPLAY_AUDIO_SAMPLE_RATE_HZ = 16000,
    MEISO_DISPLAY_AUDIO_MIN_UTTERANCE_MS = 1000,
    MEISO_DISPLAY_AUDIO_MAX_UTTERANCE_MS = 60000,
    /* A hold shorter than this is a short press of SYSTEM (§11, S-035): the Host treats it as the
     * key alone and sends none of its audio to recognition. */
    MEISO_DISPLAY_AUDIO_SHORT_PRESS_MS = 300,
    /* Wire numbering (§15): Desired/Actual delivery generations and the audio stream's epoch; the
     * production gate is the Edge Actual's. */
    MEISO_DISPLAY_AUDIO_DESIRED_GENERATION = 21,
    MEISO_DISPLAY_AUDIO_ACTUAL_GENERATION = 22,
    MEISO_DISPLAY_AUDIO_STREAM_EPOCH = 1
};
typedef uint8_t MeisoDisplayAudioFormat;
enum { MEISO_DISPLAY_AUDIO_FORMAT_S16LE = 1 };
typedef uint8_t MeisoDisplayAudioState;
enum {
    MEISO_DISPLAY_AUDIO_OFF = 0,     /* forbidden by the Desired, or no Desired yet */
    MEISO_DISPLAY_AUDIO_READY = 1,   /* records while SYSTEM is held */
    MEISO_DISPLAY_AUDIO_FAILED = 2   /* the microphone cannot be opened */
};
/* Record flags. TRUNCATED and ABORTED come only with LAST. */
enum {
    MEISO_DISPLAY_AUDIO_FIRST = 1,      /* index 0: the utterance's first record */
    MEISO_DISPLAY_AUDIO_LAST = 2,       /* the utterance's last record (after the release) */
    MEISO_DISPLAY_AUDIO_TRUNCATED = 4,  /* ended at max_utterance_ms, the key still held */
    MEISO_DISPLAY_AUDIO_ABORTED = 8     /* ended by the lock or a microphone fault: the Host drops it */
};

typedef struct {
    uint32_t sample_rate_hz;            /* 16000 */
    uint8_t channels;                   /* 1 */
    MeisoDisplayAudioFormat format;     /* S16LE */
    uint16_t max_utterance_ms;          /* MIN..MAX_UTTERANCE_MS */
} MeisoDisplayAudioCapability;

typedef struct {
    bool allowed;                       /* 0: the Edge records nothing; 1: it records while SYSTEM is held */
} MeisoDisplayAudioDesired;

/* Changes only when the Desired changes or the microphone fails or recovers (a new production epoch
 * each time); recording or not is not in it. */
typedef struct {
    MeisoDisplayAudioState state;
} MeisoDisplayAudioActual;

/* One record: header plus count samples. samples points into the decoded input (no copy). */
typedef struct {
    uint64_t time_ns;                   /* Edge clock: the first sample's */
    uint32_t utterance;                 /* from 1 in the Session, one per hold */
    uint32_t index;                     /* from 0 in the utterance, every record; a gap is a lost record */
    uint8_t flags;
    uint16_t count;                     /* 0..MAX_RECORD_SAMPLES; 0 only with LAST */
    const uint8_t *samples;             /* count S16LE samples */
} MeisoDisplayAudioRecord;

static inline size_t meiso_display_audio_record_bytes(uint16_t count)
{
    return (size_t)MEISO_DISPLAY_AUDIO_RECORD_HEADER_BYTES + 2U * count;
}

/* Codecs: inputs and outputs must be disjoint; failure leaves outputs unchanged. A record that fails
 * to decode is dropped and counted by the Host, not a Session error (as §10, §11). */
bool meiso_display_audio_capability_encode(const MeisoDisplayAudioCapability *, MeisoMutableBuffer);
bool meiso_display_audio_capability_decode(MeisoByteView, MeisoDisplayAudioCapability *);
bool meiso_display_audio_desired_encode(const MeisoDisplayAudioDesired *, MeisoMutableBuffer);
bool meiso_display_audio_desired_decode(MeisoByteView, MeisoDisplayAudioDesired *);
bool meiso_display_audio_actual_encode(const MeisoDisplayAudioActual *, MeisoMutableBuffer);
bool meiso_display_audio_actual_decode(MeisoByteView, MeisoDisplayAudioActual *);
/* Writes meiso_display_audio_record_bytes(r->count) bytes; *written says how many. */
bool meiso_display_audio_record_encode(const MeisoDisplayAudioRecord *, MeisoMutableBuffer, size_t *written);
/* out->samples points into in; valid while in is. */
bool meiso_display_audio_record_decode(MeisoByteView, MeisoDisplayAudioRecord *);
const MeisoEndpointId *meiso_display_audio_endpoint(void);
#ifdef __cplusplus
}
#endif
#endif
