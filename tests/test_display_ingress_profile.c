#include <meiso/profiles/display_ingress/profile.h>

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(                                                   \
                stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition                               \
            );                                                               \
            return false;                                                    \
        }                                                                    \
    } while (0)

typedef union {
    max_align_t alignment;
    uint8_t bytes[65536U];
} SessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[65536U];
} RuntimeArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[2048U];
} StateArena;

typedef struct {
    uint8_t capability[MEISO_DISPLAY_INGRESS_PROFILE_CAPABILITY_WIRE_SIZE];
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT
    ];
    MeisoRuntimeV4RouteDescriptor routes[
        MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
    ];
} Catalog;

typedef struct {
    SessionArena host_session_arena;
    SessionArena edge_session_arena;
    RuntimeArena host_runtime_arena;
    RuntimeArena edge_runtime_arena;
    uint8_t transfer[MEISO_DISPLAY_INGRESS_PROFILE_MAX_SESSION_FRAME_BYTES];
    MeisoSession *host_session;
    MeisoSession *edge_session;
    MeisoRuntime *host_runtime;
    MeisoRuntime *edge_runtime;
    size_t host_session_size;
    size_t edge_session_size;
    size_t host_runtime_size;
    size_t edge_runtime_size;
} RuntimePair;

typedef struct {
    StateArena desired_arena;
    StateArena actual_arena;
    MeisoStateReplica *desired;
    MeisoStateReplica *actual;
} Replicas;

typedef struct {
    uint8_t profile[MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_WIRE_SIZE];
    MeisoRuntimeDesiredState runtime;
} DesiredStateFixture;

static const MeisoOwnerScope HOST_OWNER = {MEISO_OWNER_ROLE_HOST, 501U};
static const MeisoOwnerScope EDGE_OWNER = {MEISO_OWNER_ROLE_EDGE, 601U};

static bool bytes_are(const uint8_t *bytes, uint8_t value, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

static bool capability_equal(
    const MeisoDisplayIngressCapability *lhs,
    const MeisoDisplayIngressCapability *rhs
)
{
    return lhs->max_input_width == rhs->max_input_width &&
        lhs->max_input_height == rhs->max_input_height &&
        lhs->max_frame_rate_millihz == rhs->max_frame_rate_millihz &&
        lhs->max_frame_bytes == rhs->max_frame_bytes &&
        lhs->max_fragment_payload_bytes ==
            rhs->max_fragment_payload_bytes &&
        lhs->evidence_classes == rhs->evidence_classes &&
        lhs->deadline_policy == rhs->deadline_policy;
}

static bool desired_equal(
    const MeisoDisplayIngressDesired *lhs,
    const MeisoDisplayIngressDesired *rhs
)
{
    return lhs->input_width == rhs->input_width &&
        lhs->input_height == rhs->input_height &&
        lhs->maximum_frame_rate_millihz ==
            rhs->maximum_frame_rate_millihz;
}

static bool actual_equal(
    const MeisoDisplayIngressActual *lhs,
    const MeisoDisplayIngressActual *rhs
)
{
    return lhs->effective_input_width == rhs->effective_input_width &&
        lhs->effective_input_height == rhs->effective_input_height &&
        lhs->effective_maximum_frame_rate_millihz ==
            rhs->effective_maximum_frame_rate_millihz &&
        lhs->channel_nonce.high == rhs->channel_nonce.high &&
        lhs->channel_nonce.low == rhs->channel_nonce.low;
}

static bool binding_equal(
    const MeisoDisplayIngressMediaBinding *lhs,
    const MeisoDisplayIngressMediaBinding *rhs
)
{
    return meiso_session_id_equal(
            lhs->runtime.session_id, rhs->runtime.session_id
        ) && lhs->runtime.route.catalog_generation ==
            rhs->runtime.route.catalog_generation &&
        meiso_endpoint_id_equal(
            lhs->runtime.route.endpoint_id,
            rhs->runtime.route.endpoint_id
        ) && lhs->runtime.route.endpoint_incarnation ==
            rhs->runtime.route.endpoint_incarnation &&
        meiso_resource_id_equal(
            lhs->runtime.route.resource_id,
            rhs->runtime.route.resource_id
        ) && lhs->runtime.route.facet == rhs->runtime.route.facet &&
        meiso_resource_descriptor_equal(
            &lhs->runtime.resource, &rhs->runtime.resource
        ) && lhs->runtime.production_epoch ==
            rhs->runtime.production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        lhs->channel_nonce.high == rhs->channel_nonce.high &&
        lhs->channel_nonce.low == rhs->channel_nonce.low &&
        meiso_owner_scope_equal(lhs->producer, rhs->producer);
}

static MeisoDisplayIngressCapability codec_capability(void)
{
    const MeisoDisplayIngressCapability value = {
        64U,
        32U,
        60000U,
        8192U,
        1024U,
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_ALL,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        MEISO_DISPLAY_INGRESS_FRAME_RGBA8888
    };
    return value;
}

static MeisoDisplayIngressCapability fixture_capability(void)
{
    MeisoDisplayIngressCapability value = codec_capability();

    value.evidence_classes =
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_SIMULATED;
    return value;
}

static MeisoDisplayIngressDesired desired_one(void)
{
    const MeisoDisplayIngressDesired value = {64U, 32U, 30000U};
    return value;
}

static MeisoDisplayIngressDesired desired_two(void)
{
    const MeisoDisplayIngressDesired value = {32U, 32U, 60000U};
    return value;
}

static bool desired_state_fixture_init(
    DesiredStateFixture *fixture,
    bool enabled,
    const MeisoDisplayIngressDesired *desired
)
{
    size_t profile_size = 0U;

    if (fixture == NULL || desired == NULL) {
        return false;
    }
    memset(fixture, 0, sizeof(*fixture));
    if (meiso_display_ingress_desired_encode(
            desired,
            (MeisoMutableBuffer){fixture->profile, sizeof(fixture->profile)},
            &profile_size
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        profile_size != sizeof(fixture->profile)) {
        return false;
    }
    fixture->runtime.enabled = enabled;
    fixture->runtime.profile = (MeisoByteView){
        fixture->profile, sizeof(fixture->profile)
    };
    return true;
}

static MeisoDisplayIngressActual actual_for(
    MeisoDisplayIngressDesired desired,
    MeisoSessionNonce nonce
)
{
    const MeisoDisplayIngressActual value = {
        desired.input_width,
        desired.input_height,
        desired.maximum_frame_rate_millihz,
        nonce
    };
    return value;
}

static bool test_canonical_codecs(void)
{
    static const uint8_t expected_capability[32U] = {
        0x00U, 0x00U, 0x00U, 0x40U,
        0x00U, 0x00U, 0x00U, 0x20U,
        0x00U, 0x00U, 0xeaU, 0x60U,
        0x00U, 0x00U, 0x20U, 0x00U,
        0x00U, 0x00U, 0x04U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x03U,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U
    };
    static const uint8_t expected_desired[12U] = {
        0x00U, 0x00U, 0x00U, 0x40U,
        0x00U, 0x00U, 0x00U, 0x20U,
        0x00U, 0x00U, 0x75U, 0x30U
    };
    static const uint8_t expected_actual[32U] = {
        0x00U, 0x00U, 0x00U, 0x40U,
        0x00U, 0x00U, 0x00U, 0x20U,
        0x00U, 0x00U, 0x75U, 0x30U,
        0x00U, 0x00U, 0x00U, 0x00U,
        0x01U, 0x02U, 0x03U, 0x04U,
        0x05U, 0x06U, 0x07U, 0x08U,
        0x11U, 0x12U, 0x13U, 0x14U,
        0x15U, 0x16U, 0x17U, 0x18U
    };
    const MeisoDisplayIngressCapability cap = codec_capability();
    const MeisoDisplayIngressDesired desired = desired_one();
    const MeisoDisplayIngressActual actual = actual_for(
        desired,
        (MeisoSessionNonce){
            UINT64_C(0x0102030405060708),
            UINT64_C(0x1112131415161718)
        }
    );
    MeisoDisplayIngressPresentationEvidence evidence;
    MeisoDisplayIngressCapability decoded_capability;
    MeisoDisplayIngressDesired decoded_desired;
    MeisoDisplayIngressActual decoded_actual;
    MeisoDisplayIngressPresentationEvidence decoded_evidence;
    MeisoDisplayIngressActualStateView state_view;
    MeisoRuntimeActualState stopped;
    DesiredStateFixture applied_desired;
    uint8_t wire[MEISO_DISPLAY_INGRESS_PROFILE_EVIDENCE_WIRE_SIZE];
    uint8_t before[MEISO_DISPLAY_INGRESS_PROFILE_EVIDENCE_WIRE_SIZE];
    size_t size;
    size_t index;

    memset(wire, 0xa5, sizeof(wire));
    size = 99U;
    CHECK(meiso_display_ingress_capability_encode(
        &cap, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(size == sizeof(expected_capability) &&
          memcmp(wire, expected_capability, size) == 0);
    memset(&decoded_capability, 0, sizeof(decoded_capability));
    CHECK(meiso_display_ingress_capability_decode(
        (MeisoByteView){wire, size}, &decoded_capability
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(capability_equal(&decoded_capability, &cap));

    size = 99U;
    CHECK(meiso_display_ingress_desired_encode(
        &desired, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(size == sizeof(expected_desired) &&
          memcmp(wire, expected_desired, size) == 0);
    CHECK(meiso_display_ingress_desired_decode(
        (MeisoByteView){wire, size}, &decoded_desired
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(desired_equal(&decoded_desired, &desired));

    size = 99U;
    CHECK(meiso_display_ingress_actual_encode(
        &actual, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(size == sizeof(expected_actual) &&
          memcmp(wire, expected_actual, size) == 0);
    CHECK(meiso_display_ingress_actual_decode(
        (MeisoByteView){wire, size}, &decoded_actual
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(actual_equal(&decoded_actual, &actual));

    memset(&evidence, 0, sizeof(evidence));
    evidence.evidence_class = MEISO_DISPLAY_INGRESS_EVIDENCE_SIMULATED;
    evidence.connector_kind = MEISO_DISPLAY_INGRESS_CONNECTOR_UNSPECIFIED;
    evidence.output_width = 64U;
    evidence.output_height = 32U;
    evidence.media_stream_epoch = 101U;
    evidence.displayed_frame_sequence = 7U;
    for (index = 0U; index < sizeof(evidence.input_rgba_sha256); ++index) {
        evidence.input_rgba_sha256[index] = (uint8_t)index;
    }
    size = 99U;
    CHECK(meiso_display_ingress_evidence_encode(
        &evidence, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(size == MEISO_DISPLAY_INGRESS_PROFILE_EVIDENCE_WIRE_SIZE &&
          wire[0] == MEISO_DISPLAY_INGRESS_EVIDENCE_SIMULATED &&
          wire[1] == MEISO_DISPLAY_INGRESS_CONNECTOR_UNSPECIFIED &&
          bytes_are(wire + 2U, 0U, 2U) &&
          memcmp(wire + 4U, expected_capability, 4U) == 0 &&
          memcmp(wire + 8U, expected_capability + 4U, 4U) == 0 &&
          bytes_are(wire + 12U, 0U, 4U) &&
          wire[23U] == 101U && wire[31U] == 7U &&
          memcmp(wire + 32U, evidence.input_rgba_sha256, 32U) == 0);
    CHECK(meiso_display_ingress_evidence_decode(
        (MeisoByteView){wire, size}, &decoded_evidence
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(decoded_evidence.evidence_class == evidence.evidence_class &&
          decoded_evidence.connector_kind == evidence.connector_kind &&
          decoded_evidence.output_width == evidence.output_width &&
          decoded_evidence.output_height == evidence.output_height &&
          decoded_evidence.media_stream_epoch == evidence.media_stream_epoch &&
          decoded_evidence.displayed_frame_sequence ==
              evidence.displayed_frame_sequence &&
          memcmp(decoded_evidence.input_rgba_sha256,
                 evidence.input_rgba_sha256, 32U) == 0);
    CHECK(meiso_display_ingress_evidence_supported(&cap, &evidence, 101U));
    CHECK(!meiso_display_ingress_evidence_supported(&cap, &evidence, 102U));
    CHECK(desired_state_fixture_init(&applied_desired, true, &desired));

    memmove(before, wire, sizeof(before));
    size = 99U;
    CHECK(meiso_display_ingress_capability_encode(
        &cap, (MeisoMutableBuffer){wire, 31U}, &size
    ) == MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY);
    CHECK(size == 0U && memcmp(wire, before, sizeof(wire)) == 0);
    memset(&decoded_capability, 0xa5, sizeof(decoded_capability));
    CHECK(meiso_display_ingress_capability_decode(
        (MeisoByteView){expected_capability, 31U}, &decoded_capability
    ) == MEISO_DISPLAY_INGRESS_PROFILE_TRUNCATED);
    CHECK(bytes_are(
        (const uint8_t *)&decoded_capability, 0xa5,
        sizeof(decoded_capability)
    ));
    memmove(wire, expected_capability, sizeof(expected_capability));
    wire[31U] = 1U;
    CHECK(meiso_display_ingress_capability_decode(
        (MeisoByteView){wire, 32U}, &decoded_capability
    ) == MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED);
    CHECK(bytes_are(
        (const uint8_t *)&decoded_capability, 0xa5,
        sizeof(decoded_capability)
    ));

    {
        MeisoDisplayIngressCapability aliased = cap;
        const MeisoDisplayIngressCapability aliased_before = aliased;
        size = 88U;
        CHECK(meiso_display_ingress_capability_encode(
            &aliased,
            (MeisoMutableBuffer){
                (uint8_t *)(void *)&aliased, sizeof(aliased)
            },
            &size
        ) == MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT);
        CHECK(size == 88U && memcmp(
            &aliased, &aliased_before, sizeof(aliased)
        ) == 0);
    }
    memmove(wire, expected_desired, sizeof(expected_desired));
    memmove(before, wire, sizeof(wire));
    CHECK(meiso_display_ingress_desired_decode(
        (MeisoByteView){wire, sizeof(expected_desired)},
        (MeisoDisplayIngressDesired *)(void *)wire
    ) == MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT);
    CHECK(memcmp(wire, before, sizeof(wire)) == 0);
    memmove(wire, expected_actual, sizeof(expected_actual));
    memmove(before, wire, sizeof(wire));
    CHECK(meiso_display_ingress_actual_decode(
        (MeisoByteView){wire, sizeof(expected_actual)},
        (MeisoDisplayIngressActual *)(void *)wire
    ) == MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT);
    CHECK(memcmp(wire, before, sizeof(wire)) == 0);
    CHECK(meiso_display_ingress_evidence_decode(
        (MeisoByteView){wire, sizeof(wire)},
        (MeisoDisplayIngressPresentationEvidence *)(void *)wire
    ) == MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT);

    memset(&stopped, 0, sizeof(stopped));
    stopped.apply_result = MEISO_RUNTIME_APPLY_NONE;
    stopped.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    stopped.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    stopped.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    memset(&state_view, 0xa5, sizeof(state_view));
    CHECK(meiso_display_ingress_validate_actual_state(
        &cap, NULL, &stopped, &state_view
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(bytes_are((const uint8_t *)&state_view, 0U, sizeof(state_view)));
    stopped.production_status = MEISO_RUNTIME_PRODUCTION_DEGRADED;
    stopped.effective_enabled = true;
    stopped.production_epoch = 1U;
    stopped.observed_desired = (MeisoRuntimeDesiredReference){
        true, HOST_OWNER, 1U
    };
    stopped.applied_desired = stopped.observed_desired;
    stopped.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    memset(&state_view, 0xa5, sizeof(state_view));
    CHECK(meiso_display_ingress_validate_actual_state(
        &cap, &applied_desired.runtime, &stopped, &state_view
    ) == MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE);
    CHECK(bytes_are(
        (const uint8_t *)&state_view, 0xa5, sizeof(state_view)
    ));
    stopped.production_status = MEISO_RUNTIME_PRODUCTION_STOPPING;
    memset(&state_view, 0xa5, sizeof(state_view));
    CHECK(meiso_display_ingress_validate_actual_state(
        &cap, &applied_desired.runtime, &stopped, &state_view
    ) == MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE);
    CHECK(bytes_are(
        (const uint8_t *)&state_view, 0xa5, sizeof(state_view)
    ));
    return true;
}

static bool build_catalog(Catalog *catalog)
{
    const MeisoDisplayIngressCatalogConfig config = {
        77U, fixture_capability()
    };

    memset(catalog, 0, sizeof(*catalog));
    CHECK(meiso_display_ingress_build_catalog(
        &config,
        (MeisoMutableBuffer){catalog->capability, sizeof(catalog->capability)},
        catalog->endpoints, MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT,
        catalog->routes, MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    return true;
}

static bool test_catalog_contract(Catalog *catalog)
{
    MeisoDisplayIngressCapability decoded;
    MeisoRuntimeV4RouteDescriptor altered[
        MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
    ];
    const MeisoDisplayIngressCapability cap = fixture_capability();
    MeisoDisplayIngressCatalogConfig invalid = {0U, fixture_capability()};
    Catalog untouched;

    CHECK(build_catalog(catalog));
    CHECK(meiso_display_ingress_validate_catalog(
        catalog->endpoints, MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT,
        catalog->routes, MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT, &decoded
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(capability_equal(&decoded, &cap));
    CHECK(catalog->routes[MEISO_DISPLAY_INGRESS_ROUTE_DESIRED].delivery ==
              MEISO_RUNTIME_DELIVERY_INLINE &&
          catalog->routes[MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL].delivery ==
              MEISO_RUNTIME_DELIVERY_INLINE &&
          catalog->routes[MEISO_DISPLAY_INGRESS_ROUTE_MEDIA]
                  .route.resource.kind == MEISO_RESOURCE_KIND_STREAM &&
          catalog->routes[MEISO_DISPLAY_INGRESS_ROUTE_MEDIA]
                  .route.facet == MEISO_RUNTIME_FACET_DATA &&
          catalog->routes[MEISO_DISPLAY_INGRESS_ROUTE_MEDIA]
                  .route.resource.owner == MEISO_OWNER_ROLE_HOST &&
          catalog->routes[MEISO_DISPLAY_INGRESS_ROUTE_MEDIA].delivery ==
              MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA &&
          catalog->routes[
              MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE
          ].route.resource.kind == MEISO_RESOURCE_KIND_STREAM &&
          catalog->routes[
              MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE
          ].route.facet == MEISO_RUNTIME_FACET_DATA &&
          catalog->routes[
              MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE
          ].route.resource.owner == MEISO_OWNER_ROLE_EDGE &&
          catalog->routes[
              MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE
          ].delivery == MEISO_RUNTIME_DELIVERY_INLINE);
    CHECK(catalog->routes[MEISO_DISPLAY_INGRESS_ROUTE_MEDIA]
              .route.resource.max_payload_size == cap.max_frame_bytes);

    memmove(altered, catalog->routes, sizeof(altered));
    altered[MEISO_DISPLAY_INGRESS_ROUTE_MEDIA].delivery =
        MEISO_RUNTIME_DELIVERY_INLINE;
    memset(&decoded, 0xa5, sizeof(decoded));
    CHECK(meiso_display_ingress_validate_catalog(
        catalog->endpoints, 1U, altered, 4U, &decoded
    ) == MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH);
    CHECK(bytes_are((const uint8_t *)&decoded, 0xa5, sizeof(decoded)));

    memmove(altered, catalog->routes, sizeof(altered));
    {
        const MeisoRuntimeV4RouteDescriptor first = altered[0];
        altered[0] = altered[1];
        altered[1] = first;
    }
    memset(&decoded, 0xa5, sizeof(decoded));
    CHECK(meiso_display_ingress_validate_catalog(
        catalog->endpoints, 1U, altered, 4U, &decoded
    ) == MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH);
    CHECK(bytes_are((const uint8_t *)&decoded, 0xa5, sizeof(decoded)));

    memset(&untouched, 0x5a, sizeof(untouched));
    CHECK(meiso_display_ingress_build_catalog(
        &invalid,
        (MeisoMutableBuffer){
            untouched.capability, sizeof(untouched.capability)
        },
        untouched.endpoints, 1U, untouched.routes, 4U
    ) == MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE);
    CHECK(bytes_are((const uint8_t *)&untouched, 0x5a, sizeof(untouched)));
    CHECK(MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_WIRE_SIZE == 428U &&
          MEISO_DISPLAY_INGRESS_PROFILE_MAX_RUNTIME_PAYLOAD_BYTES == 4180U &&
          MEISO_DISPLAY_INGRESS_PROFILE_MAX_SESSION_FRAME_BYTES == 4224U &&
          MEISO_DISPLAY_INGRESS_PROFILE_MAX_REGISTRY_ENTRIES == 5U);
    return true;
}

static MeisoSessionConfig session_config(
    MeisoSessionRole role,
    uint64_t nonce_salt
)
{
    MeisoSessionConfig value;
    const MeisoPeerId host = {11U, 12U};
    const MeisoPeerId edge = {21U, 22U};

    memset(&value, 0, sizeof(value));
    value.local_role = role;
    value.local_peer_id = role == MEISO_SESSION_ROLE_HOST ? host : edge;
    value.expected_peer_id = role == MEISO_SESSION_ROLE_HOST ? edge : host;
    value.local_incarnation = role == MEISO_SESSION_ROLE_HOST ? 31U : 41U;
    value.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){61U, nonce_salt + 1U} :
        (MeisoSessionNonce){71U, nonce_salt + 2U};
    value.protocol = (MeisoProtocolVersion){1U, 0U};
    if (meiso_display_ingress_configure_session(&value) !=
        MEISO_DISPLAY_INGRESS_PROFILE_OK) {
        memset(&value, 0, sizeof(value));
    }
    return value;
}

static bool init_session(
    SessionArena *arena,
    const MeisoSessionConfig *config,
    MeisoSession **out_session,
    size_t *out_size
)
{
    const size_t required = meiso_session_memory_required(config);

    CHECK(required != 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_session_init(
        arena->bytes, required, config, out_session
    ) == MEISO_SESSION_INIT_OK);
    *out_size = required;
    return true;
}

static bool handshake(MeisoSession *sender, MeisoSession *receiver)
{
    uint8_t frame[MEISO_DISPLAY_INGRESS_PROFILE_MAX_SESSION_FRAME_BYTES];
    MeisoByteView outbound;
    MeisoSessionRuntimeMessage ignored = {0};

    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(frame));
    memmove(frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver, (MeisoByteView){frame, outbound.size}, &ignored
    ) == MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS);
    return true;
}

static bool init_runtime(
    RuntimeArena *arena,
    MeisoSession *session,
    MeisoRuntime **out_runtime,
    size_t *out_size
)
{
    MeisoRuntimeV4Config config;
    const MeisoDisplayIngressProfileStatus status =
        meiso_display_ingress_configure_runtime_v4(&config, session);
    size_t required;

    CHECK(status == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(config.base.session == session && config.base.max_endpoints == 1U &&
          config.base.max_endpoint_history == 2U &&
          config.base.max_routes == 4U &&
          config.base.max_capability_bytes == 32U &&
          config.base.max_desired_profile_bytes_per_endpoint == 12U &&
          config.base.max_actual_profile_bytes_per_endpoint == 32U &&
          config.max_blob_transfer_bytes == 1U);
    required = meiso_runtime_v4_memory_required(&config);
    CHECK(required != 0U && required <= sizeof(arena->bytes));
    memset(arena->bytes, 0xa5, sizeof(arena->bytes));
    CHECK(meiso_runtime_v4_init(
        arena->bytes, required, &config, out_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    CHECK(bytes_are(
        arena->bytes + required, 0xa5, sizeof(arena->bytes) - required
    ));
    *out_size = required;
    return true;
}

static bool init_pair(RuntimePair *pair, uint64_t nonce_salt)
{
    const MeisoSessionConfig host = session_config(
        MEISO_SESSION_ROLE_HOST, nonce_salt
    );
    const MeisoSessionConfig edge = session_config(
        MEISO_SESSION_ROLE_EDGE, nonce_salt
    );

    memset(pair, 0, sizeof(*pair));
    CHECK(host.limits.max_frame_bytes == 4224U &&
          host.limits.max_runtime_payload_bytes == 4180U &&
          host.limits.max_registry_entries == 5U);
    CHECK(init_session(
        &pair->host_session_arena, &host, &pair->host_session,
        &pair->host_session_size
    ));
    CHECK(init_session(
        &pair->edge_session_arena, &edge, &pair->edge_session,
        &pair->edge_session_size
    ));
    CHECK(meiso_session_start(pair->edge_session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(pair->host_session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(handshake(pair->host_session, pair->edge_session));
    CHECK(handshake(pair->edge_session, pair->host_session));
    CHECK(handshake(pair->host_session, pair->edge_session));
    CHECK(handshake(pair->edge_session, pair->host_session));
    CHECK(init_runtime(
        &pair->host_runtime_arena, pair->host_session, &pair->host_runtime,
        &pair->host_runtime_size
    ));
    CHECK(init_runtime(
        &pair->edge_runtime_arena, pair->edge_session, &pair->edge_runtime,
        &pair->edge_runtime_size
    ));
    return true;
}

static bool pull_message(
    RuntimePair *pair,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoSessionRuntimeMessage *out_message
)
{
    MeisoByteView outbound;

    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(pair->transfer));
    memmove(pair->transfer, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver, (MeisoByteView){pair->transfer, outbound.size}, out_message
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    return true;
}

static bool transfer_runtime(
    RuntimePair *pair,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoRuntime *receiver_runtime,
    MeisoRuntimeEvent *out_event
)
{
    MeisoSessionRuntimeMessage message = {0};

    CHECK(pull_message(pair, sender, receiver, &message));
    CHECK(meiso_runtime_receive(receiver_runtime, &message, out_event) ==
          MEISO_RUNTIME_OK);
    return true;
}

static MeisoRuntimeRouteKey route_key(
    const Catalog *catalog,
    size_t route_index
)
{
    const MeisoRuntimeRouteDescriptor *route =
        &catalog->routes[route_index].route;
    const MeisoRuntimeRouteKey value = {
        1U,
        route->endpoint_id,
        route->endpoint_incarnation,
        route->resource.id,
        route->facet
    };
    return value;
}

static bool accept_catalog(RuntimePair *pair, const Catalog *catalog)
{
    const MeisoDisplayIngressCapability cap = fixture_capability();
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeV4RouteDescriptor routes[
        MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
    ];
    MeisoDisplayIngressCapability decoded;
    MeisoRuntimeEvent event;
    MeisoSessionRuntimeMessage message = {0};
    size_t index;

    CHECK(meiso_runtime_v4_offer_catalog(
        pair->edge_runtime, 1U, catalog->endpoints, 1U, catalog->routes, 4U
    ) == MEISO_RUNTIME_OK);
    CHECK(pull_message(
        pair, pair->edge_session, pair->host_session, &message
    ));
    CHECK(message.payload.size ==
          MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_WIRE_SIZE);
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED &&
          event.catalog_generation == 1U);
    CHECK(meiso_runtime_staged_endpoint_at(
        pair->host_runtime, 0U, &endpoint
    ) == MEISO_RUNTIME_OK);
    for (index = 0U;
         index < MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT;
         ++index) {
        CHECK(meiso_runtime_v4_staged_route_at(
            pair->host_runtime, index, &routes[index]
        ) == MEISO_RUNTIME_OK);
    }
    CHECK(meiso_display_ingress_validate_catalog(
        &endpoint, 1U, routes, 4U, &decoded
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(capability_equal(&decoded, &cap));
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
          event.catalog_generation == 1U);
    return true;
}

static bool init_replica(
    StateArena *arena,
    const MeisoResourceDescriptor *descriptor,
    MeisoOwnerScope owner,
    MeisoStateReplica **out_replica
)
{
    const size_t required = meiso_state_replica_memory_required(descriptor);

    CHECK(required != 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_state_replica_init(
        arena->bytes, required, descriptor, out_replica
    ) == MEISO_STATE_INIT_OK);
    CHECK(meiso_state_replica_begin_owner(*out_replica, owner) ==
          MEISO_STATE_OWNER_STARTED);
    return true;
}

static bool init_replicas(Replicas *replicas, const Catalog *catalog)
{
    memset(replicas, 0, sizeof(*replicas));
    CHECK(init_replica(
        &replicas->desired_arena,
        &catalog->routes[MEISO_DISPLAY_INGRESS_ROUTE_DESIRED].route.resource,
        HOST_OWNER, &replicas->desired
    ));
    CHECK(init_replica(
        &replicas->actual_arena,
        &catalog->routes[MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL].route.resource,
        EDGE_OWNER, &replicas->actual
    ));
    return true;
}

static bool deliver_desired(
    RuntimePair *pair,
    const Catalog *catalog,
    MeisoStateReplica *replica,
    uint64_t version,
    const MeisoRuntimeDesiredState *desired_state
)
{
    const MeisoDisplayIngressCapability cap = fixture_capability();
    uint8_t runtime_wire[
        MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView runtime_payload;
    MeisoRuntimeDesiredState decoded_runtime;
    MeisoDisplayIngressDesired expected_profile;
    MeisoDisplayIngressDesired decoded_profile;
    MeisoStateOffer offer;
    MeisoRuntimeEvent event;
    MeisoStateReceiveStatus receive_status;
    const MeisoRuntimeRouteKey key = route_key(
        catalog, MEISO_DISPLAY_INGRESS_ROUTE_DESIRED
    );

    CHECK(desired_state != NULL &&
          meiso_display_ingress_desired_decode(
              desired_state->profile, &expected_profile
          ) == MEISO_DISPLAY_INGRESS_PROFILE_OK &&
          meiso_display_ingress_desired_supported(&cap, &expected_profile));
    CHECK(meiso_runtime_desired_state_encode(
        desired_state,
        (MeisoMutableBuffer){runtime_wire, sizeof(runtime_wire)},
        &runtime_payload
    ));
    offer = (MeisoStateOffer){
        {version}, {HOST_OWNER, version, runtime_payload}
    };
    CHECK(meiso_runtime_send_state_offer(
        pair->host_runtime, key, &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
          meiso_runtime_desired_state_decode(
              event.state_offer.state.payload, &decoded_runtime
          ) && decoded_runtime.enabled == desired_state->enabled &&
          meiso_display_ingress_desired_decode(
              decoded_runtime.profile, &decoded_profile
          ) == MEISO_DISPLAY_INGRESS_PROFILE_OK &&
          desired_equal(&decoded_profile, &expected_profile) &&
          meiso_display_ingress_desired_supported(
              &cap, &decoded_profile
          ));
    receive_status = meiso_state_replica_receive(
        replica, event.state_offer.state.owner,
        event.state_offer.state.version, event.state_offer.state.payload
    );
    CHECK(receive_status == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_runtime_commit_state_receive(
        pair->edge_runtime, event.route, &event.state_offer, receive_status
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_state_accept(
        pair->edge_runtime, event.route, event.state_offer.generation,
        event.state_offer.state.owner, event.state_offer.state.version
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT);
    return true;
}

static bool encode_runtime_actual(
    const MeisoRuntimeActualState *actual,
    uint8_t profile_wire[MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE],
    uint8_t runtime_wire[
        MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ],
    MeisoByteView *out_payload
)
{
    MeisoRuntimeActualState candidate = *actual;
    size_t profile_size = 0U;
    MeisoDisplayIngressActual profile;

    CHECK(actual->profile.size == sizeof(profile));
    memmove(&profile, actual->profile.data, sizeof(profile));
    CHECK(meiso_display_ingress_actual_encode(
        &profile,
        (MeisoMutableBuffer){
            profile_wire, MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE
        },
        &profile_size
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    candidate.profile = (MeisoByteView){profile_wire, profile_size};
    CHECK(meiso_runtime_actual_state_encode(
        &candidate,
        (MeisoMutableBuffer){
            runtime_wire,
            MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_STATE_MAX_PAYLOAD_SIZE
        },
        out_payload
    ));
    return true;
}

static MeisoRuntimeActualState running_actual(
    uint64_t observed_version,
    uint64_t applied_version,
    MeisoRuntimeApplyResult apply_result,
    MeisoRuntimeProductionReason reason,
    uint64_t production_epoch,
    const MeisoDisplayIngressActual *profile
)
{
    MeisoRuntimeActualState value;

    memset(&value, 0, sizeof(value));
    value.observed_desired = (MeisoRuntimeDesiredReference){
        true, HOST_OWNER, observed_version
    };
    value.applied_desired = (MeisoRuntimeDesiredReference){
        true, HOST_OWNER, applied_version
    };
    value.apply_result = apply_result;
    value.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    value.effective_enabled = true;
    value.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    value.reason = reason;
    value.production_epoch = production_epoch;
    value.profile = (MeisoByteView){
        (const uint8_t *)(const void *)profile, sizeof(*profile)
    };
    return value;
}

static bool deliver_actual(
    RuntimePair *pair,
    const Catalog *catalog,
    MeisoStateReplica *replica,
    uint64_t state_version,
    const MeisoRuntimeActualState *actual,
    const MeisoRuntimeDesiredState *applied_desired_state,
    MeisoDisplayIngressMediaBinding *out_host_binding
)
{
    uint8_t profile_wire[MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE];
    uint8_t runtime_wire[
        MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView runtime_payload;
    MeisoRuntimeActualState wire_actual;
    MeisoRuntimeActualState decoded;
    MeisoDisplayIngressActualStateView view;
    MeisoStateOffer offer;
    MeisoRuntimeEvent event;
    MeisoStateReceiveStatus receive_status;
    const MeisoDisplayIngressCapability cap = fixture_capability();
    const MeisoRuntimeRouteKey key = route_key(
        catalog, MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL
    );

    CHECK(encode_runtime_actual(
        actual, profile_wire, runtime_wire, &runtime_payload
    ));
    wire_actual = *actual;
    wire_actual.profile = (MeisoByteView){
        profile_wire, MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE
    };
    CHECK(meiso_display_ingress_validate_actual_state(
        &cap, applied_desired_state, &wire_actual, &view
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK && view.data_active);
    offer = (MeisoStateOffer){
        {state_version}, {EDGE_OWNER, state_version, runtime_payload}
    };
    CHECK(meiso_runtime_send_state_offer(
        pair->edge_runtime, key, &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
          meiso_runtime_actual_state_decode(
              event.state_offer.state.payload, &decoded
          ));
    memset(&view, 0xa5, sizeof(view));
    CHECK(meiso_display_ingress_validate_actual_state(
        &cap, applied_desired_state, &decoded, &view
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK && view.data_active);
    receive_status = meiso_state_replica_receive(
        replica, event.state_offer.state.owner,
        event.state_offer.state.version, event.state_offer.state.payload
    );
    CHECK(receive_status == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_runtime_commit_state_receive(
        pair->host_runtime, event.route, &event.state_offer, receive_status
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->host_runtime, &cap, applied_desired_state, out_host_binding
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    return true;
}

static bool reject_invalid_actual_before_replica(
    RuntimePair *pair,
    const Catalog *catalog
)
{
    const MeisoDisplayIngressActual profile = actual_for(
        desired_one(), (MeisoSessionNonce){9U, 10U}
    );
    const MeisoDisplayIngressCapability cap = fixture_capability();
    MeisoRuntimeActualState invalid;
    MeisoRuntimeActualState decoded;
    MeisoDisplayIngressActualStateView view;
    uint8_t profile_wire[MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE];
    uint8_t runtime_wire[
        MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView runtime_payload;
    MeisoStateOffer offer;
    MeisoRuntimeEvent event;

    memset(&invalid, 0, sizeof(invalid));
    invalid.apply_result = MEISO_RUNTIME_APPLY_NONE;
    invalid.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    invalid.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    invalid.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    invalid.profile = (MeisoByteView){
        (const uint8_t *)(const void *)&profile, sizeof(profile)
    };
    CHECK(encode_runtime_actual(
        &invalid, profile_wire, runtime_wire, &runtime_payload
    ));
    offer = (MeisoStateOffer){{1U}, {EDGE_OWNER, 1U, runtime_payload}};
    CHECK(meiso_runtime_send_state_offer(
        pair->edge_runtime,
        route_key(catalog, MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL), &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event
    ));
    CHECK(meiso_runtime_actual_state_decode(
        event.state_offer.state.payload, &decoded
    ));
    memset(&view, 0xa5, sizeof(view));
    CHECK(meiso_display_ingress_validate_actual_state(
        &cap, NULL, &decoded, &view
    ) == MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE);
    CHECK(bytes_are((const uint8_t *)&view, 0xa5, sizeof(view)));
    CHECK(meiso_runtime_commit_state_receive(
        pair->host_runtime, event.route, &event.state_offer,
        MEISO_STATE_RECEIVE_INVALID
    ) == MEISO_RUNTIME_OK);
    return true;
}

static bool test_runtime_gate(
    RuntimePair *pair,
    const Catalog *catalog,
    Replicas *replicas
)
{
    const MeisoDisplayIngressCapability cap = fixture_capability();
    const MeisoDisplayIngressDesired desired1 = desired_one();
    const MeisoDisplayIngressDesired desired2 = desired_two();
    const MeisoDisplayIngressActual profile1 = actual_for(
        desired1, (MeisoSessionNonce){111U, 112U}
    );
    const MeisoDisplayIngressActual profile2 = actual_for(
        desired2, (MeisoSessionNonce){121U, 122U}
    );
    MeisoDisplayIngressActual changed_profile2 = profile2;
    MeisoRuntimeActualState actual;
    MeisoDisplayIngressMediaBinding binding1;
    MeisoDisplayIngressMediaBinding binding_after;
    MeisoDisplayIngressMediaBinding untouched;
    MeisoDisplayIngressCapability stale_capability = cap;
    DesiredStateFixture desired_state1;
    DesiredStateFixture desired_state2;
    DesiredStateFixture disabled_state3;
    MeisoDisplayIngressActualStateView actual_view;
    MeisoRuntimeActualState wire_actual;
    uint8_t profile_wire[MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE];
    uint8_t runtime_wire[
        MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView runtime_payload;
    MeisoStateOffer offer;

    CHECK(desired_state_fixture_init(&desired_state1, true, &desired1));
    CHECK(desired_state_fixture_init(&desired_state2, true, &desired2));
    CHECK(desired_state_fixture_init(&disabled_state3, false, &desired2));
    memset(&untouched, 0xa5, sizeof(untouched));
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->host_runtime, &cap, &desired_state1.runtime, &untouched
    ) == MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE);
    CHECK(bytes_are((const uint8_t *)&untouched, 0xa5, sizeof(untouched)));
    CHECK(accept_catalog(pair, catalog));
    CHECK(init_replicas(replicas, catalog));
    CHECK(reject_invalid_actual_before_replica(pair, catalog));
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->host_runtime, &cap, &desired_state1.runtime, &untouched
    ) == MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE);

    CHECK(deliver_desired(
        pair, catalog, replicas->desired, 1U, &desired_state1.runtime
    ));
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->host_runtime, &cap, &desired_state1.runtime, &untouched
    ) == MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE);

    actual = running_actual(
        1U, 1U, MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, 101U, &profile1
    );
    CHECK(deliver_actual(
        pair, catalog, replicas->actual, 1U, &actual,
        &desired_state1.runtime, &binding1
    ));
    CHECK(binding1.runtime.production_epoch == 101U &&
          binding1.media_stream_epoch == 101U &&
          binding1.channel_nonce.high == profile1.channel_nonce.high &&
          binding1.channel_nonce.low == profile1.channel_nonce.low &&
          meiso_owner_scope_equal(binding1.producer, HOST_OWNER));
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->edge_runtime, &cap, &desired_state1.runtime, &binding_after
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(binding_equal(&binding1, &binding_after));

    stale_capability.max_fragment_payload_bytes = 512U;
    memset(&untouched, 0xa5, sizeof(untouched));
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->host_runtime, &stale_capability, &desired_state1.runtime,
        &untouched
    ) == MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH);
    CHECK(bytes_are((const uint8_t *)&untouched, 0xa5, sizeof(untouched)));

    CHECK(deliver_desired(
        pair, catalog, replicas->desired, 2U, &desired_state2.runtime
    ));
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->host_runtime, &cap, &desired_state1.runtime, &binding_after
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(binding_equal(&binding1, &binding_after));
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->edge_runtime, &cap, &desired_state1.runtime, &binding_after
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(binding_equal(&binding1, &binding_after));
    actual = running_actual(
        2U, 1U, MEISO_RUNTIME_APPLY_PENDING,
        MEISO_RUNTIME_PRODUCTION_REASON_APPLYING, 101U, &profile1
    );
    CHECK(deliver_actual(
        pair, catalog, replicas->actual, 2U, &actual,
        &desired_state1.runtime,
        &binding_after
    ));
    CHECK(binding_equal(&binding1, &binding_after));

    actual = running_actual(
        2U, 1U, MEISO_RUNTIME_APPLY_REJECTED,
        MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER,
        101U, &profile1
    );
    CHECK(deliver_actual(
        pair, catalog, replicas->actual, 3U, &actual,
        &desired_state1.runtime,
        &binding_after
    ));
    CHECK(binding_equal(&binding1, &binding_after));

    actual = running_actual(
        2U, 2U, MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, 102U, &profile2
    );
    CHECK(deliver_actual(
        pair, catalog, replicas->actual, 4U, &actual,
        &desired_state2.runtime,
        &binding_after
    ));
    CHECK(binding_after.runtime.production_epoch == 102U &&
          binding_after.media_stream_epoch == 102U &&
          !binding_equal(&binding1, &binding_after));

    ++changed_profile2.channel_nonce.low;
    actual = running_actual(
        2U, 2U, MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, 102U, &changed_profile2
    );
    CHECK(encode_runtime_actual(
        &actual, profile_wire, runtime_wire, &runtime_payload
    ));
    offer = (MeisoStateOffer){{5U}, {EDGE_OWNER, 5U, runtime_payload}};
    CHECK(meiso_runtime_send_state_offer(
        pair->edge_runtime,
        route_key(catalog, MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL), &offer
    ) == MEISO_RUNTIME_STATE_INVALID);
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->edge_runtime, &cap, &desired_state2.runtime, &untouched
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(binding_equal(&binding_after, &untouched));

    CHECK(deliver_desired(
        pair, catalog, replicas->desired, 3U, &disabled_state3.runtime
    ));
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->host_runtime, &cap, &desired_state2.runtime, &untouched
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(binding_equal(&binding_after, &untouched));
    actual = running_actual(
        3U, 2U, MEISO_RUNTIME_APPLY_PENDING,
        MEISO_RUNTIME_PRODUCTION_REASON_APPLYING, 102U, &profile2
    );
    CHECK(deliver_actual(
        pair, catalog, replicas->actual, 5U, &actual,
        &desired_state2.runtime, &untouched
    ));
    CHECK(binding_equal(&binding_after, &untouched));

    actual = running_actual(
        3U, 3U, MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, 103U, &profile2
    );
    CHECK(encode_runtime_actual(
        &actual, profile_wire, runtime_wire, &runtime_payload
    ));
    wire_actual = actual;
    wire_actual.profile = (MeisoByteView){
        profile_wire, MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE
    };
    memset(&actual_view, 0xa5, sizeof(actual_view));
    CHECK(meiso_display_ingress_validate_actual_state(
        &cap, &disabled_state3.runtime, &wire_actual, &actual_view
    ) == MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE);
    CHECK(bytes_are(
        (const uint8_t *)&actual_view, 0xa5, sizeof(actual_view)
    ));
    memset(&untouched, 0xa5, sizeof(untouched));
    CHECK(meiso_display_ingress_media_binding_from_runtime(
        pair->host_runtime, &cap, &disabled_state3.runtime, &untouched
    ) == MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE);
    CHECK(bytes_are((const uint8_t *)&untouched, 0xa5, sizeof(untouched)));
    return true;
}

static bool test_evidence_flow(
    RuntimePair *pair,
    const Catalog *catalog
)
{
    const MeisoDisplayIngressCapability cap = fixture_capability();
    MeisoDisplayIngressPresentationEvidence evidence;
    MeisoDisplayIngressPresentationEvidence decoded;
    MeisoStreamSample sample;
    MeisoRuntimeEvent event;
    uint8_t payload[MEISO_DISPLAY_INGRESS_PROFILE_EVIDENCE_WIRE_SIZE];
    size_t payload_size = 0U;
    size_t index;

    memset(&evidence, 0, sizeof(evidence));
    evidence.evidence_class = MEISO_DISPLAY_INGRESS_EVIDENCE_SIMULATED;
    evidence.connector_kind = MEISO_DISPLAY_INGRESS_CONNECTOR_UNSPECIFIED;
    evidence.output_width = 32U;
    evidence.output_height = 32U;
    evidence.media_stream_epoch = 102U;
    evidence.displayed_frame_sequence = 9U;
    for (index = 0U; index < sizeof(evidence.input_rgba_sha256); ++index) {
        evidence.input_rgba_sha256[index] = (uint8_t)(0xf0U - index);
    }
    CHECK(meiso_display_ingress_evidence_supported(&cap, &evidence, 102U));
    CHECK(!meiso_display_ingress_evidence_supported(&cap, &evidence, 101U));
    CHECK(meiso_display_ingress_evidence_encode(
        &evidence, (MeisoMutableBuffer){payload, sizeof(payload)},
        &payload_size
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){EDGE_OWNER, 1U, 1U};
    sample.source_time = (MeisoTimePoint){9U, 1000U};
    sample.payload = (MeisoByteView){payload, payload_size};
    CHECK(meiso_runtime_send_stream_sample(
        pair->edge_runtime,
        route_key(
            catalog,
            MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE
        ),
        102U, &sample
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          event.production_epoch == 102U &&
          meiso_display_ingress_evidence_decode(
              event.stream_sample.payload, &decoded
          ) == MEISO_DISPLAY_INGRESS_PROFILE_OK &&
          meiso_display_ingress_evidence_supported(
              &cap, &decoded, event.production_epoch
          ));

    sample.key.producer = HOST_OWNER;
    sample.key.sequence = 2U;
    CHECK(meiso_runtime_send_stream_sample(
        pair->host_runtime,
        route_key(catalog, MEISO_DISPLAY_INGRESS_ROUTE_MEDIA),
        102U, &sample
    ) == MEISO_RUNTIME_DELIVERY_MISMATCH);
    return true;
}

static bool test_config_requires_ready(void)
{
    SessionArena arena;
    const MeisoSessionConfig config = session_config(
        MEISO_SESSION_ROLE_HOST, 800U
    );
    MeisoSession *session = NULL;
    MeisoRuntimeV4Config output;
    MeisoRuntimeV4Config before;
    size_t session_size = 0U;

    CHECK(init_session(&arena, &config, &session, &session_size));
    (void)session_size;
    memset(&output, 0xa5, sizeof(output));
    before = output;
    CHECK(meiso_display_ingress_configure_runtime_v4(
        &output, session
    ) == MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    return true;
}

static bool test_h264_catalog_selection(void)
{
    Catalog catalog;
    MeisoDisplayIngressCapability decoded;
    MeisoDisplayIngressCatalogConfig config = {0};
    MeisoDisplayIngressDesired desired = {1280U, 720U, 30000U};
    config.endpoint_incarnation = 1U;
    config.capability = codec_capability();
    config.capability.max_input_width = 1280U;
    config.capability.max_input_height = 720U;
    config.capability.max_frame_bytes = 131072U;
    config.capability.frame_format = MEISO_DISPLAY_INGRESS_FRAME_H264_IDR;
    CHECK(meiso_display_ingress_profile_for_format(config.capability.frame_format)->version == 2U);
    CHECK(meiso_display_ingress_desired_supported(&config.capability, &desired));
    CHECK(meiso_display_ingress_build_catalog(&config,
        (MeisoMutableBuffer){catalog.capability, sizeof(catalog.capability)},
        catalog.endpoints, 1U, catalog.routes, 4U) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(catalog.capability[25] == 1U && catalog.endpoints[0].capability_schema.version == 2U);
    CHECK(catalog.routes[2].route.resource.schema.version == 2U);
    CHECK(catalog.routes[3].route.resource.schema.version == 2U);
    CHECK(catalog.routes[0].route.resource.schema.version == 1U);
    CHECK(meiso_display_ingress_validate_catalog(catalog.endpoints, 1U,
        catalog.routes, 4U, &decoded) == MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(decoded.frame_format == MEISO_DISPLAY_INGRESS_FRAME_H264_IDR);
    catalog.endpoints[0].capability_schema.version = 1U;
    CHECK(meiso_display_ingress_validate_catalog(catalog.endpoints, 1U,
        catalog.routes, 4U, &decoded) == MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH);
    catalog.endpoints[0].capability_schema.version = 2U;
    catalog.routes[2].route.resource.schema.version = 1U;
    CHECK(meiso_display_ingress_validate_catalog(catalog.endpoints, 1U,
        catalog.routes, 4U, &decoded) == MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH);
    return true;
}

int main(void)
{
    Catalog catalog;
    RuntimePair pair;
    RuntimePair fresh;
    Replicas replicas;
    MeisoDisplayIngressMediaBinding binding;
    DesiredStateFixture desired_state;
    const MeisoDisplayIngressCapability cap = fixture_capability();
    const MeisoDisplayIngressDesired desired = desired_two();

    if (!desired_state_fixture_init(&desired_state, true, &desired) ||
        !test_canonical_codecs() || !test_h264_catalog_selection() || !test_catalog_contract(&catalog) ||
        !test_config_requires_ready() || !init_pair(&pair, 900U) ||
        !test_runtime_gate(&pair, &catalog, &replicas) ||
        !test_evidence_flow(&pair, &catalog) ||
        !init_pair(&fresh, 1000U) || !accept_catalog(&fresh, &catalog)) {
        return 1;
    }
    memset(&binding, 0xa5, sizeof(binding));
    if (meiso_display_ingress_media_binding_from_runtime(
            fresh.host_runtime, &cap, &desired_state.runtime, &binding
        ) != MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE ||
        !bytes_are((const uint8_t *)&binding, 0xa5, sizeof(binding))) {
        return 1;
    }
    puts(
        "DISPLAY_INGRESS_PROFILE_CONTRACT_FIXTURE_ONLY "
        "physical_evidence=false"
    );
    return 0;
}
