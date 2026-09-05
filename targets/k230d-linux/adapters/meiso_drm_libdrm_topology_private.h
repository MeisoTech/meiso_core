#ifndef MEISO_TARGETS_K230D_LINUX_ADAPTERS_DRM_LIBDRM_TOPOLOGY_PRIVATE_H
#define MEISO_TARGETS_K230D_LINUX_ADAPTERS_DRM_LIBDRM_TOPOLOGY_PRIVATE_H

#include "meiso_drm_scanout_private.h"

#include <stdio.h>

enum {
    MEISO_K230D_DRM_NATIVE_RECEIPT_CAPACITY = 8192U
};

typedef uint8_t MeisoK230dDrmNativeReadStatus;
enum {
    MEISO_K230D_DRM_NATIVE_READ_COMPLETE = 0,
    MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED = 1,
    MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED = 2,
    MEISO_K230D_DRM_NATIVE_READ_INVALID_ARGUMENT = 3
};

typedef uint8_t MeisoK230dDrmNativeStage;
enum {
    MEISO_K230D_DRM_NATIVE_STAGE_NONE = 0,
    MEISO_K230D_DRM_NATIVE_STAGE_OPEN = 1,
    MEISO_K230D_DRM_NATIVE_STAGE_FD_CONTRACT = 2,
    MEISO_K230D_DRM_NATIVE_STAGE_MASTER_OBSERVATION = 3,
    MEISO_K230D_DRM_NATIVE_STAGE_UNIVERSAL_PLANES_CAP = 4,
    MEISO_K230D_DRM_NATIVE_STAGE_ATOMIC_CAP = 5,
    MEISO_K230D_DRM_NATIVE_STAGE_DRIVER_VERSION = 6,
    MEISO_K230D_DRM_NATIVE_STAGE_TIMESTAMP_CAP = 7,
    MEISO_K230D_DRM_NATIVE_STAGE_RESOURCES = 8,
    MEISO_K230D_DRM_NATIVE_STAGE_PLANE_RESOURCES = 9,
    MEISO_K230D_DRM_NATIVE_STAGE_CONNECTOR = 10,
    MEISO_K230D_DRM_NATIVE_STAGE_ENCODER = 11,
    MEISO_K230D_DRM_NATIVE_STAGE_CRTC = 12,
    MEISO_K230D_DRM_NATIVE_STAGE_CRTC_PROPERTIES = 13,
    MEISO_K230D_DRM_NATIVE_STAGE_PLANE = 14,
    MEISO_K230D_DRM_NATIVE_STAGE_PLANE_PROPERTIES = 15,
    MEISO_K230D_DRM_NATIVE_STAGE_TRANSLATION = 16,
    MEISO_K230D_DRM_NATIVE_STAGE_CLOSE = 17
};

typedef struct {
    MeisoK230dDrmNativeReadStatus status;
    MeisoK230dDrmNativeStage operation_stage;
    int error;
    MeisoK230dDrmAdmissionReason preclassified_reason;
    bool topology_ready_for_evaluation;
    bool topology_complete;
    bool universal_planes_client_cap_set;
    bool atomic_client_cap_set;
    bool master_observation_attempted;
    bool master_observation_supported;
    bool master_observed_while_open;
    bool all_plane_properties_complete;
    bool fd_opened;
    bool primary_node_verified;
    bool close_attempted;
    bool close_consumed;
    bool device_closed;
    bool libdrm_objects_released;
    uint32_t raw_connector_count;
    uint32_t raw_crtc_count;
    uint32_t raw_plane_count;
    uint32_t libdrm_objects_seen;
    uint32_t libdrm_objects_released_count;
    MeisoK230dDrmTopology topology;
} MeisoK230dDrmNativeObservation;

typedef MeisoK230dDrmNativeReadStatus (*MeisoK230dDrmNativeObserveFn)(
    const char *device_path,
    MeisoK230dDrmNativeObservation *out_observation
);

MEISO_K230D_DRM_PRIVATE_API
MeisoK230dDrmNativeReadStatus meiso_k230d_drm_libdrm_observe(
    const char *device_path,
    MeisoK230dDrmNativeObservation *out_observation
);

/* Host-testable process seam; production main binds the real libdrm reader. */
MEISO_K230D_DRM_PRIVATE_API
int meiso_k230d_drm_topology_admission_once_run(
    FILE *out_stream,
    FILE *error_stream,
    int argument_count,
    char **arguments,
    MeisoK230dDrmNativeObserveFn observe
);

#endif
