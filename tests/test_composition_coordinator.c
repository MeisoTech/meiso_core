#include "composition_coordinator.h"
#include "composition_scene.h"
#include "meiso_blob.h"
#include "meiso_state.h"
#include "meiso_stream.h"
#include "scene_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(stderr, "CHECK failed at %s:%d: %s\n",             \
                          __FILE__, __LINE__, #condition);                       \
            return false;                                                       \
        }                                                                       \
    } while (0)

/* Independently computed C0 SHA-256 vector; not produced by this API. */
static const uint8_t PLAN_DIGEST_GOLDEN[MEISO_SHA256_DIGEST_SIZE] = {
    0xf6U, 0x3eU, 0x4cU, 0xc6U, 0x12U, 0x96U, 0x53U, 0xe4U,
    0x29U, 0x9dU, 0x33U, 0x3cU, 0x49U, 0x9dU, 0x46U, 0x4bU,
    0x22U, 0xfbU, 0xf2U, 0x18U, 0x26U, 0x3cU, 0x62U, 0xecU,
    0xf6U, 0xcaU, 0x76U, 0xb2U, 0xbcU, 0xcbU, 0x11U, 0x39U
};

typedef struct {
    void *blob_arena;
    void *scene_store_arena;
    void *coordinator_arena;
    void *scene_publisher_arena;
    void *scene_replica_arena;
    void *plan_publisher_arena;
    void *plan_replica_arena;
    void *actual_publisher_arena;
    void *actual_replica_arena;
    void *stream_arena;
    MeisoBlobStore *blob_store;
    MeisoCompositionSceneStore *scene_store;
    MeisoCompositionCoordinator *coordinator;
    MeisoStatePublisher *scene_publisher;
    MeisoStateReplica *scene_replica;
    MeisoStatePublisher *plan_publisher;
    MeisoStateReplica *plan_replica;
    MeisoStatePublisher *actual_publisher;
    MeisoStateReplica *actual_replica;
    MeisoStream *stream;
    MeisoOwnerScope host_owner;
    MeisoOwnerScope edge_owner;
    MeisoSessionId session_id;
    MeisoDeliveryGeneration scene_delivery;
    MeisoDeliveryGeneration plan_delivery;
    MeisoDeliveryGeneration actual_delivery;
    MeisoCompositionSceneResourceUsage usage;
} Fixture;

typedef struct {
    MeisoStateView state;
    MeisoRuntimeDesiredState runtime;
    MeisoCompositionScenePresentationPlan plan;
} ReceivedPlan;

typedef struct {
    MeisoStateView state;
    MeisoRuntimeDesiredState runtime;
    MeisoCompositionSceneDesired desired;
} ReceivedScene;

static MeisoTimePoint time_point(uint64_t monotonic_ns)
{
    return (MeisoTimePoint){77U, monotonic_ns};
}

static bool bytes_nonzero(const uint8_t *bytes, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return true;
        }
    }
    return false;
}

static void fill_nonce(uint8_t output[MEISO_SHA256_DIGEST_SIZE], uint8_t seed)
{
    size_t index;

    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        output[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static void fixture_destroy(Fixture *fixture)
{
    free(fixture->stream_arena);
    free(fixture->actual_replica_arena);
    free(fixture->actual_publisher_arena);
    free(fixture->plan_replica_arena);
    free(fixture->plan_publisher_arena);
    free(fixture->scene_replica_arena);
    free(fixture->scene_publisher_arena);
    free(fixture->coordinator_arena);
    free(fixture->scene_store_arena);
    free(fixture->blob_arena);
    memset(fixture, 0, sizeof(*fixture));
}

static bool allocate_handle_arena(void **out_arena, size_t required)
{
    if (out_arena == NULL || required == 0U) {
        return false;
    }
    *out_arena = calloc(1U, required);
    return *out_arena != NULL;
}

static bool fixture_init(Fixture *fixture)
{
    const MeisoResourceDescriptor *blob = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
    );
    const MeisoResourceDescriptor *scene_desired =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_DESIRED
        );
    const MeisoResourceDescriptor *plan_desired =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_PRESENTATION_DESIRED
        );
    const MeisoResourceDescriptor *plan_actual =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_PRESENTATION_ACTUAL
        );
    const MeisoResourceDescriptor *result = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_RENDER_RESULT
    );
    const MeisoBlobConfig blob_config = {
        MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS
    };
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS,
        0U,
        0U
    };
    MeisoCompositionCoordinatorConfig coordinator_config;
    size_t required;

    memset(fixture, 0, sizeof(*fixture));
    fixture->host_owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 101U};
    fixture->edge_owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 202U};
    fixture->session_id = (MeisoSessionId){
        {UINT64_C(0x1111111111111111), UINT64_C(0x2222222222222222)},
        {UINT64_C(0x3333333333333333), UINT64_C(0x4444444444444444)}
    };
    fixture->scene_delivery = (MeisoDeliveryGeneration){11U};
    fixture->plan_delivery = (MeisoDeliveryGeneration){12U};
    fixture->actual_delivery = (MeisoDeliveryGeneration){13U};
    if (blob == NULL || scene_desired == NULL || plan_desired == NULL ||
        plan_actual == NULL || result == NULL) {
        return false;
    }

    required = meiso_blob_store_memory_required(blob, &blob_config);
    if (!allocate_handle_arena(&fixture->blob_arena, required) ||
        meiso_blob_store_init(
            fixture->blob_arena, required, blob, &blob_config,
            &fixture->blob_store) != MEISO_BLOB_INIT_OK) {
        goto fail;
    }
    required = meiso_composition_scene_store_memory_required();
    if (!allocate_handle_arena(&fixture->scene_store_arena, required) ||
        meiso_composition_scene_store_init(
            fixture->scene_store_arena, required, fixture->blob_store,
            &fixture->scene_store) !=
            MEISO_COMPOSITION_SCENE_STORE_INIT_OK) {
        goto fail;
    }
    memset(&coordinator_config, 0, sizeof(coordinator_config));
    coordinator_config.scene_store = fixture->scene_store;
    coordinator_config.session_id = fixture->session_id;
    coordinator_config.edge_incarnation = fixture->edge_owner.incarnation;
    coordinator_config.capability_generation = 5U;
    coordinator_config.edge_clock_epoch = 77U;
    required = meiso_composition_coordinator_memory_required();
    if (!allocate_handle_arena(&fixture->coordinator_arena, required) ||
        meiso_composition_coordinator_init(
            fixture->coordinator_arena, required, &coordinator_config,
            &fixture->coordinator) != MEISO_COMPOSITION_COORDINATOR_INIT_OK ||
        meiso_composition_coordinator_begin_owner(
            fixture->coordinator, fixture->host_owner.incarnation) !=
            MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED) {
        goto fail;
    }

#define INIT_STATE_PAIR(prefix, descriptor)                                     \
    do {                                                                        \
        required = meiso_state_publisher_memory_required(                       \
            (descriptor), &state_config                                         \
        );                                                                      \
        if (!allocate_handle_arena(                                             \
                &fixture->prefix##_publisher_arena, required) ||                \
            meiso_state_publisher_init(                                         \
                fixture->prefix##_publisher_arena, required, (descriptor),      \
                &state_config, &fixture->prefix##_publisher) !=                 \
                MEISO_STATE_INIT_OK) {                                          \
            goto fail;                                                          \
        }                                                                       \
        required = meiso_state_replica_memory_required((descriptor));           \
        if (!allocate_handle_arena(                                             \
                &fixture->prefix##_replica_arena, required) ||                  \
            meiso_state_replica_init(                                           \
                fixture->prefix##_replica_arena, required, (descriptor),        \
                &fixture->prefix##_replica) != MEISO_STATE_INIT_OK) {            \
            goto fail;                                                          \
        }                                                                       \
    } while (0)

    INIT_STATE_PAIR(scene, scene_desired);
    INIT_STATE_PAIR(plan, plan_desired);
    INIT_STATE_PAIR(actual, plan_actual);
#undef INIT_STATE_PAIR

    required = meiso_stream_memory_required(result, &stream_config);
    if (!allocate_handle_arena(&fixture->stream_arena, required) ||
        meiso_stream_init(
            fixture->stream_arena, required, result, &stream_config,
            &fixture->stream) != MEISO_STREAM_INIT_OK) {
        goto fail;
    }
    if (meiso_state_publisher_begin_delivery(
            fixture->scene_publisher, fixture->scene_delivery) !=
            MEISO_STATE_DELIVERY_STARTED ||
        meiso_state_publisher_begin_delivery(
            fixture->plan_publisher, fixture->plan_delivery) !=
            MEISO_STATE_DELIVERY_STARTED ||
        meiso_state_publisher_begin_delivery(
            fixture->actual_publisher, fixture->actual_delivery) !=
            MEISO_STATE_DELIVERY_STARTED ||
        meiso_state_replica_begin_owner(
            fixture->scene_replica, fixture->host_owner) !=
            MEISO_STATE_OWNER_STARTED ||
        meiso_state_replica_begin_owner(
            fixture->plan_replica, fixture->host_owner) !=
            MEISO_STATE_OWNER_STARTED ||
        meiso_state_replica_begin_owner(
            fixture->actual_replica, fixture->edge_owner) !=
            MEISO_STATE_OWNER_STARTED ||
        meiso_stream_begin_producer(
            fixture->stream, fixture->edge_owner, 41U) !=
            MEISO_STREAM_PRODUCER_STARTED) {
        goto fail;
    }
    return true;

fail:
    fixture_destroy(fixture);
    return false;
}

static bool deliver_state(
    MeisoStatePublisher *publisher,
    MeisoStateReplica *replica,
    MeisoDeliveryGeneration delivery,
    MeisoOwnerScope owner,
    uint64_t version,
    MeisoByteView payload,
    MeisoStateView *out_view
)
{
    uint8_t transferred[MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE];
    MeisoStateOffer offer;

    CHECK(payload.size <= sizeof(transferred));
    CHECK(meiso_state_publisher_publish(
              publisher, owner, version, payload) ==
          MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_next_offer(publisher, &offer));
    CHECK(offer.state.payload.size <= sizeof(transferred));
    memmove(transferred, offer.state.payload.data, offer.state.payload.size);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
              publisher, delivery, offer.state.owner, offer.state.version) ==
          MEISO_STATE_ACCEPT_CONFIRMED);
    CHECK(meiso_state_replica_receive(
              replica, offer.state.owner, offer.state.version,
              (MeisoByteView){transferred, offer.state.payload.size}) ==
          MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_state_replica_current(replica, out_view));
    return true;
}

static bool encode_cube(
    uint8_t output[512],
    MeisoByteView *out_wire,
    MeisoContentId *out_id
)
{
    size_t size = 0U;

    CHECK(meiso_composition_scene_mesh_encode(
              meiso_composition_scene_canonical_cube(),
              (MeisoMutableBuffer){output, 512U}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    *out_wire = (MeisoByteView){output, size};
    CHECK(meiso_composition_scene_mesh_content_id(*out_wire, out_id));
    return true;
}

static bool publish_mesh(
    Fixture *fixture,
    MeisoContentId id,
    MeisoByteView wire,
    MeisoBlobCommitStatus expected_commit,
    MeisoCompositionSceneBlobVisibilityStatus expected_visibility
)
{
    MeisoMutableBuffer staging;

    CHECK(meiso_blob_store_stage_begin(
              fixture->blob_store, &id, wire.size) ==
          MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(fixture->blob_store, &staging));
    memmove(staging.data, wire.data, wire.size);
    CHECK(meiso_blob_store_commit_verified(
              fixture->blob_store, wire.size,
              meiso_composition_scene_mesh_blob_verify, NULL) ==
          expected_commit);
    if (expected_commit == MEISO_BLOB_COMMIT_PUBLISHED) {
        fixture->usage.published_blob_count++;
        fixture->usage.published_blob_bytes += wire.size;
    }
    CHECK(meiso_composition_scene_store_mark_blob_visible(
              fixture->scene_store, fixture->host_owner.incarnation,
              &id, wire.size) == expected_visibility);
    return true;
}

static MeisoCompositionSceneDesired scene_value(MeisoContentId mesh_id)
{
    MeisoCompositionSceneDesired value;
    MeisoCompositionSceneObject *object;

    memset(&value, 0, sizeof(value));
    value.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    value.camera.space_epoch = 9U;
    value.camera.source_time = time_point(10U);
    value.camera.position[2] = -3.0F;
    value.camera.orientation_xyzw[3] = 1.0F;
    value.camera.projection =
        MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    value.camera.vertical_fov_degrees = 60.0F;
    value.camera.near_z = 0.1F;
    value.camera.far_z = 100.0F;
    value.camera.viewport_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    value.camera.viewport_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    value.object_count = 1U;
    object = &value.objects[0];
    object->id = (MeisoCompositionSceneObjectId){1U, 1U};
    object->mesh_content_id = mesh_id;
    object->space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    object->space_epoch = value.camera.space_epoch;
    object->transform.orientation_xyzw[3] = 1.0F;
    object->transform.scale[0] = 1.0F;
    object->transform.scale[1] = 1.0F;
    object->transform.scale[2] = 1.0F;
    object->flags = MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE;
    return value;
}

static bool publish_scene(
    Fixture *fixture,
    uint64_t state_version,
    uint64_t plan_generation,
    const MeisoCompositionSceneDesired *desired,
    ReceivedScene *out_received,
    MeisoCompositionScenePrepareStatus expected_prepare
)
{
    uint8_t suffix[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    uint8_t payload_storage[
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoRuntimeDesiredState runtime;
    MeisoByteView payload;
    size_t suffix_size = 0U;

    CHECK(meiso_composition_scene_desired_encode(
              desired, (MeisoMutableBuffer){suffix, sizeof(suffix)},
              &suffix_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    runtime.enabled = true;
    runtime.profile = (MeisoByteView){suffix, suffix_size};
    CHECK(meiso_runtime_desired_state_encode(
        &runtime, (MeisoMutableBuffer){payload_storage, sizeof(payload_storage)},
        &payload
    ));
    memset(out_received, 0, sizeof(*out_received));
    CHECK(deliver_state(
        fixture->scene_publisher, fixture->scene_replica,
        fixture->scene_delivery, fixture->host_owner, state_version, payload,
        &out_received->state
    ));
    CHECK(meiso_runtime_desired_state_decode(
        out_received->state.payload, &out_received->runtime
    ));
    CHECK(meiso_composition_scene_desired_decode(
              out_received->runtime.profile, &out_received->desired) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(meiso_composition_scene_store_prepare(
              fixture->scene_store, out_received->state.owner.incarnation,
              out_received->state.version, plan_generation,
              &out_received->desired) == expected_prepare);
    return true;
}

static MeisoCompositionScenePresentationPlan plan_value(
    uint64_t generation,
    uint32_t offline_lease_ms
)
{
    MeisoCompositionScenePresentationPlan plan;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    memset(&plan, 0, sizeof(plan));
    plan.plan_generation = generation;
    plan.capability_generation = 5U;
    plan.scene_binding = *meiso_composition_scene_scene_endpoint();
    plan.source = MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE;
    plan.geometry = MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION;
    plan.target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    plan.target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    plan.color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    plan.depth_format = MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    plan.max_result_hz = MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ;
    plan.offline_lease_ms = offline_lease_ms;
    if (!meiso_composition_coordinator_plan_digest(&plan, digest)) {
        memset(&plan, 0, sizeof(plan));
    } else {
        memcpy(plan.candidate_digest, digest, sizeof(digest));
    }
    return plan;
}

static bool test_plan_digest_independent_golden(void)
{
    MeisoCompositionScenePresentationPlan plan;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    memset(&plan, 0, sizeof(plan));
    plan.flags = MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME;
    plan.plan_generation = UINT64_C(0x0102030405060708);
    plan.capability_generation = UINT64_C(0x1112131415161718);
    memset(plan.candidate_digest, 0xa5, sizeof(plan.candidate_digest));
    plan.scene_binding = *meiso_composition_scene_scene_endpoint();
    plan.source = MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE;
    plan.geometry = MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION;
    plan.target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    plan.target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    plan.color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    plan.depth_format = MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    plan.max_result_hz = MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ;
    plan.offline_lease_ms = 1000U;
    CHECK(meiso_composition_coordinator_plan_digest(&plan, digest));
    CHECK(memcmp(digest, PLAN_DIGEST_GOLDEN, sizeof(digest)) == 0);

    plan.prepared_token_echo_present = true;
    fill_nonce(plan.prepared_token_echo, 0x21U);
    memset(digest, 0, sizeof(digest));
    CHECK(meiso_composition_coordinator_plan_digest(&plan, digest));
    CHECK(memcmp(digest, PLAN_DIGEST_GOLDEN, sizeof(digest)) == 0);
    return true;
}

static bool publish_plan(
    Fixture *fixture,
    uint64_t state_version,
    bool enabled,
    const MeisoCompositionScenePresentationPlan *plan,
    MeisoTimePoint observed_at,
    ReceivedPlan *out_received,
    MeisoCompositionCoordinatorObserveStatus expected_observe
)
{
    uint8_t suffix[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];
    uint8_t payload_storage[
        MEISO_COMPOSITION_SCENE_PRESENTATION_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoRuntimeDesiredState runtime;
    MeisoByteView payload;
    size_t suffix_size = 0U;

    memset(&runtime, 0, sizeof(runtime));
    runtime.enabled = enabled;
    if (enabled) {
        CHECK(plan != NULL);
        CHECK(meiso_composition_scene_presentation_plan_encode(
                  plan, (MeisoMutableBuffer){suffix, sizeof(suffix)},
                  &suffix_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
        runtime.profile = (MeisoByteView){suffix, suffix_size};
    } else {
        CHECK(plan == NULL);
    }
    CHECK(meiso_runtime_desired_state_encode(
        &runtime, (MeisoMutableBuffer){payload_storage, sizeof(payload_storage)},
        &payload
    ));
    CHECK(payload.size == (enabled ?
              MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE +
                  MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE :
              MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE));
    memset(out_received, 0, sizeof(*out_received));
    CHECK(deliver_state(
        fixture->plan_publisher, fixture->plan_replica,
        fixture->plan_delivery, fixture->host_owner, state_version, payload,
        &out_received->state
    ));
    CHECK(meiso_runtime_desired_state_decode(
        out_received->state.payload, &out_received->runtime
    ));
    if (enabled) {
        CHECK(meiso_composition_scene_presentation_plan_decode(
                  out_received->runtime.profile, &out_received->plan) ==
              MEISO_COMPOSITION_SCENE_CODEC_OK);
    } else {
        CHECK(out_received->runtime.profile.size == 0U);
    }
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture->coordinator,
              out_received->state.owner.incarnation,
              out_received->state.version,
              &out_received->runtime,
              observed_at) == expected_observe);
    return true;
}

static bool publish_presentation_actual(
    Fixture *fixture,
    uint64_t actual_version,
    const MeisoCompositionCoordinatorPlanView *active
)
{
    uint8_t suffix[MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE];
    uint8_t payload_storage[
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    uint8_t transferred[
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoCompositionScenePresentationStatus status;
    MeisoCompositionScenePresentationStatus decoded_status;
    MeisoRuntimeActualState runtime;
    MeisoRuntimeActualState decoded_runtime;
    MeisoByteView payload;
    MeisoStateOffer offer;
    MeisoStateView current;
    size_t suffix_size = 0U;

    CHECK(meiso_composition_coordinator_status(
        fixture->coordinator, &status
    ));
    CHECK(meiso_composition_scene_presentation_status_encode(
              &status, (MeisoMutableBuffer){suffix, sizeof(suffix)},
              &suffix_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&runtime, 0, sizeof(runtime));
    runtime.observed_desired = (MeisoRuntimeDesiredReference){
        true, fixture->host_owner, active->ref.desired_state_version
    };
    runtime.applied_desired = runtime.observed_desired;
    runtime.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    runtime.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    runtime.effective_enabled = true;
    runtime.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    runtime.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    runtime.production_epoch = 41U;
    runtime.profile = (MeisoByteView){suffix, suffix_size};
    CHECK(meiso_runtime_actual_state_encode(
        &runtime, (MeisoMutableBuffer){payload_storage, sizeof(payload_storage)},
        &payload
    ));
    CHECK(meiso_state_publisher_publish(
              fixture->actual_publisher, fixture->edge_owner, actual_version,
              payload) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_next_offer(
        fixture->actual_publisher, &offer
    ));
    memmove(transferred, offer.state.payload.data, offer.state.payload.size);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
              fixture->actual_publisher, fixture->actual_delivery,
              offer.state.owner, offer.state.version) ==
          MEISO_STATE_ACCEPT_CONFIRMED);
    CHECK(meiso_state_replica_receive(
              fixture->actual_replica, offer.state.owner, offer.state.version,
              (MeisoByteView){transferred, offer.state.payload.size}) ==
          MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_state_replica_current(fixture->actual_replica, &current));
    CHECK(meiso_runtime_actual_state_decode(
        current.payload, &decoded_runtime
    ));
    CHECK(meiso_composition_scene_presentation_status_decode(
              decoded_runtime.profile, &decoded_status) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded_status.active_plan_generation ==
              active->ref.plan_generation &&
          decoded_status.active_state ==
              MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE);
    return true;
}

static bool status_roundtrip(
    Fixture *fixture,
    MeisoCompositionSceneCandidateState expected_candidate,
    MeisoCompositionSceneActiveState expected_active,
    MeisoCompositionScenePresentationReason expected_reason
)
{
    uint8_t first[MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE];
    uint8_t second[MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE];
    MeisoCompositionScenePresentationStatus status;
    MeisoCompositionScenePresentationStatus decoded;
    size_t first_size = 0U;
    size_t second_size = 0U;

    CHECK(meiso_composition_coordinator_status(
        fixture->coordinator, &status
    ));
    CHECK(status.candidate_state == expected_candidate &&
          status.active_state == expected_active &&
          status.reason == expected_reason);
    CHECK(meiso_composition_scene_presentation_status_encode(
              &status, (MeisoMutableBuffer){first, sizeof(first)},
              &first_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(meiso_composition_scene_presentation_status_decode(
              (MeisoByteView){first, first_size}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(meiso_composition_scene_presentation_status_encode(
              &decoded, (MeisoMutableBuffer){second, sizeof(second)},
              &second_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(first_size == second_size &&
          memcmp(first, second, first_size) == 0);
    return true;
}

static bool publish_stopped_actual(
    Fixture *fixture,
    uint64_t actual_version,
    uint64_t disabled_desired_version
)
{
    uint8_t suffix[MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE];
    uint8_t payload_storage[
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoCompositionScenePresentationStatus status;
    MeisoCompositionScenePresentationStatus decoded_status;
    MeisoRuntimeActualState runtime;
    MeisoRuntimeActualState decoded_runtime;
    MeisoStateView current;
    MeisoByteView payload;
    size_t suffix_size = 0U;

    CHECK(meiso_composition_coordinator_status(
        fixture->coordinator, &status
    ));
    CHECK(meiso_composition_scene_presentation_status_encode(
              &status, (MeisoMutableBuffer){suffix, sizeof(suffix)},
              &suffix_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&runtime, 0, sizeof(runtime));
    runtime.observed_desired = (MeisoRuntimeDesiredReference){
        true, fixture->host_owner, disabled_desired_version
    };
    runtime.applied_desired = runtime.observed_desired;
    runtime.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    runtime.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    runtime.effective_enabled = false;
    runtime.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    runtime.reason = MEISO_RUNTIME_PRODUCTION_REASON_DISABLED;
    runtime.production_epoch = 0U;
    runtime.profile = (MeisoByteView){suffix, suffix_size};
    CHECK(meiso_runtime_actual_state_encode(
        &runtime, (MeisoMutableBuffer){payload_storage, sizeof(payload_storage)},
        &payload
    ));
    CHECK(deliver_state(
        fixture->actual_publisher, fixture->actual_replica,
        fixture->actual_delivery, fixture->edge_owner, actual_version,
        payload, &current
    ));
    CHECK(meiso_runtime_actual_state_decode(
        current.payload, &decoded_runtime
    ));
    CHECK(!decoded_runtime.effective_enabled &&
          decoded_runtime.production_status ==
              MEISO_RUNTIME_PRODUCTION_STOPPED &&
          decoded_runtime.reason ==
              MEISO_RUNTIME_PRODUCTION_REASON_DISABLED &&
          decoded_runtime.production_epoch == 0U &&
          decoded_runtime.applied_desired.present &&
          decoded_runtime.applied_desired.version ==
              disabled_desired_version);
    CHECK(meiso_composition_scene_presentation_status_decode(
              decoded_runtime.profile, &decoded_status) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded_status.active_state ==
              MEISO_COMPOSITION_SCENE_ACTIVE_NONE &&
          decoded_status.candidate_state ==
              MEISO_COMPOSITION_SCENE_CANDIDATE_NONE);
    return true;
}

static bool render_to_stream(
    Fixture *fixture,
    const MeisoCompositionSceneWorldRef *world,
    uint64_t sequence
)
{
    uint8_t payload[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE];
    MeisoCompositionSceneRenderResult result;
    MeisoStreamSample sample;
    MeisoStreamReceiveResult receive;
    size_t payload_size = 0U;

    CHECK(meiso_composition_scene_store_fake_render(
              fixture->scene_store, 77U, 200U, 210U, &result) ==
          MEISO_COMPOSITION_SCENE_RENDER_COMPLETE);
    CHECK(result.applied_scene_owner_incarnation ==
              world->desired_owner_incarnation &&
          result.applied_scene_state_version ==
              world->desired_state_version &&
          result.active_plan_generation == world->plan_generation &&
          result.world_generation == world->world_generation);
    CHECK(meiso_composition_scene_render_result_encode(
              &result, (MeisoMutableBuffer){payload, sizeof(payload)},
              &payload_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){fixture->edge_owner, 41U, sequence};
    sample.source_time = time_point(210U);
    sample.payload = (MeisoByteView){payload, payload_size};
    receive = meiso_stream_receive(fixture->stream, &sample, NULL);
    CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    fixture->usage.result_queue_depth =
        (uint32_t)meiso_stream_count(fixture->stream);
    CHECK(meiso_composition_scene_store_record_result_published(
        fixture->scene_store, world, 41U, sequence
    ));
    return true;
}

static bool prepare_ready_plan(
    Fixture *fixture,
    uint64_t plan_state_version,
    uint64_t plan_generation,
    uint8_t nonce_seed,
    MeisoCompositionScenePresentationPlan *out_plan,
    MeisoCompositionCoordinatorPlanView *out_candidate
)
{
    ReceivedPlan received;
    MeisoCompositionCoordinatorPlanRef expected;
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];

    *out_plan = plan_value(plan_generation, 1000U);
    CHECK(out_plan->plan_generation == plan_generation);
    CHECK(publish_plan(
        fixture, plan_state_version, true, out_plan, time_point(100U),
        &received, MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture->coordinator, out_candidate
    ));
    expected = out_candidate->ref;
    fill_nonce(nonce, nonce_seed);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture->coordinator,
              &expected,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(110U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED);
    CHECK(meiso_composition_coordinator_candidate(
        fixture->coordinator, out_candidate
    ));
    CHECK(out_candidate->candidate_state ==
              MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED &&
          out_candidate->prepared_token_present &&
          bytes_nonzero(out_candidate->prepared_token,
                        MEISO_SHA256_DIGEST_SIZE));
    return true;
}

static bool echo_and_activate(
    Fixture *fixture,
    uint64_t echo_state_version,
    MeisoCompositionScenePresentationPlan *plan,
    const MeisoCompositionCoordinatorPlanView *prepared,
    MeisoCompositionSceneWorldRef *out_world
)
{
    ReceivedPlan received;
    uint8_t prepared_token[MEISO_SHA256_DIGEST_SIZE];

    memcpy(prepared_token, prepared->prepared_token,
           sizeof(prepared_token));
    plan->prepared_token_echo_present = true;
    memcpy(plan->prepared_token_echo, prepared_token, sizeof(prepared_token));
    CHECK(publish_plan(
        fixture, echo_state_version, true, plan, time_point(120U), &received,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING
    ));
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
              fixture->coordinator, prepared_token,
              time_point(130U), out_world) ==
          MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED);
    return true;
}

static bool establish_active(
    Fixture *fixture,
    uint8_t cube_bytes[512],
    MeisoByteView *out_cube_wire,
    MeisoContentId *out_cube_id,
    MeisoCompositionSceneDesired *out_scene,
    MeisoCompositionScenePresentationPlan *out_plan,
    MeisoCompositionSceneWorldRef *out_world,
    uint8_t out_prepared_token[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoCompositionCoordinatorPlanView prepared;
    ReceivedScene received_scene;

    CHECK(encode_cube(cube_bytes, out_cube_wire, out_cube_id));
    CHECK(publish_mesh(
        fixture, *out_cube_id, *out_cube_wire,
        MEISO_BLOB_COMMIT_PUBLISHED,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));
    *out_scene = scene_value(*out_cube_id);
    CHECK(publish_scene(
        fixture, 1U, 1U, out_scene, &received_scene,
        MEISO_COMPOSITION_SCENE_PREPARE_PREPARED
    ));
    CHECK(prepare_ready_plan(
        fixture, 1U, 1U, 1U, out_plan, &prepared
    ));
    memcpy(out_prepared_token, prepared.prepared_token,
           MEISO_SHA256_DIGEST_SIZE);
    CHECK(echo_and_activate(
        fixture, 2U, out_plan, &prepared, out_world
    ));
    return true;
}

static bool active_unchanged(
    Fixture *fixture,
    const MeisoCompositionCoordinatorPlanRef *expected_plan,
    const MeisoCompositionSceneWorldRef *expected_world
)
{
    MeisoCompositionCoordinatorPlanView active;
    MeisoCompositionSceneWorldView world;

    CHECK(meiso_composition_coordinator_active(
        fixture->coordinator, &active
    ));
    CHECK(memcmp(&active.ref, expected_plan, sizeof(*expected_plan)) == 0);
    CHECK(meiso_composition_scene_store_active(
        fixture->scene_store, &world
    ));
    CHECK(memcmp(&world.ref, expected_world, sizeof(*expected_world)) == 0);
    return true;
}

static bool test_runtime_shape_and_observation_order(void)
{
    Fixture fixture;
    MeisoCompositionScenePresentationPlan plan;
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoRuntimeDesiredState desired;
    MeisoCompositionCoordinatorObservation observation;
    uint8_t wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE + 1U];
    size_t wire_size = 0U;

    CHECK(fixture_init(&fixture));
    plan = plan_value(1U, MEISO_COMPOSITION_SCENE_MAX_OFFLINE_LEASE_MS);
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &plan, (MeisoMutableBuffer){wire, sizeof(wire)}, &wire_size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(wire_size == MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE);

    desired = (MeisoRuntimeDesiredState){false, {wire, 1U}};
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture.coordinator, fixture.host_owner.incarnation, 1U,
              &desired, time_point(10U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN);
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture.coordinator, fixture.host_owner.incarnation, 1U,
              &desired, time_point(11U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE);
    CHECK(meiso_composition_coordinator_observation(
        fixture.coordinator, &observation
    ));
    CHECK(!observation.enabled && observation.desired_state_version == 1U);
    desired = (MeisoRuntimeDesiredState){true, {NULL, 0U}};
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture.coordinator, fixture.host_owner.incarnation, 2U,
              &desired, time_point(20U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN);
    desired.profile = (MeisoByteView){wire, wire_size - 1U};
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture.coordinator, fixture.host_owner.incarnation, 3U,
              &desired, time_point(30U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN);
    desired.profile = (MeisoByteView){wire, wire_size + 1U};
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture.coordinator, fixture.host_owner.incarnation, 4U,
              &desired, time_point(40U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN);
    wire[0] ^= 0xffU;
    desired.profile = (MeisoByteView){wire, wire_size};
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture.coordinator, fixture.host_owner.incarnation, 5U,
              &desired, time_point(50U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN);
    wire[0] ^= 0xffU;
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture.coordinator, fixture.host_owner.incarnation, 6U,
              &desired, time_point(60U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING);
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture.coordinator, fixture.host_owner.incarnation, 6U,
              &desired, time_point(61U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE);
    desired.enabled = false;
    desired.profile = (MeisoByteView){NULL, 0U};
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture.coordinator, fixture.host_owner.incarnation, 6U,
              &desired, time_point(62U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT);
    CHECK(meiso_composition_coordinator_observe_desired(
              fixture.coordinator, fixture.host_owner.incarnation, 5U,
              &desired, time_point(63U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_STALE);
    CHECK(meiso_composition_coordinator_observation(
        fixture.coordinator, &observation
    ));
    CHECK(observation.desired_state_version == 6U && observation.enabled);
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    CHECK(candidate.offline_lease_ms ==
          MEISO_COMPOSITION_SCENE_MAX_OFFLINE_LEASE_MS);
    fixture_destroy(&fixture);
    return true;
}

static bool test_async_failures_and_late_completion(void)
{
    Fixture fixture;
    uint8_t cube_bytes[512];
    uint8_t old_token[MEISO_SHA256_DIGEST_SIZE];
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];
    MeisoByteView cube_wire;
    MeisoContentId cube_id;
    MeisoCompositionSceneDesired scene;
    MeisoCompositionScenePresentationPlan baseline_plan;
    MeisoCompositionScenePresentationPlan plan;
    MeisoCompositionCoordinatorPlanView active;
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionCoordinatorPlanView late_before;
    MeisoCompositionCoordinatorPlanView late_after;
    MeisoCompositionCoordinatorPlanRef old_candidate;
    MeisoCompositionCoordinatorObservation observation_before;
    MeisoCompositionCoordinatorObservation observation_after;
    MeisoCompositionCoordinatorStats stats_before;
    MeisoCompositionCoordinatorStats stats_after;
    MeisoCompositionSceneWorldRef active_world;
    ReceivedPlan received_plan;
    ReceivedScene received_scene;

    CHECK(fixture_init(&fixture));
    CHECK(establish_active(
        &fixture, cube_bytes, &cube_wire, &cube_id, &scene,
        &baseline_plan, &active_world, old_token
    ));
    CHECK(meiso_composition_coordinator_active(
        fixture.coordinator, &active
    ));

    plan = plan_value(2U, 1000U);
    CHECK(publish_plan(
        &fixture, 3U, true, &plan, time_point(200U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    old_candidate = candidate.ref;
    memset(nonce, 0, sizeof(nonce));
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &old_candidate,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(205U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_NONCE);
    fill_nonce(nonce, 1U);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &old_candidate,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(206U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_NONCE_REUSED);
    fill_nonce(nonce, 2U);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &old_candidate,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(210U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_WAITING_SCENE);
    CHECK(status_roundtrip(
        &fixture, MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING,
        MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE,
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_WAITING_SCENE
    ));
    CHECK(active_unchanged(&fixture, &active.ref, &active_world));

    CHECK(publish_scene(
        &fixture, 2U, 2U, &scene, &received_scene,
        MEISO_COMPOSITION_SCENE_PREPARE_PREPARED
    ));
    plan = plan_value(3U, 1000U);
    CHECK(publish_plan(
        &fixture, 4U, true, &plan, time_point(220U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    memset(&late_before, 0, sizeof(late_before));
    memset(&late_after, 0, sizeof(late_after));
    memset(&observation_before, 0, sizeof(observation_before));
    memset(&observation_after, 0, sizeof(observation_after));
    memset(&stats_before, 0, sizeof(stats_before));
    memset(&stats_after, 0, sizeof(stats_after));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &late_before
    ));
    CHECK(meiso_composition_coordinator_observation(
        fixture.coordinator, &observation_before
    ));
    CHECK(meiso_composition_coordinator_stats(
        fixture.coordinator, &stats_before
    ));
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &old_candidate,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(230U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_CANDIDATE_MISMATCH);
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &late_after
    ));
    CHECK(meiso_composition_coordinator_observation(
        fixture.coordinator, &observation_after
    ));
    CHECK(meiso_composition_coordinator_stats(
        fixture.coordinator, &stats_after
    ));
    CHECK(memcmp(&late_before, &late_after, sizeof(late_before)) == 0 &&
          memcmp(&observation_before, &observation_after,
                 sizeof(observation_before)) == 0 &&
          memcmp(&stats_before, &stats_after, sizeof(stats_before)) == 0);
    old_candidate = late_after.ref;
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &old_candidate,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_PREPARE_FAILED,
              time_point(225U), NULL) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_REJECTED);
    CHECK(status_roundtrip(
        &fixture, MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
        MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE,
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN
    ));
    CHECK(active_unchanged(&fixture, &active.ref, &active_world));

    CHECK(publish_plan(
        &fixture, 5U, true, &plan, time_point(250U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    old_candidate = candidate.ref;
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &old_candidate,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_CAPACITY,
              time_point(260U), NULL) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_REJECTED);
    CHECK(publish_plan(
        &fixture, 6U, true, &plan, time_point(270U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    old_candidate = candidate.ref;
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &old_candidate,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_RENDERER_FAILED,
              time_point(280U), NULL) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_FAILED);
    CHECK(status_roundtrip(
        &fixture, MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED,
        MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE,
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_RENDERER_FAILED
    ));
    CHECK(publish_plan(
        &fixture, 7U, true, &plan, time_point(290U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    old_candidate = candidate.ref;
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &old_candidate,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_DEADLINE,
              time_point(300U), NULL) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_FAILED);
    CHECK(active_unchanged(&fixture, &active.ref, &active_world));

    baseline_plan = plan;
    baseline_plan.flags = MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME;
    CHECK(meiso_composition_coordinator_plan_digest(
        &baseline_plan, nonce
    ));
    memcpy(baseline_plan.candidate_digest, nonce, sizeof(nonce));
    CHECK(publish_plan(
        &fixture, 8U, true, &baseline_plan, time_point(310U),
        &received_plan, MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT
    ));
    CHECK(active_unchanged(&fixture, &active.ref, &active_world));
    fixture_destroy(&fixture);
    return true;
}

static bool test_token_scene_supersede_and_generation_guards(void)
{
    Fixture fixture;
    uint8_t cube_bytes[512];
    uint8_t baseline_token[MEISO_SHA256_DIGEST_SIZE];
    uint8_t prepared_token[MEISO_SHA256_DIGEST_SIZE];
    uint8_t wrong_token[MEISO_SHA256_DIGEST_SIZE];
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];
    MeisoByteView cube_wire;
    MeisoContentId cube_id;
    MeisoCompositionSceneDesired scene;
    MeisoCompositionScenePresentationPlan baseline_plan;
    MeisoCompositionScenePresentationPlan plan;
    MeisoCompositionScenePresentationPlan conflict;
    MeisoCompositionCoordinatorPlanView active;
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionCoordinatorPlanView unchanged;
    MeisoCompositionCoordinatorPlanRef expected;
    MeisoCompositionSceneWorldRef active_world;
    MeisoCompositionSceneWorldRef output_world;
    MeisoCompositionSceneWorldRef output_sentinel;
    ReceivedPlan received_plan;
    ReceivedScene received_scene;

    CHECK(fixture_init(&fixture));
    CHECK(establish_active(
        &fixture, cube_bytes, &cube_wire, &cube_id, &scene,
        &baseline_plan, &active_world, baseline_token
    ));
    CHECK(meiso_composition_coordinator_active(
        fixture.coordinator, &active
    ));
    scene.objects[0].transform.position[0] = 1.0F;
    CHECK(publish_scene(
        &fixture, 2U, 2U, &scene, &received_scene,
        MEISO_COMPOSITION_SCENE_PREPARE_PREPARED
    ));
    plan = plan_value(2U, 1000U);
    CHECK(publish_plan(
        &fixture, 3U, true, &plan, time_point(200U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    expected = candidate.ref;
    fill_nonce(nonce, 2U);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &expected,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(210U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED);
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    memcpy(prepared_token, candidate.prepared_token, sizeof(prepared_token));

    CHECK(publish_plan(
        &fixture, 4U, true, &plan, time_point(220U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &unchanged
    ));
    CHECK(memcmp(&candidate.ref, &unchanged.ref, sizeof(candidate.ref)) == 0 &&
          candidate.candidate_state == unchanged.candidate_state &&
          candidate.prepared_token_present == unchanged.prepared_token_present &&
          memcmp(candidate.prepared_token, unchanged.prepared_token,
                 sizeof(candidate.prepared_token)) == 0);

    fill_nonce(wrong_token, 99U);
    plan.prepared_token_echo_present = true;
    memcpy(plan.prepared_token_echo, wrong_token, sizeof(wrong_token));
    CHECK(publish_plan(
        &fixture, 5U, true, &plan, time_point(230U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &unchanged
    ));
    CHECK(memcmp(&candidate.ref, &unchanged.ref, sizeof(candidate.ref)) == 0 &&
          memcmp(candidate.prepared_token, unchanged.prepared_token,
                 sizeof(candidate.prepared_token)) == 0);

    memcpy(plan.prepared_token_echo, prepared_token, sizeof(prepared_token));
    CHECK(publish_plan(
        &fixture, 6U, true, &plan, time_point(240U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    CHECK(candidate.candidate_state ==
          MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING);

    scene.objects[0].transform.position[0] = 2.0F;
    CHECK(publish_scene(
        &fixture, 3U, 2U, &scene, &received_scene,
        MEISO_COMPOSITION_SCENE_PREPARE_PREPARED
    ));
    memset(&output_world, 0xa5, sizeof(output_world));
    output_sentinel = output_world;
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
              fixture.coordinator, prepared_token, time_point(250U),
              &output_world) ==
          MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_MISMATCH);
    CHECK(memcmp(&output_world, &output_sentinel, sizeof(output_world)) == 0);
    CHECK(active_unchanged(&fixture, &active.ref, &active_world));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    CHECK(candidate.candidate_state ==
              MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING &&
          !candidate.prepared_token_present &&
          !bytes_nonzero(candidate.prepared_token,
                         sizeof(candidate.prepared_token)));
    expected = candidate.ref;
    fill_nonce(nonce, 3U);
    unchanged = candidate;
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &expected,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(249U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_TIME);
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    CHECK(memcmp(&unchanged, &candidate, sizeof(candidate)) == 0);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &expected,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(260U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED);
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    memcpy(prepared_token, candidate.prepared_token, sizeof(prepared_token));

    conflict = plan;
    conflict.prepared_token_echo_present = false;
    memset(conflict.prepared_token_echo, 0,
           sizeof(conflict.prepared_token_echo));
    conflict.candidate_digest[0] ^= 0x01U;
    CHECK(publish_plan(
        &fixture, 7U, true, &conflict, time_point(270U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &unchanged
    ));
    CHECK(memcmp(&candidate.ref, &unchanged.ref, sizeof(candidate.ref)) == 0 &&
          memcmp(candidate.prepared_token, unchanged.prepared_token,
                 sizeof(candidate.prepared_token)) == 0);

    conflict = plan;
    conflict.prepared_token_echo_present = false;
    memset(conflict.prepared_token_echo, 0,
           sizeof(conflict.prepared_token_echo));
    conflict.flags = MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME;
    CHECK(meiso_composition_coordinator_plan_digest(&conflict, nonce));
    memcpy(conflict.candidate_digest, nonce, sizeof(nonce));
    CHECK(publish_plan(
        &fixture, 8U, true, &conflict, time_point(280U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &unchanged
    ));
    CHECK(memcmp(&candidate.ref, &unchanged.ref, sizeof(candidate.ref)) == 0 &&
          memcmp(candidate.prepared_token, unchanged.prepared_token,
                 sizeof(candidate.prepared_token)) == 0);

    plan.prepared_token_echo_present = true;
    memcpy(plan.prepared_token_echo, prepared_token, sizeof(prepared_token));
    CHECK(publish_plan(
        &fixture, 9U, true, &plan, time_point(290U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING
    ));
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
              fixture.coordinator, prepared_token, time_point(300U),
              &output_world) ==
          MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED);
    CHECK(output_world.plan_generation == 2U &&
          output_world.desired_state_version == 3U &&
          output_world.world_generation == 2U);

    baseline_plan.prepared_token_echo_present = false;
    memset(baseline_plan.prepared_token_echo, 0,
           sizeof(baseline_plan.prepared_token_echo));
    CHECK(publish_plan(
        &fixture, 10U, true, &baseline_plan, time_point(310U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN
    ));
    CHECK(meiso_composition_coordinator_active(
        fixture.coordinator, &unchanged
    ));
    CHECK(unchanged.ref.plan_generation == 2U);
    fixture_destroy(&fixture);
    return true;
}

static bool test_same_owner_new_session_no_resurrection(void)
{
    Fixture fixture;
    uint8_t cube_bytes[512];
    uint8_t old_token[MEISO_SHA256_DIGEST_SIZE];
    uint8_t new_token[MEISO_SHA256_DIGEST_SIZE];
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];
    MeisoByteView cube_wire;
    MeisoContentId cube_id;
    MeisoCompositionSceneDesired scene;
    MeisoCompositionScenePresentationPlan baseline_plan;
    MeisoCompositionScenePresentationPlan plan;
    MeisoCompositionCoordinatorConfig config;
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionCoordinatorPlanRef expected;
    MeisoCompositionSceneStoreStats before;
    MeisoCompositionSceneStoreStats after;
    MeisoCompositionSceneWorldRef old_world;
    MeisoCompositionSceneWorldRef new_world;
    MeisoCompositionSceneWorldView world_view;
    MeisoCompositionScenePresentationStatus status;
    ReceivedPlan received_plan;
    ReceivedScene received_scene;
    size_t coordinator_size;

    CHECK(fixture_init(&fixture));
    CHECK(establish_active(
        &fixture, cube_bytes, &cube_wire, &cube_id, &scene,
        &baseline_plan, &old_world, old_token
    ));
    CHECK(meiso_composition_scene_store_stats(fixture.scene_store, &before));

    fixture.session_id.edge_nonce.low++;
    memset(&config, 0, sizeof(config));
    config.scene_store = fixture.scene_store;
    config.session_id = fixture.session_id;
    config.edge_incarnation = fixture.edge_owner.incarnation;
    config.capability_generation = 5U;
    config.edge_clock_epoch = 77U;
    coordinator_size = meiso_composition_coordinator_memory_required();
    CHECK(meiso_composition_coordinator_init(
              fixture.coordinator_arena, coordinator_size, &config,
              &fixture.coordinator) == MEISO_COMPOSITION_COORDINATOR_INIT_OK);
    CHECK(meiso_composition_coordinator_begin_owner(
              fixture.coordinator, fixture.host_owner.incarnation) ==
          MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED);
    CHECK(meiso_composition_scene_store_stats(fixture.scene_store, &after));
    CHECK(after.control_scope_resets == before.control_scope_resets + 1U &&
          after.owner_changes == before.owner_changes &&
          after.owner_starts == before.owner_starts);
    CHECK(!meiso_composition_scene_store_active(
        fixture.scene_store, &world_view
    ));
    CHECK(!meiso_composition_coordinator_active(
        fixture.coordinator, &candidate
    ));
    CHECK(meiso_composition_coordinator_status(
        fixture.coordinator, &status
    ));
    CHECK(status.active_state == MEISO_COMPOSITION_SCENE_ACTIVE_NONE &&
          status.candidate_state == MEISO_COMPOSITION_SCENE_CANDIDATE_NONE &&
          !status.prepared_token_present);
    CHECK(meiso_blob_store_count(fixture.blob_store) == 1U);

    plan = plan_value(2U, 1000U);
    CHECK(publish_scene(
        &fixture, 2U, 2U, &scene, &received_scene,
        MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB
    ));
    CHECK(publish_plan(
        &fixture, 3U, true, &plan, time_point(200U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    expected = candidate.ref;
    fill_nonce(nonce, 1U);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &expected,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(210U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_WAITING_SCENE);

    CHECK(publish_mesh(
        &fixture, cube_id, cube_wire, MEISO_BLOB_COMMIT_DUPLICATE,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));
    CHECK(publish_scene(
        &fixture, 3U, 2U, &scene, &received_scene,
        MEISO_COMPOSITION_SCENE_PREPARE_PREPARED
    ));
    CHECK(publish_plan(
        &fixture, 4U, true, &plan, time_point(220U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING
    ));
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    expected = candidate.ref;
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              fixture.coordinator, &expected,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(230U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED);
    CHECK(meiso_composition_coordinator_candidate(
        fixture.coordinator, &candidate
    ));
    memcpy(new_token, candidate.prepared_token, sizeof(new_token));
    CHECK(memcmp(old_token, new_token, sizeof(new_token)) != 0);

    plan.prepared_token_echo_present = true;
    memcpy(plan.prepared_token_echo, old_token, sizeof(old_token));
    CHECK(publish_plan(
        &fixture, 5U, true, &plan, time_point(240U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH
    ));
    memcpy(plan.prepared_token_echo, new_token, sizeof(new_token));
    CHECK(publish_plan(
        &fixture, 6U, true, &plan, time_point(250U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING
    ));
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
              fixture.coordinator, new_token, time_point(260U), &new_world) ==
          MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED);
    CHECK(new_world.world_generation == old_world.world_generation + 1U &&
          new_world.plan_generation == 2U &&
          new_world.desired_state_version == 3U);
    CHECK(meiso_composition_scene_store_stats(fixture.scene_store, &before));
    CHECK(meiso_composition_coordinator_begin_owner(
              fixture.coordinator, 102U) ==
          MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED);
    CHECK(meiso_composition_scene_store_stats(fixture.scene_store, &after));
    CHECK(after.owner_changes == before.owner_changes + 1U &&
          after.control_scope_resets == before.control_scope_resets);
    CHECK(!meiso_composition_coordinator_active(
        fixture.coordinator, &candidate
    ));
    CHECK(!meiso_composition_scene_store_active(
        fixture.scene_store, &world_view
    ));
    old_world = new_world;
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
              fixture.coordinator, new_token, time_point(270U), &old_world) ==
          MEISO_COMPOSITION_COORDINATOR_ACTIVATE_NOT_PENDING);
    CHECK(memcmp(&old_world, &new_world, sizeof(old_world)) == 0);
    CHECK(meiso_composition_scene_store_prepare(
              fixture.scene_store, 102U, 1U, 1U, &scene) ==
          MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB);
    fixture_destroy(&fixture);
    return true;
}

static bool test_api_memory_alias_and_time_guards(void)
{
    Fixture fixture;
    void *arena;
    MeisoCompositionCoordinator *coordinator = NULL;
    MeisoCompositionCoordinatorConfig config;
    MeisoCompositionScenePresentationPlan plan;
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionCoordinatorPlanView candidate_after;
    MeisoCompositionCoordinatorObservation observation;
    MeisoCompositionCoordinatorObservation observation_after;
    MeisoCompositionCoordinatorStats stats;
    MeisoCompositionCoordinatorStats stats_after;
    MeisoCompositionScenePresentationStatus status;
    MeisoCompositionScenePresentationStatus status_after;
    MeisoRuntimeDesiredState desired;
    MeisoCompositionSceneWorldRef output_world;
    uint8_t token[MEISO_SHA256_DIGEST_SIZE];
    uint8_t wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];
    size_t wire_size = 0U;
    const size_t required = meiso_composition_coordinator_memory_required();
    const size_t alignment =
        meiso_composition_coordinator_alignment_required();

    CHECK(fixture_init(&fixture));
    CHECK(required != 0U && alignment >= _Alignof(void *));
    arena = calloc(1U, required + alignment + MEISO_SHA256_DIGEST_SIZE);
    CHECK(arena != NULL);
    memset(&config, 0, sizeof(config));
    config.scene_store = fixture.scene_store;
    config.session_id = fixture.session_id;
    config.edge_incarnation = fixture.edge_owner.incarnation;
    config.capability_generation = 5U;
    config.edge_clock_epoch = 77U;

    CHECK(meiso_composition_coordinator_init(
              arena, required - 1U, &config, &coordinator) ==
          MEISO_COMPOSITION_COORDINATOR_INIT_ARENA_TOO_SMALL);
    CHECK(coordinator == NULL);
    CHECK(meiso_composition_coordinator_init(
              (uint8_t *)arena + 1U, required, &config, &coordinator) ==
          MEISO_COMPOSITION_COORDINATOR_INIT_MISALIGNED_ARENA);
    CHECK(coordinator == NULL);
    CHECK(meiso_composition_coordinator_init(
              arena, required, &config,
              (MeisoCompositionCoordinator **)arena) ==
          MEISO_COMPOSITION_COORDINATOR_INIT_INVALID_ARGUMENT);
    memcpy(arena, &config, sizeof(config));
    CHECK(meiso_composition_coordinator_init(
              arena, required,
              (const MeisoCompositionCoordinatorConfig *)arena,
              &coordinator) ==
          MEISO_COMPOSITION_COORDINATOR_INIT_INVALID_ARGUMENT);
    memset(arena, 0, required);
    config.session_id.edge_nonce = (MeisoSessionNonce){0U, 0U};
    CHECK(meiso_composition_coordinator_init(
              arena, required, &config, &coordinator) ==
          MEISO_COMPOSITION_COORDINATOR_INIT_INVALID_ARGUMENT);
    config.session_id = fixture.session_id;
    CHECK(meiso_composition_coordinator_init(
              arena, required, &config, &coordinator) ==
          MEISO_COMPOSITION_COORDINATOR_INIT_OK);
    CHECK(meiso_composition_coordinator_begin_owner(
              coordinator, fixture.host_owner.incarnation) ==
          MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED);
    CHECK(!meiso_composition_coordinator_status(
        coordinator, (MeisoCompositionScenePresentationStatus *)arena
    ));

    plan = plan_value(1U, 0U);
    CHECK(!meiso_composition_coordinator_plan_digest(
        &plan, plan.candidate_digest
    ));
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &plan, (MeisoMutableBuffer){wire, sizeof(wire)}, &wire_size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    desired = (MeisoRuntimeDesiredState){true, {wire, wire_size}};
    CHECK(meiso_composition_coordinator_observe_desired(
              coordinator, fixture.host_owner.incarnation, 1U, &desired,
              (MeisoTimePoint){76U, 1U}) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_TIME);
    CHECK(meiso_composition_coordinator_observe_desired(
              coordinator, fixture.host_owner.incarnation + 1U, 1U,
              &desired, time_point(1U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_WRONG_OWNER);
    CHECK(meiso_composition_coordinator_observe_desired(
              coordinator, fixture.host_owner.incarnation, 1U, &desired,
              time_point(2U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING);
    CHECK(meiso_composition_coordinator_candidate(
        coordinator, &candidate
    ));
    CHECK(meiso_composition_coordinator_observation(
        coordinator, &observation
    ));
    CHECK(meiso_composition_coordinator_stats(coordinator, &stats));
    CHECK(meiso_composition_coordinator_status(coordinator, &status));
    fill_nonce(token, 1U);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              coordinator,
              (const MeisoCompositionCoordinatorPlanRef *)arena,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(3U), token) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_ARGUMENT);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              coordinator, &candidate.ref,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(3U), (const uint8_t *)arena) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_ARGUMENT);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              coordinator, &candidate.ref,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(3U),
              (const uint8_t *)arena + required - 16U) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_ARGUMENT);
    CHECK(meiso_composition_coordinator_candidate(
        coordinator, &candidate_after
    ));
    CHECK(meiso_composition_coordinator_observation(
        coordinator, &observation_after
    ));
    CHECK(meiso_composition_coordinator_stats(coordinator, &stats_after));
    CHECK(meiso_composition_coordinator_status(coordinator, &status_after));
    CHECK(memcmp(&candidate, &candidate_after, sizeof(candidate)) == 0 &&
          memcmp(&observation, &observation_after,
                 sizeof(observation)) == 0 &&
          memcmp(&stats, &stats_after, sizeof(stats)) == 0 &&
          memcmp(&status, &status_after, sizeof(status)) == 0);
    desired.profile = (MeisoByteView){arena,
        MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE};
    CHECK(meiso_composition_coordinator_observe_desired(
              coordinator, fixture.host_owner.incarnation, 2U, &desired,
              time_point(3U)) ==
          MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_ARGUMENT);
    CHECK(!meiso_composition_coordinator_observation(
        coordinator, (MeisoCompositionCoordinatorObservation *)arena
    ));
    CHECK(!meiso_composition_coordinator_candidate(
        coordinator, (MeisoCompositionCoordinatorPlanView *)arena
    ));
    CHECK(!meiso_composition_coordinator_active(
        coordinator, (MeisoCompositionCoordinatorPlanView *)arena
    ));
    CHECK(!meiso_composition_coordinator_stats(
        coordinator, (MeisoCompositionCoordinatorStats *)arena
    ));
    fill_nonce(token, 1U);
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
              coordinator, token, time_point(3U),
              (MeisoCompositionSceneWorldRef *)arena) ==
          MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_ARGUMENT);
    output_world = (MeisoCompositionSceneWorldRef){1U, 1U, 1U, 1U};
    CHECK(meiso_composition_coordinator_swap_scene_frame_boundary(
              coordinator, fixture.host_owner.incarnation, 1U,
              time_point(3U), (MeisoCompositionSceneWorldRef *)arena) ==
          MEISO_COMPOSITION_COORDINATOR_SWAP_INVALID_ARGUMENT);
    CHECK(output_world.world_generation == 1U);
    free(arena);
    fixture_destroy(&fixture);
    return true;
}

static bool test_parallel_session_scope_invalidation(void)
{
    Fixture fixture;
    void *second_arena;
    MeisoCompositionCoordinator *first;
    MeisoCompositionCoordinator *second = NULL;
    MeisoCompositionCoordinatorConfig config;
    MeisoCompositionSceneControlScope scope_before;
    MeisoCompositionSceneControlScope scope_after;
    uint8_t cube_bytes[512];
    uint8_t old_token[MEISO_SHA256_DIGEST_SIZE];
    uint8_t new_token[MEISO_SHA256_DIGEST_SIZE];
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];
    MeisoByteView cube_wire;
    MeisoContentId cube_id;
    MeisoCompositionSceneDesired scene;
    MeisoCompositionScenePresentationPlan baseline_plan;
    MeisoCompositionScenePresentationPlan plan;
    MeisoCompositionCoordinatorPlanView old_active;
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionCoordinatorPlanRef expected;
    MeisoCompositionCoordinatorObservation observation;
    MeisoCompositionCoordinatorStats stats;
    MeisoCompositionScenePresentationStatus status;
    MeisoCompositionSceneWorldRef old_world;
    MeisoCompositionSceneWorldRef output_world;
    MeisoCompositionSceneWorldView scene_candidate;
    ReceivedPlan received_plan;
    ReceivedScene received_scene;
    const size_t required = meiso_composition_coordinator_memory_required();

    CHECK(fixture_init(&fixture));
    CHECK(establish_active(
        &fixture, cube_bytes, &cube_wire, &cube_id, &scene,
        &baseline_plan, &old_world, old_token
    ));
    first = fixture.coordinator;
    CHECK(meiso_composition_coordinator_active(first, &old_active));
    CHECK(meiso_composition_scene_store_control_scope(
        fixture.scene_store, &scope_before
    ));
    CHECK(scope_before.control_scope_generation != 0U);

    second_arena = calloc(1U, required);
    CHECK(second_arena != NULL);
    memset(&config, 0, sizeof(config));
    config.scene_store = fixture.scene_store;
    config.session_id = fixture.session_id;
    config.session_id.host_nonce.low++;
    config.edge_incarnation = fixture.edge_owner.incarnation;
    config.capability_generation = 5U;
    config.edge_clock_epoch = 77U;
    CHECK(meiso_composition_coordinator_init(
              second_arena, required, &config, &second) ==
          MEISO_COMPOSITION_COORDINATOR_INIT_OK);
    CHECK(meiso_composition_coordinator_begin_owner(
              second, fixture.host_owner.incarnation) ==
          MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED);
    CHECK(meiso_composition_scene_store_control_scope(
        fixture.scene_store, &scope_after
    ));
    CHECK(scope_after.desired_owner_incarnation ==
              scope_before.desired_owner_incarnation &&
          scope_after.control_scope_generation ==
              scope_before.control_scope_generation + 1U);

    CHECK(!meiso_composition_coordinator_observation(first, &observation));
    CHECK(!meiso_composition_coordinator_candidate(first, &candidate));
    CHECK(!meiso_composition_coordinator_active(first, &candidate));
    CHECK(!meiso_composition_coordinator_status(first, &status));
    CHECK(!meiso_composition_coordinator_stats(first, &stats));
    expected = old_active.ref;
    fill_nonce(nonce, 2U);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              first, &expected, MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(200U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_ARGUMENT);
    output_world = old_world;
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
              first, old_token, time_point(200U), &output_world) ==
          MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_ARGUMENT);
    CHECK(memcmp(&output_world, &old_world, sizeof(output_world)) == 0);

    fixture.coordinator = second;
    fixture.session_id = config.session_id;
    CHECK(publish_mesh(
        &fixture, cube_id, cube_wire, MEISO_BLOB_COMMIT_DUPLICATE,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));
    CHECK(publish_scene(
        &fixture, 2U, 1U, &scene, &received_scene,
        MEISO_COMPOSITION_SCENE_PREPARE_PREPARED
    ));
    plan = plan_value(1U, 1000U);
    CHECK(publish_plan(
        &fixture, 3U, true, &plan, time_point(210U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING
    ));
    CHECK(meiso_composition_coordinator_candidate(second, &candidate));
    expected = candidate.ref;
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
              second, &expected,
              MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
              time_point(220U), nonce) ==
          MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED);
    CHECK(meiso_composition_coordinator_candidate(second, &candidate));
    memcpy(new_token, candidate.prepared_token, sizeof(new_token));
    CHECK(memcmp(new_token, old_token, sizeof(new_token)) != 0);

    output_world = old_world;
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
              first, old_token, time_point(230U), &output_world) ==
          MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_ARGUMENT);
    CHECK(meiso_composition_scene_store_candidate(
        fixture.scene_store, &scene_candidate
    ));
    CHECK(scene_candidate.ref.desired_state_version == 2U &&
          scene_candidate.ref.plan_generation == 1U);
    plan.prepared_token_echo_present = true;
    memcpy(plan.prepared_token_echo, new_token, sizeof(new_token));
    CHECK(publish_plan(
        &fixture, 4U, true, &plan, time_point(230U), &received_plan,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING
    ));
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
              second, new_token, time_point(240U), &output_world) ==
          MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED);
    CHECK(output_world.world_generation == old_world.world_generation + 1U);
    CHECK(!meiso_composition_coordinator_active(first, &candidate));
    free(second_arena);
    fixture_destroy(&fixture);
    return true;
}

static bool test_success_swap_status_and_disable(void)
{
    Fixture fixture;
    uint8_t cube_bytes[512];
    MeisoByteView cube_wire;
    MeisoContentId cube_id;
    MeisoCompositionSceneDesired scene;
    MeisoCompositionScenePresentationPlan plan;
    MeisoCompositionCoordinatorPlanView prepared;
    MeisoCompositionCoordinatorPlanView active_plan;
    MeisoCompositionCoordinatorPlanView wait_active_before;
    MeisoCompositionCoordinatorPlanView wait_active_after;
    MeisoCompositionCoordinatorObservation wait_observation_before;
    MeisoCompositionCoordinatorObservation wait_observation_after;
    MeisoCompositionCoordinatorStats wait_stats_before;
    MeisoCompositionCoordinatorStats wait_stats_after;
    MeisoCompositionScenePresentationStatus wait_status_before;
    MeisoCompositionScenePresentationStatus wait_status_after;
    MeisoCompositionSceneWorldRef world;
    MeisoCompositionSceneWorldRef old_world;
    MeisoCompositionSceneWorldView scene_world;
    MeisoCompositionSceneWorldView wait_world_before;
    MeisoCompositionSceneWorldView wait_world_after;
    ReceivedScene received_scene;
    ReceivedPlan disabled;

    CHECK(fixture_init(&fixture));
    CHECK(encode_cube(cube_bytes, &cube_wire, &cube_id));
    CHECK(publish_mesh(
        &fixture, cube_id, cube_wire, MEISO_BLOB_COMMIT_PUBLISHED,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));
    scene = scene_value(cube_id);
    CHECK(publish_scene(
        &fixture, 1U, 1U, &scene, &received_scene,
        MEISO_COMPOSITION_SCENE_PREPARE_PREPARED
    ));
    CHECK(prepare_ready_plan(
        &fixture, 1U, 1U, 1U, &plan, &prepared
    ));
    CHECK(status_roundtrip(
        &fixture, MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED,
        MEISO_COMPOSITION_SCENE_ACTIVE_NONE,
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE
    ));
    CHECK(prepared.first_observed_at.monotonic_ns == 100U &&
          prepared.prepared_at.monotonic_ns == 110U &&
          prepared.offline_lease_ms == 1000U);
    CHECK(!meiso_composition_coordinator_active(
        fixture.coordinator, &active_plan
    ));
    CHECK(!meiso_composition_scene_store_active(
        fixture.scene_store, &scene_world
    ));
    CHECK(echo_and_activate(&fixture, 2U, &plan, &prepared, &world));
    CHECK(world.desired_state_version == 1U &&
          world.plan_generation == 1U && world.world_generation == 1U);
    CHECK(meiso_composition_coordinator_active(
        fixture.coordinator, &active_plan
    ));
    CHECK(active_plan.ref.desired_state_version == 2U &&
          active_plan.active_state == MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE &&
          active_plan.activated_at.monotonic_ns == 130U);
    CHECK(publish_presentation_actual(&fixture, 1U, &active_plan));
    CHECK(render_to_stream(&fixture, &world, 1U));

    old_world = world;
    scene.objects[0].transform.position[0] = 1.0F;
    CHECK(publish_scene(
        &fixture, 2U, 1U, &scene, &received_scene,
        MEISO_COMPOSITION_SCENE_PREPARE_PREPARED
    ));
    CHECK(meiso_composition_coordinator_swap_scene_frame_boundary(
              fixture.coordinator, fixture.host_owner.incarnation, 3U,
              time_point(140U), &world) ==
          MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_MISMATCH);
    CHECK(meiso_composition_scene_store_active(
        fixture.scene_store, &scene_world
    ));
    CHECK(scene_world.ref.world_generation == old_world.world_generation);
    CHECK(meiso_composition_coordinator_swap_scene_frame_boundary(
              fixture.coordinator, fixture.host_owner.incarnation, 2U,
              time_point(140U), &world) ==
          MEISO_COMPOSITION_COORDINATOR_SWAP_ACTIVATED);
    CHECK(world.world_generation == 2U && world.desired_state_version == 2U);

    CHECK(publish_plan(
        &fixture, 3U, false, NULL, time_point(150U), &disabled,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING
    ));
    memset(&wait_active_before, 0, sizeof(wait_active_before));
    memset(&wait_active_after, 0, sizeof(wait_active_after));
    memset(&wait_observation_before, 0, sizeof(wait_observation_before));
    memset(&wait_observation_after, 0, sizeof(wait_observation_after));
    memset(&wait_stats_before, 0, sizeof(wait_stats_before));
    memset(&wait_stats_after, 0, sizeof(wait_stats_after));
    memset(&wait_status_before, 0, sizeof(wait_status_before));
    memset(&wait_status_after, 0, sizeof(wait_status_after));
    memset(&wait_world_before, 0, sizeof(wait_world_before));
    memset(&wait_world_after, 0, sizeof(wait_world_after));
    CHECK(meiso_composition_coordinator_active(
        fixture.coordinator, &wait_active_before
    ));
    CHECK(meiso_composition_coordinator_observation(
        fixture.coordinator, &wait_observation_before
    ));
    CHECK(meiso_composition_coordinator_stats(
        fixture.coordinator, &wait_stats_before
    ));
    CHECK(meiso_composition_coordinator_status(
        fixture.coordinator, &wait_status_before
    ));
    CHECK(meiso_composition_scene_store_active(
        fixture.scene_store, &wait_world_before
    ));
    CHECK(meiso_composition_coordinator_disable_frame_boundary(
              fixture.coordinator, false, time_point(160U)) ==
          MEISO_COMPOSITION_COORDINATOR_DISABLE_WAITING_DRAIN);
    CHECK(meiso_composition_coordinator_active(
        fixture.coordinator, &wait_active_after
    ));
    CHECK(meiso_composition_coordinator_observation(
        fixture.coordinator, &wait_observation_after
    ));
    CHECK(meiso_composition_coordinator_stats(
        fixture.coordinator, &wait_stats_after
    ));
    CHECK(meiso_composition_coordinator_status(
        fixture.coordinator, &wait_status_after
    ));
    CHECK(meiso_composition_scene_store_active(
        fixture.scene_store, &wait_world_after
    ));
    CHECK(memcmp(&wait_active_before, &wait_active_after,
                 sizeof(wait_active_before)) == 0 &&
          memcmp(&wait_observation_before, &wait_observation_after,
                 sizeof(wait_observation_before)) == 0 &&
          memcmp(&wait_stats_before, &wait_stats_after,
                 sizeof(wait_stats_before)) == 0 &&
          memcmp(&wait_status_before, &wait_status_after,
                 sizeof(wait_status_before)) == 0 &&
          memcmp(&wait_world_before, &wait_world_after,
                 sizeof(wait_world_before)) == 0);
    CHECK(meiso_composition_coordinator_active(
        fixture.coordinator, &active_plan
    ));
    CHECK(meiso_composition_scene_store_active(
        fixture.scene_store, &scene_world
    ));
    CHECK(meiso_composition_coordinator_disable_frame_boundary(
              fixture.coordinator, true, time_point(155U)) ==
          MEISO_COMPOSITION_COORDINATOR_DISABLE_DISABLED);
    CHECK(!meiso_composition_coordinator_active(
        fixture.coordinator, &active_plan
    ));
    CHECK(!meiso_composition_scene_store_active(
        fixture.scene_store, &scene_world
    ));
    CHECK(status_roundtrip(
        &fixture, MEISO_COMPOSITION_SCENE_CANDIDATE_NONE,
        MEISO_COMPOSITION_SCENE_ACTIVE_NONE,
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE
    ));
    CHECK(publish_stopped_actual(&fixture, 2U, 3U));
    fixture_destroy(&fixture);
    return true;
}

int main(void)
{
    if (!test_plan_digest_independent_golden() ||
        !test_success_swap_status_and_disable() ||
        !test_runtime_shape_and_observation_order() ||
        !test_async_failures_and_late_completion() ||
        !test_token_scene_supersede_and_generation_guards() ||
        !test_same_owner_new_session_no_resurrection() ||
        !test_api_memory_alias_and_time_guards() ||
        !test_parallel_session_scope_invalidation()) {
        return 1;
    }
    (void)puts("composition coordinator tests passed");
    return 0;
}
