#ifndef MEISO_DISPLAY_INGRESS_INPUT_H
#define MEISO_DISPLAY_INGRESS_INPUT_H
#include <meiso/core/resource.h>
#include <meiso/runtime/runtime.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Display Ingress v12 input endpoint (planes-v12.md §11, meiso_host #66): key presses and releases
 * up, each with the plane under the Edge's head-ray reticle, timed on the Edge clock (clock epoch 4).
 * Lock and choosing a Host stay on the Edge and never go on the wire. Big-endian. */
enum {
    MEISO_DISPLAY_INPUT_CAPABILITY_BYTES = 16,
    MEISO_DISPLAY_INPUT_DESIRED_BYTES = 16,
    MEISO_DISPLAY_INPUT_ACTUAL_BYTES = 16,
    MEISO_DISPLAY_INPUT_KEY_BYTES = 24,
    /* Events the Edge keeps while they cannot be sent; beyond this the oldest go. */
    MEISO_DISPLAY_INPUT_MAX_BACKLOG = 32,
    /* Wire numbering (§11): Desired/Actual delivery generations and the key stream's epoch; the
     * production gate is the Edge Actual's. */
    MEISO_DISPLAY_INPUT_DESIRED_GENERATION = 17,
    MEISO_DISPLAY_INPUT_ACTUAL_GENERATION = 18,
    MEISO_DISPLAY_INPUT_KEY_STREAM_EPOCH = 1
};
typedef uint8_t MeisoDisplayInputKey;
enum {
    MEISO_DISPLAY_INPUT_KEY_UP = 1,
    MEISO_DISPLAY_INPUT_KEY_DOWN = 2,
    MEISO_DISPLAY_INPUT_KEY_LEFT = 3,
    MEISO_DISPLAY_INPUT_KEY_RIGHT = 4,
    MEISO_DISPLAY_INPUT_KEY_SELECT = 5,  /* the five-way key pressed in */
    MEISO_DISPLAY_INPUT_KEY_BACK = 6,
    MEISO_DISPLAY_INPUT_KEY_SYSTEM = 7,  /* Claire (S-033): the system's alone, never an APP's */
    MEISO_DISPLAY_INPUT_KEY_MENU = 8     /* the system key (S-045): the system menu; the system's alone too */
};
typedef uint8_t MeisoDisplayInputAction;
enum { MEISO_DISPLAY_INPUT_PRESS = 1, MEISO_DISPLAY_INPUT_RELEASE = 2 };
typedef uint8_t MeisoDisplayInputPointer;
enum { MEISO_DISPLAY_INPUT_POINTER_NONE = 0, MEISO_DISPLAY_INPUT_POINTER_HEAD_RAY = 1 };
typedef uint8_t MeisoDisplayInputKeysState;
enum { MEISO_DISPLAY_INPUT_KEYS_OFF = 0, MEISO_DISPLAY_INPUT_KEYS_RUNNING = 1 };

typedef struct {
    uint32_t keys;                      /* bit k for key code k; nonzero, bits 1..8 only */
    MeisoDisplayInputPointer pointer;   /* HEAD_RAY: the Edge draws a reticle and names its target */
} MeisoDisplayInputCapability;

typedef struct {
    bool keys_enabled;
} MeisoDisplayInputDesired;

/* Changes only when the Desired changes or the Edge is locked or unlocked (a new production epoch
 * each time). locked is reported whatever the Desired: nothing is drawn then, so no evidence comes. */
typedef struct {
    MeisoDisplayInputKeysState keys_state;
    bool locked;
} MeisoDisplayInputActual;

typedef struct {
    uint64_t time_ns;                   /* Edge clock: the first contact change of this press or release */
    MeisoDisplayInputKey key;
    MeisoDisplayInputAction action;
    uint32_t target_plane;              /* plane under the reticle at time_ns; 0: none */
    uint64_t target_generation;         /* the model generation it belongs to; 0 with no target */
} MeisoDisplayInputKeyEvent;

static inline uint32_t meiso_display_input_key_bit(MeisoDisplayInputKey key) { return key < 32 ? UINT32_C(1) << key : 0; }

/* Codecs: inputs and outputs must be disjoint; failure leaves outputs unchanged. A key event that fails
 * to decode is dropped and counted by the Host, not a Session error (§11, as §10). */
bool meiso_display_input_capability_encode(const MeisoDisplayInputCapability *, MeisoMutableBuffer);
bool meiso_display_input_capability_decode(MeisoByteView, MeisoDisplayInputCapability *);
bool meiso_display_input_desired_encode(const MeisoDisplayInputDesired *, MeisoMutableBuffer);
bool meiso_display_input_desired_decode(MeisoByteView, MeisoDisplayInputDesired *);
bool meiso_display_input_actual_encode(const MeisoDisplayInputActual *, MeisoMutableBuffer);
bool meiso_display_input_actual_decode(MeisoByteView, MeisoDisplayInputActual *);
/* Key events are checked against the capability: a key the glasses have, a target only with HEAD_RAY. */
bool meiso_display_input_key_encode(const MeisoDisplayInputCapability *, const MeisoDisplayInputKeyEvent *,
    MeisoMutableBuffer);
bool meiso_display_input_key_decode(const MeisoDisplayInputCapability *, MeisoByteView, MeisoDisplayInputKeyEvent *);
const MeisoEndpointId *meiso_display_input_endpoint(void);
#ifdef __cplusplus
}
#endif
#endif
