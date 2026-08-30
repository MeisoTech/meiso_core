#include "meiso_composition_profile_private.h"

#include <string.h>

_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE +
        MEISO_COMPOSITION_PROFILE_PRIVATE_CATALOG_PREFIX_WIRE_SIZE +
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT *
            MEISO_COMPOSITION_PROFILE_PRIVATE_ENDPOINT_RECORD_WIRE_SIZE +
        MEISO_COMPOSITION_PROFILE_CAPABILITY_BYTES +
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT *
            MEISO_COMPOSITION_PROFILE_ROUTE_WIRE_SIZE ==
        MEISO_COMPOSITION_PROFILE_CATALOG_WIRE_SIZE,
    "Composition Profile catalog size"
);
_Static_assert(
    MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_COMPOSITION_SCENE_PRESENTATION_CAPABILITY_WIRE_SIZE +
        MEISO_COMPOSITION_SCENE_CAPABILITY_WIRE_SIZE +
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE +
        2U * MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE ==
        MEISO_COMPOSITION_PROFILE_CAPABILITY_BYTES,
    "Composition Profile capability bytes"
);
/* Atomic catalog construction, shape validation, and MRT0 encoding. */

static bool outputs_disjoint(
    const MeisoCompositionProfileCatalogConfig *config,
    MeisoMutableBuffer capability,
    MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_bytes,
    MeisoCompositionProfileRouteDescriptor *routes,
    size_t route_bytes
)
{
    return !meiso_composition_profile_private_ranges_overlap(config, sizeof(*config), capability.data,
                           capability.capacity) &&
        !meiso_composition_profile_private_ranges_overlap(config, sizeof(*config), endpoints, endpoint_bytes) &&
        !meiso_composition_profile_private_ranges_overlap(config, sizeof(*config), routes, route_bytes) &&
        !meiso_composition_profile_private_ranges_overlap(capability.data, capability.capacity,
                        endpoints, endpoint_bytes) &&
        !meiso_composition_profile_private_ranges_overlap(capability.data, capability.capacity,
                        routes, route_bytes) &&
        !meiso_composition_profile_private_ranges_overlap(endpoints, endpoint_bytes, routes, route_bytes);
}

static bool descriptor_ids_disjoint(
    const MeisoRuntimeEndpointDescriptor *endpoints,
    const MeisoCompositionProfileRouteDescriptor *routes
)
{
    size_t i;
    for (i = 0U; i < MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT; ++i) {
        size_t j;
        for (j = i + 1U; j < MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT;
             ++j) {
            if (meiso_composition_profile_private_endpoint_equal(endpoints[i].id, endpoints[j].id)) {
                return false;
            }
        }
    }
    for (i = 0U; i < MEISO_COMPOSITION_PROFILE_ROUTE_COUNT; ++i) {
        size_t j;
        for (j = i + 1U; j < MEISO_COMPOSITION_PROFILE_ROUTE_COUNT; ++j) {
            if (meiso_composition_profile_private_resource_equal(routes[i].route.resource.id,
                               routes[j].route.resource.id)) {
                return false;
            }
        }
    }
    return true;
}

static void set_routes_for_endpoint(
    MeisoCompositionProfileRouteDescriptor *output,
    const MeisoEndpointId *endpoint,
    uint64_t incarnation,
    const MeisoResourceDescriptor *resources,
    MeisoCompositionDeliveryClass data_delivery
)
{
    static const MeisoRuntimeFacet facets[MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_RESOURCE_COUNT] = {
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    size_t i;
    for (i = 0U; i < MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_RESOURCE_COUNT; ++i) {
        output[i].route.endpoint_id = *endpoint;
        output[i].route.endpoint_incarnation = incarnation;
        output[i].route.resource = resources[i];
        output[i].route.facet = facets[i];
        output[i].delivery = i == 2U ? data_delivery :
            MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME;
    }
}

MeisoCompositionProfileStatus meiso_composition_profile_build_catalog(
    const MeisoCompositionProfileCatalogConfig *config,
    MeisoMutableBuffer dynamic_capability_storage,
    MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_capacity,
    MeisoCompositionProfileRouteDescriptor *routes,
    size_t route_capacity
)
{
    static const MeisoRuntimeFacet wifi_facets[3] = {
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    MeisoRuntimeEndpointDescriptor endpoint_candidate[
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT
    ];
    MeisoCompositionProfileRouteDescriptor route_candidate[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    uint8_t capability_candidate[
        MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES
    ];
    const MeisoResourceDescriptor *wifi_resources;
    const MeisoResourceDescriptor *device_resources;
    size_t wifi_count = 0U;
    size_t device_count = 0U;
    size_t composition_endpoint_count = 0U;
    size_t composition_route_count = 0U;
    size_t encoded_size = 0U;
    size_t endpoint_bytes;
    size_t route_bytes;
    size_t i;

    if (config == NULL || endpoints == NULL || routes == NULL ||
        dynamic_capability_storage.data == NULL ||
        !meiso_composition_profile_private_checked_multiply(endpoint_capacity, sizeof(*endpoints),
                          &endpoint_bytes) ||
        !meiso_composition_profile_private_checked_multiply(route_capacity, sizeof(*routes), &route_bytes)) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (endpoint_capacity < MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT ||
        route_capacity < MEISO_COMPOSITION_PROFILE_ROUTE_COUNT ||
        dynamic_capability_storage.capacity <
            MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES) {
        return MEISO_COMPOSITION_PROFILE_CAPACITY;
    }
    endpoint_bytes = MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT *
        sizeof(*endpoints);
    route_bytes = MEISO_COMPOSITION_PROFILE_ROUTE_COUNT * sizeof(*routes);
    if (!outputs_disjoint(config, dynamic_capability_storage, endpoints,
                          endpoint_bytes, routes, route_bytes)) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (config->wifi_incarnation == 0U ||
        config->presentation_incarnation == 0U ||
        config->scene_incarnation == 0U ||
        config->device_telemetry_incarnation == 0U ||
        config->render_media_incarnation == 0U ||
        config->camera_media_incarnation == 0U ||
        config->render_media_capability.source !=
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ||
        config->camera_media_capability.source !=
            MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) {
        return MEISO_COMPOSITION_PROFILE_INVALID_CONFIG;
    }

    memset(endpoint_candidate, 0, sizeof(endpoint_candidate));
    memset(route_candidate, 0, sizeof(route_candidate));
    if (meiso_wifi_telemetry_capability_encode(
            &config->wifi_capability,
            (MeisoMutableBuffer){
                capability_candidate,
                MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE
            },
            &encoded_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        encoded_size != MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE ||
        meiso_device_telemetry_capability_encode(
            &config->device_telemetry_capability,
            (MeisoMutableBuffer){
                capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE,
                MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE
            },
            &encoded_size
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
        encoded_size != MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE ||
        meiso_composition_media_capability_encode(
            &config->render_media_capability,
            (MeisoMutableBuffer){
                capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
                    MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE,
                MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE
            },
            &encoded_size
        ) != MEISO_COMPOSITION_PROFILE_OK ||
        encoded_size != MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE ||
        meiso_composition_media_capability_encode(
            &config->camera_media_capability,
            (MeisoMutableBuffer){
                capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
                    MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE +
                    MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE,
                MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE
            },
            &encoded_size
        ) != MEISO_COMPOSITION_PROFILE_OK ||
        encoded_size != MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE) {
        return MEISO_COMPOSITION_PROFILE_CODEC_FAILURE;
    }

    endpoint_candidate[0] = (MeisoRuntimeEndpointDescriptor){
        *meiso_wifi_telemetry_endpoint(), config->wifi_incarnation,
        *meiso_wifi_telemetry_capability_schema(),
        {capability_candidate, MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE}
    };
    if (meiso_composition_scene_build_endpoints(
            config->presentation_incarnation, config->scene_incarnation,
            endpoint_candidate + 1U,
            MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT,
            &composition_endpoint_count
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
        composition_endpoint_count != MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT) {
        return MEISO_COMPOSITION_PROFILE_CODEC_FAILURE;
    }
    endpoint_candidate[3] = (MeisoRuntimeEndpointDescriptor){
        *meiso_device_telemetry_endpoint(),
        config->device_telemetry_incarnation,
        *meiso_device_telemetry_capability_schema(),
        {
            capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE,
            MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE
        }
    };
    endpoint_candidate[4] = (MeisoRuntimeEndpointDescriptor){
        meiso_composition_profile_private_render_endpoint, config->render_media_incarnation,
        meiso_composition_profile_private_media_capability_schema,
        {
            capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
                MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE,
            MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE
        }
    };
    endpoint_candidate[5] = (MeisoRuntimeEndpointDescriptor){
        meiso_composition_profile_private_camera_endpoint, config->camera_media_incarnation,
        meiso_composition_profile_private_media_capability_schema,
        {
            capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
                MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE +
                MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE,
            MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE
        }
    };

    wifi_resources = meiso_wifi_telemetry_resources(&wifi_count);
    device_resources = meiso_device_telemetry_resources(&device_count);
    if (wifi_resources == NULL || device_resources == NULL || wifi_count != 3U ||
        device_count != 3U) {
        return MEISO_COMPOSITION_PROFILE_INVALID_CONFIG;
    }
    for (i = 0U; i < 3U; ++i) {
        route_candidate[i].route = (MeisoRuntimeRouteDescriptor){
            *meiso_wifi_telemetry_endpoint(), config->wifi_incarnation,
            wifi_resources[i], wifi_facets[i]
        };
        route_candidate[i].delivery =
            MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME;
    }
    {
        MeisoRuntimeRouteDescriptor composition_routes[
            MEISO_COMPOSITION_SCENE_RESOURCE_COUNT
        ];
        if (meiso_composition_scene_build_routes(
                config->presentation_incarnation, config->scene_incarnation,
                composition_routes, MEISO_COMPOSITION_SCENE_RESOURCE_COUNT,
                &composition_route_count
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
            composition_route_count != MEISO_COMPOSITION_SCENE_RESOURCE_COUNT) {
            return MEISO_COMPOSITION_PROFILE_CODEC_FAILURE;
        }
        for (i = 0U; i < MEISO_COMPOSITION_SCENE_RESOURCE_COUNT; ++i) {
            route_candidate[3U + i].route = composition_routes[i];
            route_candidate[3U + i].delivery =
                MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME;
        }
    }
    set_routes_for_endpoint(route_candidate + 9U,
                            meiso_device_telemetry_endpoint(),
                            config->device_telemetry_incarnation,
                            device_resources,
                            MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME);
    set_routes_for_endpoint(route_candidate + 12U, &meiso_composition_profile_private_render_endpoint,
                            config->render_media_incarnation,
                            meiso_composition_profile_private_render_resources,
                            MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA);
    set_routes_for_endpoint(route_candidate + 15U, &meiso_composition_profile_private_camera_endpoint,
                            config->camera_media_incarnation,
                            meiso_composition_profile_private_camera_resources,
                            MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA);
    if (!descriptor_ids_disjoint(endpoint_candidate, route_candidate)) {
        return MEISO_COMPOSITION_PROFILE_COLLISION;
    }

    memmove(dynamic_capability_storage.data, capability_candidate,
            sizeof(capability_candidate));
    endpoint_candidate[0].capability.data = dynamic_capability_storage.data;
    endpoint_candidate[3].capability.data =
        dynamic_capability_storage.data +
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE;
    endpoint_candidate[4].capability.data =
        dynamic_capability_storage.data +
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE;
    endpoint_candidate[5].capability.data =
        dynamic_capability_storage.data +
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE;
    memmove(endpoints, endpoint_candidate, sizeof(endpoint_candidate));
    memmove(routes, route_candidate, sizeof(route_candidate));
    return MEISO_COMPOSITION_PROFILE_OK;
}

static bool expected_catalog_shape(
    const MeisoRuntimeEndpointDescriptor *endpoints,
    const MeisoCompositionProfileRouteDescriptor *routes
)
{
    static const size_t expected_capability_sizes[6] = {
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE,
        MEISO_COMPOSITION_SCENE_PRESENTATION_CAPABILITY_WIRE_SIZE,
        MEISO_COMPOSITION_SCENE_CAPABILITY_WIRE_SIZE,
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE,
        MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE,
        MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE
    };
    const MeisoEndpointId expected_endpoints[6] = {
        *meiso_wifi_telemetry_endpoint(),
        *meiso_composition_scene_presentation_endpoint(),
        *meiso_composition_scene_scene_endpoint(),
        *meiso_device_telemetry_endpoint(), meiso_composition_profile_private_render_endpoint, meiso_composition_profile_private_camera_endpoint
    };
    const MeisoSchemaRef expected_schemas[6] = {
        *meiso_wifi_telemetry_capability_schema(),
        *meiso_composition_scene_presentation_capability_schema(),
        *meiso_composition_scene_capability_schema(),
        *meiso_device_telemetry_capability_schema(),
        meiso_composition_profile_private_media_capability_schema, meiso_composition_profile_private_media_capability_schema
    };
    const MeisoResourceDescriptor *wifi_resources;
    const MeisoResourceDescriptor *composition_resources;
    const MeisoResourceDescriptor *device_resources;
    const MeisoResourceDescriptor *expected_resources[18];
    static const MeisoRuntimeFacet expected_facets[18] = {
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_IMMUTABLE, MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    MeisoWifiTelemetryCapability wifi_capability;
    MeisoDeviceTelemetryCapability device_capability;
    MeisoCompositionMediaCapability render_capability;
    MeisoCompositionMediaCapability camera_capability;
    MeisoByteView presentation_capability =
        meiso_composition_scene_presentation_capability();
    MeisoByteView scene_capability = meiso_composition_scene_capability();
    size_t wifi_count = 0U;
    size_t composition_count = 0U;
    size_t device_count = 0U;
    size_t capability_sum = 0U;
    size_t i;
    for (i = 0U; i < 6U; ++i) {
        if (!meiso_composition_profile_private_endpoint_equal(endpoints[i].id, expected_endpoints[i]) ||
            endpoints[i].incarnation == 0U ||
            endpoints[i].capability.data == NULL ||
            endpoints[i].capability.size != expected_capability_sizes[i] ||
            !meiso_composition_profile_private_schema_equal(endpoints[i].capability_schema,
                          expected_schemas[i])) {
            return false;
        }
        capability_sum += endpoints[i].capability.size;
    }
    if (capability_sum != MEISO_COMPOSITION_PROFILE_CAPABILITY_BYTES ||
        !descriptor_ids_disjoint(endpoints, routes) ||
        meiso_wifi_telemetry_capability_decode(
            endpoints[0].capability, &wifi_capability
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        presentation_capability.size != endpoints[1].capability.size ||
        memcmp(presentation_capability.data, endpoints[1].capability.data,
               presentation_capability.size) != 0 ||
        scene_capability.size != endpoints[2].capability.size ||
        memcmp(scene_capability.data, endpoints[2].capability.data,
               scene_capability.size) != 0 ||
        meiso_device_telemetry_capability_decode(
            endpoints[3].capability, &device_capability
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
        meiso_composition_media_capability_decode(
            endpoints[4].capability, &render_capability
        ) != MEISO_COMPOSITION_PROFILE_OK ||
        render_capability.source !=
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ||
        meiso_composition_media_capability_decode(
            endpoints[5].capability, &camera_capability
        ) != MEISO_COMPOSITION_PROFILE_OK ||
        camera_capability.source != MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) {
        return false;
    }
    wifi_resources = meiso_wifi_telemetry_resources(&wifi_count);
    composition_resources =
        meiso_composition_scene_resources(&composition_count);
    device_resources = meiso_device_telemetry_resources(&device_count);
    if (wifi_resources == NULL || composition_resources == NULL ||
        device_resources == NULL || wifi_count != 3U ||
        composition_count != 6U || device_count != 3U) {
        return false;
    }
    for (i = 0U; i < 3U; ++i) {
        expected_resources[i] = &wifi_resources[i];
        expected_resources[9U + i] = &device_resources[i];
        expected_resources[12U + i] = &meiso_composition_profile_private_render_resources[i];
        expected_resources[15U + i] = &meiso_composition_profile_private_camera_resources[i];
    }
    for (i = 0U; i < 6U; ++i) {
        expected_resources[3U + i] = &composition_resources[i];
    }
    for (i = 0U; i < MEISO_COMPOSITION_PROFILE_ROUTE_COUNT; ++i) {
        const MeisoRuntimeRouteDescriptor *route = &routes[i].route;
        const size_t endpoint_index = i < 3U ? 0U : i < 5U ? 1U :
            i < 9U ? 2U : i < 12U ? 3U : i < 15U ? 4U : 5U;
        const bool external = (i == 14U || i == 17U);
        if (!meiso_composition_profile_private_endpoint_equal(route->endpoint_id, endpoints[endpoint_index].id) ||
            route->endpoint_incarnation !=
                endpoints[endpoint_index].incarnation ||
            route->facet != expected_facets[i] ||
            !meiso_resource_descriptor_equal(
                &route->resource, expected_resources[i]
            ) ||
            (external ? routes[i].delivery !=
                    MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA :
                routes[i].delivery !=
                    MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME)) {
            return false;
        }
    }
    return true;
}

MeisoCompositionProfileStatus meiso_composition_profile_catalog_encode(
    uint64_t generation,
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoCompositionProfileRouteDescriptor *routes,
    size_t route_count,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    uint8_t candidate[MEISO_COMPOSITION_PROFILE_CATALOG_WIRE_SIZE];
    const size_t endpoint_bytes =
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT * sizeof(*endpoints);
    const size_t route_bytes =
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT * sizeof(*routes);
    size_t cursor = MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_COMPOSITION_PROFILE_PRIVATE_CATALOG_PREFIX_WIRE_SIZE;
    size_t i;
    if (out_size == NULL) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if ((endpoints != NULL && meiso_composition_profile_private_ranges_overlap(
             endpoints, endpoint_bytes, out_size, sizeof(*out_size)
         )) ||
        (routes != NULL && meiso_composition_profile_private_ranges_overlap(
             routes, route_bytes, out_size, sizeof(*out_size)
         ))) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (endpoints == NULL || routes == NULL || output.data == NULL) {
        *out_size = 0U;
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (meiso_composition_profile_private_ranges_overlap(endpoints, endpoint_bytes, routes, route_bytes) ||
        meiso_composition_profile_private_ranges_overlap(endpoints, endpoint_bytes, output.data,
                       sizeof(candidate)) ||
        meiso_composition_profile_private_ranges_overlap(routes, route_bytes, output.data, sizeof(candidate)) ||
        meiso_composition_profile_private_ranges_overlap(output.data, sizeof(candidate), out_size,
                       sizeof(*out_size))) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (generation == 0U ||
        endpoint_count != MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT ||
        route_count != MEISO_COMPOSITION_PROFILE_ROUTE_COUNT) {
        *out_size = 0U;
        return MEISO_COMPOSITION_PROFILE_INVALID_CONFIG;
    }
    for (i = 0U; i < MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT; ++i) {
        if (meiso_composition_profile_private_ranges_overlap(endpoints[i].capability.data,
                           endpoints[i].capability.size,
                           output.data, sizeof(candidate)) ||
            meiso_composition_profile_private_ranges_overlap(endpoints[i].capability.data,
                           endpoints[i].capability.size,
                           out_size, sizeof(*out_size)) ||
            meiso_composition_profile_private_ranges_overlap(endpoints[i].capability.data,
                           endpoints[i].capability.size,
                           endpoints, endpoint_bytes) ||
            meiso_composition_profile_private_ranges_overlap(endpoints[i].capability.data,
                           endpoints[i].capability.size,
                           routes, route_bytes)) {
            return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
        }
    }
    *out_size = 0U;
    if (!expected_catalog_shape(endpoints, routes)) {
        return MEISO_COMPOSITION_PROFILE_INVALID_CONFIG;
    }
    if (output.capacity < sizeof(candidate)) {
        return MEISO_COMPOSITION_PROFILE_CAPACITY;
    }
    memset(candidate, 0, sizeof(candidate));
    candidate[0] = (uint8_t)'M';
    candidate[1] = (uint8_t)'R';
    candidate[2] = (uint8_t)'T';
    candidate[3] = (uint8_t)'0';
    candidate[4] = MEISO_COMPOSITION_PROFILE_RUNTIME_WIRE_VERSION;
    candidate[5] = MEISO_COMPOSITION_PROFILE_PRIVATE_MRT0_CATALOG_TYPE;
    meiso_composition_profile_private_put_u32(candidate + 8U, (uint32_t)sizeof(candidate));
    meiso_composition_profile_private_put_u64(candidate + MEISO_RUNTIME_WIRE_HEADER_SIZE, generation);
    meiso_composition_profile_private_put_u32(candidate + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U,
            MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT);
    meiso_composition_profile_private_put_u32(candidate + MEISO_RUNTIME_WIRE_HEADER_SIZE + 12U,
            MEISO_COMPOSITION_PROFILE_ROUTE_COUNT);
    for (i = 0U; i < endpoint_count; ++i) {
        meiso_composition_profile_private_put_id(candidate + cursor, endpoints[i].id.high, endpoints[i].id.low);
        meiso_composition_profile_private_put_u64(candidate + cursor + 16U, endpoints[i].incarnation);
        meiso_composition_profile_private_put_id(candidate + cursor + 24U,
               endpoints[i].capability_schema.id.high,
               endpoints[i].capability_schema.id.low);
        meiso_composition_profile_private_put_u32(candidate + cursor + 40U,
                endpoints[i].capability_schema.version);
        meiso_composition_profile_private_put_u32(candidate + cursor + 44U,
                (uint32_t)endpoints[i].capability.size);
        cursor += MEISO_COMPOSITION_PROFILE_PRIVATE_ENDPOINT_RECORD_WIRE_SIZE;
        memmove(candidate + cursor, endpoints[i].capability.data,
                endpoints[i].capability.size);
        cursor += endpoints[i].capability.size;
    }
    for (i = 0U; i < route_count; ++i) {
        const MeisoRuntimeRouteDescriptor *route = &routes[i].route;
        meiso_composition_profile_private_put_id(candidate + cursor, route->endpoint_id.high,
               route->endpoint_id.low);
        meiso_composition_profile_private_put_u64(candidate + cursor + 16U, route->endpoint_incarnation);
        meiso_composition_profile_private_put_id(candidate + cursor + 24U, route->resource.id.high,
               route->resource.id.low);
        candidate[cursor + 40U] = route->resource.kind;
        candidate[cursor + 41U] = route->resource.owner;
        candidate[cursor + 42U] = route->facet;
        meiso_composition_profile_private_put_id(candidate + cursor + 44U, route->resource.schema.id.high,
               route->resource.schema.id.low);
        meiso_composition_profile_private_put_u32(candidate + cursor + 60U, route->resource.schema.version);
        meiso_composition_profile_private_put_u64(candidate + cursor + 64U,
                (uint64_t)route->resource.max_payload_size);
        candidate[cursor + 72U] = routes[i].delivery;
        cursor += MEISO_COMPOSITION_PROFILE_ROUTE_WIRE_SIZE;
    }
    if (cursor != sizeof(candidate)) {
        return MEISO_COMPOSITION_PROFILE_CODEC_FAILURE;
    }
    memmove(output.data, candidate, sizeof(candidate));
    *out_size = sizeof(candidate);
    return MEISO_COMPOSITION_PROFILE_OK;
}
