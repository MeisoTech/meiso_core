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
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                       \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct {
    MeisoContentId id;
    MeisoByteView content;
} BlobVerifierContext;

typedef struct {
    void *blob_arena;
    void *desired_publisher_arena;
    void *desired_replica_arena;
    void *actual_publisher_arena;
    void *actual_replica_arena;
    void *result_stream_arena;
    void *scene_store_arena;
    MeisoBlobStore *blob_store;
    MeisoStatePublisher *desired_publisher;
    MeisoStateReplica *desired_replica;
    MeisoStatePublisher *actual_publisher;
    MeisoStateReplica *actual_replica;
    MeisoStream *result_stream;
    MeisoCompositionSceneStore *scene_store;
    MeisoOwnerScope host_owner;
    MeisoOwnerScope edge_owner;
    MeisoDeliveryGeneration desired_delivery;
    MeisoDeliveryGeneration actual_delivery;
    MeisoCompositionSceneResourceUsage resource_usage;
} Fixture;

typedef struct {
    MeisoStateView view;
    MeisoRuntimeDesiredState runtime;
    MeisoCompositionSceneDesired desired;
} ReceivedDesired;

static const uint8_t CANONICAL_CUBE_SHA256[MEISO_SHA256_DIGEST_SIZE] = {
    0x80U, 0xa8U, 0x0fU, 0x8dU, 0x4eU, 0x5bU, 0x73U, 0x8fU,
    0x09U, 0x6eU, 0x01U, 0x62U, 0xdfU, 0x9eU, 0x01U, 0x3aU,
    0x1cU, 0xc1U, 0xddU, 0xd7U, 0x07U, 0xd6U, 0xb0U, 0x29U,
    0x83U, 0x8bU, 0x10U, 0x61U, 0x93U, 0x1fU, 0xb0U, 0x84U
};

static void fill_bytes(uint8_t *output, size_t size, uint8_t first)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        output[index] = (uint8_t)(first + (uint8_t)index);
    }
}

static MeisoContentId synthetic_content_id(uint8_t first)
{
    MeisoContentId value;

    memset(&value, 0, sizeof(value));
    value.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    value.digest_size = MEISO_SHA256_DIGEST_SIZE;
    fill_bytes(value.digest, MEISO_SHA256_DIGEST_SIZE, first);
    return value;
}

static bool verify_blob(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    const BlobVerifierContext *expected = context;

    return expected != NULL &&
        meiso_content_id_equal(expected_content_id, &expected->id) &&
        staged_content.size == expected->content.size &&
        (staged_content.size == 0U ||
         memcmp(staged_content.data, expected->content.data,
                staged_content.size) == 0);
}

static bool mesh_content_id(MeisoByteView content, MeisoContentId *out_id)
{
    return meiso_composition_scene_mesh_content_id(content, out_id);
}

static void fixture_destroy(Fixture *fixture)
{
    free(fixture->scene_store_arena);
    free(fixture->result_stream_arena);
    free(fixture->actual_replica_arena);
    free(fixture->actual_publisher_arena);
    free(fixture->desired_replica_arena);
    free(fixture->desired_publisher_arena);
    free(fixture->blob_arena);
    memset(fixture, 0, sizeof(*fixture));
}

static bool fixture_init(Fixture *fixture)
{
    const MeisoResourceDescriptor *blob_descriptor =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
        );
    const MeisoResourceDescriptor *desired_descriptor =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_DESIRED
        );
    const MeisoResourceDescriptor *actual_descriptor =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_ACTUAL
        );
    const MeisoResourceDescriptor *result_descriptor =
        meiso_composition_scene_resource(
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
    size_t required;

    memset(fixture, 0, sizeof(*fixture));
    fixture->host_owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 101U};
    fixture->edge_owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 202U};
    fixture->desired_delivery = (MeisoDeliveryGeneration){11U};
    fixture->actual_delivery = (MeisoDeliveryGeneration){12U};

    CHECK(blob_descriptor != NULL && desired_descriptor != NULL &&
          actual_descriptor != NULL && result_descriptor != NULL);

    required = meiso_blob_store_memory_required(blob_descriptor, &blob_config);
    CHECK(required != 0U);
    fixture->blob_arena = calloc(1U, required);
    CHECK(fixture->blob_arena != NULL);
    CHECK(meiso_blob_store_init(
              fixture->blob_arena, required, blob_descriptor, &blob_config,
              &fixture->blob_store) == MEISO_BLOB_INIT_OK);

    required = meiso_state_publisher_memory_required(
        desired_descriptor, &state_config
    );
    CHECK(required != 0U);
    fixture->desired_publisher_arena = calloc(1U, required);
    CHECK(fixture->desired_publisher_arena != NULL);
    CHECK(meiso_state_publisher_init(
              fixture->desired_publisher_arena, required, desired_descriptor,
              &state_config, &fixture->desired_publisher) ==
          MEISO_STATE_INIT_OK);

    required = meiso_state_replica_memory_required(desired_descriptor);
    CHECK(required != 0U);
    fixture->desired_replica_arena = calloc(1U, required);
    CHECK(fixture->desired_replica_arena != NULL);
    CHECK(meiso_state_replica_init(
              fixture->desired_replica_arena, required, desired_descriptor,
              &fixture->desired_replica) == MEISO_STATE_INIT_OK);

    required = meiso_state_publisher_memory_required(
        actual_descriptor, &state_config
    );
    CHECK(required != 0U);
    fixture->actual_publisher_arena = calloc(1U, required);
    CHECK(fixture->actual_publisher_arena != NULL);
    CHECK(meiso_state_publisher_init(
              fixture->actual_publisher_arena, required, actual_descriptor,
              &state_config, &fixture->actual_publisher) ==
          MEISO_STATE_INIT_OK);

    required = meiso_state_replica_memory_required(actual_descriptor);
    CHECK(required != 0U);
    fixture->actual_replica_arena = calloc(1U, required);
    CHECK(fixture->actual_replica_arena != NULL);
    CHECK(meiso_state_replica_init(
              fixture->actual_replica_arena, required, actual_descriptor,
              &fixture->actual_replica) == MEISO_STATE_INIT_OK);

    required = meiso_stream_memory_required(result_descriptor, &stream_config);
    CHECK(required != 0U);
    fixture->result_stream_arena = calloc(1U, required);
    CHECK(fixture->result_stream_arena != NULL);
    CHECK(meiso_stream_init(
              fixture->result_stream_arena, required, result_descriptor,
              &stream_config, &fixture->result_stream) ==
          MEISO_STREAM_INIT_OK);

    required = meiso_composition_scene_store_memory_required();
    CHECK(required != 0U);
    fixture->scene_store_arena = calloc(1U, required);
    CHECK(fixture->scene_store_arena != NULL);
    CHECK(meiso_composition_scene_store_init(
              fixture->scene_store_arena, required, fixture->blob_store,
              &fixture->scene_store) ==
          MEISO_COMPOSITION_SCENE_STORE_INIT_OK);
    CHECK(meiso_composition_scene_store_begin_owner(
              fixture->scene_store, fixture->host_owner.incarnation) ==
          MEISO_COMPOSITION_SCENE_OWNER_STARTED);

    CHECK(meiso_state_publisher_begin_delivery(
              fixture->desired_publisher, fixture->desired_delivery) ==
          MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_replica_begin_owner(
              fixture->desired_replica, fixture->host_owner) ==
          MEISO_STATE_OWNER_STARTED);
    CHECK(meiso_state_publisher_begin_delivery(
              fixture->actual_publisher, fixture->actual_delivery) ==
          MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_replica_begin_owner(
              fixture->actual_replica, fixture->edge_owner) ==
          MEISO_STATE_OWNER_STARTED);
    CHECK(meiso_stream_begin_producer(
              fixture->result_stream, fixture->edge_owner, 31U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    return true;
}

static MeisoCompositionSceneDesired desired_with_mesh(MeisoContentId mesh_id)
{
    MeisoCompositionSceneDesired value;
    MeisoCompositionSceneObject *object;

    memset(&value, 0, sizeof(value));
    value.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    value.camera.space_epoch = 9U;
    value.camera.source_time = (MeisoTimePoint){7U, 100U};
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

static bool encode_desired_payload(
    bool enabled,
    const MeisoCompositionSceneDesired *desired,
    uint8_t *output,
    size_t output_capacity,
    MeisoByteView *out_payload
)
{
    uint8_t profile[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    MeisoRuntimeDesiredState runtime;
    size_t profile_size = 0U;

    memset(&runtime, 0, sizeof(runtime));
    runtime.enabled = enabled;
    if (enabled) {
        CHECK(desired != NULL);
        CHECK(meiso_composition_scene_desired_encode(
                  desired,
                  (MeisoMutableBuffer){profile, sizeof(profile)},
                  &profile_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
        runtime.profile = (MeisoByteView){profile, profile_size};
    }
    CHECK(meiso_runtime_desired_state_encode(
        &runtime, (MeisoMutableBuffer){output, output_capacity}, out_payload
    ));
    return true;
}

static bool publish_and_receive_desired(
    Fixture *fixture,
    uint64_t version,
    bool enabled,
    const MeisoCompositionSceneDesired *desired,
    MeisoStatePublishStatus expected_publish,
    MeisoStateReceiveStatus expected_receive,
    ReceivedDesired *out_received
)
{
    uint8_t payload_storage[
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    uint8_t transferred[
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView payload;
    MeisoStateOffer offer;
    MeisoStateView current;

    CHECK(encode_desired_payload(
        enabled, desired, payload_storage, sizeof(payload_storage), &payload
    ));
    CHECK(meiso_state_publisher_publish(
              fixture->desired_publisher, fixture->host_owner, version,
              payload) == expected_publish);
    if (expected_publish != MEISO_STATE_PUBLISH_NEW &&
        expected_publish != MEISO_STATE_PUBLISH_REPLACED_PENDING) {
        return true;
    }
    CHECK(meiso_state_publisher_next_offer(
        fixture->desired_publisher, &offer
    ));
    CHECK(offer.state.payload.size <= sizeof(transferred));
    memmove(transferred, offer.state.payload.data, offer.state.payload.size);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
              fixture->desired_publisher, fixture->desired_delivery,
              offer.state.owner, offer.state.version) ==
          MEISO_STATE_ACCEPT_CONFIRMED);
    CHECK(meiso_state_replica_receive(
              fixture->desired_replica, offer.state.owner,
              offer.state.version,
              (MeisoByteView){transferred, offer.state.payload.size}) ==
          expected_receive);
    if (expected_receive != MEISO_STATE_RECEIVE_NEW || out_received == NULL) {
        return true;
    }
    CHECK(meiso_state_replica_current(fixture->desired_replica, &current));
    memset(out_received, 0, sizeof(*out_received));
    out_received->view = current;
    CHECK(meiso_runtime_desired_state_decode(
        current.payload, &out_received->runtime
    ));
    if (out_received->runtime.enabled) {
        CHECK(meiso_composition_scene_desired_decode(
                  out_received->runtime.profile, &out_received->desired) ==
              MEISO_COMPOSITION_SCENE_CODEC_OK);
    }
    return true;
}

static MeisoCompositionScenePrepareStatus prepare_received(
    Fixture *fixture,
    uint64_t plan_generation,
    const ReceivedDesired *received
)
{
    return meiso_composition_scene_store_prepare(
        fixture->scene_store,
        received->view.owner.incarnation,
        received->view.version,
        plan_generation,
        &received->desired
    );
}

static bool publish_blob_bytes(
    Fixture *fixture,
    MeisoContentId id,
    MeisoByteView bytes,
    MeisoBlobCommitStatus expected_commit,
    MeisoCompositionSceneBlobVisibilityStatus expected_visibility
)
{
    MeisoMutableBuffer staging;

    CHECK(meiso_blob_store_stage_begin(
              fixture->blob_store, &id, bytes.size) ==
          MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(fixture->blob_store, &staging));
    CHECK(staging.capacity == bytes.size);
    if (bytes.size != 0U) {
        memmove(staging.data, bytes.data, bytes.size);
    }
    CHECK(meiso_blob_store_commit_verified(
              fixture->blob_store, bytes.size,
              meiso_composition_scene_mesh_blob_verify, NULL) ==
          expected_commit);
    if (expected_commit == MEISO_BLOB_COMMIT_PUBLISHED) {
        fixture->resource_usage.published_blob_count++;
        fixture->resource_usage.published_blob_bytes += bytes.size;
    }
    if (expected_commit == MEISO_BLOB_COMMIT_PUBLISHED ||
        expected_commit == MEISO_BLOB_COMMIT_DUPLICATE) {
        CHECK(meiso_composition_scene_store_mark_blob_visible(
                  fixture->scene_store, fixture->host_owner.incarnation,
                  &id, bytes.size) == expected_visibility);
    }
    return true;
}

static bool encode_cube(uint8_t *output, size_t capacity, MeisoByteView *view)
{
    size_t encoded_size = 0U;

    CHECK(meiso_composition_scene_mesh_encode(
              meiso_composition_scene_canonical_cube(),
              (MeisoMutableBuffer){output, capacity}, &encoded_size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    view->data = output;
    view->size = encoded_size;
    return true;
}

static bool encode_triangle_variant(
    uint8_t variant,
    uint8_t *output,
    size_t capacity,
    MeisoByteView *view,
    MeisoContentId *out_id
)
{
    MeisoCompositionSceneMeshVertex vertices[3];
    const uint32_t indices[3] = {0U, 1U, 2U};
    MeisoCompositionSceneMesh mesh;
    const float offset = (float)variant;
    size_t encoded_size = 0U;

    memset(vertices, 0, sizeof(vertices));
    vertices[0].position[0] = offset;
    vertices[1].position[0] = offset + 1.0F;
    vertices[2].position[0] = offset;
    vertices[2].position[1] = 1.0F;
    memset(&mesh, 0, sizeof(mesh));
    mesh.vertex_count = 3U;
    mesh.vertices = vertices;
    mesh.index_count = 3U;
    mesh.indices = indices;
    CHECK(meiso_composition_scene_mesh_encode(
              &mesh, (MeisoMutableBuffer){output, capacity}, &encoded_size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    *view = (MeisoByteView){output, encoded_size};
    CHECK(mesh_content_id(*view, out_id));
    return true;
}

static bool publish_running_actual(
    Fixture *fixture,
    uint64_t actual_version,
    uint64_t desired_version,
    uint64_t production_epoch
)
{
    uint8_t suffix[MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE];
    uint8_t payload_storage[
        MEISO_COMPOSITION_SCENE_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    uint8_t transferred[
        MEISO_COMPOSITION_SCENE_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoCompositionSceneStatus status;
    MeisoCompositionSceneStatus decoded_status;
    MeisoRuntimeActualState actual;
    MeisoRuntimeActualState decoded_actual;
    MeisoByteView payload;
    MeisoStateOffer offer;
    MeisoStateView current;
    size_t suffix_size = 0U;

    CHECK(meiso_composition_scene_store_status(
        fixture->scene_store, &fixture->resource_usage, &status
    ));
    CHECK(meiso_composition_scene_status_encode(
              &status, (MeisoMutableBuffer){suffix, sizeof(suffix)},
              &suffix_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = (MeisoRuntimeDesiredReference){
        true, fixture->host_owner, desired_version
    };
    actual.applied_desired = actual.observed_desired;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = production_epoch;
    actual.profile = (MeisoByteView){suffix, suffix_size};
    CHECK(meiso_runtime_actual_state_encode(
        &actual, (MeisoMutableBuffer){payload_storage, sizeof(payload_storage)},
        &payload
    ));
    CHECK(meiso_state_publisher_publish(
              fixture->actual_publisher, fixture->edge_owner, actual_version,
              payload) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_next_offer(
        fixture->actual_publisher, &offer
    ));
    CHECK(offer.state.payload.size <= sizeof(transferred));
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
    CHECK(meiso_runtime_actual_state_decode(current.payload, &decoded_actual));
    CHECK(decoded_actual.applied_desired.present &&
          decoded_actual.applied_desired.owner.incarnation ==
              fixture->host_owner.incarnation &&
          decoded_actual.applied_desired.version == desired_version &&
          decoded_actual.production_epoch == production_epoch);
    CHECK(meiso_composition_scene_status_decode(
              decoded_actual.profile, &decoded_status) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded_status.world_generation == status.world_generation &&
          decoded_status.active_object_count == status.active_object_count);
    return true;
}

static bool receive_render_result(
    Fixture *fixture,
    const MeisoCompositionSceneRenderResult *result,
    const MeisoCompositionSceneWorldRef *rendered_world,
    uint64_t sequence,
    MeisoStreamReceiveFlags expected_flags
)
{
    uint8_t payload[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE];
    MeisoStreamSample sample;
    MeisoStreamReceiveResult receive;
    size_t payload_size = 0U;

    CHECK(meiso_composition_scene_render_result_encode(
              result, (MeisoMutableBuffer){payload, sizeof(payload)},
              &payload_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){fixture->edge_owner, 31U, sequence};
    sample.source_time = (MeisoTimePoint){
        result->source_clock_epoch, result->render_end_monotonic_ns
    };
    sample.payload = (MeisoByteView){payload, payload_size};
    receive = meiso_stream_receive(fixture->result_stream, &sample, NULL);
    CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          receive.flags == expected_flags);
    fixture->resource_usage.result_queue_depth =
        (uint32_t)meiso_stream_count(fixture->result_stream);
    CHECK(meiso_composition_scene_store_record_result_published(
        fixture->scene_store, rendered_world, 31U, sequence
    ));
    return true;
}

static bool render_and_check(
    Fixture *fixture,
    uint64_t expected_desired_version,
    uint64_t sequence,
    MeisoCompositionSceneRenderResult *out_result
)
{
    MeisoCompositionSceneWorldView active;
    MeisoStreamSample queued;
    MeisoCompositionSceneRenderResult decoded;

    CHECK(meiso_composition_scene_store_active(fixture->scene_store, &active));
    CHECK(meiso_composition_scene_store_fake_render(
              fixture->scene_store, 55U, 1000U, 1010U, out_result) ==
          MEISO_COMPOSITION_SCENE_RENDER_COMPLETE);
    CHECK(out_result->applied_scene_owner_incarnation ==
              fixture->host_owner.incarnation &&
          out_result->applied_scene_state_version ==
              expected_desired_version &&
          out_result->active_plan_generation == active.ref.plan_generation &&
          out_result->world_generation == active.ref.world_generation);
    CHECK(receive_render_result(
        fixture, out_result, &active.ref, sequence,
        MEISO_STREAM_RECEIVE_FLAG_NONE
    ));
    CHECK(meiso_stream_peek(fixture->result_stream, &queued));
    CHECK(meiso_composition_scene_render_result_decode(
              queued.payload, &decoded) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded.applied_scene_state_version == expected_desired_version &&
          decoded.world_generation == active.ref.world_generation);
    CHECK(meiso_stream_pop(fixture->result_stream));
    fixture->resource_usage.result_queue_depth =
        (uint32_t)meiso_stream_count(fixture->result_stream);
    return true;
}

static bool test_success_transcript(void)
{
    Fixture fixture;
    uint8_t cube_wire[512];
    uint8_t second_mesh_wire[128];
    MeisoByteView cube;
    MeisoByteView second_mesh;
    MeisoContentId cube_id;
    MeisoContentId second_mesh_id;
    const uint64_t plan_generation = 71U;
    MeisoCompositionSceneDesired desired;
    ReceivedDesired received;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionSceneWorldView candidate;
    MeisoCompositionSceneWorldRef active_ref;
    MeisoCompositionSceneWorldRef wrong_ref;
    MeisoCompositionSceneStatus status;
    MeisoCompositionSceneRenderResult first_result;
    MeisoCompositionSceneRenderResult moved_result;
    MeisoCompositionSceneRenderResult camera_result;
    MeisoCompositionSceneRenderResult second_mesh_result;
    MeisoCompositionSceneRenderResult shared_mesh_result;

    CHECK(fixture_init(&fixture));
    CHECK(encode_cube(cube_wire, sizeof(cube_wire), &cube));
    CHECK(mesh_content_id(cube, &cube_id));
    CHECK(memcmp(cube_id.digest, CANONICAL_CUBE_SHA256,
                 sizeof(CANONICAL_CUBE_SHA256)) == 0);
    CHECK(encode_triangle_variant(
        1U, second_mesh_wire, sizeof(second_mesh_wire), &second_mesh,
        &second_mesh_id
    ));
    desired = desired_with_mesh(cube_id);

    CHECK(publish_and_receive_desired(
        &fixture, 1U, true, &desired, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, plan_generation, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB);
    CHECK(!meiso_composition_scene_store_active(
        fixture.scene_store, &active
    ));
    CHECK(!meiso_composition_scene_store_candidate(
        fixture.scene_store, &candidate
    ));

    CHECK(publish_blob_bytes(
        &fixture, cube_id, cube, MEISO_BLOB_COMMIT_PUBLISHED,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));
    CHECK(prepare_received(&fixture, plan_generation, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_candidate(
        fixture.scene_store, &candidate
    ));
    CHECK(candidate.ref.desired_state_version == 1U &&
          candidate.ref.world_generation == 0U &&
          candidate.object_count == 1U && candidate.unique_mesh_count == 1U);
    CHECK(!meiso_composition_scene_store_active(
        fixture.scene_store, &active
    ));
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 1U,
              plan_generation + 1U, &active_ref) ==
          MEISO_COMPOSITION_SCENE_ACTIVATE_MISMATCH);
    CHECK(!meiso_composition_scene_store_active(
        fixture.scene_store, &active
    ));
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 1U,
              plan_generation, &active_ref) ==
          MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(active_ref.world_generation == 1U);
    CHECK(publish_running_actual(&fixture, 1U, 1U, 31U));
    CHECK(render_and_check(&fixture, 1U, 1U, &first_result));

    desired.objects[0].transform.position[0] = 1.0F;
    CHECK(publish_and_receive_desired(
        &fixture, 2U, true, &desired, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, plan_generation, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_active(fixture.scene_store, &active));
    CHECK(active.ref.desired_state_version == 1U);
    CHECK(meiso_composition_scene_store_candidate(
        fixture.scene_store, &candidate
    ));
    CHECK(candidate.ref.desired_state_version == 2U);
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 2U,
              plan_generation, &active_ref) ==
          MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(render_and_check(&fixture, 2U, 2U, &moved_result));
    CHECK(memcmp(first_result.full_target_sha256,
                 moved_result.full_target_sha256,
                 MEISO_SHA256_DIGEST_SIZE) != 0);
    CHECK(memcmp(first_result.thumbnail_rgb565,
                 moved_result.thumbnail_rgb565,
                 MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE) != 0);

    desired.camera.position[1] = 1.0F;
    CHECK(publish_and_receive_desired(
        &fixture, 3U, true, &desired, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, plan_generation, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 3U,
              plan_generation, &active_ref) ==
          MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(render_and_check(&fixture, 3U, 3U, &camera_result));
    CHECK(memcmp(moved_result.full_target_sha256,
                 camera_result.full_target_sha256,
                 MEISO_SHA256_DIGEST_SIZE) != 0);
    CHECK(memcmp(moved_result.thumbnail_rgb565,
                 camera_result.thumbnail_rgb565,
                 MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE) != 0);

    CHECK(publish_blob_bytes(
        &fixture, second_mesh_id, second_mesh,
        MEISO_BLOB_COMMIT_PUBLISHED,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));

    desired.objects[0].mesh_content_id = second_mesh_id;
    CHECK(publish_and_receive_desired(
        &fixture, 4U, true, &desired, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, plan_generation, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 4U,
              plan_generation, &active_ref) ==
          MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(render_and_check(&fixture, 4U, 4U, &second_mesh_result));
    CHECK(second_mesh_result.unique_mesh_count == 1U &&
          memcmp(camera_result.full_target_sha256,
                 second_mesh_result.full_target_sha256,
                 MEISO_SHA256_DIGEST_SIZE) != 0 &&
          memcmp(camera_result.thumbnail_rgb565,
                 second_mesh_result.thumbnail_rgb565,
                 MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE) != 0);

    desired.object_count = 2U;
    desired.objects[1] = desired.objects[0];
    desired.objects[1].id.low = 2U;
    desired.objects[1].transform.position[0] = -1.0F;
    CHECK(publish_and_receive_desired(
        &fixture, 5U, true, &desired, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, plan_generation, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 5U,
              plan_generation, &active_ref) ==
          MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(render_and_check(&fixture, 5U, 5U, &shared_mesh_result));
    CHECK(shared_mesh_result.unique_mesh_count == 1U &&
          memcmp(second_mesh_result.full_target_sha256,
                 shared_mesh_result.full_target_sha256,
                 MEISO_SHA256_DIGEST_SIZE) != 0 &&
          memcmp(second_mesh_result.thumbnail_rgb565,
                 shared_mesh_result.thumbnail_rgb565,
                 MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE) != 0);
    CHECK(meiso_composition_scene_store_status(
        fixture.scene_store, &fixture.resource_usage, &status
    ));
    CHECK(status.active_object_count == 2U &&
          status.unique_mesh_count == 1U &&
          status.published_blob_count == 2U &&
          status.target_slots_used == 0U && status.target_bytes == 0U &&
          status.target_slots_used <=
              MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT &&
          status.target_bytes <= UINT64_C(2) *
              MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
              MEISO_COMPOSITION_SCENE_TARGET_HEIGHT * 4U * 2U &&
          status.render_jobs_in_flight <=
              MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS &&
          status.result_queue_depth <=
              MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS);

    desired.objects[0] = desired.objects[1];
    desired.object_count = 1U;
    CHECK(publish_and_receive_desired(
        &fixture, 6U, true, &desired, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, plan_generation, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 6U,
              plan_generation, &active_ref) ==
          MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(meiso_composition_scene_store_active(fixture.scene_store, &active));
    CHECK(active.object_count == 1U && active.desired->objects[0].id.low == 2U);

    CHECK(publish_and_receive_desired(
        &fixture, 7U, false, NULL, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    wrong_ref = active_ref;
    wrong_ref.world_generation++;
    CHECK(meiso_composition_scene_store_disable(
              fixture.scene_store, &wrong_ref) ==
          MEISO_COMPOSITION_SCENE_DISABLE_MISMATCH);
    CHECK(meiso_composition_scene_store_active(fixture.scene_store, &active));
    CHECK(meiso_composition_scene_store_disable(
              fixture.scene_store, &active_ref) ==
          MEISO_COMPOSITION_SCENE_DISABLE_DISABLED);
    CHECK(!meiso_composition_scene_store_active(fixture.scene_store, &active));
    CHECK(meiso_composition_scene_store_fake_render(
              fixture.scene_store, 55U, 700U, 710U, &first_result) ==
          MEISO_COMPOSITION_SCENE_RENDER_NO_ACTIVE_WORLD);

    fixture_destroy(&fixture);
    return true;
}

static bool test_state_blob_and_supersede_failures(void)
{
    Fixture fixture;
    uint8_t cube_wire[512];
    uint8_t late_wire[128];
    uint8_t changed_wire[512];
    uint8_t invalid_wire[64];
    uint8_t payload_a[MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE];
    uint8_t payload_b[MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE];
    MeisoByteView cube;
    MeisoByteView late_mesh;
    MeisoByteView payload_a_view;
    MeisoByteView payload_b_view;
    MeisoContentId cube_id;
    MeisoContentId late_id;
    const MeisoContentId invalid_id = synthetic_content_id(3U);
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneDesired changed;
    ReceivedDesired received;
    MeisoCompositionSceneWorldRef active_ref;
    MeisoCompositionSceneWorldView candidate;
    MeisoCompositionSceneObservation observation;
    MeisoCompositionSceneStoreStats stats;
    MeisoMutableBuffer staging;
    BlobVerifierContext verifier;
    MeisoContentId bad_digest_id;

    CHECK(fixture_init(&fixture));
    CHECK(encode_cube(cube_wire, sizeof(cube_wire), &cube));
    CHECK(mesh_content_id(cube, &cube_id));
    CHECK(encode_triangle_variant(
        1U, late_wire, sizeof(late_wire), &late_mesh, &late_id
    ));
    desired = desired_with_mesh(cube_id);
    changed = desired;
    CHECK(publish_blob_bytes(
        &fixture, cube_id, cube, MEISO_BLOB_COMMIT_PUBLISHED,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));

    CHECK(encode_desired_payload(
        true, &desired, payload_a, sizeof(payload_a), &payload_a_view
    ));
    changed.objects[0].transform.position[0] = 2.0F;
    CHECK(encode_desired_payload(
        true, &changed, payload_b, sizeof(payload_b), &payload_b_view
    ));
    CHECK(meiso_state_publisher_publish(
              fixture.desired_publisher, fixture.host_owner, 2U,
              payload_a_view) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_publish(
              fixture.desired_publisher, fixture.host_owner, 2U,
              payload_a_view) == MEISO_STATE_PUBLISH_DUPLICATE);
    CHECK(meiso_state_publisher_publish(
              fixture.desired_publisher, fixture.host_owner, 2U,
              payload_b_view) == MEISO_STATE_PUBLISH_CONFLICT);
    CHECK(meiso_state_publisher_publish(
              fixture.desired_publisher, fixture.host_owner, 1U,
              payload_a_view) == MEISO_STATE_PUBLISH_STALE);
    CHECK(publish_and_receive_desired(
        &fixture, 3U, true, &desired, MEISO_STATE_PUBLISH_REPLACED_PENDING,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(meiso_state_replica_receive(
              fixture.desired_replica, fixture.host_owner, 3U,
              received.view.payload) == MEISO_STATE_RECEIVE_DUPLICATE);
    CHECK(meiso_state_replica_receive(
              fixture.desired_replica, fixture.host_owner, 2U,
              payload_a_view) == MEISO_STATE_RECEIVE_STALE);
    CHECK(meiso_state_replica_receive(
              fixture.desired_replica, fixture.host_owner, 3U,
              payload_b_view) == MEISO_STATE_RECEIVE_CONFLICT);

    CHECK(prepare_received(&fixture, 80U, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_prepare(
              fixture.scene_store, fixture.host_owner.incarnation, 3U, 80U,
              &changed) == MEISO_COMPOSITION_SCENE_PREPARE_CONFLICT);
    CHECK(meiso_composition_scene_store_prepare(
              fixture.scene_store, fixture.host_owner.incarnation, 3U, 80U,
              &received.desired) == MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED);
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 3U, 80U,
              &active_ref) == MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(meiso_composition_scene_store_prepare(
              fixture.scene_store, fixture.host_owner.incarnation, 3U, 80U,
              &received.desired) ==
          MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE);

    changed = desired;
    changed.objects[0].transform.position[0] = 3.0F;
    CHECK(publish_and_receive_desired(
        &fixture, 4U, true, &changed, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, 80U, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    changed.objects[0].transform.position[0] = 4.0F;
    CHECK(publish_and_receive_desired(
        &fixture, 5U, true, &changed, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, 80U, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_candidate(
        fixture.scene_store, &candidate
    ));
    CHECK(candidate.ref.desired_state_version == 5U);

    changed.objects[0].mesh_content_id = late_id;
    CHECK(publish_and_receive_desired(
        &fixture, 6U, true, &changed, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, 80U, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB);
    CHECK(!meiso_composition_scene_store_candidate(
        fixture.scene_store, &candidate
    ));
    CHECK(meiso_composition_scene_store_observation(
        fixture.scene_store, &observation
    ));
    CHECK(observation.desired_state_version == 6U &&
          observation.reason == MEISO_COMPOSITION_SCENE_REASON_WAITING_BLOB);

    changed.objects[0].mesh_content_id = cube_id;
    changed.objects[0].transform.position[0] = 5.0F;
    CHECK(publish_and_receive_desired(
        &fixture, 7U, true, &changed, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, 80U, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(publish_blob_bytes(
        &fixture, late_id, late_mesh, MEISO_BLOB_COMMIT_PUBLISHED,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));
    CHECK(meiso_composition_scene_store_candidate(
        fixture.scene_store, &candidate
    ));
    CHECK(candidate.ref.desired_state_version == 7U);

    memset(invalid_wire, 0, sizeof(invalid_wire));
    CHECK(meiso_blob_store_stage_begin(
              fixture.blob_store, &invalid_id, sizeof(invalid_wire)) ==
          MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(fixture.blob_store, &staging));
    memcpy(staging.data, invalid_wire, sizeof(invalid_wire));
    verifier.id = invalid_id;
    verifier.content = (MeisoByteView){invalid_wire, sizeof(invalid_wire)};
    CHECK(meiso_blob_store_commit_verified(
              fixture.blob_store, sizeof(invalid_wire), verify_blob,
              &verifier) == MEISO_BLOB_COMMIT_PUBLISHED);
    fixture.resource_usage.published_blob_count++;
    fixture.resource_usage.published_blob_bytes += sizeof(invalid_wire);
    CHECK(meiso_composition_scene_store_mark_blob_visible(
              fixture.scene_store, fixture.host_owner.incarnation,
              &invalid_id, sizeof(invalid_wire)) ==
          MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INVALID_MESH);
    changed.objects[0].mesh_content_id = invalid_id;
    CHECK(publish_and_receive_desired(
        &fixture, 8U, true, &changed, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, 80U, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB);
    CHECK(!meiso_composition_scene_store_candidate(
        fixture.scene_store, &candidate
    ));

    CHECK(publish_blob_bytes(
        &fixture, cube_id, cube, MEISO_BLOB_COMMIT_DUPLICATE,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_UNCHANGED
    ));
    CHECK(cube.size > 64U && cube.size <= sizeof(changed_wire));
    memcpy(changed_wire, cube_wire, cube.size);
    changed_wire[64] ^= 1U;
    CHECK(meiso_blob_store_stage_begin(
              fixture.blob_store, &cube_id, cube.size) ==
          MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(fixture.blob_store, &staging));
    memcpy(staging.data, changed_wire, cube.size);
    CHECK(meiso_blob_store_commit_verified(
              fixture.blob_store, cube.size,
              meiso_composition_scene_mesh_blob_verify, NULL) ==
          MEISO_BLOB_COMMIT_INTEGRITY_FAILED);
    CHECK(meiso_blob_store_abort_staging(
              fixture.blob_store, MEISO_BLOB_ABORT_CALLER_REQUEST) ==
          MEISO_BLOB_ABORTED);

    bad_digest_id = cube_id;
    bad_digest_id.digest[0] ^= 1U;
    CHECK(meiso_blob_store_stage_begin(
              fixture.blob_store, &bad_digest_id, cube.size) ==
          MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(fixture.blob_store, &staging));
    memcpy(staging.data, cube.data, cube.size);
    CHECK(meiso_blob_store_commit_verified(
              fixture.blob_store, cube.size,
              meiso_composition_scene_mesh_blob_verify, NULL) ==
          MEISO_BLOB_COMMIT_INTEGRITY_FAILED);
    CHECK(meiso_blob_store_abort_staging(
              fixture.blob_store, MEISO_BLOB_ABORT_CALLER_REQUEST) ==
          MEISO_BLOB_ABORTED);
    CHECK(meiso_blob_store_stage_begin(
              fixture.blob_store, &cube_id, cube.size + 1U) ==
          MEISO_BLOB_STAGE_CONFLICT);

    CHECK(meiso_composition_scene_store_stats(fixture.scene_store, &stats));
    CHECK(stats.superseded_candidates >= 2U && stats.missing_blobs == 2U &&
          stats.blob_visibility_rejections == 1U && stats.conflicts == 1U);
    fixture_destroy(&fixture);
    return true;
}

static bool test_new_owner_requires_blob_republication(void)
{
    Fixture fixture;
    uint8_t cube_wire[512];
    MeisoByteView cube;
    MeisoContentId cube_id;
    MeisoCompositionSceneDesired desired;
    ReceivedDesired received;
    MeisoCompositionSceneWorldRef active_ref;
    MeisoCompositionSceneWorldView world;
    MeisoCompositionSceneObservation observation;
    MeisoCompositionSceneStatus status;
    MeisoCompositionSceneStoreStats stats;
    const MeisoResourceDescriptor *desired_descriptor =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_DESIRED
        );
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoOwnerScope next_owner = {MEISO_OWNER_ROLE_HOST, 102U};
    size_t required;

    CHECK(fixture_init(&fixture));
    CHECK(encode_cube(cube_wire, sizeof(cube_wire), &cube));
    CHECK(mesh_content_id(cube, &cube_id));
    CHECK(!meiso_composition_scene_mesh_content_id(
        cube, (MeisoContentId *)cube_wire
    ));
    desired = desired_with_mesh(cube_id);
    CHECK(publish_blob_bytes(
        &fixture, cube_id, cube, MEISO_BLOB_COMMIT_PUBLISHED,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));
    CHECK(publish_and_receive_desired(
        &fixture, 1U, true, &desired, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, 100U, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 1U, 100U,
              &active_ref) == MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);

    CHECK(meiso_state_publisher_end_delivery(
              fixture.desired_publisher, fixture.desired_delivery) ==
          MEISO_STATE_DELIVERY_ENDED);
    required = meiso_state_publisher_memory_required(
        desired_descriptor, &state_config
    );
    memset(fixture.desired_publisher_arena, 0, required);
    CHECK(meiso_state_publisher_init(
              fixture.desired_publisher_arena, required, desired_descriptor,
              &state_config, &fixture.desired_publisher) ==
          MEISO_STATE_INIT_OK);
    fixture.desired_delivery = (MeisoDeliveryGeneration){21U};
    CHECK(meiso_state_publisher_begin_delivery(
              fixture.desired_publisher, fixture.desired_delivery) ==
          MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_replica_begin_owner(
              fixture.desired_replica, next_owner) ==
          MEISO_STATE_OWNER_STARTED);
    CHECK(meiso_composition_scene_store_begin_owner(
              fixture.scene_store, next_owner.incarnation) ==
          MEISO_COMPOSITION_SCENE_OWNER_STARTED);
    fixture.host_owner = next_owner;
    CHECK(!meiso_composition_scene_store_active(fixture.scene_store, &world));
    CHECK(!meiso_composition_scene_store_candidate(
        fixture.scene_store, &world
    ));
    CHECK(!meiso_composition_scene_store_observation(
        fixture.scene_store, &observation
    ));
    CHECK(meiso_blob_store_count(fixture.blob_store) == 1U);
    CHECK(meiso_composition_scene_store_status(
        fixture.scene_store, &fixture.resource_usage, &status
    ));
    CHECK(status.published_blob_count == 1U &&
          status.published_blob_bytes == cube.size &&
          status.last_result_stream_epoch == 0U &&
          status.last_result_sequence == 0U);

    CHECK(publish_and_receive_desired(
        &fixture, 1U, true, &desired, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, 101U, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB);
    CHECK(publish_blob_bytes(
        &fixture, cube_id, cube, MEISO_BLOB_COMMIT_DUPLICATE,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));
    CHECK(fixture.resource_usage.published_blob_count == 1U &&
          fixture.resource_usage.published_blob_bytes == cube.size);
    CHECK(prepare_received(&fixture, 101U, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, next_owner.incarnation, 1U, 101U,
              &active_ref) == MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(active_ref.desired_owner_incarnation == next_owner.incarnation &&
          active_ref.world_generation == 2U);
    CHECK(meiso_composition_scene_store_stats(fixture.scene_store, &stats));
    CHECK(stats.owner_changes == 1U &&
          stats.blobs_made_visible == 2U);

    fixture_destroy(&fixture);
    return true;
}

static bool test_store_memory_alias_and_borrow_boundaries(void)
{
    Fixture fixture;
    uint8_t cube_wire[512];
    MeisoByteView cube;
    MeisoContentId cube_id;
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneRenderResult rejected_result;
    MeisoCompositionSceneRenderResult rejected_before;
    MeisoCompositionSceneStatus overlap_status;
    MeisoCompositionSceneWorldView candidate;
    MeisoCompositionSceneWorldRef active_ref;
    MeisoCompositionSceneStore *other_store = NULL;
    void *scratch;
    size_t required;
    size_t alignment;

    CHECK(fixture_init(&fixture));
    required = meiso_composition_scene_store_memory_required();
    alignment = meiso_composition_scene_store_alignment_required();
    CHECK(required != 0U && alignment >= _Alignof(void *) &&
          (alignment & (alignment - 1U)) == 0U);
    scratch = malloc(required + alignment);
    CHECK(scratch != NULL);
    CHECK(meiso_composition_scene_store_init(
              (uint8_t *)scratch + 1U, required, fixture.blob_store,
              &other_store) ==
          MEISO_COMPOSITION_SCENE_STORE_INIT_MISALIGNED_ARENA);
    CHECK(meiso_composition_scene_store_init(
              scratch, required - 1U, fixture.blob_store, &other_store) ==
          MEISO_COMPOSITION_SCENE_STORE_INIT_ARENA_TOO_SMALL);
    CHECK(meiso_composition_scene_store_init(
              scratch, required, fixture.blob_store,
              (MeisoCompositionSceneStore **)scratch) ==
          MEISO_COMPOSITION_SCENE_STORE_INIT_INVALID_ARGUMENT);
    free(scratch);

    CHECK(encode_cube(cube_wire, sizeof(cube_wire), &cube));
    CHECK(mesh_content_id(cube, &cube_id));
    desired = desired_with_mesh(cube_id);
    CHECK(publish_blob_bytes(
        &fixture, cube_id, cube, MEISO_BLOB_COMMIT_PUBLISHED,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));
    CHECK(meiso_composition_scene_store_prepare(
              fixture.scene_store, fixture.host_owner.incarnation, 1U, 110U,
              (const MeisoCompositionSceneDesired *)
                  fixture.scene_store_arena) ==
          MEISO_COMPOSITION_SCENE_PREPARE_INVALID_ARGUMENT);
    CHECK(meiso_composition_scene_store_prepare(
              fixture.scene_store, fixture.host_owner.incarnation, 1U, 110U,
              &desired) == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_candidate(
        fixture.scene_store, &candidate
    ));
    CHECK(candidate.desired != &desired &&
          candidate.desired->objects[0].transform.position[0] == 0.0F);
    desired.objects[0].transform.position[0] = 9.0F;
    CHECK(candidate.desired->objects[0].transform.position[0] == 0.0F);

    CHECK(!meiso_composition_scene_store_candidate(
        fixture.scene_store,
        (MeisoCompositionSceneWorldView *)fixture.scene_store_arena
    ));
    CHECK(!meiso_composition_scene_store_observation(
        fixture.scene_store,
        (MeisoCompositionSceneObservation *)fixture.scene_store_arena
    ));
    CHECK(!meiso_composition_scene_store_status(
        fixture.scene_store, &fixture.resource_usage,
        (MeisoCompositionSceneStatus *)fixture.scene_store_arena
    ));
    CHECK(!meiso_composition_scene_store_status(
        fixture.scene_store,
        (const MeisoCompositionSceneResourceUsage *)&overlap_status,
        &overlap_status
    ));
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 1U, 110U,
              (MeisoCompositionSceneWorldRef *)fixture.scene_store_arena) ==
          MEISO_COMPOSITION_SCENE_ACTIVATE_INVALID_ARGUMENT);
    CHECK(meiso_composition_scene_store_candidate(
        fixture.scene_store, &candidate
    ));
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 1U, 110U,
              &active_ref) == MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(!meiso_composition_scene_store_active(
        fixture.scene_store,
        (MeisoCompositionSceneWorldView *)fixture.scene_store_arena
    ));
    CHECK(meiso_composition_scene_store_fake_render(
              fixture.scene_store, 1U, 1U, 2U,
              (MeisoCompositionSceneRenderResult *)
                  fixture.scene_store_arena) ==
          MEISO_COMPOSITION_SCENE_RENDER_INVALID_ARGUMENT);
    memset(&rejected_result, 0xa5, sizeof(rejected_result));
    rejected_before = rejected_result;
    CHECK(meiso_composition_scene_store_fake_render(
              fixture.scene_store, 1U, 3U, 2U, &rejected_result) ==
          MEISO_COMPOSITION_SCENE_RENDER_INVALID_TIME);
    CHECK(memcmp(&rejected_result, &rejected_before,
                 sizeof(rejected_result)) == 0);
    CHECK(!meiso_composition_scene_store_record_result_published(
        fixture.scene_store,
        (const MeisoCompositionSceneWorldRef *)fixture.scene_store_arena,
        1U, 1U
    ));
    CHECK(!meiso_composition_scene_store_stats(
        fixture.scene_store,
        (MeisoCompositionSceneStoreStats *)fixture.scene_store_arena
    ));

    fixture_destroy(&fixture);
    return true;
}

static bool test_object_mesh_and_blob_capacity(void)
{
    Fixture fixture;
    uint8_t variant_wire[128];
    MeisoByteView variant_mesh;
    MeisoCompositionSceneDesired desired =
        desired_with_mesh(synthetic_content_id(1U));
    MeisoCompositionSceneObservation observation;
    MeisoMutableBuffer staging;
    MeisoContentId seventeenth_id;
    size_t index;

    CHECK(fixture_init(&fixture));

    desired.object_count = MEISO_COMPOSITION_SCENE_MAX_OBJECTS + 1U;
    CHECK(meiso_composition_scene_store_prepare(
              fixture.scene_store, fixture.host_owner.incarnation, 1U, 90U,
              &desired) == MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY);
    CHECK(meiso_composition_scene_store_observation(
        fixture.scene_store, &observation
    ));
    CHECK(observation.reason == MEISO_COMPOSITION_SCENE_REASON_OBJECT_CAPACITY);

    desired = desired_with_mesh(synthetic_content_id(1U));
    desired.object_count = MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES + 1U;
    for (index = 0U; index < desired.object_count; ++index) {
        desired.objects[index] = desired.objects[0];
        desired.objects[index].id.low = index + 1U;
        desired.objects[index].mesh_content_id =
            synthetic_content_id((uint8_t)(1U + index));
    }
    CHECK(meiso_composition_scene_store_prepare(
              fixture.scene_store, fixture.host_owner.incarnation, 2U, 90U,
              &desired) == MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY);
    CHECK(meiso_composition_scene_store_observation(
        fixture.scene_store, &observation
    ));
    CHECK(observation.reason == MEISO_COMPOSITION_SCENE_REASON_BLOB_CAPACITY);

    for (index = 0U; index < MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS;
         ++index) {
        MeisoContentId variant_id;

        CHECK(encode_triangle_variant(
            (uint8_t)(1U + index), variant_wire, sizeof(variant_wire),
            &variant_mesh, &variant_id
        ));
        CHECK(publish_blob_bytes(
            &fixture, variant_id, variant_mesh,
            MEISO_BLOB_COMMIT_PUBLISHED,
            MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
        ));
    }
    CHECK(meiso_blob_store_count(fixture.blob_store) ==
          MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS);
    CHECK(encode_triangle_variant(
        0xf0U, variant_wire, sizeof(variant_wire), &variant_mesh,
        &seventeenth_id
    ));
    CHECK(meiso_blob_store_stage_begin(
              fixture.blob_store, &seventeenth_id,
              variant_mesh.size) == MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(fixture.blob_store, &staging));
    memcpy(staging.data, variant_mesh.data, variant_mesh.size);
    CHECK(meiso_blob_store_commit_verified(
              fixture.blob_store, variant_mesh.size,
              meiso_composition_scene_mesh_blob_verify, NULL) ==
          MEISO_BLOB_COMMIT_CAPACITY);
    CHECK(meiso_blob_store_abort_staging(
              fixture.blob_store, MEISO_BLOB_ABORT_CALLER_REQUEST) ==
          MEISO_BLOB_ABORTED);
    CHECK(meiso_blob_store_stage_begin(
              fixture.blob_store, &seventeenth_id,
              MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE + 1U) ==
          MEISO_BLOB_STAGE_CAPACITY);

    fixture_destroy(&fixture);
    return true;
}

static bool make_max_mesh(
    uint8_t variant,
    uint8_t **out_wire,
    size_t *out_size,
    MeisoContentId *out_id
)
{
    const uint32_t vertex_count = 21839U;
    MeisoCompositionSceneMeshVertex *vertices;
    MeisoCompositionSceneMesh mesh;
    uint32_t indices[3] = {0U, 0U, 0U};
    uint8_t *wire;
    size_t size = 0U;

    vertices = calloc(vertex_count, sizeof(*vertices));
    wire = malloc(MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE);
    if (vertices == NULL || wire == NULL) {
        free(wire);
        free(vertices);
        return false;
    }
    vertices[0].position[0] = (float)variant;
    memset(&mesh, 0, sizeof(mesh));
    mesh.vertex_count = vertex_count;
    mesh.vertices = vertices;
    mesh.index_count = 3U;
    mesh.indices = indices;
    if (meiso_composition_scene_mesh_encode(
            &mesh,
            (MeisoMutableBuffer){
                wire, MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE
            },
            &size) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
        size != MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE ||
        !mesh_content_id((MeisoByteView){wire, size}, out_id)) {
        free(wire);
        free(vertices);
        return false;
    }
    free(vertices);
    *out_wire = wire;
    *out_size = size;
    return true;
}

static void fill_eight_mesh_world(
    MeisoCompositionSceneDesired *desired,
    const MeisoContentId *mesh_ids
)
{
    size_t index;

    *desired = desired_with_mesh(mesh_ids[0]);
    desired->object_count = MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES;
    for (index = 0U; index < desired->object_count; ++index) {
        desired->objects[index] = desired->objects[0];
        desired->objects[index].id.low = index + 1U;
        desired->objects[index].mesh_content_id = mesh_ids[index];
    }
}

static bool test_active_candidate_resident_boundary(void)
{
    Fixture fixture;
    MeisoContentId mesh_ids[16];
    MeisoCompositionSceneDesired active_desired;
    MeisoCompositionSceneDesired candidate_desired;
    MeisoCompositionSceneWorldRef active_ref;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionSceneWorldView candidate;
    size_t index;

    CHECK(fixture_init(&fixture));
    for (index = 0U; index < 16U; ++index) {
        uint8_t *max_mesh_wire = NULL;
        size_t max_mesh_size = 0U;

        CHECK(make_max_mesh(
            (uint8_t)(index + 1U), &max_mesh_wire, &max_mesh_size,
            &mesh_ids[index]
        ));
        CHECK(publish_blob_bytes(
            &fixture, mesh_ids[index],
            (MeisoByteView){max_mesh_wire, max_mesh_size},
            MEISO_BLOB_COMMIT_PUBLISHED,
            MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
        ));
        free(max_mesh_wire);
    }
    fill_eight_mesh_world(&active_desired, mesh_ids);
    fill_eight_mesh_world(&candidate_desired, mesh_ids + 8U);
    CHECK(meiso_composition_scene_store_prepare(
              fixture.scene_store, fixture.host_owner.incarnation, 1U, 91U,
              &active_desired) == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 1U, 91U,
              &active_ref) == MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(meiso_composition_scene_store_prepare(
              fixture.scene_store, fixture.host_owner.incarnation, 2U, 91U,
              &candidate_desired) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_active(fixture.scene_store, &active));
    CHECK(meiso_composition_scene_store_candidate(
        fixture.scene_store, &candidate
    ));
    CHECK(active.unique_mesh_count == 8U && candidate.unique_mesh_count == 8U);
    CHECK(active.resident_mesh_bytes == UINT64_C(8) *
              (MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE -
               MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE) &&
          candidate.resident_mesh_bytes == active.resident_mesh_bytes &&
          active.resident_mesh_bytes + candidate.resident_mesh_bytes ==
              UINT64_C(4193280));

    fixture_destroy(&fixture);
    return true;
}

static bool test_result_stream_queue_and_identity(void)
{
    Fixture fixture;
    uint8_t cube_wire[512];
    uint8_t result_wire[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE];
    MeisoByteView cube;
    MeisoContentId cube_id;
    MeisoCompositionSceneDesired desired;
    ReceivedDesired received;
    MeisoCompositionSceneWorldRef active_ref;
    MeisoCompositionSceneRenderResult result;
    MeisoCompositionSceneStatus status;
    MeisoCompositionSceneResourceUsage invalid_usage;
    MeisoStreamSample queued;
    MeisoStreamSample replay;
    MeisoStreamReceiveResult replay_receive;
    MeisoStreamStats stats;
    MeisoCompositionSceneWorldRef wrong_ref;
    size_t result_size = 0U;
    uint64_t sequence;

    CHECK(fixture_init(&fixture));
    CHECK(encode_cube(cube_wire, sizeof(cube_wire), &cube));
    CHECK(mesh_content_id(cube, &cube_id));
    desired = desired_with_mesh(cube_id);
    CHECK(publish_blob_bytes(
        &fixture, cube_id, cube, MEISO_BLOB_COMMIT_PUBLISHED,
        MEISO_COMPOSITION_SCENE_BLOB_VISIBLE
    ));
    CHECK(publish_and_receive_desired(
        &fixture, 1U, true, &desired, MEISO_STATE_PUBLISH_NEW,
        MEISO_STATE_RECEIVE_NEW, &received
    ));
    CHECK(prepare_received(&fixture, 92U, &received) ==
          MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_activate(
              fixture.scene_store, fixture.host_owner.incarnation, 1U, 92U,
              &active_ref) == MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    wrong_ref = active_ref;
    wrong_ref.desired_state_version++;
    CHECK(!meiso_composition_scene_store_record_result_published(
        fixture.scene_store, &wrong_ref, 31U, 1U
    ));

    CHECK(meiso_composition_scene_store_fake_render(
              fixture.scene_store, 55U, 1000U, 1010U, &result) ==
          MEISO_COMPOSITION_SCENE_RENDER_COMPLETE);
    CHECK(meiso_composition_scene_render_result_encode(
              &result,
              (MeisoMutableBuffer){result_wire, sizeof(result_wire)},
              &result_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&replay, 0, sizeof(replay));
    replay.key = (MeisoStreamKey){fixture.edge_owner, 32U, 1U};
    replay.source_time = (MeisoTimePoint){55U, 1010U};
    replay.payload = (MeisoByteView){result_wire, result_size};
    CHECK(meiso_stream_receive(fixture.result_stream, &replay, NULL).code ==
          MEISO_STREAM_RECEIVE_EPOCH_MISMATCH);
    CHECK(meiso_composition_scene_store_status(
        fixture.scene_store, &fixture.resource_usage, &status
    ));
    CHECK(status.last_result_stream_epoch == 0U &&
          status.last_result_sequence == 0U);

    for (sequence = 1U;
         sequence <= MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS + 1U;
         ++sequence) {
        CHECK(meiso_composition_scene_store_fake_render(
                  fixture.scene_store, 55U, sequence * 100U,
                  sequence * 100U + 10U, &result) ==
              MEISO_COMPOSITION_SCENE_RENDER_COMPLETE);
        CHECK(receive_render_result(
            &fixture, &result, &active_ref, sequence,
            sequence <= MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS
                ? MEISO_STREAM_RECEIVE_FLAG_NONE
                : MEISO_STREAM_RECEIVE_FLAG_DROPPED_OLDEST
        ));
    }
    CHECK(meiso_stream_count(fixture.result_stream) ==
          MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS);
    CHECK(meiso_stream_peek(fixture.result_stream, &queued));
    CHECK(queued.key.sequence == 2U);
    CHECK(meiso_stream_stats(fixture.result_stream, &stats));
    CHECK(stats.overflow_drops == 1U && stats.accepted == 5U);
    CHECK(meiso_composition_scene_store_status(
        fixture.scene_store, &fixture.resource_usage, &status
    ));
    CHECK(status.result_queue_depth ==
              MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS &&
          status.last_result_stream_epoch == 31U &&
          status.last_result_sequence == 5U);

    invalid_usage = fixture.resource_usage;
    invalid_usage.result_queue_depth =
        MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS + 1U;
    CHECK(!meiso_composition_scene_store_status(
        fixture.scene_store, &invalid_usage, &status
    ));
    invalid_usage = fixture.resource_usage;
    invalid_usage.target_slots_used =
        MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT + 1U;
    CHECK(!meiso_composition_scene_store_status(
        fixture.scene_store, &invalid_usage, &status
    ));
    invalid_usage = fixture.resource_usage;
    invalid_usage.target_slots_used = 1U;
    invalid_usage.target_bytes = 0U;
    CHECK(!meiso_composition_scene_store_status(
        fixture.scene_store, &invalid_usage, &status
    ));
    invalid_usage = fixture.resource_usage;
    invalid_usage.render_jobs_in_flight =
        MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS + 1U;
    CHECK(!meiso_composition_scene_store_status(
        fixture.scene_store, &invalid_usage, &status
    ));

    CHECK(meiso_composition_scene_render_result_encode(
              &result,
              (MeisoMutableBuffer){result_wire, sizeof(result_wire)},
              &result_size) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&replay, 0, sizeof(replay));
    replay.key = (MeisoStreamKey){fixture.edge_owner, 31U, 5U};
    replay.source_time = (MeisoTimePoint){
        result.source_clock_epoch, result.render_end_monotonic_ns
    };
    replay.payload = (MeisoByteView){result_wire, result_size};
    CHECK(meiso_stream_receive(fixture.result_stream, &replay, NULL).code ==
          MEISO_STREAM_RECEIVE_DUPLICATE);
    result_wire[160] ^= 1U;
    CHECK(meiso_stream_receive(fixture.result_stream, &replay, NULL).code ==
          MEISO_STREAM_RECEIVE_CONFLICT);
    result_wire[160] ^= 1U;
    replay.key.sequence = 1U;
    CHECK(meiso_stream_receive(fixture.result_stream, &replay, NULL).code ==
          MEISO_STREAM_RECEIVE_STALE);
    replay.key.epoch = 32U;
    replay.key.sequence = 6U;
    replay_receive = meiso_stream_receive(
        fixture.result_stream, &replay, NULL
    );
    CHECK(replay_receive.code == MEISO_STREAM_RECEIVE_EPOCH_MISMATCH);

    fixture_destroy(&fixture);
    return true;
}

static bool report_memory_breakdown(void)
{
    const MeisoResourceDescriptor *blob = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
    );
    const MeisoResourceDescriptor *desired = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_DESIRED
    );
    const MeisoResourceDescriptor *actual = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_ACTUAL
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
    const size_t blob_bytes =
        meiso_blob_store_memory_required(blob, &blob_config);
    const size_t desired_publisher_bytes =
        meiso_state_publisher_memory_required(desired, &state_config);
    const size_t desired_replica_bytes =
        meiso_state_replica_memory_required(desired);
    const size_t actual_publisher_bytes =
        meiso_state_publisher_memory_required(actual, &state_config);
    const size_t actual_replica_bytes =
        meiso_state_replica_memory_required(actual);
    const size_t stream_bytes =
        meiso_stream_memory_required(result, &stream_config);
    const size_t store_bytes =
        meiso_composition_scene_store_memory_required();

    CHECK(blob_bytes != 0U && desired_publisher_bytes != 0U &&
          desired_replica_bytes != 0U && actual_publisher_bytes != 0U &&
          actual_replica_bytes != 0U && stream_bytes != 0U &&
          store_bytes != 0U);
    (void)printf(
        "memory bytes: blob=%zu desired_pub=%zu desired_rep=%zu "
        "actual_pub=%zu actual_rep=%zu result_stream=%zu store=%zu "
        "logical_mesh_limit=%u target=0 jobs=0\n",
        blob_bytes, desired_publisher_bytes, desired_replica_bytes,
        actual_publisher_bytes, actual_replica_bytes, stream_bytes,
        store_bytes,
        (unsigned int)MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_TOTAL
    );
    return true;
}

int main(void)
{
    if (!test_success_transcript() ||
        !test_state_blob_and_supersede_failures() ||
        !test_new_owner_requires_blob_republication() ||
        !test_store_memory_alias_and_borrow_boundaries() ||
        !test_active_candidate_resident_boundary() ||
        !test_result_stream_queue_and_identity() ||
        !test_object_mesh_and_blob_capacity() ||
        !report_memory_breakdown()) {
        return 1;
    }
    (void)puts("composition scene resource tests passed");
    return 0;
}
