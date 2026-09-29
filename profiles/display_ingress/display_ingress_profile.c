#include <meiso/profiles/display_ingress/application.h>
#include "display_ingress_profile_private.h"

#include <string.h>

/* Exact Display Ingress identity, Catalog shape and Runtime binding gate. */

static const MeisoProfileRef DISPLAY_INGRESS_PROFILE = {
    {
        UINT64_C(0x444953504c41595f), /* DISPLAY_ */
        UINT64_C(0x494e47524553535f)  /* INGRESS_ */
    },
    MEISO_DISPLAY_INGRESS_PROFILE_VERSION
};

static const MeisoEndpointId DISPLAY_INGRESS_ENDPOINT = {
    UINT64_C(0x444953504c41595f), /* DISPLAY_ */
    UINT64_C(0x494e47525f454e44)  /* INGR_END */
};

static const MeisoSchemaRef DISPLAY_INGRESS_CAPABILITY_SCHEMA = {
    {
        UINT64_C(0x444953505f534348), /* DISP_SCH */
        UINT64_C(0x4341504142494c5f)  /* CAPABIL_ */
    },
    1U
};

static const MeisoResourceId DISPLAY_INGRESS_RESOURCE_IDS[
    MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
] = {
    {
        UINT64_C(0x444953504c41595f), /* DISPLAY_ */
        UINT64_C(0x4445535f53544154)  /* DES_STAT */
    },
    {
        UINT64_C(0x444953504c41595f), /* DISPLAY_ */
        UINT64_C(0x4143545f53544154)  /* ACT_STAT */
    },
    {
        UINT64_C(0x444953504c41595f), /* DISPLAY_ */
        UINT64_C(0x444154415f4d4544)  /* DATA_MED */
    },
    {
        UINT64_C(0x444953504c41595f), /* DISPLAY_ */
        UINT64_C(0x505245535f455654)  /* PRES_EVT */
    }
};

static const MeisoSchemaRef DISPLAY_INGRESS_RESOURCE_SCHEMAS[
    MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
] = {
    {
        {
            UINT64_C(0x444953505f534348), /* DISP_SCH */
            UINT64_C(0x444553495245445f)  /* DESIRED_ */
        },
        1U
    },
    {
        {
            UINT64_C(0x444953505f534348), /* DISP_SCH */
            UINT64_C(0x41435455414c5f5f)  /* ACTUAL__ */
        },
        1U
    },
    {
        {
            UINT64_C(0x444953505f534348), /* DISP_SCH */
            UINT64_C(0x4652414d455f5f5f)  /* FRAME___ */
        },
        1U
    },
    {
        {
            UINT64_C(0x444953505f534348), /* DISP_SCH */
            UINT64_C(0x505245535f455654)  /* PRES_EVT */
        },
        1U
    }
};

static const MeisoResourceKind DISPLAY_INGRESS_RESOURCE_KINDS[
    MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
] = {
    MEISO_RESOURCE_KIND_STATE,
    MEISO_RESOURCE_KIND_STATE,
    MEISO_RESOURCE_KIND_STREAM,
    MEISO_RESOURCE_KIND_STREAM
};

static const MeisoOwnerRole DISPLAY_INGRESS_RESOURCE_OWNERS[
    MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
] = {
    MEISO_OWNER_ROLE_HOST,
    MEISO_OWNER_ROLE_EDGE,
    MEISO_OWNER_ROLE_HOST,
    MEISO_OWNER_ROLE_EDGE
};

static const MeisoRuntimeFacet DISPLAY_INGRESS_RESOURCE_FACETS[
    MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
] = {
    MEISO_RUNTIME_FACET_DESIRED,
    MEISO_RUNTIME_FACET_ACTUAL,
    MEISO_RUNTIME_FACET_DATA,
    MEISO_RUNTIME_FACET_DATA
};

static const MeisoRuntimeDeliveryClass DISPLAY_INGRESS_DELIVERY_CLASSES[
    MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
] = {
    MEISO_RUNTIME_DELIVERY_INLINE,
    MEISO_RUNTIME_DELIVERY_INLINE,
    MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA,
    MEISO_RUNTIME_DELIVERY_INLINE
};

_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 16U + 48U +
        MEISO_DISPLAY_INGRESS_PROFILE_CAPABILITY_WIRE_SIZE +
        MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT *
            MEISO_RUNTIME_V4_ROUTE_RECORD_WIRE_SIZE ==
        MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_WIRE_SIZE,
    "Display Ingress Catalog wire size"
);
_Static_assert(
    MEISO_DISPLAY_INGRESS_PROFILE_MAX_RUNTIME_PAYLOAD_BYTES == 4180U,
    "Display Ingress Runtime v4 Blob-carriage floor"
);
_Static_assert(
    MEISO_DISPLAY_INGRESS_PROFILE_MAX_SESSION_FRAME_BYTES == 4224U,
    "Display Ingress Session frame floor"
);

static size_t expected_max_payload(
    size_t route_index,
    const MeisoDisplayIngressCapability *capability
)
{
    static const size_t fixed[MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT] = {
        MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_STATE_MAX_PAYLOAD_SIZE,
        MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_STATE_MAX_PAYLOAD_SIZE,
        0U,
        MEISO_DISPLAY_INGRESS_PROFILE_EVIDENCE_WIRE_SIZE
    };

    if (route_index == MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE &&
        capability->evidence_classes == MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_XR_FRAME_SUBMITTED)
        return MEISO_DISPLAY_INGRESS_XR_EVIDENCE_WIRE_SIZE;
    return route_index == MEISO_DISPLAY_INGRESS_ROUTE_MEDIA ?
        capability->max_frame_bytes : fixed[route_index];
}

static MeisoResourceDescriptor expected_resource(
    size_t route_index,
    const MeisoDisplayIngressCapability *capability
)
{
    MeisoResourceDescriptor value;

    memset(&value, 0, sizeof(value));
    value.id = DISPLAY_INGRESS_RESOURCE_IDS[route_index];
    value.kind = DISPLAY_INGRESS_RESOURCE_KINDS[route_index];
    value.schema = *meiso_display_ingress_resource_schema_for_format(
        (MeisoDisplayIngressProfileRoute)route_index, capability->frame_format);
    value.owner = DISPLAY_INGRESS_RESOURCE_OWNERS[route_index];
    value.max_payload_size = expected_max_payload(route_index, capability);
    return value;
}

static bool session_id_valid(MeisoSessionId value)
{
    return meiso_session_nonce_valid(value.host_nonce) &&
        meiso_session_nonce_valid(value.edge_nonce);
}

static bool capability_equal(
    const MeisoDisplayIngressCapability *lhs,
    const MeisoDisplayIngressCapability *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->max_input_width == rhs->max_input_width &&
        lhs->max_input_height == rhs->max_input_height &&
        lhs->max_frame_rate_millihz == rhs->max_frame_rate_millihz &&
        lhs->max_frame_bytes == rhs->max_frame_bytes &&
        lhs->max_fragment_payload_bytes ==
            rhs->max_fragment_payload_bytes &&
        lhs->evidence_classes == rhs->evidence_classes &&
        lhs->deadline_policy == rhs->deadline_policy &&
        lhs->frame_format == rhs->frame_format;
}

const MeisoProfileRef *meiso_display_ingress_profile_for_format(
    MeisoDisplayIngressFrameFormat format)
{
    static const MeisoProfileRef h264 = {
        {UINT64_C(0x444953504c41595f), UINT64_C(0x494e47524553535f)}, 2U
    };
    return format == MEISO_DISPLAY_INGRESS_FRAME_RGBA8888 ? &DISPLAY_INGRESS_PROFILE :
        format == MEISO_DISPLAY_INGRESS_FRAME_H264_IDR ? &h264 : NULL;
}

const MeisoSchemaRef *meiso_display_ingress_capability_schema_for_format(
    MeisoDisplayIngressFrameFormat format)
{
    static const MeisoSchemaRef h264 = {
        {UINT64_C(0x444953505f534348), UINT64_C(0x4341504142494c5f)}, 2U
    };
    return format == MEISO_DISPLAY_INGRESS_FRAME_RGBA8888 ? &DISPLAY_INGRESS_CAPABILITY_SCHEMA :
        format == MEISO_DISPLAY_INGRESS_FRAME_H264_IDR ? &h264 : NULL;
}

const MeisoSchemaRef *meiso_display_ingress_resource_schema_for_format(
    MeisoDisplayIngressProfileRoute route, MeisoDisplayIngressFrameFormat format)
{
    static const MeisoSchemaRef media = {
        {UINT64_C(0x444953505f534348), UINT64_C(0x4652414d455f5f5f)}, 2U
    };
    static const MeisoSchemaRef evidence = {
        {UINT64_C(0x444953505f534348), UINT64_C(0x505245535f455654)}, 2U
    };
    if (format > MEISO_DISPLAY_INGRESS_FRAME_H264_IDR ||
        route >= MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT) return NULL;
    if (format == MEISO_DISPLAY_INGRESS_FRAME_H264_IDR) {
        if (route == MEISO_DISPLAY_INGRESS_ROUTE_MEDIA) return &media;
        if (route == MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE) return &evidence;
    }
    return &DISPLAY_INGRESS_RESOURCE_SCHEMAS[route];
}

const MeisoProfileRef *meiso_display_ingress_profile(void)
{
    return &DISPLAY_INGRESS_PROFILE;
}

const MeisoEndpointId *meiso_display_ingress_endpoint(void)
{
    return &DISPLAY_INGRESS_ENDPOINT;
}

const MeisoSchemaRef *meiso_display_ingress_capability_schema(void)
{
    return &DISPLAY_INGRESS_CAPABILITY_SCHEMA;
}

const MeisoResourceId *meiso_display_ingress_resource_id(
    MeisoDisplayIngressProfileRoute route
)
{
    return route < MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT ?
        &DISPLAY_INGRESS_RESOURCE_IDS[route] : NULL;
}

const MeisoSchemaRef *meiso_display_ingress_resource_schema(
    MeisoDisplayIngressProfileRoute route
)
{
    return route < MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT ?
        &DISPLAY_INGRESS_RESOURCE_SCHEMAS[route] : NULL;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_configure_session(
    MeisoSessionConfig *in_out_config)
{
    return meiso_display_ingress_configure_session_for_format(
        in_out_config, MEISO_DISPLAY_INGRESS_FRAME_RGBA8888);
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_configure_session_for_format(
    MeisoSessionConfig *in_out_config, MeisoDisplayIngressFrameFormat format
)
{
    MeisoSessionConfig candidate;

    if (in_out_config == NULL) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    candidate = *in_out_config;
    const MeisoProfileRef *profile = meiso_display_ingress_profile_for_format(format);
    if (profile == NULL) return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    candidate.profile = *profile;
    candidate.limits = (MeisoSessionLimits){
        MEISO_DISPLAY_INGRESS_PROFILE_MAX_SESSION_FRAME_BYTES,
        MEISO_DISPLAY_INGRESS_PROFILE_MAX_RUNTIME_PAYLOAD_BYTES,
        MEISO_DISPLAY_INGRESS_PROFILE_MAX_REGISTRY_ENTRIES
    };
    if (meiso_session_memory_required(&candidate) == 0U) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    }
    *in_out_config = candidate;
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus
meiso_display_ingress_configure_runtime_v4(
    MeisoRuntimeV4Config *out_config,
    MeisoSession *session
)
{
    MeisoRuntimeV4Config candidate;

    if (out_config == NULL || session == NULL) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.base.session = session;
    candidate.base.max_endpoints =
        MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT;
    candidate.base.max_endpoint_history =
        MEISO_DISPLAY_INGRESS_PROFILE_MAX_ENDPOINT_HISTORY;
    candidate.base.max_routes = MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT;
    candidate.base.max_capability_bytes =
        MEISO_DISPLAY_INGRESS_PROFILE_CAPABILITY_WIRE_SIZE;
    candidate.base.max_desired_profile_bytes_per_endpoint =
        MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_WIRE_SIZE;
    candidate.base.max_actual_profile_bytes_per_endpoint =
        MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE;
    candidate.max_blob_transfer_bytes = 1U;
    if (meiso_runtime_v4_memory_required(&candidate) == 0U) {
        return MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE;
    }
    *out_config = candidate;
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

static bool build_outputs_disjoint(
    const MeisoDisplayIngressCatalogConfig *config,
    MeisoMutableBuffer capability_storage,
    MeisoRuntimeEndpointDescriptor *endpoints,
    MeisoRuntimeV4RouteDescriptor *routes
)
{
    const size_t endpoint_bytes =
        MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT * sizeof(*endpoints);
    const size_t route_bytes =
        MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT * sizeof(*routes);

    return !meiso_display_ingress_private_ranges_overlap(
            config, sizeof(*config), capability_storage.data,
            capability_storage.capacity
        ) && !meiso_display_ingress_private_ranges_overlap(
            config, sizeof(*config), endpoints, endpoint_bytes
        ) && !meiso_display_ingress_private_ranges_overlap(
            config, sizeof(*config), routes, route_bytes
        ) && !meiso_display_ingress_private_ranges_overlap(
            capability_storage.data, capability_storage.capacity,
            endpoints, endpoint_bytes
        ) && !meiso_display_ingress_private_ranges_overlap(
            capability_storage.data, capability_storage.capacity,
            routes, route_bytes
        ) && !meiso_display_ingress_private_ranges_overlap(
            endpoints, endpoint_bytes, routes, route_bytes
        );
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_private_build_catalog(
    const MeisoDisplayIngressCatalogConfig *config,
    MeisoMutableBuffer capability_storage,
    MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_capacity,
    MeisoRuntimeV4RouteDescriptor *routes,
    size_t route_capacity,
    bool allow_xr
)
{
    MeisoRuntimeEndpointDescriptor endpoint_candidate[
        MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT
    ];
    MeisoRuntimeV4RouteDescriptor route_candidate[
        MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
    ];
    uint8_t capability_candidate[
        MEISO_DISPLAY_INGRESS_PROFILE_CAPABILITY_WIRE_SIZE
    ];
    size_t capability_size = 0U;
    size_t index;

    if (config == NULL || capability_storage.data == NULL ||
        endpoints == NULL || routes == NULL) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (endpoint_capacity < MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT ||
        route_capacity < MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT ||
        capability_storage.capacity <
            MEISO_DISPLAY_INGRESS_PROFILE_CAPABILITY_WIRE_SIZE) {
        return MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY;
    }
    if (!build_outputs_disjoint(
            config, capability_storage, endpoints, routes
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (config->endpoint_incarnation == 0U || (!allow_xr &&
        (config->capability.evidence_classes & MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_XR_FRAME_SUBMITTED))) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    }
    if (meiso_display_ingress_capability_encode(
            &config->capability,
            (MeisoMutableBuffer){
                capability_candidate, sizeof(capability_candidate)
            },
            &capability_size
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        capability_size != sizeof(capability_candidate)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    }

    memset(endpoint_candidate, 0, sizeof(endpoint_candidate));
    memset(route_candidate, 0, sizeof(route_candidate));
    endpoint_candidate[0].id = DISPLAY_INGRESS_ENDPOINT;
    endpoint_candidate[0].incarnation = config->endpoint_incarnation;
    endpoint_candidate[0].capability_schema =
        *meiso_display_ingress_capability_schema_for_format(config->capability.frame_format);
    endpoint_candidate[0].capability = (MeisoByteView){
        capability_storage.data, sizeof(capability_candidate)
    };
    for (index = 0U;
         index < MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT;
         ++index) {
        route_candidate[index].route.endpoint_id = DISPLAY_INGRESS_ENDPOINT;
        route_candidate[index].route.endpoint_incarnation =
            config->endpoint_incarnation;
        route_candidate[index].route.resource = expected_resource(
            index, &config->capability
        );
        route_candidate[index].route.facet =
            DISPLAY_INGRESS_RESOURCE_FACETS[index];
        route_candidate[index].delivery =
            DISPLAY_INGRESS_DELIVERY_CLASSES[index];
    }

    memmove(capability_storage.data, capability_candidate,
            sizeof(capability_candidate));
    memmove(endpoints, endpoint_candidate, sizeof(endpoint_candidate));
    memmove(routes, route_candidate, sizeof(route_candidate));
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_private_validate_catalog(
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoRuntimeV4RouteDescriptor *routes,
    size_t route_count,
    MeisoDisplayIngressCapability *out_capability,
    bool allow_xr
)
{
    MeisoDisplayIngressCapability capability_candidate;
    size_t index;

    if (endpoints == NULL || routes == NULL || out_capability == NULL) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (endpoint_count != MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT ||
        route_count != MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT) {
        return MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH;
    }
    if (meiso_display_ingress_private_ranges_overlap(
            out_capability, sizeof(*out_capability), endpoints,
            sizeof(*endpoints)
        ) || meiso_display_ingress_private_ranges_overlap(
            out_capability, sizeof(*out_capability), routes,
            route_count * sizeof(*routes)
        ) || meiso_display_ingress_private_ranges_overlap(
            out_capability, sizeof(*out_capability),
            endpoints[0].capability.data, endpoints[0].capability.size
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (!meiso_endpoint_id_equal(endpoints[0].id, DISPLAY_INGRESS_ENDPOINT) ||
        endpoints[0].incarnation == 0U ||
        endpoints[0].capability.size !=
            MEISO_DISPLAY_INGRESS_PROFILE_CAPABILITY_WIRE_SIZE ||
        meiso_display_ingress_capability_decode(
            endpoints[0].capability, &capability_candidate
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        (!allow_xr && (capability_candidate.evidence_classes & MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_XR_FRAME_SUBMITTED)) ||
        !meiso_schema_ref_equal(endpoints[0].capability_schema,
            *meiso_display_ingress_capability_schema_for_format(capability_candidate.frame_format))) {
        return MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH;
    }
    for (index = 0U;
         index < MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT;
         ++index) {
        const MeisoRuntimeRouteDescriptor *route = &routes[index].route;
        const MeisoResourceDescriptor expected = expected_resource(
            index, &capability_candidate
        );

        if (!meiso_endpoint_id_equal(
                route->endpoint_id, DISPLAY_INGRESS_ENDPOINT
            ) || route->endpoint_incarnation != endpoints[0].incarnation ||
            route->facet != DISPLAY_INGRESS_RESOURCE_FACETS[index] ||
            !meiso_resource_descriptor_equal(&route->resource, &expected) ||
            routes[index].delivery !=
                DISPLAY_INGRESS_DELIVERY_CLASSES[index]) {
            return MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH;
        }
    }
    *out_capability = capability_candidate;
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_build_catalog(
    const MeisoDisplayIngressCatalogConfig *config, MeisoMutableBuffer storage,
    MeisoRuntimeEndpointDescriptor *ep, size_t ec, MeisoRuntimeV4RouteDescriptor *rt, size_t rc)
{
    return meiso_display_ingress_private_build_catalog(config, storage, ep, ec, rt, rc, false);
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_validate_catalog(
    const MeisoRuntimeEndpointDescriptor *ep, size_t ec, const MeisoRuntimeV4RouteDescriptor *rt, size_t rc,
    MeisoDisplayIngressCapability *out)
{
    return meiso_display_ingress_private_validate_catalog(ep, ec, rt, rc, out, false);
}

MeisoDisplayIngressProfileStatus
meiso_display_ingress_validate_actual_state(
    const MeisoDisplayIngressCapability *capability,
    const MeisoRuntimeDesiredState *applied_desired_state,
    const MeisoRuntimeActualState *state,
    MeisoDisplayIngressActualStateView *out_view
)
{
    MeisoDisplayIngressActualStateView candidate;
    MeisoDisplayIngressDesired applied_desired;
    MeisoDisplayIngressActual actual;

    if (capability == NULL || state == NULL || out_view == NULL) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (meiso_display_ingress_private_ranges_overlap(
            out_view, sizeof(*out_view), capability, sizeof(*capability)
        ) || (applied_desired_state != NULL &&
              meiso_display_ingress_private_ranges_overlap(
                  out_view, sizeof(*out_view), applied_desired_state,
                  sizeof(*applied_desired_state)
              )) || (applied_desired_state != NULL &&
              meiso_display_ingress_private_ranges_overlap(
                  out_view, sizeof(*out_view),
                  applied_desired_state->profile.data,
                  applied_desired_state->profile.size
              )) || meiso_display_ingress_private_ranges_overlap(
            out_view, sizeof(*out_view), state, sizeof(*state)
        ) || meiso_display_ingress_private_ranges_overlap(
            out_view, sizeof(*out_view), state->profile.data,
            state->profile.size
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (!meiso_display_ingress_private_capability_valid(capability) ||
        meiso_runtime_actual_state_encoded_size(state) == 0U ||
        (state->observed_desired.present &&
         state->observed_desired.owner.role != MEISO_OWNER_ROLE_HOST) ||
        (state->applied_desired.present &&
         state->applied_desired.owner.role != MEISO_OWNER_ROLE_HOST)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    }

    memset(&candidate, 0, sizeof(candidate));
    if (state->production_status == MEISO_RUNTIME_PRODUCTION_RUNNING) {
        if (!state->effective_enabled ||
            state->availability != MEISO_RUNTIME_AVAILABILITY_PRESENT ||
            state->production_epoch == 0U ||
            !state->applied_desired.present ||
            applied_desired_state == NULL ||
            !applied_desired_state->enabled ||
            applied_desired_state->profile.size !=
                MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_WIRE_SIZE ||
            meiso_runtime_desired_state_encoded_size(
                applied_desired_state
            ) != MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_STATE_MAX_PAYLOAD_SIZE ||
            meiso_display_ingress_desired_decode(
                applied_desired_state->profile, &applied_desired
            ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
            state->profile.size !=
                MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE ||
            meiso_display_ingress_actual_decode(
                state->profile, &actual
            ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
            !meiso_display_ingress_actual_matches_desired(
                capability, &applied_desired, &actual
            )) {
            return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
        }
        candidate.data_active = true;
        candidate.effective = actual;
    } else if (
        state->production_status == MEISO_RUNTIME_PRODUCTION_STOPPED ||
        state->production_status == MEISO_RUNTIME_PRODUCTION_STARTING ||
        state->production_status == MEISO_RUNTIME_PRODUCTION_FAILED
    ) {
        if (state->profile.size != 0U) {
            return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
        }
    } else {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    }
    *out_view = candidate;
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus
meiso_display_ingress_media_binding_from_runtime(
    const MeisoRuntime *runtime,
    const MeisoDisplayIngressCapability *capability,
    const MeisoRuntimeDesiredState *applied_desired_state,
    MeisoDisplayIngressMediaBinding *out_binding
)
{
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoDisplayIngressCapability catalog_capability;
    MeisoRuntimeProductionSnapshot snapshot;
    MeisoRuntimeV4ExternalMediaBinding runtime_binding;
    MeisoDisplayIngressMediaBinding candidate;
    MeisoDisplayIngressDesired applied_desired;
    MeisoDisplayIngressActual actual;
    MeisoResourceDescriptor expected_media;
    MeisoRuntimeStatus runtime_status;

    if (runtime == NULL || capability == NULL ||
        applied_desired_state == NULL || out_binding == NULL) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (meiso_display_ingress_private_ranges_overlap(
            out_binding, sizeof(*out_binding), capability,
            sizeof(*capability)
        ) || meiso_display_ingress_private_ranges_overlap(
            out_binding, sizeof(*out_binding), applied_desired_state,
            sizeof(*applied_desired_state)
        ) || meiso_display_ingress_private_ranges_overlap(
            out_binding, sizeof(*out_binding),
            applied_desired_state->profile.data,
            applied_desired_state->profile.size
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (!meiso_display_ingress_private_capability_valid(capability) ||
        !applied_desired_state->enabled ||
        applied_desired_state->profile.size !=
            MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_WIRE_SIZE ||
        meiso_runtime_desired_state_encoded_size(
            applied_desired_state
        ) != MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_STATE_MAX_PAYLOAD_SIZE ||
        meiso_display_ingress_desired_decode(
            applied_desired_state->profile, &applied_desired
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        !meiso_display_ingress_desired_supported(
            capability, &applied_desired
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    }

    runtime_status = meiso_runtime_find_endpoint(
        runtime, DISPLAY_INGRESS_ENDPOINT, &endpoint
    );
    if (runtime_status != MEISO_RUNTIME_OK) {
        return runtime_status == MEISO_RUNTIME_INVALID_ARGUMENT ?
            MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT :
            MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE;
    }
    uint32_t output_width, output_height;
    uint8_t video_layout;
    const bool application = meiso_schema_ref_equal(endpoint.capability_schema,
        *meiso_display_application_video_capability_schema());
    const bool decoded = application ? meiso_display_application_video_capability_decode(
        endpoint.capability, &catalog_capability, &output_width, &output_height, &video_layout) :
        (meiso_schema_ref_equal(endpoint.capability_schema,
            *meiso_display_ingress_capability_schema_for_format(capability->frame_format)) &&
         meiso_display_ingress_capability_decode(endpoint.capability, &catalog_capability) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    if (!meiso_endpoint_id_equal(endpoint.id, DISPLAY_INGRESS_ENDPOINT) ||
        endpoint.incarnation == 0U || !decoded || !capability_equal(&catalog_capability, capability)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH;
    }
    expected_media = expected_resource(
        MEISO_DISPLAY_INGRESS_ROUTE_MEDIA, &catalog_capability
    );

    runtime_status = meiso_runtime_production_snapshot(
        runtime, DISPLAY_INGRESS_ENDPOINT, &snapshot
    );
    if (runtime_status != MEISO_RUNTIME_OK) {
        return runtime_status == MEISO_RUNTIME_INVALID_ARGUMENT ?
            MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT :
            MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE;
    }
    if (!snapshot.actual_present || !snapshot.data_active ||
        snapshot.production_status != MEISO_RUNTIME_PRODUCTION_RUNNING ||
        !snapshot.effective_enabled ||
        snapshot.availability != MEISO_RUNTIME_AVAILABILITY_PRESENT ||
        snapshot.production_epoch == 0U ||
        !snapshot.applied_desired.present ||
        snapshot.applied_desired.owner.role != MEISO_OWNER_ROLE_HOST ||
        snapshot.effective_profile.size !=
            MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE ||
        meiso_display_ingress_actual_decode(
            snapshot.effective_profile, &actual
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        !meiso_display_ingress_actual_matches_desired(
            capability, &applied_desired, &actual
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE;
    }
    runtime_status = meiso_runtime_v4_external_media_binding(
        runtime, DISPLAY_INGRESS_RESOURCE_IDS[MEISO_DISPLAY_INGRESS_ROUTE_MEDIA],
        &runtime_binding
    );
    if (runtime_status != MEISO_RUNTIME_OK) {
        return runtime_status == MEISO_RUNTIME_INVALID_ARGUMENT ?
            MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT :
            MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE;
    }
    if (!session_id_valid(runtime_binding.session_id) ||
        runtime_binding.route.catalog_generation == 0U ||
        !meiso_endpoint_id_equal(
            runtime_binding.route.endpoint_id, DISPLAY_INGRESS_ENDPOINT
        ) || runtime_binding.route.endpoint_incarnation !=
            endpoint.incarnation ||
        !meiso_resource_id_equal(
            runtime_binding.route.resource_id,
            DISPLAY_INGRESS_RESOURCE_IDS[MEISO_DISPLAY_INGRESS_ROUTE_MEDIA]
        ) || runtime_binding.route.facet != MEISO_RUNTIME_FACET_DATA ||
        !meiso_resource_descriptor_equal(
            &runtime_binding.resource, &expected_media
        ) || runtime_binding.production_epoch != snapshot.production_epoch) {
        return MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH;
    }

    memset(&candidate, 0, sizeof(candidate));
    candidate.runtime = runtime_binding;
    candidate.media_stream_epoch = runtime_binding.production_epoch;
    candidate.channel_nonce = actual.channel_nonce;
    candidate.producer = snapshot.applied_desired.owner;
    *out_binding = candidate;
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}
