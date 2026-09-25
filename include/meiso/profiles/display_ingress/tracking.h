#ifndef MEISO_DISPLAY_INGRESS_TRACKING_H
#define MEISO_DISPLAY_INGRESS_TRACKING_H
#include <meiso/core/resource.h>
#include <meiso/runtime/runtime.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Display Ingress v12 tracking endpoint (planes-v12.md §10, meiso_host #53): the Edge's IMU up,
 * the Host's head pose down, both timed on the Edge clock (clock epoch 4). Big-endian. */
enum {
    MEISO_DISPLAY_TRACKING_CAPABILITY_BYTES = 32,
    MEISO_DISPLAY_TRACKING_DESIRED_BYTES = 16,
    MEISO_DISPLAY_TRACKING_ACTUAL_BYTES = 16,
    MEISO_DISPLAY_TRACKING_IMU_HEADER_BYTES = 16,
    MEISO_DISPLAY_TRACKING_IMU_SAMPLE_BYTES = 32,
    MEISO_DISPLAY_TRACKING_MAX_IMU_BATCH = 32,
    MEISO_DISPLAY_TRACKING_MAX_IMU_BYTES = 16 + 32 * 32,
    MEISO_DISPLAY_TRACKING_POSE_BYTES = 72,
    MEISO_DISPLAY_TRACKING_MAX_PREDICTION_MS = 200,
    /* Wire numbering (§10): Desired/Actual delivery generations, and the two streams' epochs. The
     * production gate is the Edge Actual's, for the Host's pose stream as for the Edge's IMU. */
    MEISO_DISPLAY_TRACKING_DESIRED_GENERATION = 15,
    MEISO_DISPLAY_TRACKING_ACTUAL_GENERATION = 16,
    MEISO_DISPLAY_TRACKING_IMU_STREAM_EPOCH = 1,
    MEISO_DISPLAY_TRACKING_POSE_STREAM_EPOCH = 1
};
typedef uint8_t MeisoDisplayTrackingCorrection;
enum { MEISO_DISPLAY_TRACKING_CORRECTION_NONE = 0, MEISO_DISPLAY_TRACKING_CORRECTION_IMU = 1 };
typedef uint8_t MeisoDisplayTrackingImuState;
enum { MEISO_DISPLAY_TRACKING_IMU_OFF = 0, MEISO_DISPLAY_TRACKING_IMU_RUNNING = 1, MEISO_DISPLAY_TRACKING_IMU_FAILED = 2 };
typedef uint8_t MeisoDisplayTrackingPoseState;
enum { MEISO_DISPLAY_TRACKING_POSE_OFF = 0, MEISO_DISPLAY_TRACKING_POSE_APPLYING = 1 };
typedef uint8_t MeisoDisplayTrackingPoseQuality;
enum { MEISO_DISPLAY_TRACKING_TRACKING = 1, MEISO_DISPLAY_TRACKING_LOST = 2 };

typedef struct {
    uint32_t imu_rate_millihz;          /* 0: no IMU uplink; else 50000..2000000 */
    float accel_range_mps2, gyro_range_rps; /* full scale; 0 without IMU */
    uint8_t max_imu_batch;              /* 1..32 */
    MeisoDisplayTrackingCorrection correction;
    uint16_t max_prediction_ms;         /* 1..200 */
} MeisoDisplayTrackingCapability;

typedef struct {
    bool imu_enabled, pose_enabled;
} MeisoDisplayTrackingDesired;

/* Changes only with the effective configuration (a new production epoch each time); whether a
 * pose was fresh enough for a given frame is not reported. */
typedef struct {
    MeisoDisplayTrackingImuState imu_state;
    MeisoDisplayTrackingPoseState pose_state;
} MeisoDisplayTrackingActual;

typedef struct {
    uint64_t time_ns;                   /* Edge clock */
    float accel_mps2[3], gyro_rps[3];   /* IMU axes; gravity included */
} MeisoDisplayTrackingImuSample;

typedef struct {
    uint64_t first_sequence;            /* from 1 per Session, consecutive; a gap is lost samples */
    uint8_t count;
    MeisoDisplayTrackingImuSample samples[MEISO_DISPLAY_TRACKING_MAX_IMU_BATCH];
} MeisoDisplayTrackingImu;

typedef struct {
    uint64_t time_ns;                   /* Edge clock: the instant this pose describes */
    uint32_t world_epoch;               /* nonzero; changes when the Host's world frame is reset */
    MeisoDisplayTrackingPoseQuality quality;
    float position[3];                  /* head origin in the world, metres */
    float orientation[4];               /* head -> world, unit quaternion x y z w */
    float velocity[3];                  /* world frame, m/s */
    float angular[3];                   /* head frame, rad/s */
} MeisoDisplayTrackingPose;

/* Codecs: inputs and outputs must be disjoint; failure leaves outputs unchanged. An IMU or pose
 * record that fails to decode is dropped and counted by its receiver, not a Session error (§10). */
bool meiso_display_tracking_capability_encode(const MeisoDisplayTrackingCapability *, MeisoMutableBuffer);
bool meiso_display_tracking_capability_decode(MeisoByteView, MeisoDisplayTrackingCapability *);
bool meiso_display_tracking_desired_encode(const MeisoDisplayTrackingDesired *, MeisoMutableBuffer);
bool meiso_display_tracking_desired_decode(MeisoByteView, MeisoDisplayTrackingDesired *);
bool meiso_display_tracking_actual_encode(const MeisoDisplayTrackingActual *, MeisoMutableBuffer);
bool meiso_display_tracking_actual_decode(MeisoByteView, MeisoDisplayTrackingActual *);
/* IMU records are checked against the capability (batch size, full scale); size is 16 + 32 * count. */
size_t meiso_display_tracking_imu_encode(const MeisoDisplayTrackingCapability *, const MeisoDisplayTrackingImu *,
    MeisoMutableBuffer);
bool meiso_display_tracking_imu_decode(const MeisoDisplayTrackingCapability *, MeisoByteView, MeisoDisplayTrackingImu *);
bool meiso_display_tracking_pose_encode(const MeisoDisplayTrackingPose *, MeisoMutableBuffer);
bool meiso_display_tracking_pose_decode(MeisoByteView, MeisoDisplayTrackingPose *);
/* The Desired fits this capability (no IMU asked of an Edge without one). */
bool meiso_display_tracking_desired_supported(const MeisoDisplayTrackingCapability *, const MeisoDisplayTrackingDesired *);
const MeisoEndpointId *meiso_display_tracking_endpoint(void);
#ifdef __cplusplus
}
#endif
#endif
