#define _POSIX_C_SOURCE 200809L

#include "meiso_drm_libdrm_topology_private.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static const char *const PLANE_PROPERTY_NAMES[
    MEISO_K230D_DRM_PLANE_PROPERTY_COUNT
] = {
    "FB_ID",
    "CRTC_ID",
    "SRC_X",
    "SRC_Y",
    "SRC_W",
    "SRC_H",
    "CRTC_X",
    "CRTC_Y",
    "CRTC_W",
    "CRTC_H"
};

typedef struct {
    drmVersionPtr version;
    drmModeResPtr resources;
    drmModePlaneResPtr plane_resources;
    drmModeConnectorPtr connector;
    drmModeEncoderPtr encoder;
    drmModeCrtcPtr crtc;
    drmModePlanePtr plane;
    drmModeObjectPropertiesPtr object_properties;
    drmModePropertyPtr property;
} LibdrmObjects;

static int positive_error(int result)
{
    if (errno > 0) {
        return errno;
    }
    if (result < -1 && result != INT_MIN) {
        return -result;
    }
    return EIO;
}

static bool client_capability_unsupported(int error)
{
    return error == EINVAL || error == EOPNOTSUPP || error == ENOTTY;
}

static void set_operation_failure(
    MeisoK230dDrmNativeObservation *observation,
    MeisoK230dDrmNativeStage stage,
    int error
)
{
    if (observation->operation_stage == MEISO_K230D_DRM_NATIVE_STAGE_NONE) {
        observation->operation_stage = stage;
        observation->error = error > 0 ? error : EIO;
    }
    observation->status = MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED;
}

static void set_preclassified(
    MeisoK230dDrmNativeObservation *observation,
    MeisoK230dDrmAdmissionReason reason
)
{
    if (observation->operation_stage == MEISO_K230D_DRM_NATIVE_STAGE_NONE) {
        observation->status = MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED;
        observation->preclassified_reason = reason;
    }
}

static void object_seen(MeisoK230dDrmNativeObservation *observation)
{
    if (observation->libdrm_objects_seen != UINT32_MAX) {
        ++observation->libdrm_objects_seen;
    }
}

static void object_released(MeisoK230dDrmNativeObservation *observation)
{
    if (observation->libdrm_objects_released_count != UINT32_MAX) {
        ++observation->libdrm_objects_released_count;
    }
}

static void release_property(
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    if (objects->property != NULL) {
        drmModeFreeProperty(objects->property);
        objects->property = NULL;
        object_released(observation);
    }
}

static void release_object_properties(
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    if (objects->object_properties != NULL) {
        drmModeFreeObjectProperties(objects->object_properties);
        objects->object_properties = NULL;
        object_released(observation);
    }
}

static void release_plane(
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    if (objects->plane != NULL) {
        drmModeFreePlane(objects->plane);
        objects->plane = NULL;
        object_released(observation);
    }
}

static void release_crtc(
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    if (objects->crtc != NULL) {
        drmModeFreeCrtc(objects->crtc);
        objects->crtc = NULL;
        object_released(observation);
    }
}

static void release_encoder(
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    if (objects->encoder != NULL) {
        drmModeFreeEncoder(objects->encoder);
        objects->encoder = NULL;
        object_released(observation);
    }
}

static void release_connector(
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    if (objects->connector != NULL) {
        drmModeFreeConnector(objects->connector);
        objects->connector = NULL;
        object_released(observation);
    }
}

static void release_all(
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    release_property(objects, observation);
    release_object_properties(objects, observation);
    release_plane(objects, observation);
    release_crtc(objects, observation);
    release_encoder(objects, observation);
    release_connector(objects, observation);
    if (objects->plane_resources != NULL) {
        drmModeFreePlaneResources(objects->plane_resources);
        objects->plane_resources = NULL;
        object_released(observation);
    }
    if (objects->resources != NULL) {
        drmModeFreeResources(objects->resources);
        objects->resources = NULL;
        object_released(observation);
    }
    if (objects->version != NULL) {
        drmFreeVersion(objects->version);
        objects->version = NULL;
        object_released(observation);
    }
}

static bool property_name_valid(const drmModePropertyRes *property)
{
    return property != NULL &&
        memchr(property->name, '\0', DRM_PROP_NAME_LEN) != NULL;
}

static bool property_arrays_valid(const drmModeObjectProperties *properties)
{
    return properties != NULL &&
        (properties->count_props == 0U ||
         (properties->props != NULL && properties->prop_values != NULL));
}

static bool range_property_valid(
    drmModePropertyRes *property,
    uint64_t value,
    uint32_t expected_type
)
{
    return property != NULL &&
        drm_property_type_is(property, expected_type) &&
        property->count_values == 2 && property->values != NULL &&
        value >= property->values[0] && value <= property->values[1];
}

static bool decode_signed(uint64_t encoded, int64_t *out_value)
{
    uint64_t magnitude;
    if (out_value == NULL) {
        return false;
    }
    if (encoded <= (uint64_t)INT64_MAX) {
        *out_value = (int64_t)encoded;
        return true;
    }
    magnitude = UINT64_MAX - encoded + UINT64_C(1);
    if (magnitude == (uint64_t)INT64_MAX + UINT64_C(1)) {
        *out_value = INT64_MIN;
        return true;
    }
    if (magnitude == 0U || magnitude > (uint64_t)INT64_MAX) {
        return false;
    }
    *out_value = -(int64_t)magnitude;
    return true;
}

static bool zpos_value(
    drmModePropertyRes *property,
    uint64_t raw_value,
    int64_t *out_value
)
{
    if (drm_property_type_is(property, DRM_MODE_PROP_RANGE)) {
        if (!range_property_valid(property, raw_value, DRM_MODE_PROP_RANGE) ||
            raw_value > (uint64_t)INT64_MAX) {
            return false;
        }
        *out_value = (int64_t)raw_value;
        return true;
    }
    if (drm_property_type_is(property, DRM_MODE_PROP_SIGNED_RANGE)) {
        int64_t minimum;
        int64_t maximum;
        int64_t value;
        if (property->count_values != 2 || property->values == NULL ||
            !decode_signed(property->values[0], &minimum) ||
            !decode_signed(property->values[1], &maximum) ||
            !decode_signed(raw_value, &value) || minimum > maximum ||
            value < minimum || value > maximum) {
            return false;
        }
        *out_value = value;
        return true;
    }
    return false;
}

static bool writable_plane_property_shape(
    uint32_t property_index,
    drmModePropertyRes *property
)
{
    uint32_t expected_type;
    if (property == NULL ||
        (property->flags & DRM_MODE_PROP_IMMUTABLE) != 0U) {
        return false;
    }
    switch (property_index) {
        case MEISO_K230D_DRM_PLANE_FB_ID:
        case MEISO_K230D_DRM_PLANE_CRTC_ID:
            expected_type = DRM_MODE_PROP_OBJECT;
            break;
        case MEISO_K230D_DRM_PLANE_CRTC_X:
        case MEISO_K230D_DRM_PLANE_CRTC_Y:
            expected_type = DRM_MODE_PROP_SIGNED_RANGE;
            break;
        default:
            expected_type = DRM_MODE_PROP_RANGE;
            break;
    }
    return drm_property_type_is(property, expected_type);
}

static bool map_plane_type(
    uint64_t value,
    MeisoK230dDrmPlaneType *out_type
)
{
    if (out_type == NULL) {
        return false;
    }
    switch (value) {
        case DRM_PLANE_TYPE_OVERLAY:
            *out_type = MEISO_K230D_DRM_PLANE_TYPE_OVERLAY;
            return true;
        case DRM_PLANE_TYPE_PRIMARY:
            *out_type = MEISO_K230D_DRM_PLANE_TYPE_PRIMARY;
            return true;
        case DRM_PLANE_TYPE_CURSOR:
            *out_type = MEISO_K230D_DRM_PLANE_TYPE_CURSOR;
            return true;
        default:
            return false;
    }
}

static int compare_connectors(const void *lhs, const void *rhs)
{
    const MeisoK230dDrmConnector *left =
        (const MeisoK230dDrmConnector *)lhs;
    const MeisoK230dDrmConnector *right =
        (const MeisoK230dDrmConnector *)rhs;
    return left->connector_id < right->connector_id ? -1 :
        (left->connector_id > right->connector_id ? 1 : 0);
}

static int compare_crtcs(const void *lhs, const void *rhs)
{
    const MeisoK230dDrmCrtc *left = (const MeisoK230dDrmCrtc *)lhs;
    const MeisoK230dDrmCrtc *right = (const MeisoK230dDrmCrtc *)rhs;
    return left->crtc_id < right->crtc_id ? -1 :
        (left->crtc_id > right->crtc_id ? 1 : 0);
}

static int compare_planes(const void *lhs, const void *rhs)
{
    const MeisoK230dDrmPlane *left = (const MeisoK230dDrmPlane *)lhs;
    const MeisoK230dDrmPlane *right = (const MeisoK230dDrmPlane *)rhs;
    return left->plane_id < right->plane_id ? -1 :
        (left->plane_id > right->plane_id ? 1 : 0);
}

static bool copy_mode(
    const drmModeModeInfo *source,
    MeisoK230dDrmMode *destination
)
{
    const char *terminator;
    size_t name_size;
    if (source == NULL || destination == NULL) {
        return false;
    }
    terminator = (const char *)memchr(
        source->name, '\0', sizeof(source->name)
    );
    if (terminator == NULL) {
        return false;
    }
    name_size = (size_t)(terminator - source->name);
    if (name_size == 0U || name_size >= sizeof(destination->name)) {
        return false;
    }
    memset(destination, 0, sizeof(*destination));
    memcpy(destination->name, source->name, name_size);
    destination->clock_khz = source->clock;
    destination->hdisplay = source->hdisplay;
    destination->hsync_start = source->hsync_start;
    destination->hsync_end = source->hsync_end;
    destination->htotal = source->htotal;
    destination->hskew = source->hskew;
    destination->vdisplay = source->vdisplay;
    destination->vsync_start = source->vsync_start;
    destination->vsync_end = source->vsync_end;
    destination->vtotal = source->vtotal;
    destination->vscan = source->vscan;
    destination->flags = source->flags;
    destination->type = source->type;
    return true;
}

static bool read_crtc_active(
    int fd,
    uint32_t crtc_id,
    bool *out_active,
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    bool found = false;
    bool active = false;
    uint32_t index;

    errno = 0;
    objects->object_properties = drmModeObjectGetProperties(
        fd, crtc_id, DRM_MODE_OBJECT_CRTC
    );
    if (objects->object_properties == NULL) {
        set_operation_failure(
            observation,
            MEISO_K230D_DRM_NATIVE_STAGE_CRTC_PROPERTIES,
            positive_error(-1)
        );
        return false;
    }
    object_seen(observation);
    if (!property_arrays_valid(objects->object_properties)) {
        set_preclassified(
            observation, MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
        );
        return false;
    }
    for (index = 0U; index < objects->object_properties->count_props;
         ++index) {
        const uint32_t property_id =
            objects->object_properties->props[index];
        const uint64_t value = objects->object_properties->prop_values[index];
        if (property_id == 0U) {
            set_preclassified(
                observation,
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
            );
            return false;
        }
        errno = 0;
        objects->property = drmModeGetProperty(
            fd, property_id
        );
        if (objects->property == NULL) {
            set_operation_failure(
                observation,
                MEISO_K230D_DRM_NATIVE_STAGE_CRTC_PROPERTIES,
                positive_error(-1)
            );
            return false;
        }
        object_seen(observation);
        if (!property_name_valid(objects->property) ||
            objects->property->prop_id != property_id) {
            set_preclassified(
                observation,
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
            );
            return false;
        }
        if (strcmp(objects->property->name, "ACTIVE") == 0) {
            if (found || objects->property->prop_id == 0U ||
                (objects->property->flags & DRM_MODE_PROP_IMMUTABLE) != 0U ||
                !range_property_valid(
                    objects->property, value, DRM_MODE_PROP_RANGE
                ) || value > 1U) {
                set_preclassified(
                    observation,
                    MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
                );
                return false;
            }
            found = true;
            active = value != 0U;
        }
        release_property(objects, observation);
    }
    release_object_properties(objects, observation);
    if (!found) {
        set_preclassified(
            observation, MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
        );
        return false;
    }
    *out_active = active;
    return true;
}

static bool read_plane_properties(
    int fd,
    const drmModePlane *native_plane,
    MeisoK230dDrmPlane *plane,
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    bool writable_found[MEISO_K230D_DRM_PLANE_PROPERTY_COUNT] = {false};
    bool type_found = false;
    bool zpos_found = false;
    uint32_t index;

    errno = 0;
    objects->object_properties = drmModeObjectGetProperties(
        fd, native_plane->plane_id, DRM_MODE_OBJECT_PLANE
    );
    if (objects->object_properties == NULL) {
        set_operation_failure(
            observation,
            MEISO_K230D_DRM_NATIVE_STAGE_PLANE_PROPERTIES,
            positive_error(-1)
        );
        return false;
    }
    object_seen(observation);
    if (!property_arrays_valid(objects->object_properties)) {
        set_preclassified(
            observation, MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
        );
        return false;
    }
    for (index = 0U; index < objects->object_properties->count_props;
         ++index) {
        const uint32_t property_id = objects->object_properties->props[index];
        const uint64_t value = objects->object_properties->prop_values[index];
        uint32_t required_index;

        if (property_id == 0U) {
            set_preclassified(
                observation,
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
            );
            return false;
        }
        errno = 0;
        objects->property = drmModeGetProperty(fd, property_id);
        if (objects->property == NULL) {
            set_operation_failure(
                observation,
                MEISO_K230D_DRM_NATIVE_STAGE_PLANE_PROPERTIES,
                positive_error(-1)
            );
            return false;
        }
        object_seen(observation);
        if (!property_name_valid(objects->property) ||
            objects->property->prop_id != property_id) {
            set_preclassified(
                observation,
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
            );
            return false;
        }
        for (required_index = 0U;
             required_index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
             ++required_index) {
            if (strcmp(
                    objects->property->name,
                    PLANE_PROPERTY_NAMES[required_index]
                ) == 0) {
                if (writable_found[required_index] ||
                    !writable_plane_property_shape(
                        required_index, objects->property
                    )) {
                    set_preclassified(
                        observation,
                        MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
                    );
                    return false;
                }
                writable_found[required_index] = true;
                plane->properties[required_index].property_id = property_id;
                plane->properties[required_index].value = value;
                break;
            }
        }
        if (strcmp(objects->property->name, "type") == 0) {
            if (type_found ||
                !drm_property_type_is(objects->property, DRM_MODE_PROP_ENUM) ||
                !map_plane_type(value, &plane->type)) {
                set_preclassified(
                    observation,
                    MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
                );
                return false;
            }
            type_found = true;
            plane->type_immutable =
                (objects->property->flags & DRM_MODE_PROP_IMMUTABLE) != 0U;
        } else if (strcmp(objects->property->name, "zpos") == 0) {
            if (zpos_found ||
                !zpos_value(objects->property, value, &plane->zpos)) {
                set_preclassified(
                    observation,
                    MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
                );
                return false;
            }
            zpos_found = true;
            plane->zpos_present = true;
            plane->zpos_immutable =
                (objects->property->flags & DRM_MODE_PROP_IMMUTABLE) != 0U;
        }
        release_property(objects, observation);
    }
    release_object_properties(objects, observation);
    if (!type_found) {
        set_preclassified(
            observation, MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
        );
        return false;
    }
    for (index = 0U; index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++index) {
        if (!writable_found[index]) {
            set_preclassified(
                observation,
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
            );
            return false;
        }
    }
    if (native_plane->fb_id !=
            plane->properties[MEISO_K230D_DRM_PLANE_FB_ID].value ||
        native_plane->crtc_id !=
            plane->properties[MEISO_K230D_DRM_PLANE_CRTC_ID].value) {
        set_preclassified(
            observation, MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
        );
        return false;
    }
    return true;
}

static bool translate_connectors(
    int fd,
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    uint32_t index;
    for (index = 0U; index < observation->raw_connector_count; ++index) {
        MeisoK230dDrmConnector *destination =
            &observation->topology.connectors[index];
        const uint32_t expected_id = objects->resources->connectors[index];
        errno = 0;
        objects->connector = drmModeGetConnectorCurrent(fd, expected_id);
        if (objects->connector == NULL) {
            set_operation_failure(
                observation,
                MEISO_K230D_DRM_NATIVE_STAGE_CONNECTOR,
                positive_error(-1)
            );
            return false;
        }
        object_seen(observation);
        if (expected_id == 0U || objects->connector->connector_id != expected_id) {
            set_preclassified(
                observation,
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
            );
            return false;
        }
        destination->connector_id = objects->connector->connector_id;
        destination->connector_type = objects->connector->connector_type;
        destination->connector_type_id = objects->connector->connector_type_id;
        destination->encoder_id = objects->connector->encoder_id;
        destination->connected =
            objects->connector->connection == DRM_MODE_CONNECTED;
        if (destination->encoder_id != 0U) {
            errno = 0;
            objects->encoder = drmModeGetEncoder(
                fd, destination->encoder_id
            );
            if (objects->encoder == NULL) {
                set_operation_failure(
                    observation,
                    MEISO_K230D_DRM_NATIVE_STAGE_ENCODER,
                    positive_error(-1)
                );
                return false;
            }
            object_seen(observation);
            if (objects->encoder->encoder_id != destination->encoder_id) {
                set_preclassified(
                    observation,
                    MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
                );
                return false;
            }
            destination->crtc_id = objects->encoder->crtc_id;
            release_encoder(objects, observation);
        }
        release_connector(objects, observation);
    }
    qsort(
        observation->topology.connectors,
        observation->raw_connector_count,
        sizeof(observation->topology.connectors[0]),
        compare_connectors
    );
    return true;
}

static bool translate_crtcs(
    int fd,
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    uint32_t index;
    for (index = 0U; index < observation->raw_crtc_count; ++index) {
        MeisoK230dDrmCrtc *destination = &observation->topology.crtcs[index];
        const uint32_t expected_id = objects->resources->crtcs[index];
        bool active;
        errno = 0;
        objects->crtc = drmModeGetCrtc(fd, expected_id);
        if (objects->crtc == NULL) {
            set_operation_failure(
                observation,
                MEISO_K230D_DRM_NATIVE_STAGE_CRTC,
                positive_error(-1)
            );
            return false;
        }
        object_seen(observation);
        if (expected_id == 0U || objects->crtc->crtc_id != expected_id) {
            set_preclassified(
                observation,
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
            );
            return false;
        }
        if (!read_crtc_active(
                fd, expected_id, &active, objects, observation
            )) {
            return false;
        }
        destination->crtc_id = objects->crtc->crtc_id;
        destination->framebuffer_id = objects->crtc->buffer_id;
        destination->x = objects->crtc->x;
        destination->y = objects->crtc->y;
        destination->active = active;
        destination->mode_valid = objects->crtc->mode_valid != 0;
        if (destination->mode_valid &&
            !copy_mode(&objects->crtc->mode, &destination->mode)) {
            set_preclassified(
                observation,
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
            );
            return false;
        }
        release_crtc(objects, observation);
    }
    qsort(
        observation->topology.crtcs,
        observation->raw_crtc_count,
        sizeof(observation->topology.crtcs[0]),
        compare_crtcs
    );
    return true;
}

static bool translate_planes(
    int fd,
    LibdrmObjects *objects,
    MeisoK230dDrmNativeObservation *observation
)
{
    uint32_t resource_ids[MEISO_K230D_DRM_MAX_CRTCS] = {0U};
    uint32_t canonical_ids[MEISO_K230D_DRM_MAX_CRTCS] = {0U};
    uint32_t index;

    for (index = 0U; index < observation->raw_crtc_count; ++index) {
        resource_ids[index] = objects->resources->crtcs[index];
        canonical_ids[index] = observation->topology.crtcs[index].crtc_id;
    }
    for (index = 0U; index < observation->raw_plane_count; ++index) {
        MeisoK230dDrmPlane *destination = &observation->topology.planes[index];
        const uint32_t expected_id = objects->plane_resources->planes[index];
        uint32_t format_index;
        errno = 0;
        objects->plane = drmModeGetPlane(fd, expected_id);
        if (objects->plane == NULL) {
            set_operation_failure(
                observation,
                MEISO_K230D_DRM_NATIVE_STAGE_PLANE,
                positive_error(-1)
            );
            return false;
        }
        object_seen(observation);
        if (expected_id == 0U || objects->plane->plane_id != expected_id ||
            (objects->plane->count_formats != 0U &&
             objects->plane->formats == NULL) ||
            !meiso_k230d_drm_private_remap_crtc_mask(
                resource_ids,
                observation->raw_crtc_count,
                canonical_ids,
                observation->raw_crtc_count,
                objects->plane->possible_crtcs,
                &destination->possible_crtcs
            )) {
            set_preclassified(
                observation,
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
            );
            return false;
        }
        destination->plane_id = objects->plane->plane_id;
        for (format_index = 0U;
             format_index < objects->plane->count_formats;
             ++format_index) {
            if (objects->plane->formats[format_index] == DRM_FORMAT_RGB565) {
                destination->supports_rgb565 = true;
            }
        }
        if (!read_plane_properties(
                fd, objects->plane, destination, objects, observation
            )) {
            return false;
        }
        release_plane(objects, observation);
    }
    qsort(
        observation->topology.planes,
        observation->raw_plane_count,
        sizeof(observation->topology.planes[0]),
        compare_planes
    );
    observation->all_plane_properties_complete = true;
    return true;
}

MeisoK230dDrmNativeReadStatus meiso_k230d_drm_libdrm_observe(
    const char *device_path,
    MeisoK230dDrmNativeObservation *out_observation
)
{
    _Static_assert(
        MEISO_K230D_DRM_RGB565_FOURCC == DRM_FORMAT_RGB565,
        "Meiso RGB565 fourcc must match libdrm"
    );
    MeisoK230dDrmNativeObservation candidate;
    LibdrmObjects objects;
    struct stat descriptor_stat;
    uint64_t monotonic_cap = 0U;
    uint32_t index;
    size_t path_size;
    int fd = -1;
    int result;

    if (device_path == NULL || out_observation == NULL ||
        !meiso_k230d_drm_private_path_size(device_path, &path_size) ||
        meiso_k230d_drm_private_ranges_overlap(
            device_path, path_size + 1U,
            out_observation, sizeof(*out_observation)
        )) {
        return MEISO_K230D_DRM_NATIVE_READ_INVALID_ARGUMENT;
    }
    memset(&candidate, 0, sizeof(candidate));
    memset(&objects, 0, sizeof(objects));
    candidate.status = MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED;

    errno = 0;
    fd = open(device_path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        set_operation_failure(
            &candidate, MEISO_K230D_DRM_NATIVE_STAGE_OPEN,
            positive_error(-1)
        );
        goto done;
    }
    candidate.fd_opened = true;
    if (fstat(fd, &descriptor_stat) != 0 || !S_ISCHR(descriptor_stat.st_mode)) {
        set_operation_failure(
            &candidate, MEISO_K230D_DRM_NATIVE_STAGE_FD_CONTRACT,
            errno > 0 ? errno : ENODEV
        );
        goto done;
    }
    errno = 0;
    result = drmGetNodeTypeFromFd(fd);
    if (result != DRM_NODE_PRIMARY) {
        set_operation_failure(
            &candidate, MEISO_K230D_DRM_NATIVE_STAGE_FD_CONTRACT,
            result < 0 ? positive_error(result) : ENODEV
        );
        goto done;
    }
    candidate.primary_node_verified = true;

    candidate.master_observation_attempted = true;
    errno = 0;
    result = drmIsMaster(fd);
    if (result < 0) {
        set_operation_failure(
            &candidate,
            MEISO_K230D_DRM_NATIVE_STAGE_MASTER_OBSERVATION,
            positive_error(result)
        );
        goto done;
    }
    candidate.master_observation_supported = true;
    candidate.master_observed_while_open = result != 0;

    errno = 0;
    result = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1U);
    if (result != 0) {
        const int cap_error = positive_error(result);
        if (client_capability_unsupported(cap_error)) {
            set_preclassified(
                &candidate, MEISO_K230D_DRM_ADMISSION_CLIENT_CAPABILITY
            );
        } else {
            set_operation_failure(
                &candidate,
                MEISO_K230D_DRM_NATIVE_STAGE_UNIVERSAL_PLANES_CAP,
                cap_error
            );
        }
        goto done;
    }
    candidate.universal_planes_client_cap_set = true;
    errno = 0;
    result = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1U);
    if (result != 0) {
        const int cap_error = positive_error(result);
        if (client_capability_unsupported(cap_error)) {
            set_preclassified(
                &candidate, MEISO_K230D_DRM_ADMISSION_CLIENT_CAPABILITY
            );
        } else {
            set_operation_failure(
                &candidate, MEISO_K230D_DRM_NATIVE_STAGE_ATOMIC_CAP,
                cap_error
            );
        }
        goto done;
    }
    candidate.atomic_client_cap_set = true;

    errno = 0;
    objects.version = drmGetVersion(fd);
    if (objects.version == NULL) {
        set_operation_failure(
            &candidate, MEISO_K230D_DRM_NATIVE_STAGE_DRIVER_VERSION,
            positive_error(-1)
        );
        goto done;
    }
    object_seen(&candidate);
    if (objects.version->name == NULL || objects.version->name_len <= 0 ||
        (size_t)objects.version->name_len >=
            sizeof(candidate.topology.driver_name) ||
        memchr(objects.version->name, '\0', objects.version->name_len) != NULL ||
        objects.version->version_major < 0 ||
        objects.version->version_minor < 0 ||
        objects.version->version_patchlevel < 0) {
        set_preclassified(
            &candidate, MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
        );
        goto done;
    }
    memcpy(
        candidate.topology.driver_name,
        objects.version->name,
        objects.version->name_len
    );
    candidate.topology.driver_version_major =
        (uint32_t)objects.version->version_major;
    candidate.topology.driver_version_minor =
        (uint32_t)objects.version->version_minor;
    candidate.topology.driver_version_patch =
        (uint32_t)objects.version->version_patchlevel;
    release_all(&objects, &candidate);

    errno = 0;
    result = drmGetCap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, &monotonic_cap);
    if (result != 0) {
        set_operation_failure(
            &candidate, MEISO_K230D_DRM_NATIVE_STAGE_TIMESTAMP_CAP,
            positive_error(result)
        );
        goto done;
    }
    candidate.topology.page_flip_timestamps_monotonic = monotonic_cap != 0U;

    errno = 0;
    objects.resources = drmModeGetResources(fd);
    if (objects.resources == NULL) {
        set_operation_failure(
            &candidate, MEISO_K230D_DRM_NATIVE_STAGE_RESOURCES,
            positive_error(-1)
        );
        goto done;
    }
    object_seen(&candidate);
    errno = 0;
    objects.plane_resources = drmModeGetPlaneResources(fd);
    if (objects.plane_resources == NULL) {
        set_operation_failure(
            &candidate, MEISO_K230D_DRM_NATIVE_STAGE_PLANE_RESOURCES,
            positive_error(-1)
        );
        goto done;
    }
    object_seen(&candidate);
    if (objects.resources->count_connectors < 0 ||
        objects.resources->count_crtcs < 0) {
        set_preclassified(
            &candidate, MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
        );
        goto done;
    }
    candidate.raw_connector_count =
        (uint32_t)objects.resources->count_connectors;
    candidate.raw_crtc_count = (uint32_t)objects.resources->count_crtcs;
    candidate.raw_plane_count = objects.plane_resources->count_planes;
    candidate.topology.connector_count = candidate.raw_connector_count;
    candidate.topology.crtc_count = candidate.raw_crtc_count;
    candidate.topology.plane_count = candidate.raw_plane_count;
    if (candidate.raw_connector_count > MEISO_K230D_DRM_MAX_CONNECTORS ||
        candidate.raw_crtc_count > MEISO_K230D_DRM_MAX_CRTCS ||
        candidate.raw_crtc_count > 32U ||
        candidate.raw_plane_count > MEISO_K230D_DRM_MAX_PLANES) {
        candidate.status = MEISO_K230D_DRM_NATIVE_READ_COMPLETE;
        candidate.topology_ready_for_evaluation = true;
        goto done;
    }
    if ((candidate.raw_connector_count != 0U &&
         objects.resources->connectors == NULL) ||
        (candidate.raw_crtc_count != 0U && objects.resources->crtcs == NULL) ||
        (candidate.raw_plane_count != 0U &&
         objects.plane_resources->planes == NULL)) {
        set_preclassified(
            &candidate, MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
        );
        goto done;
    }
    for (index = 0U; index < candidate.raw_crtc_count; ++index) {
        uint32_t other;
        if (objects.resources->crtcs[index] == 0U) {
            set_preclassified(
                &candidate,
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
            );
            goto done;
        }
        for (other = 0U; other < index; ++other) {
            if (objects.resources->crtcs[other] ==
                objects.resources->crtcs[index]) {
                set_preclassified(
                    &candidate,
                    MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
                );
                goto done;
            }
        }
    }
    if (!translate_crtcs(fd, &objects, &candidate) ||
        !translate_connectors(fd, &objects, &candidate) ||
        !translate_planes(fd, &objects, &candidate)) {
        goto done;
    }
    if (!meiso_k230d_drm_private_topology_valid(&candidate.topology)) {
        set_preclassified(
            &candidate, MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL
        );
        goto done;
    }
    candidate.topology_complete = true;
    candidate.topology_ready_for_evaluation = true;
    candidate.status = MEISO_K230D_DRM_NATIVE_READ_COMPLETE;

done:
    release_all(&objects, &candidate);
    candidate.libdrm_objects_released =
        candidate.libdrm_objects_seen ==
        candidate.libdrm_objects_released_count;
    if (!candidate.libdrm_objects_released) {
        set_operation_failure(
            &candidate, MEISO_K230D_DRM_NATIVE_STAGE_TRANSLATION, EBUSY
        );
    }
    if (fd >= 0) {
        candidate.close_attempted = true;
        errno = 0;
        result = close(fd);
        candidate.close_consumed = true;
        if (result == 0) {
            candidate.device_closed = true;
        } else {
            set_operation_failure(
                &candidate, MEISO_K230D_DRM_NATIVE_STAGE_CLOSE,
                positive_error(result)
            );
        }
    }
    *out_observation = candidate;
    return candidate.status;
}
