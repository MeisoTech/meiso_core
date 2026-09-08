#ifndef MEISO_PROFILES_DISPLAY_INGRESS_PROJECTION_H
#define MEISO_PROFILES_DISPLAY_INGRESS_PROJECTION_H

#include <meiso/profiles/display_ingress/profile.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_DISPLAY_PROJECTION_SEI_MAX_BYTES 256U

/* Authored rendering facts, never a report of Edge tracking or correction. */
typedef struct {
    float orientation_xyzw[4];
    float position_meters[3];
    float fov_left_right_up_down[4]; /* Angles in radians. */
} MeisoDisplayProjectionView;

typedef struct MeisoDisplayProjection {
    uint8_t view_count; /* 1 = one source view; 2 = left, right. */
    uint8_t space_class; /* 1 = head, 2 = application, 3 = world. */
    uint64_t source_space_id;
    uint64_t source_space_epoch;
    bool target_time_present;
    MeisoTimePoint target_time; /* Host producer clock; not an Edge deadline. */
    MeisoDisplayProjectionView views[2];
} MeisoDisplayProjection;

/* Failure leaves outputs unchanged. Unused views and absent target time are zero. */
bool meiso_display_projection_valid(const MeisoDisplayProjection *value);
MeisoDisplayIngressProfileStatus meiso_display_projection_sei_write(
    const MeisoDisplayProjection *value, MeisoMutableBuffer output, size_t *out_size);
/* Reads Annex B NALs without allocating. Absent metadata leaves *out_value unchanged.
 * Duplicate or malformed known metadata is rejected; unrelated SEI is skipped.
 * The caller still validates the complete H.264 access unit for its selected Profile. */
MeisoDisplayIngressProfileStatus meiso_display_projection_sei_read(
    MeisoByteView access_unit, bool *out_present, MeisoDisplayProjection *out_value);
/* Insert before the IDR of a valid independent AU. Exactly in-place output is allowed;
 * all other input/output overlap is rejected. The resulting AU includes the SEI in
 * its existing byte limit, fragmentation and presentation SHA-256 identity. */
MeisoDisplayIngressProfileStatus meiso_display_projection_sei_insert(
    MeisoByteView access_unit, const MeisoDisplayProjection *value,
    MeisoMutableBuffer output, size_t *out_size);

#ifdef __cplusplus
}
#endif
#endif
