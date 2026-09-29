#ifndef MEISO_DISPLAY_INGRESS_LOCAL_POSE_H
#define MEISO_DISPLAY_INGRESS_LOCAL_POSE_H
#include <meiso/profiles/display_ingress/tracking.h>
#ifdef __cplusplus
extern "C" {
#endif
/* v12 section 14: Edge-owned current head pose, independent of the renderer's
 * predicted pose. Same 72-byte numeric layout as tracking.h, opposite owner. */
enum {
    MEISO_DISPLAY_LOCAL_POSE_CAPABILITY_BYTES = 16,
    MEISO_DISPLAY_LOCAL_POSE_DESIRED_BYTES = 16,
    MEISO_DISPLAY_LOCAL_POSE_ACTUAL_BYTES = 16,
    MEISO_DISPLAY_LOCAL_POSE_BYTES = 72,
    MEISO_DISPLAY_LOCAL_POSE_MAX_RATE = 200,
    MEISO_DISPLAY_LOCAL_POSE_DESIRED_GENERATION = 19,
    MEISO_DISPLAY_LOCAL_POSE_ACTUAL_GENERATION = 20,
    MEISO_DISPLAY_LOCAL_POSE_STREAM_EPOCH = 1,
    MEISO_DISPLAY_LOCAL_POSE_ROUTE_DESIRED = 0,
    MEISO_DISPLAY_LOCAL_POSE_ROUTE_ACTUAL = 1,
    MEISO_DISPLAY_LOCAL_POSE_ROUTE_POSE = 2
};
typedef struct { uint16_t max_samples_per_second; } MeisoDisplayLocalPoseCapability;
typedef struct { bool enabled; } MeisoDisplayLocalPoseDesired;
typedef uint8_t MeisoDisplayLocalPoseState;
enum { MEISO_DISPLAY_LOCAL_POSE_OFF = 0, MEISO_DISPLAY_LOCAL_POSE_RUNNING = 1,
    MEISO_DISPLAY_LOCAL_POSE_FAILED = 2 };
typedef struct { MeisoDisplayLocalPoseState state; } MeisoDisplayLocalPoseActual;
/* world_epoch belongs to the Edge; time_ns is current Edge clock epoch 4. */
typedef MeisoDisplayTrackingPose MeisoDisplayLocalPose;
bool meiso_display_local_pose_capability_encode(const MeisoDisplayLocalPoseCapability *, MeisoMutableBuffer);
bool meiso_display_local_pose_capability_decode(MeisoByteView, MeisoDisplayLocalPoseCapability *);
bool meiso_display_local_pose_desired_encode(const MeisoDisplayLocalPoseDesired *, MeisoMutableBuffer);
bool meiso_display_local_pose_desired_decode(MeisoByteView, MeisoDisplayLocalPoseDesired *);
bool meiso_display_local_pose_actual_encode(const MeisoDisplayLocalPoseActual *, MeisoMutableBuffer);
bool meiso_display_local_pose_actual_decode(MeisoByteView, MeisoDisplayLocalPoseActual *);
/* Numeric codecs only. The producer checks time_ns <= send time; the receiver
 * drops/counts stale or regressing samples without closing the Session. */
bool meiso_display_local_pose_encode(const MeisoDisplayLocalPose *, MeisoMutableBuffer);
bool meiso_display_local_pose_decode(MeisoByteView, MeisoDisplayLocalPose *);
const MeisoEndpointId *meiso_display_local_pose_endpoint(void);
#ifdef __cplusplus
}
#endif
#endif
