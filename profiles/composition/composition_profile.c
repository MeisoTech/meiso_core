#include "meiso_composition_profile_private.h"

/* Immutable Profile identity, media descriptors, and Session policy. */

const MeisoProfileRef meiso_composition_profile_private_profile = {
    {UINT64_C(0x434f4d504f53455f), UINT64_C(0x5f50524f46494c45)},
    MEISO_COMPOSITION_PROFILE_VERSION
};
const MeisoEndpointId meiso_composition_profile_private_render_endpoint = {
    UINT64_C(0x52454e4445525f4d), UINT64_C(0x454449415f454e44)
};
const MeisoEndpointId meiso_composition_profile_private_camera_endpoint = {
    UINT64_C(0x43414d4552415f4d), UINT64_C(0x454449415f454e44)
};
const MeisoSchemaRef meiso_composition_profile_private_media_capability_schema = {
    {UINT64_C(0x4d454449415f5343), UINT64_C(0x4341504142494c5f)}, 1U
};

const MeisoResourceDescriptor meiso_composition_profile_private_render_resources[MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_RESOURCE_COUNT] = {
    {
        {UINT64_C(0x52454e4445525f4d), UINT64_C(0x4445535f53544154)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x444553495245445f)}, 1U},
        MEISO_OWNER_ROLE_HOST,
        MEISO_COMPOSITION_MEDIA_DESIRED_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {UINT64_C(0x52454e4445525f4d), UINT64_C(0x4143545f53544154)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x41435455414c5f5f)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_MEDIA_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {UINT64_C(0x52454e4445525f4d), UINT64_C(0x444154415f4d4544)},
        MEISO_RESOURCE_KIND_STREAM,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x554e49545f5f5f5f)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES
    }
};
const MeisoResourceDescriptor meiso_composition_profile_private_camera_resources[MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_RESOURCE_COUNT] = {
    {
        {UINT64_C(0x43414d4552415f4d), UINT64_C(0x4445535f53544154)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x444553495245445f)}, 1U},
        MEISO_OWNER_ROLE_HOST,
        MEISO_COMPOSITION_MEDIA_DESIRED_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {UINT64_C(0x43414d4552415f4d), UINT64_C(0x4143545f53544154)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x41435455414c5f5f)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_MEDIA_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {UINT64_C(0x43414d4552415f4d), UINT64_C(0x444154415f4d4544)},
        MEISO_RESOURCE_KIND_STREAM,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x554e49545f5f5f5f)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES
    }
};

const MeisoProfileRef *meiso_composition_profile_ref(void)
{
    return &meiso_composition_profile_private_profile;
}

const MeisoEndpointId *meiso_composition_render_media_endpoint(void)
{
    return &meiso_composition_profile_private_render_endpoint;
}

const MeisoEndpointId *meiso_composition_camera_media_endpoint(void)
{
    return &meiso_composition_profile_private_camera_endpoint;
}

const MeisoSchemaRef *meiso_composition_media_capability_schema(void)
{
    return &meiso_composition_profile_private_media_capability_schema;
}

const MeisoResourceDescriptor *meiso_composition_render_media_resources(
    size_t *out_count
)
{
    if (out_count != NULL) {
        *out_count = MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_RESOURCE_COUNT;
    }
    return meiso_composition_profile_private_render_resources;
}

const MeisoResourceDescriptor *meiso_composition_camera_media_resources(
    size_t *out_count
)
{
    if (out_count != NULL) {
        *out_count = MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_RESOURCE_COUNT;
    }
    return meiso_composition_profile_private_camera_resources;
}

MeisoCompositionProfileStatus meiso_composition_profile_configure_session(
    MeisoSessionConfig *in_out_config
)
{
    MeisoSessionConfig candidate;
    if (in_out_config == NULL) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    candidate = *in_out_config;
    candidate.profile = meiso_composition_profile_private_profile;
    candidate.limits = (MeisoSessionLimits){
        MEISO_COMPOSITION_PROFILE_MAX_FRAME_BYTES,
        MEISO_COMPOSITION_PROFILE_MAX_RUNTIME_PAYLOAD_BYTES,
        MEISO_COMPOSITION_PROFILE_MAX_REGISTRY_ENTRIES
    };
    if (meiso_session_memory_required(&candidate) == 0U) {
        return MEISO_COMPOSITION_PROFILE_INVALID_CONFIG;
    }
    *in_out_config = candidate;
    return MEISO_COMPOSITION_PROFILE_OK;
}
