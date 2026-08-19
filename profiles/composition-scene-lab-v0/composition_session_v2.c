#include "composition_session_v2.h"

#include <stdint.h>
#include <string.h>

_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 16U +
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT * 48U +
        MEISO_COMPOSITION_SESSION_V2_MAX_CAPABILITY_BYTES +
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT * 72U ==
            MEISO_COMPOSITION_SESSION_V2_CATALOG_WIRE_SIZE,
    "combined catalog wire size"
);
_Static_assert(
    MEISO_COMPOSITION_SESSION_V2_MAX_DESIRED_PROFILE_BYTES == 3696U,
    "combined desired profile capacity"
);
_Static_assert(
    MEISO_COMPOSITION_SESSION_V2_MAX_ACTUAL_PROFILE_BYTES == 160U,
    "combined actual profile capacity"
);

static bool checked_multiply(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || (lhs != 0U && rhs > SIZE_MAX / lhs)) {
        return false;
    }
    *out_value = lhs * rhs;
    return true;
}

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    const uintptr_t lhs_begin = (uintptr_t)lhs;
    const uintptr_t rhs_begin = (uintptr_t)rhs;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_begin > UINTPTR_MAX - lhs_size ||
        rhs_begin > UINTPTR_MAX - rhs_size) {
        return true;
    }
    return lhs_begin < rhs_begin + rhs_size &&
        rhs_begin < lhs_begin + lhs_size;
}

static bool selected_limits_equal(MeisoSessionLimits limits)
{
    return limits.max_frame_bytes ==
            MEISO_COMPOSITION_SESSION_V2_MAX_FRAME_BYTES &&
        limits.max_runtime_payload_bytes ==
            MEISO_COMPOSITION_SESSION_V2_MAX_RUNTIME_PAYLOAD_BYTES &&
        limits.max_registry_entries ==
            MEISO_COMPOSITION_SESSION_V2_MAX_REGISTRY_ENTRIES;
}

MeisoCompositionSessionV2Status meiso_composition_session_v2_configure_session(
    MeisoSessionConfig *in_out_config
)
{
    MeisoSessionConfig candidate;

    if (in_out_config == NULL) {
        return MEISO_COMPOSITION_SESSION_V2_INVALID_ARGUMENT;
    }
    candidate = *in_out_config;
    candidate.profile = *meiso_composition_scene_profile_v2();
    candidate.limits = (MeisoSessionLimits){
        MEISO_COMPOSITION_SESSION_V2_MAX_FRAME_BYTES,
        MEISO_COMPOSITION_SESSION_V2_MAX_RUNTIME_PAYLOAD_BYTES,
        MEISO_COMPOSITION_SESSION_V2_MAX_REGISTRY_ENTRIES
    };
    if (meiso_session_memory_required(&candidate) == 0U) {
        return MEISO_COMPOSITION_SESSION_V2_INVALID_CONFIG;
    }
    *in_out_config = candidate;
    return MEISO_COMPOSITION_SESSION_V2_OK;
}

MeisoCompositionSessionV2Status meiso_composition_session_v2_runtime_config(
    MeisoSession *session,
    MeisoRuntimeV3Config *out_config
)
{
    MeisoSessionSnapshot snapshot;
    MeisoRuntimeV3Config candidate;

    if (session == NULL || out_config == NULL) {
        return MEISO_COMPOSITION_SESSION_V2_INVALID_ARGUMENT;
    }
    if (!meiso_session_snapshot(session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_READY ||
        !snapshot.session_id_known || !selected_limits_equal(
            snapshot.negotiated_limits
        )) {
        return MEISO_COMPOSITION_SESSION_V2_INVALID_CONFIG;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.base.session = session;
    candidate.base.max_endpoints =
        MEISO_COMPOSITION_SESSION_V2_MAX_ENDPOINTS;
    candidate.base.max_endpoint_history =
        MEISO_COMPOSITION_SESSION_V2_MAX_ENDPOINT_HISTORY;
    candidate.base.max_routes = MEISO_COMPOSITION_SESSION_V2_MAX_ROUTES;
    candidate.base.max_capability_bytes =
        MEISO_COMPOSITION_SESSION_V2_MAX_CAPABILITY_BYTES;
    candidate.base.max_desired_profile_bytes_per_endpoint =
        MEISO_COMPOSITION_SESSION_V2_MAX_DESIRED_PROFILE_BYTES;
    candidate.base.max_actual_profile_bytes_per_endpoint =
        MEISO_COMPOSITION_SESSION_V2_MAX_ACTUAL_PROFILE_BYTES;
    candidate.max_blob_transfer_bytes =
        MEISO_COMPOSITION_SESSION_V2_MAX_BLOB_TRANSFER_BYTES;
    if (meiso_runtime_v3_memory_required(&candidate) == 0U) {
        return MEISO_COMPOSITION_SESSION_V2_INVALID_CONFIG;
    }
    *out_config = candidate;
    return MEISO_COMPOSITION_SESSION_V2_OK;
}

MeisoCompositionSessionV2Status meiso_composition_session_v2_build_catalog(
    const MeisoCompositionSessionV2CatalogConfig *config,
    MeisoMutableBuffer wifi_capability_storage,
    MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_capacity,
    MeisoRuntimeRouteDescriptor *routes,
    size_t route_capacity
)
{
    static const MeisoRuntimeFacet wifi_facets[
        MEISO_WIFI_TELEMETRY_RESOURCE_COUNT
    ] = {
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    MeisoRuntimeEndpointDescriptor endpoint_candidate[
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT
    ];
    MeisoRuntimeRouteDescriptor route_candidate[
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ];
    uint8_t capability_candidate[
        MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES
    ];
    MeisoMutableBuffer capability_output = {
        capability_candidate, sizeof(capability_candidate)
    };
    const MeisoResourceDescriptor *wifi_resources;
    size_t wifi_resource_count = 0U;
    size_t composition_endpoint_count = 0U;
    size_t composition_route_count = 0U;
    size_t capability_size = 0U;
    size_t endpoint_bytes;
    size_t route_bytes;
    size_t index;

    if (config == NULL || endpoints == NULL || routes == NULL ||
        !meiso_mutable_buffer_valid(wifi_capability_storage) ||
        !checked_multiply(
            endpoint_capacity, sizeof(*endpoints), &endpoint_bytes
        ) || !checked_multiply(
            route_capacity, sizeof(*routes), &route_bytes
        )) {
        return MEISO_COMPOSITION_SESSION_V2_INVALID_ARGUMENT;
    }
    if (endpoint_capacity < MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT ||
        route_capacity < MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT ||
        wifi_capability_storage.capacity <
            MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES) {
        return MEISO_COMPOSITION_SESSION_V2_CAPACITY;
    }
    endpoint_bytes = MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT *
        sizeof(*endpoints);
    route_bytes = MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT * sizeof(*routes);
    if (ranges_overlap(config, sizeof(*config), endpoints, endpoint_bytes) ||
        ranges_overlap(config, sizeof(*config), routes, route_bytes) ||
        ranges_overlap(
            config,
            sizeof(*config),
            wifi_capability_storage.data,
            MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES
        ) || ranges_overlap(endpoints, endpoint_bytes, routes, route_bytes) ||
        ranges_overlap(
            endpoints,
            endpoint_bytes,
            wifi_capability_storage.data,
            MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES
        ) || ranges_overlap(
            routes,
            route_bytes,
            wifi_capability_storage.data,
            MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES
        )) {
        return MEISO_COMPOSITION_SESSION_V2_INVALID_ARGUMENT;
    }
    if (config->wifi_incarnation == 0U ||
        config->presentation_incarnation == 0U ||
        config->scene_incarnation == 0U) {
        return MEISO_COMPOSITION_SESSION_V2_INVALID_CONFIG;
    }
    if (meiso_wifi_telemetry_capability_encode(
            &config->wifi_capability, capability_output, &capability_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        capability_size != sizeof(capability_candidate)) {
        return MEISO_COMPOSITION_SESSION_V2_CODEC_FAILURE;
    }
    memset(endpoint_candidate, 0, sizeof(endpoint_candidate));
    memset(route_candidate, 0, sizeof(route_candidate));
    endpoint_candidate[0].id = *meiso_wifi_telemetry_endpoint();
    endpoint_candidate[0].incarnation = config->wifi_incarnation;
    endpoint_candidate[0].capability_schema =
        *meiso_wifi_telemetry_capability_schema();
    endpoint_candidate[0].capability = (MeisoByteView){
        capability_candidate, sizeof(capability_candidate)
    };
    if (meiso_composition_scene_build_endpoints(
            config->presentation_incarnation,
            config->scene_incarnation,
            endpoint_candidate + 1U,
            MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT,
            &composition_endpoint_count
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
        composition_endpoint_count != MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT) {
        return MEISO_COMPOSITION_SESSION_V2_CODEC_FAILURE;
    }
    wifi_resources = meiso_wifi_telemetry_resources(&wifi_resource_count);
    if (wifi_resources == NULL ||
        wifi_resource_count != MEISO_WIFI_TELEMETRY_RESOURCE_COUNT) {
        return MEISO_COMPOSITION_SESSION_V2_INVALID_CONFIG;
    }
    for (index = 0U; index < wifi_resource_count; ++index) {
        route_candidate[index].endpoint_id =
            *meiso_wifi_telemetry_endpoint();
        route_candidate[index].endpoint_incarnation =
            config->wifi_incarnation;
        route_candidate[index].resource = wifi_resources[index];
        route_candidate[index].facet = wifi_facets[index];
    }
    if (meiso_composition_scene_build_routes(
            config->presentation_incarnation,
            config->scene_incarnation,
            route_candidate + MEISO_WIFI_TELEMETRY_RESOURCE_COUNT,
            MEISO_COMPOSITION_SCENE_RESOURCE_COUNT,
            &composition_route_count
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
        composition_route_count != MEISO_COMPOSITION_SCENE_RESOURCE_COUNT) {
        return MEISO_COMPOSITION_SESSION_V2_CODEC_FAILURE;
    }
    if (!meiso_composition_scene_catalog_identifiers_disjoint(
            endpoint_candidate,
            1U,
            route_candidate,
            MEISO_WIFI_TELEMETRY_RESOURCE_COUNT
        )) {
        return MEISO_COMPOSITION_SESSION_V2_COLLISION;
    }

    memmove(
        wifi_capability_storage.data,
        capability_candidate,
        sizeof(capability_candidate)
    );
    endpoint_candidate[0].capability.data = wifi_capability_storage.data;
    memmove(endpoints, endpoint_candidate, sizeof(endpoint_candidate));
    memmove(routes, route_candidate, sizeof(route_candidate));
    return MEISO_COMPOSITION_SESSION_V2_OK;
}
