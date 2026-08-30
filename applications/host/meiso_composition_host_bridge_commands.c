#include "meiso_composition_host_bridge_private.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Bounded NDJSON parsing and application command dispatch. */

typedef struct {
    const char *text;
    size_t size;
    size_t offset;
} MeisoCompositionHostBridgePrivateJsonCursor;

typedef struct {
    uint64_t request_id;
    bool enabled;
    MeisoCompositionSceneDesired value;
    uint64_t editor_ids[MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_EDITOR_OBJECTS];
} MeisoCompositionHostBridgePrivateSceneCommand;

static void json_skip_space(MeisoCompositionHostBridgePrivateJsonCursor *cursor)
{
    while (cursor->offset < cursor->size &&
           (cursor->text[cursor->offset] == ' ' ||
            cursor->text[cursor->offset] == '\t' ||
            cursor->text[cursor->offset] == '\r' ||
            cursor->text[cursor->offset] == '\n')) {
        ++cursor->offset;
    }
}

static bool json_literal(MeisoCompositionHostBridgePrivateJsonCursor *cursor, const char *literal)
{
    size_t size;

    if (cursor == NULL || literal == NULL) {
        return false;
    }
    json_skip_space(cursor);
    size = strlen(literal);
    if (size > cursor->size - cursor->offset ||
        memcmp(cursor->text + cursor->offset, literal, size) != 0) {
        return false;
    }
    cursor->offset += size;
    return true;
}

static bool json_end(MeisoCompositionHostBridgePrivateJsonCursor *cursor)
{
    json_skip_space(cursor);
    return cursor->offset == cursor->size;
}

static bool json_u64(MeisoCompositionHostBridgePrivateJsonCursor *cursor, uint64_t maximum, uint64_t *out_value)
{
    size_t start;
    char buffer[32];
    size_t size;

    if (cursor == NULL || out_value == NULL) {
        return false;
    }
    json_skip_space(cursor);
    start = cursor->offset;
    while (cursor->offset < cursor->size &&
           cursor->text[cursor->offset] >= '0' &&
           cursor->text[cursor->offset] <= '9') {
        ++cursor->offset;
    }
    size = cursor->offset - start;
    if (size == 0U || size >= sizeof(buffer)) {
        cursor->offset = start;
        return false;
    }
    memcpy(buffer, cursor->text + start, size);
    buffer[size] = '\0';
    if (!meiso_composition_host_bridge_private_parse_u64_text(buffer, maximum, out_value)) {
        cursor->offset = start;
        return false;
    }
    return true;
}

static bool json_u32(MeisoCompositionHostBridgePrivateJsonCursor *cursor, uint32_t *out_value)
{
    uint64_t value;

    if (!json_u64(cursor, UINT32_MAX, &value)) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

static bool json_u16(MeisoCompositionHostBridgePrivateJsonCursor *cursor, uint16_t *out_value)
{
    uint64_t value;

    if (!json_u64(cursor, UINT16_MAX, &value)) {
        return false;
    }
    *out_value = (uint16_t)value;
    return true;
}

static bool json_bool(MeisoCompositionHostBridgePrivateJsonCursor *cursor, bool *out_value)
{
    if (cursor == NULL || out_value == NULL) {
        return false;
    }
    if (json_literal(cursor, "true")) {
        *out_value = true;
        return true;
    }
    if (json_literal(cursor, "false")) {
        *out_value = false;
        return true;
    }
    return false;
}

static bool json_float(MeisoCompositionHostBridgePrivateJsonCursor *cursor, float *out_value)
{
    size_t start;
    size_t size;
    char buffer[64];
    char *end = NULL;
    float value;

    if (cursor == NULL || out_value == NULL) {
        return false;
    }
    json_skip_space(cursor);
    start = cursor->offset;
    while (cursor->offset < cursor->size &&
           strchr("-+0123456789.eE", cursor->text[cursor->offset]) != NULL) {
        ++cursor->offset;
    }
    size = cursor->offset - start;
    if (size == 0U || size >= sizeof(buffer)) {
        cursor->offset = start;
        return false;
    }
    memcpy(buffer, cursor->text + start, size);
    buffer[size] = '\0';
    errno = 0;
    value = strtof(buffer, &end);
    if (errno != 0 || end == buffer || *end != '\0' || !isfinite(value)) {
        cursor->offset = start;
        return false;
    }
    *out_value = value;
    return true;
}

static bool json_float_array(
    MeisoCompositionHostBridgePrivateJsonCursor *cursor,
    float *values,
    size_t count
)
{
    size_t index;

    if (!json_literal(cursor, "[")) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if ((index != 0U && !json_literal(cursor, ",")) ||
            !json_float(cursor, &values[index])) {
            return false;
        }
    }
    return json_literal(cursor, "]");
}

static bool json_color_format(
    MeisoCompositionHostBridgePrivateJsonCursor *cursor,
    MeisoCompositionSceneColorFormat *out_format
)
{
    if (json_literal(cursor, "\"rgba8888\"")) {
        *out_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
        return true;
    }
    if (json_literal(cursor, "\"rgb565\"")) {
        *out_format = MEISO_COMPOSITION_SCENE_COLOR_RGB565;
        return true;
    }
    return false;
}

static bool json_pixel_format(
    MeisoCompositionHostBridgePrivateJsonCursor *cursor,
    MeisoCompositionMediaPixelFormat *out_format
)
{
    if (json_literal(cursor, "\"rgba8888\"")) {
        *out_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
        return true;
    }
    if (json_literal(cursor, "\"rgb565\"")) {
        *out_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGB565;
        return true;
    }
    if (json_literal(cursor, "\"nv12\"")) {
        *out_format = MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
        return true;
    }
    if (json_literal(cursor, "\"yuyv\"")) {
        *out_format = MEISO_COMPOSITION_MEDIA_PIXEL_YUYV;
        return true;
    }
    return false;
}

static bool parse_scene_camera(
    MeisoCompositionHostBridgePrivateJsonCursor *cursor,
    MeisoCompositionSceneCamera *camera
)
{
    memset(camera, 0, sizeof(*camera));
    camera->space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    camera->projection = MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    if (!json_literal(cursor, "{\"position\":") ||
        !json_float_array(cursor, camera->position, 3U) ||
        !json_literal(cursor, ",\"orientation_xyzw\":") ||
        !json_float_array(cursor, camera->orientation_xyzw, 4U) ||
        !json_literal(cursor, ",\"vertical_fov_degrees\":") ||
        !json_float(cursor, &camera->vertical_fov_degrees) ||
        !json_literal(cursor, ",\"near_z\":") ||
        !json_float(cursor, &camera->near_z) ||
        !json_literal(cursor, ",\"far_z\":") ||
        !json_float(cursor, &camera->far_z) || !json_literal(cursor, "}")) {
        return false;
    }
    return true;
}

static bool parse_scene_object(
    MeisoCompositionHostBridgePrivateJsonCursor *cursor,
    uint64_t *out_editor_id,
    MeisoCompositionSceneObject *object
)
{
    bool visible;

    memset(object, 0, sizeof(*object));
    object->space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    if (!json_literal(cursor, "{\"editor_id\":") ||
        !json_u64(cursor, UINT64_MAX, out_editor_id) ||
        !json_literal(cursor, ",\"asset_handle\":\"cube\"") ||
        !json_literal(cursor, ",\"position\":") ||
        !json_float_array(cursor, object->transform.position, 3U) ||
        !json_literal(cursor, ",\"orientation_xyzw\":") ||
        !json_float_array(cursor, object->transform.orientation_xyzw, 4U) ||
        !json_literal(cursor, ",\"scale\":") ||
        !json_float_array(cursor, object->transform.scale, 3U) ||
        !json_literal(cursor, ",\"visible\":") ||
        !json_bool(cursor, &visible) || !json_literal(cursor, "}")) {
        return false;
    }
    object->flags = visible ? MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE : 0U;
    return true;
}

static bool parse_scene_command(MeisoCompositionHostBridgePrivateJsonCursor *cursor, MeisoCompositionHostBridgePrivateSceneCommand *out_command)
{
    uint32_t count = 0U;

    memset(out_command, 0, sizeof(*out_command));
    if (!json_literal(cursor, "{\"command\":\"set_scene\",\"request_id\":") ||
        !json_u64(cursor, UINT64_MAX, &out_command->request_id) ||
        !json_literal(cursor, ",\"enabled\":") ||
        !json_bool(cursor, &out_command->enabled)) {
        return false;
    }
    if (!out_command->enabled) {
        return json_literal(cursor, "}") && json_end(cursor);
    }
    if (!json_literal(cursor, ",\"camera\":") ||
        !parse_scene_camera(cursor, &out_command->value.camera) ||
        !json_literal(cursor, ",\"objects\":[")) {
        return false;
    }
    while (!json_literal(cursor, "]")) {
        if (count >= MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_EDITOR_OBJECTS ||
            (count != 0U && !json_literal(cursor, ",")) ||
            !parse_scene_object(cursor, &out_command->editor_ids[count],
                                &out_command->value.objects[count])) {
            return false;
        }
        ++count;
    }
    out_command->value.flags = MEISO_COMPOSITION_SCENE_FLAGS_NONE;
    out_command->value.object_count = count;
    return json_literal(cursor, "}") && json_end(cursor);
}

static bool allocate_scene_ids(MeisoCompositionHostBridgePrivateProcess *service, MeisoCompositionHostBridgePrivateSceneCommand *command)
{
    MeisoCompositionHostBridgePrivateEditorObjectIdentity candidate[MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_EDITOR_OBJECTS];
    uint64_t next_serial;
    uint32_t object_index;
    size_t identity_index;

    memset(candidate, 0, sizeof(candidate));
    next_serial = service->next_object_serial;
    for (object_index = 0U; object_index < command->value.object_count;
         ++object_index) {
        MeisoCompositionHostBridgePrivateEditorObjectIdentity *found = NULL;
        uint32_t previous;

        for (previous = 0U; previous < object_index; ++previous) {
            if (command->editor_ids[previous] ==
                command->editor_ids[object_index]) {
                return false;
            }
        }
        for (identity_index = 0U; identity_index < MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_EDITOR_OBJECTS;
             ++identity_index) {
            if (service->objects[identity_index].used &&
                service->objects[identity_index].editor_id ==
                    command->editor_ids[object_index]) {
                size_t candidate_index;
                for (candidate_index = 0U;
                     candidate_index < MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_EDITOR_OBJECTS;
                     ++candidate_index) {
                    if (!candidate[candidate_index].used) {
                        candidate[candidate_index] =
                            service->objects[identity_index];
                        found = &candidate[candidate_index];
                        break;
                    }
                }
                break;
            }
        }
        if (found == NULL) {
            for (identity_index = 0U; identity_index < MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_EDITOR_OBJECTS;
                 ++identity_index) {
                if (!candidate[identity_index].used) {
                    found = &candidate[identity_index];
                    break;
                }
            }
            if (found == NULL || next_serial == UINT64_MAX) {
                return false;
            }
            ++next_serial;
            found->used = true;
            found->editor_id = command->editor_ids[object_index];
            found->object_id.high = service->config.local_incarnation;
            found->object_id.low = next_serial;
        }
        command->value.objects[object_index].id = found->object_id;
        command->value.objects[object_index].mesh_content_id =
            service->cube_content_id;
    }
    memcpy(service->objects, candidate, sizeof(candidate));
    service->next_object_serial = next_serial;
    return true;
}

static bool stamp_scene_truth(MeisoCompositionHostBridgePrivateProcess *service, MeisoCompositionHostBridgePrivateSceneCommand *command)
{
    struct timespec now;
    uint64_t seconds;
    uint64_t monotonic_ns;
    uint32_t index;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return false;
    }
    seconds = (uint64_t)now.tv_sec;
    monotonic_ns = seconds * UINT64_C(1000000000);
    if ((uint64_t)now.tv_nsec > UINT64_MAX - monotonic_ns) {
        return false;
    }
    monotonic_ns += (uint64_t)now.tv_nsec;
    command->value.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    command->value.camera.space_epoch = service->scene_space_epoch;
    command->value.camera.source_time =
        (MeisoTimePoint){service->host_clock_epoch, monotonic_ns};
    command->value.camera.projection =
        MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    command->value.camera.viewport_x = 0U;
    command->value.camera.viewport_y = 0U;
    command->value.camera.viewport_width =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    command->value.camera.viewport_height =
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    for (index = 0U; index < command->value.object_count; ++index) {
        command->value.objects[index].space =
            MEISO_COMPOSITION_SCENE_SPACE_WORLD;
        command->value.objects[index].space_epoch = service->scene_space_epoch;
    }
    return true;
}

static bool handle_asset_command(
    MeisoCompositionHostBridgePrivateProcess *service,
    const char *line,
    size_t size
)
{
    MeisoCompositionHostBridgePrivateJsonCursor cursor = {line, size, 0U};
    uint64_t request_id;
    MeisoEdgeCommandReceipt receipt;
    MeisoEdgeStatus status;

    if (!json_literal(
            &cursor,
            "{\"command\":\"asset.load_builtin\",\"request_id\":"
        ) || !json_u64(&cursor, UINT64_MAX, &request_id) ||
        !json_literal(&cursor, ",\"asset_handle\":\"cube\"}") ||
        !json_end(&cursor)) {
        return meiso_composition_host_bridge_private_emit_error(0U, "INVALID_COMMAND");
    }
    if (service->cube_loaded) {
        MeisoCompositionEdgeSnapshot snapshot;
        if (meiso_composition_edge_snapshot(service->edge, &snapshot) !=
            MEISO_EDGE_OK) {
            return meiso_composition_host_bridge_private_emit_error(request_id, "SNAPSHOT_FAILED");
        }
        if (snapshot.mesh.edge_core_published) {
            char output[256];
            int written = snprintf(
                output, sizeof(output),
                "{\"type\":\"asset.loaded\",\"request_id\":%" PRIu64
                ",\"asset_handle\":\"cube\",\"already_loaded\":true}",
                request_id
            );
            return written > 0 && (size_t)written < sizeof(output) &&
                meiso_composition_host_bridge_private_emit_line(output);
        }
        if (snapshot.mesh.queued || snapshot.mesh.transfer_active ||
            snapshot.mesh.waiting_result) {
            return meiso_composition_host_bridge_private_emit_error(request_id, "BUSY");
        }
    } else {
        service->cube_size = sizeof(service->cube_bytes);
        if (meiso_composition_scene_mesh_encode(
                meiso_composition_scene_canonical_cube(),
                (MeisoMutableBuffer){
                    service->cube_bytes, sizeof(service->cube_bytes)
                },
                &service->cube_size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return meiso_composition_host_bridge_private_emit_error(request_id, "CUBE_ENCODE_FAILED");
        }
    }
    status = meiso_composition_edge_put_mesh(
        service->edge,
        (MeisoByteView){service->cube_bytes, service->cube_size}, &receipt
    );
    if (status != MEISO_EDGE_OK) {
        return meiso_composition_host_bridge_private_emit_error(request_id, meiso_composition_host_bridge_private_edge_status_code(status));
    }
    service->cube_content_id = receipt.mesh_content_id;
    service->cube_loaded = true;
    return meiso_composition_host_bridge_private_emit_receipt(request_id, &receipt);
}

static bool handle_scene_command(
    MeisoCompositionHostBridgePrivateProcess *service,
    const char *line,
    size_t size
)
{
    MeisoCompositionHostBridgePrivateJsonCursor cursor = {line, size, 0U};
    MeisoCompositionHostBridgePrivateSceneCommand command;
    MeisoEdgeCommandReceipt receipt;
    MeisoEdgeStatus status;
    MeisoCompositionHostBridgePrivateEditorObjectIdentity old_objects[MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_EDITOR_OBJECTS];
    uint64_t old_serial;

    if (!parse_scene_command(&cursor, &command)) {
        return meiso_composition_host_bridge_private_emit_error(0U, "INVALID_COMMAND");
    }
    if (command.enabled && !service->cube_loaded) {
        return meiso_composition_host_bridge_private_emit_error(command.request_id, "ASSET_NOT_LOADED");
    }
    memcpy(old_objects, service->objects, sizeof(old_objects));
    old_serial = service->next_object_serial;
    if (command.enabled && !stamp_scene_truth(service, &command)) {
        return meiso_composition_host_bridge_private_emit_error(command.request_id, "CLOCK_FAILURE");
    }
    if (command.enabled && !allocate_scene_ids(service, &command)) {
        return meiso_composition_host_bridge_private_emit_error(command.request_id, "OBJECT_ID_CAPACITY");
    }
    status = meiso_composition_edge_set_scene(
        service->edge, command.enabled,
        command.enabled ? &command.value : NULL, &receipt
    );
    if (status != MEISO_EDGE_OK) {
        memcpy(service->objects, old_objects, sizeof(old_objects));
        service->next_object_serial = old_serial;
        return meiso_composition_host_bridge_private_emit_error(command.request_id, meiso_composition_host_bridge_private_edge_status_code(status));
    }
    return meiso_composition_host_bridge_private_emit_receipt(command.request_id, &receipt);
}

static bool parse_enabled_prefix(
    MeisoCompositionHostBridgePrivateJsonCursor *cursor,
    const char *command,
    uint64_t *request_id,
    bool *enabled
)
{
    char prefix[96];
    int written;

    written = snprintf(prefix, sizeof(prefix),
                       "{\"command\":\"%s\",\"request_id\":", command);
    return written > 0 && (size_t)written < sizeof(prefix) &&
        json_literal(cursor, prefix) &&
        json_u64(cursor, UINT64_MAX, request_id) &&
        json_literal(cursor, ",\"enabled\":") && json_bool(cursor, enabled);
}

static bool handle_presentation_command(
    MeisoCompositionHostBridgePrivateProcess *service,
    const char *line,
    size_t size
)
{
    MeisoCompositionHostBridgePrivateJsonCursor cursor = {line, size, 0U};
    MeisoCompositionEdgePresentationSettings settings;
    MeisoEdgeCommandReceipt receipt;
    MeisoEdgeStatus status;
    uint64_t request_id;
    bool enabled;
    bool safety_frame;

    memset(&settings, 0, sizeof(settings));
    if (!parse_enabled_prefix(&cursor, "set_presentation", &request_id,
                              &enabled)) {
        return meiso_composition_host_bridge_private_emit_error(0U, "INVALID_COMMAND");
    }
    if (enabled) {
        if (!json_literal(&cursor, ",\"use_safety_frame\":") ||
            !json_bool(&cursor, &safety_frame) ||
            !json_literal(&cursor, ",\"target_width\":") ||
            !json_u16(&cursor, &settings.target_width) ||
            !json_literal(&cursor, ",\"target_height\":") ||
            !json_u16(&cursor, &settings.target_height) ||
            !json_literal(&cursor, ",\"color_format\":") ||
            !json_color_format(&cursor, &settings.color_format) ||
            !json_literal(&cursor, ",\"depth_format\":\"implementation32\"") ||
            !json_literal(&cursor, ",\"max_result_hz\":") ||
            !json_u16(&cursor, &settings.max_result_hz) ||
            !json_literal(&cursor, ",\"offline_lease_ms\":") ||
            !json_u32(&cursor, &settings.offline_lease_ms)) {
            return meiso_composition_host_bridge_private_emit_error(request_id, "INVALID_COMMAND");
        }
        settings.flags = safety_frame ?
            MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME : 0U;
        settings.depth_format =
            MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    }
    if (!json_literal(&cursor, "}") || !json_end(&cursor)) {
        return meiso_composition_host_bridge_private_emit_error(request_id, "INVALID_COMMAND");
    }
    status = meiso_composition_edge_set_presentation(
        service->edge, enabled, enabled ? &settings : NULL, &receipt
    );
    return status == MEISO_EDGE_OK ?
        meiso_composition_host_bridge_private_emit_receipt(request_id, &receipt) :
        meiso_composition_host_bridge_private_emit_error(request_id, meiso_composition_host_bridge_private_edge_status_code(status));
}

static bool handle_periodic_command(
    MeisoCompositionHostBridgePrivateProcess *service,
    const char *line,
    size_t size,
    bool device
)
{
    MeisoCompositionHostBridgePrivateJsonCursor cursor = {line, size, 0U};
    uint64_t request_id;
    bool enabled;
    uint32_t selected_fields = 0U;
    uint32_t sample_period_ms = 0U;
    MeisoEdgeCommandReceipt receipt;
    MeisoEdgeStatus status;

    if (!parse_enabled_prefix(
            &cursor, device ? "set_device" : "set_wifi", &request_id,
            &enabled
        )) {
        return meiso_composition_host_bridge_private_emit_error(0U, "INVALID_COMMAND");
    }
    if (enabled &&
        (!json_literal(&cursor, ",\"selected_fields\":") ||
         !json_u32(&cursor, &selected_fields) ||
         !json_literal(&cursor, ",\"sample_period_ms\":") ||
         !json_u32(&cursor, &sample_period_ms))) {
        return meiso_composition_host_bridge_private_emit_error(request_id, "INVALID_COMMAND");
    }
    if (!json_literal(&cursor, "}") || !json_end(&cursor)) {
        return meiso_composition_host_bridge_private_emit_error(request_id, "INVALID_COMMAND");
    }
    if (device) {
        MeisoDeviceTelemetryParameters parameters = {
            selected_fields, sample_period_ms
        };
        status = meiso_composition_edge_set_device(
            service->edge, enabled, enabled ? &parameters : NULL, &receipt
        );
    } else {
        MeisoWifiTelemetryDesiredParameters parameters = {
            selected_fields, sample_period_ms
        };
        status = meiso_composition_edge_set_wifi(
            service->edge, enabled, enabled ? &parameters : NULL, &receipt
        );
    }
    return status == MEISO_EDGE_OK ?
        meiso_composition_host_bridge_private_emit_receipt(request_id, &receipt) :
        meiso_composition_host_bridge_private_emit_error(request_id, meiso_composition_host_bridge_private_edge_status_code(status));
}

static bool handle_media_desired_command(
    MeisoCompositionHostBridgePrivateProcess *service,
    const char *line,
    size_t size,
    bool camera
)
{
    MeisoCompositionHostBridgePrivateJsonCursor cursor = {line, size, 0U};
    MeisoCompositionMediaDesired desired;
    MeisoEdgeCommandReceipt receipt;
    MeisoEdgeStatus status;
    uint64_t request_id;
    bool enabled;
    bool request_key;

    memset(&desired, 0, sizeof(desired));
    if (!parse_enabled_prefix(
            &cursor, camera ? "set_camera_media" : "set_render_media",
            &request_id, &enabled
        )) {
        return meiso_composition_host_bridge_private_emit_error(0U, "INVALID_COMMAND");
    }
    if (enabled &&
        (!json_literal(&cursor, ",\"pixel_format\":") ||
         !json_pixel_format(&cursor, &desired.pixel_format) ||
         !json_literal(&cursor, ",\"codec\":\"raw\"") ||
         !json_literal(&cursor, ",\"width\":") ||
         !json_u16(&cursor, &desired.width) ||
         !json_literal(&cursor, ",\"height\":") ||
         !json_u16(&cursor, &desired.height) ||
         !json_literal(&cursor, ",\"fps_millihz\":") ||
         !json_u32(&cursor, &desired.fps_millihz) ||
         !json_literal(&cursor, ",\"bitrate_bps\":") ||
         !json_u32(&cursor, &desired.bitrate_bps) ||
         !json_literal(&cursor, ",\"gop_frames\":") ||
         !json_u32(&cursor, &desired.gop_frames) ||
         !json_literal(&cursor, ",\"request_key\":") ||
         !json_bool(&cursor, &request_key))) {
        return meiso_composition_host_bridge_private_emit_error(request_id, "INVALID_COMMAND");
    }
    if (enabled) {
        desired.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
        desired.recovery_flags = request_key ?
            MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY : 0U;
    }
    if (!json_literal(&cursor, "}") || !json_end(&cursor)) {
        return meiso_composition_host_bridge_private_emit_error(request_id, "INVALID_COMMAND");
    }
    status = camera ? meiso_composition_edge_set_camera_media(
                          service->edge, enabled, enabled ? &desired : NULL,
                          &receipt
                      ) :
                      meiso_composition_edge_set_render_media(
                          service->edge, enabled, enabled ? &desired : NULL,
                          &receipt
                      );
    return status == MEISO_EDGE_OK ?
        meiso_composition_host_bridge_private_emit_receipt(request_id, &receipt) :
        meiso_composition_host_bridge_private_emit_error(request_id, meiso_composition_host_bridge_private_edge_status_code(status));
}

static bool parse_request_only(
    const char *line,
    size_t size,
    const char *command,
    uint64_t *out_request_id
)
{
    MeisoCompositionHostBridgePrivateJsonCursor cursor = {line, size, 0U};
    char prefix[96];
    int written;

    written = snprintf(prefix, sizeof(prefix),
                       "{\"command\":\"%s\",\"request_id\":", command);
    return written > 0 && (size_t)written < sizeof(prefix) &&
        json_literal(&cursor, prefix) &&
        json_u64(&cursor, UINT64_MAX, out_request_id) &&
        json_literal(&cursor, "}") && json_end(&cursor);
}

static bool line_contains_literal(
    const char *line,
    size_t line_size,
    const char *literal
)
{
    size_t literal_size;
    size_t offset;

    if (line == NULL || literal == NULL) {
        return false;
    }
    literal_size = strlen(literal);
    if (literal_size > line_size) {
        return false;
    }
    for (offset = 0U; offset <= line_size - literal_size; ++offset) {
        if (memcmp(line + offset, literal, literal_size) == 0) {
            return true;
        }
    }
    return false;
}

bool meiso_composition_host_bridge_private_handle_command(MeisoCompositionHostBridgePrivateProcess *service, const char *line, size_t size)
{
    uint64_t request_id;

    #define LINE_HAS(literal) line_contains_literal(line, size, literal)

    if (size >= MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONTROL_LINE_BYTES || memchr(line, '\0', size) != NULL) {
        return meiso_composition_host_bridge_private_emit_error(0U, "INVALID_COMMAND");
    }
    if (LINE_HAS("\"command\":\"asset.load_builtin\"")) {
        return handle_asset_command(service, line, size);
    }
    if (LINE_HAS("\"command\":\"set_scene\"")) {
        return handle_scene_command(service, line, size);
    }
    if (LINE_HAS("\"command\":\"set_presentation\"")) {
        return handle_presentation_command(service, line, size);
    }
    if (LINE_HAS("\"command\":\"set_wifi\"")) {
        return handle_periodic_command(service, line, size, false);
    }
    if (LINE_HAS("\"command\":\"set_device\"")) {
        return handle_periodic_command(service, line, size, true);
    }
    if (LINE_HAS("\"command\":\"set_render_media\"")) {
        return handle_media_desired_command(service, line, size, false);
    }
    if (LINE_HAS("\"command\":\"set_camera_media\"")) {
        return handle_media_desired_command(service, line, size, true);
    }
    if (parse_request_only(line, size, "snapshot", &request_id)) {
        return meiso_composition_host_bridge_private_emit_snapshot(service, request_id) && meiso_composition_host_bridge_private_emit_typed_truth(service);
    }
    if (parse_request_only(line, size, "close", &request_id)) {
        char output[160];
        int written;
        service->close_requested = true;
        written = snprintf(
            output, sizeof(output),
            "{\"type\":\"closing\",\"request_id\":%" PRIu64 "}",
            request_id
        );
        return written > 0 && (size_t)written < sizeof(output) &&
            meiso_composition_host_bridge_private_emit_line(output);
    }
    #undef LINE_HAS
    return meiso_composition_host_bridge_private_emit_error(0U, "INVALID_COMMAND");
}
