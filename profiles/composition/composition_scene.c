#include "meiso_composition_scene_private.h"

#include <string.h>

/* Immutable schema/catalog identity and endpoint/route construction. */

static const MeisoEndpointId PRESENTATION_ENDPOINT = {
    UINT64_C(0x434f4d504f53455f),
    UINT64_C(0x50524553454e545f)
};

const MeisoEndpointId meiso_composition_scene_private_scene_endpoint = {
    UINT64_C(0x434f4d504f53455f),
    UINT64_C(0x5343454e455f5f5f)
};

static const MeisoSchemaRef PRESENTATION_CAPABILITY_SCHEMA = {
    {
        UINT64_C(0x434f4d505f534348),
        UINT64_C(0x505245535f434150)
    },
    1U
};

static const MeisoSchemaRef SCENE_CAPABILITY_SCHEMA = {
    {
        UINT64_C(0x434f4d505f534348),
        UINT64_C(0x5343454e5f434150)
    },
    1U
};

/*
 * Literal, catalog-embedded capability bytes. They are deliberately exposed
 * as views so conformance tests can freeze the complete catalog payload.
 */
static const uint8_t PRESENTATION_CAPABILITY[
    MEISO_COMPOSITION_SCENE_PRESENTATION_CAPABILITY_WIRE_SIZE
] = {
    0x50U, 0x43U, 0x41U, 0x50U, /* PCAP */
    0x00U, 0x01U,               /* version */
    0x00U, 0x01U,               /* one required layer */
    0x01U, 0x40U, 0x00U, 0xf0U, /* 320 x 240 */
    0x00U, 0x01U,               /* RGBA8888 */
    0x00U, 0x01U,               /* implementation-local depth32 */
    0x00U, 0x0aU,               /* max 10 Hz */
    0x00U, 0x00U,               /* reserved */
    0x00U, 0x00U, 0x13U, 0x88U, /* max 5000-ms offline lease */
    0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U
};

static const uint8_t SCENE_CAPABILITY[
    MEISO_COMPOSITION_SCENE_CAPABILITY_WIRE_SIZE
] = {
    0x53U, 0x43U, 0x41U, 0x50U, /* SCAP */
    0x00U, 0x01U, 0x00U, 0x00U, /* version, flags */
    0x00U, 0x00U, 0x00U, 0x20U, /* 32 objects */
    0x00U, 0x00U, 0x00U, 0x08U, /* 8 unique meshes */
    0x00U, 0x04U, 0x00U, 0x00U, /* 256 KiB Blob */
    0x00U, 0x00U, 0x00U, 0x10U, /* 16 published slots */
    0x00U, 0x00U, 0x00U, 0x01U, /* one staging transfer */
    0x00U, 0x00U, 0x10U, 0x00U, /* 4096-byte chunks */
    0x00U, 0x20U, 0x00U, 0x00U, /* 2 MiB per world */
    0x00U, 0x40U, 0x00U, 0x00U, /* 4 MiB total */
    0x00U, 0x40U, 0x00U, 0x30U, /* 64 x 48 thumbnail */
    0x00U, 0x02U, 0x0aU, 0x04U, /* RGB565, 10 Hz, 4 slots */
    0x02U, 0x02U, 0x00U, 0x00U  /* 2 jobs, 2 target sets, reserved */
};

static const MeisoResourceDescriptor RESOURCES[
    MEISO_COMPOSITION_SCENE_RESOURCE_COUNT
] = {
    {
        {
            UINT64_C(0x434f4d504f53455f),
            UINT64_C(0x504c414e5f444553)
        },
        MEISO_RESOURCE_KIND_STATE,
        {
            {
                UINT64_C(0x434f4d505f534348),
                UINT64_C(0x505245535f504c4e)
            },
            1U
        },
        MEISO_OWNER_ROLE_HOST,
        MEISO_COMPOSITION_SCENE_PRESENTATION_DESIRED_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {
            UINT64_C(0x434f4d504f53455f),
            UINT64_C(0x504c414e5f414354)
        },
        MEISO_RESOURCE_KIND_STATE,
        {
            {
                UINT64_C(0x434f4d505f534348),
                UINT64_C(0x505245535f535441)
            },
            1U
        },
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {
            UINT64_C(0x434f4d504f53455f),
            UINT64_C(0x53434e5f44455300)
        },
        MEISO_RESOURCE_KIND_STATE,
        {
            {
                UINT64_C(0x434f4d505f534348),
                UINT64_C(0x5343454e5f444553)
            },
            1U
        },
        MEISO_OWNER_ROLE_HOST,
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {
            UINT64_C(0x434f4d504f53455f),
            UINT64_C(0x53434e5f41435400)
        },
        MEISO_RESOURCE_KIND_STATE,
        {
            {
                UINT64_C(0x434f4d505f534348),
                UINT64_C(0x5343454e5f535441)
            },
            1U
        },
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_SCENE_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {
            UINT64_C(0x434f4d504f53455f),
            UINT64_C(0x4d4553485f424c42)
        },
        MEISO_RESOURCE_KIND_BLOB,
        {
            {
                UINT64_C(0x434f4d505f534348),
                UINT64_C(0x4d4553485f464d54)
            },
            1U
        },
        MEISO_OWNER_ROLE_HOST,
        MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE
    },
    {
        {
            UINT64_C(0x434f4d504f53455f),
            UINT64_C(0x5245535f53545200)
        },
        MEISO_RESOURCE_KIND_STREAM,
        {
            {
                UINT64_C(0x434f4d505f534348),
                UINT64_C(0x52454e445f524553)
            },
            1U
        },
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    }
};

const MeisoEndpointId *meiso_composition_scene_presentation_endpoint(void)
{
    return &PRESENTATION_ENDPOINT;
}

const MeisoEndpointId *meiso_composition_scene_scene_endpoint(void)
{
    return &meiso_composition_scene_private_scene_endpoint;
}

const MeisoSchemaRef *
meiso_composition_scene_presentation_capability_schema(void)
{
    return &PRESENTATION_CAPABILITY_SCHEMA;
}

const MeisoSchemaRef *meiso_composition_scene_capability_schema(void)
{
    return &SCENE_CAPABILITY_SCHEMA;
}

MeisoByteView meiso_composition_scene_presentation_capability(void)
{
    MeisoByteView value;

    value.data = PRESENTATION_CAPABILITY;
    value.size = sizeof(PRESENTATION_CAPABILITY);
    return value;
}

MeisoByteView meiso_composition_scene_capability(void)
{
    MeisoByteView value;

    value.data = SCENE_CAPABILITY;
    value.size = sizeof(SCENE_CAPABILITY);
    return value;
}

const MeisoResourceDescriptor *meiso_composition_scene_resources(
    size_t *out_count
)
{
    if (out_count != NULL) {
        *out_count = MEISO_COMPOSITION_SCENE_RESOURCE_COUNT;
    }
    return RESOURCES;
}

const MeisoResourceDescriptor *meiso_composition_scene_resource(
    MeisoCompositionSceneResource resource
)
{
    if (resource >= MEISO_COMPOSITION_SCENE_RESOURCE_COUNT) {
        return NULL;
    }
    return &RESOURCES[resource];
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_build_endpoints(
    uint64_t presentation_incarnation,
    uint64_t scene_incarnation,
    MeisoRuntimeEndpointDescriptor *output,
    size_t output_capacity,
    size_t *out_count
)
{
    if (out_count == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    *out_count = 0U;
    if (output == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (presentation_incarnation == 0U || scene_incarnation == 0U) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
    }
    if (output_capacity < MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT) {
        return MEISO_COMPOSITION_SCENE_CODEC_CAPACITY;
    }

    output[0].id = PRESENTATION_ENDPOINT;
    output[0].incarnation = presentation_incarnation;
    output[0].capability_schema = PRESENTATION_CAPABILITY_SCHEMA;
    output[0].capability = meiso_composition_scene_presentation_capability();
    output[1].id = meiso_composition_scene_private_scene_endpoint;
    output[1].incarnation = scene_incarnation;
    output[1].capability_schema = SCENE_CAPABILITY_SCHEMA;
    output[1].capability = meiso_composition_scene_capability();
    *out_count = MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_build_routes(
    uint64_t presentation_incarnation,
    uint64_t scene_incarnation,
    MeisoRuntimeRouteDescriptor *output,
    size_t output_capacity,
    size_t *out_count
)
{
    static const MeisoRuntimeFacet facets[
        MEISO_COMPOSITION_SCENE_RESOURCE_COUNT
    ] = {
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_IMMUTABLE,
        MEISO_RUNTIME_FACET_DATA
    };
    size_t index;

    if (out_count == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    *out_count = 0U;
    if (output == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (presentation_incarnation == 0U || scene_incarnation == 0U) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
    }
    if (output_capacity < MEISO_COMPOSITION_SCENE_RESOURCE_COUNT) {
        return MEISO_COMPOSITION_SCENE_CODEC_CAPACITY;
    }

    for (index = 0U; index < MEISO_COMPOSITION_SCENE_RESOURCE_COUNT;
         ++index) {
        const bool presentation = index < 2U;

        output[index].endpoint_id = presentation ?
            PRESENTATION_ENDPOINT : meiso_composition_scene_private_scene_endpoint;
        output[index].endpoint_incarnation = presentation ?
            presentation_incarnation : scene_incarnation;
        output[index].resource = RESOURCES[index];
        output[index].facet = facets[index];
    }
    *out_count = MEISO_COMPOSITION_SCENE_RESOURCE_COUNT;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

static bool schema_collides_with_internal(MeisoSchemaId schema)
{
    size_t index;

    if (meiso_composition_scene_private_schema_id_equal(schema, PRESENTATION_CAPABILITY_SCHEMA.id) ||
        meiso_composition_scene_private_schema_id_equal(schema, SCENE_CAPABILITY_SCHEMA.id)) {
        return true;
    }
    for (index = 0U; index < MEISO_COMPOSITION_SCENE_RESOURCE_COUNT;
         ++index) {
        if (meiso_composition_scene_private_schema_id_equal(schema, RESOURCES[index].schema.id)) {
            return true;
        }
    }
    return false;
}

bool meiso_composition_scene_catalog_identifiers_disjoint(
    const MeisoRuntimeEndpointDescriptor *external_endpoints,
    size_t external_endpoint_count,
    const MeisoRuntimeRouteDescriptor *external_routes,
    size_t external_route_count
)
{
    size_t index;

    if ((external_endpoint_count != 0U && external_endpoints == NULL) ||
        (external_route_count != 0U && external_routes == NULL)) {
        return false;
    }
    for (index = 0U; index < external_endpoint_count; ++index) {
        if (meiso_composition_scene_private_endpoint_id_equal(external_endpoints[index].id,
                              PRESENTATION_ENDPOINT) ||
            meiso_composition_scene_private_endpoint_id_equal(external_endpoints[index].id,
                              meiso_composition_scene_private_scene_endpoint) ||
            schema_collides_with_internal(
                external_endpoints[index].capability_schema.id)) {
            return false;
        }
    }
    for (index = 0U; index < external_route_count; ++index) {
        size_t resource_index;

        if (meiso_composition_scene_private_endpoint_id_equal(external_routes[index].endpoint_id,
                              PRESENTATION_ENDPOINT) ||
            meiso_composition_scene_private_endpoint_id_equal(external_routes[index].endpoint_id,
                              meiso_composition_scene_private_scene_endpoint) ||
            schema_collides_with_internal(
                external_routes[index].resource.schema.id)) {
            return false;
        }
        for (resource_index = 0U;
             resource_index < MEISO_COMPOSITION_SCENE_RESOURCE_COUNT;
             ++resource_index) {
            if (meiso_composition_scene_private_resource_id_equal(external_routes[index].resource.id,
                                  RESOURCES[resource_index].id)) {
                return false;
            }
        }
    }
    return true;
}
