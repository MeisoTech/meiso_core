#ifndef MEISO_COMPOSITION_SESSION_V2_H
#define MEISO_COMPOSITION_SESSION_V2_H

#include "composition_scene.h"
#include "wifi_telemetry.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT 3U
#define MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT 9U
#define MEISO_COMPOSITION_SESSION_V2_CATALOG_WIRE_SIZE 920U
#define MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES \
    MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE

#define MEISO_COMPOSITION_SESSION_V2_MAX_FRAME_BYTES 8236U
#define MEISO_COMPOSITION_SESSION_V2_MAX_RUNTIME_PAYLOAD_BYTES 8192U
#define MEISO_COMPOSITION_SESSION_V2_MAX_REGISTRY_ENTRIES 12U
#define MEISO_COMPOSITION_SESSION_V2_MAX_ENDPOINTS 3U
#define MEISO_COMPOSITION_SESSION_V2_MAX_ENDPOINT_HISTORY 8U
#define MEISO_COMPOSITION_SESSION_V2_MAX_ROUTES 9U
#define MEISO_COMPOSITION_SESSION_V2_MAX_CAPABILITY_BYTES 100U
#define MEISO_COMPOSITION_SESSION_V2_MAX_DESIRED_PROFILE_BYTES \
    MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE
#define MEISO_COMPOSITION_SESSION_V2_MAX_ACTUAL_PROFILE_BYTES \
    MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE
#define MEISO_COMPOSITION_SESSION_V2_MAX_BLOB_TRANSFER_BYTES \
    MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE

typedef struct {
    uint64_t wifi_incarnation;
    uint64_t presentation_incarnation;
    uint64_t scene_incarnation;
    MeisoWifiTelemetryCapability wifi_capability;
} MeisoCompositionSessionV2CatalogConfig;

typedef uint8_t MeisoCompositionSessionV2Status;
enum {
    MEISO_COMPOSITION_SESSION_V2_OK = 0,
    MEISO_COMPOSITION_SESSION_V2_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_SESSION_V2_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_SESSION_V2_CAPACITY = 3,
    MEISO_COMPOSITION_SESSION_V2_CODEC_FAILURE = 4,
    MEISO_COMPOSITION_SESSION_V2_COLLISION = 5
};

/*
 * Sets the exact composite Profile v2 and exact selected Session limits
 * (8236/8192/12). Identity, role, nonce, peer, incarnation, and protocol
 * fields are preserved and must already be valid. Failure leaves the complete
 * config byte-for-byte unchanged. The result is passed unchanged to
 * meiso_session_init on each peer.
 */
MeisoCompositionSessionV2Status meiso_composition_session_v2_configure_session(
    MeisoSessionConfig *in_out_config
);

/*
 * Produces the exact MRT0 v3 Runtime capacities for a READY Session created
 * from configure_session. Failure leaves out_config byte-for-byte unchanged.
 * The caller owns the resulting Runtime arena.
 */
MeisoCompositionSessionV2Status meiso_composition_session_v2_runtime_config(
    MeisoSession *session,
    MeisoRuntimeV3Config *out_config
);

/*
 * Builds Wi-Fi first, Presentation second, and Scene third. Routes follow the
 * same endpoint order and the Profile tables: Wi-Fi 3, Presentation 2,
 * Scene 4. wifi_capability_storage is retained by endpoints[0] until the
 * catalog offer has synchronously copied it into Runtime. All four outputs
 * are caller-owned, mutually disjoint, and disjoint from config. On any
 * validation/capacity/codec/collision failure, storage, endpoints, and routes
 * remain byte-for-byte unchanged. The resulting catalog canonical wire size
 * is exactly 920 bytes under the selected MRT0 v3 encoder.
 */
MeisoCompositionSessionV2Status meiso_composition_session_v2_build_catalog(
    const MeisoCompositionSessionV2CatalogConfig *config,
    MeisoMutableBuffer wifi_capability_storage,
    MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_capacity,
    MeisoRuntimeRouteDescriptor *routes,
    size_t route_capacity
);

#ifdef __cplusplus
}
#endif

#endif
