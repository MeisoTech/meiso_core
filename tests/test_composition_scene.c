#include <meiso/profiles/composition/scene.h>
#include <meiso/profiles/wifi_telemetry.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                         \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

enum {
    TEST_RUNTIME_LIMIT = 8192,
    TEST_SESSION_RUNTIME_OVERHEAD = 44,
    TEST_STATE_OFFER_OVERHEAD = 96,
    TEST_STREAM_SAMPLE_OVERHEAD = 144,
    TEST_CATALOG_FIXED_BYTES = 28,
    TEST_CATALOG_ENDPOINT_BYTES = 48,
    TEST_CATALOG_ROUTE_BYTES = 72,
    TEST_MAX_MESH_VERTICES = 21839
};

_Static_assert(MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE == 3696U,
               "32-object SceneDesired suffix size");
_Static_assert(MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE == 3704U,
               "32-object canonical desired State size");
_Static_assert(MEISO_RUNTIME_WIRE_HEADER_SIZE + TEST_STATE_OFFER_OVERHEAD +
                   MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE ==
                   3812U,
               "32-object Runtime STATE_OFFER size");
_Static_assert(MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE == 6304U,
               "RenderResult profile payload size");
_Static_assert(MEISO_RUNTIME_WIRE_HEADER_SIZE + TEST_STREAM_SAMPLE_OVERHEAD +
                   MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE == 6460U,
               "RenderResult Runtime STREAM_SAMPLE size");
_Static_assert(3812U <= TEST_RUNTIME_LIMIT && 6460U <= TEST_RUNTIME_LIMIT,
               "selected payloads fit the 8192-byte Runtime limit");
_Static_assert(TEST_RUNTIME_LIMIT + TEST_SESSION_RUNTIME_OVERHEAD == 8236U,
               "selected Session max_frame_bytes minimum");
_Static_assert(TEST_CATALOG_FIXED_BYTES +
                   3U * TEST_CATALOG_ENDPOINT_BYTES +
                   9U * TEST_CATALOG_ROUTE_BYTES +
                   MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
                   MEISO_COMPOSITION_SCENE_PRESENTATION_CAPABILITY_WIRE_SIZE +
                   MEISO_COMPOSITION_SCENE_CAPABILITY_WIRE_SIZE == 920U,
               "three-endpoint composite catalog size");

static const uint8_t PRESENTATION_CAPABILITY_GOLDEN[32] = {
    0x50U, 0x43U, 0x41U, 0x50U, 0x00U, 0x01U, 0x00U, 0x01U,
    0x01U, 0x40U, 0x00U, 0xf0U, 0x00U, 0x01U, 0x00U, 0x01U,
    0x00U, 0x0aU, 0x00U, 0x00U, 0x00U, 0x00U, 0x13U, 0x88U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
};

static const uint8_t SCENE_CAPABILITY_GOLDEN[52] = {
    0x53U, 0x43U, 0x41U, 0x50U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x20U, 0x00U, 0x00U, 0x00U, 0x08U,
    0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x10U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x10U, 0x00U,
    0x00U, 0x20U, 0x00U, 0x00U, 0x00U, 0x40U, 0x00U, 0x00U,
    0x00U, 0x40U, 0x00U, 0x30U, 0x00U, 0x02U, 0x0aU, 0x04U,
    0x02U, 0x02U, 0x00U, 0x00U
};

/* SHA-256 below was calculated independently over these literal 128 bytes. */
static const uint8_t PLAN_DIGEST_PREIMAGE[128] = {
    0x50U, 0x4cU, 0x4eU, 0x30U, 0x00U, 0x01U, 0x00U, 0x01U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x43U, 0x4fU, 0x4dU, 0x50U, 0x4fU, 0x53U, 0x45U, 0x5fU,
    0x53U, 0x43U, 0x45U, 0x4eU, 0x45U, 0x5fU, 0x5fU, 0x5fU,
    0x01U, 0x01U, 0x01U, 0x00U, 0x01U, 0x40U, 0x00U, 0xf0U,
    0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x0aU, 0x00U, 0x00U,
    0x00U, 0x00U, 0x03U, 0xe8U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
};

static const uint8_t PLAN_DIGEST_GOLDEN[MEISO_SHA256_DIGEST_SIZE] = {
    0xd2U, 0x3cU, 0x94U, 0x65U, 0x9dU, 0xd8U, 0xf0U, 0xcbU,
    0xe4U, 0x12U, 0x9dU, 0x8dU, 0x97U, 0x63U, 0x96U, 0x40U,
    0x40U, 0xe7U, 0x28U, 0x45U, 0xc8U, 0x10U, 0xd5U, 0xa1U,
    0x83U, 0x84U, 0xdaU, 0xcaU, 0x2dU, 0x00U, 0xc6U, 0x4eU
};

static const uint8_t PLAN_GOLDEN[128] = {
    0x50U, 0x4cU, 0x4eU, 0x30U, 0x00U, 0x01U, 0x00U, 0x01U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0xd2U, 0x3cU, 0x94U, 0x65U, 0x9dU, 0xd8U, 0xf0U, 0xcbU,
    0xe4U, 0x12U, 0x9dU, 0x8dU, 0x97U, 0x63U, 0x96U, 0x40U,
    0x40U, 0xe7U, 0x28U, 0x45U, 0xc8U, 0x10U, 0xd5U, 0xa1U,
    0x83U, 0x84U, 0xdaU, 0xcaU, 0x2dU, 0x00U, 0xc6U, 0x4eU,
    0x43U, 0x4fU, 0x4dU, 0x50U, 0x4fU, 0x53U, 0x45U, 0x5fU,
    0x53U, 0x43U, 0x45U, 0x4eU, 0x45U, 0x5fU, 0x5fU, 0x5fU,
    0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x40U, 0x00U, 0xf0U,
    0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x0aU, 0x00U, 0x00U,
    0x00U, 0x00U, 0x03U, 0xe8U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x29U, 0x2aU, 0x2bU, 0x2cU, 0x2dU, 0x2eU, 0x2fU, 0x30U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x39U, 0x3aU, 0x3bU, 0x3cU, 0x3dU, 0x3eU, 0x3fU, 0x40U
};

static const uint8_t PRESENTATION_STATUS_GOLDEN[160] = {
    0x50U, 0x53U, 0x54U, 0x30U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0xd2U, 0x3cU, 0x94U, 0x65U, 0x9dU, 0xd8U, 0xf0U, 0xcbU,
    0xe4U, 0x12U, 0x9dU, 0x8dU, 0x97U, 0x63U, 0x96U, 0x40U,
    0x40U, 0xe7U, 0x28U, 0x45U, 0xc8U, 0x10U, 0xd5U, 0xa1U,
    0x83U, 0x84U, 0xdaU, 0xcaU, 0x2dU, 0x00U, 0xc6U, 0x4eU,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x29U, 0x2aU, 0x2bU, 0x2cU, 0x2dU, 0x2eU, 0x2fU, 0x30U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x39U, 0x3aU, 0x3bU, 0x3cU, 0x3dU, 0x3eU, 0x3fU, 0x40U,
    0x03U, 0x02U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U,
    0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
    0x49U, 0x4aU, 0x4bU, 0x4cU, 0x4dU, 0x4eU, 0x4fU, 0x50U,
    0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U, 0x58U,
    0x59U, 0x5aU, 0x5bU, 0x5cU, 0x5dU, 0x5eU, 0x5fU, 0x60U,
    0x01U, 0x40U, 0x00U, 0xf0U, 0x00U, 0x01U, 0x00U, 0x01U,
    0x43U, 0x4fU, 0x4dU, 0x50U, 0x4fU, 0x53U, 0x45U, 0x5fU,
    0x53U, 0x43U, 0x45U, 0x4eU, 0x45U, 0x5fU, 0x5fU, 0x5fU
};

static const uint8_t CUBE_GOLDEN[304] = {
    0x4dU, 0x53U, 0x48U, 0x30U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x24U,
    0x00U, 0x00U, 0x00U, 0x0cU, 0x00U, 0x00U, 0x00U, 0x60U,
    0x00U, 0x00U, 0x00U, 0x90U, 0x00U, 0x00U, 0x00U, 0x00U,
    0xbfU, 0x00U, 0x00U, 0x00U, 0xbfU, 0x00U, 0x00U, 0x00U,
    0xbfU, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U,
    0x3fU, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    /* eight literal xyz records */
    0xbfU, 0x00U, 0x00U, 0x00U, 0xbfU, 0x00U, 0x00U, 0x00U, 0xbfU, 0x00U, 0x00U, 0x00U,
    0x3fU, 0x00U, 0x00U, 0x00U, 0xbfU, 0x00U, 0x00U, 0x00U, 0xbfU, 0x00U, 0x00U, 0x00U,
    0x3fU, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U, 0xbfU, 0x00U, 0x00U, 0x00U,
    0xbfU, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U, 0xbfU, 0x00U, 0x00U, 0x00U,
    0xbfU, 0x00U, 0x00U, 0x00U, 0xbfU, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U,
    0x3fU, 0x00U, 0x00U, 0x00U, 0xbfU, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U,
    0x3fU, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U,
    0xbfU, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x02U,
    0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x05U,
    0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U, 0x00U, 0x07U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x05U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x05U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x07U,
    0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U, 0x00U, 0x03U,
    0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U, 0x00U, 0x02U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x03U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x02U,
    0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U, 0x00U, 0x05U
};

static const uint8_t DESIRED_GOLDEN[224] = {
    0x53U, 0x43U, 0x4eU, 0x30U, 0x00U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x01U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x3fU, 0x80U, 0x00U, 0x00U,
    0xbfU, 0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x3fU, 0x80U, 0x00U, 0x00U, 0x42U, 0xb4U, 0x00U, 0x00U,
    0x3fU, 0x00U, 0x00U, 0x00U, 0x42U, 0xc8U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x40U, 0x00U, 0xf0U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
    0x00U, 0x01U, 0x20U, 0x00U, 0x01U, 0x02U, 0x03U, 0x04U,
    0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0aU, 0x0bU, 0x0cU,
    0x0dU, 0x0eU, 0x0fU, 0x10U, 0x11U, 0x12U, 0x13U, 0x14U,
    0x15U, 0x16U, 0x17U, 0x18U, 0x19U, 0x1aU, 0x1bU, 0x1cU,
    0x1dU, 0x1eU, 0x1fU, 0x20U, 0x01U, 0x01U, 0x00U, 0x00U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x3fU, 0x80U, 0x00U, 0x00U, 0x40U, 0x00U, 0x00U, 0x00U,
    0x40U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x3fU, 0x80U, 0x00U, 0x00U, 0x3fU, 0x80U, 0x00U, 0x00U,
    0x40U, 0x00U, 0x00U, 0x00U, 0x3fU, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
};

static const uint8_t SCENE_STATUS_GOLDEN[112] = {
    0x53U, 0x53U, 0x54U, 0x30U, 0x00U, 0x01U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U,
    0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x30U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x09U, 0x60U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x02U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
};

static const uint8_t RENDER_RESULT_GOLDEN[
    MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
] = {
    0x52U, 0x52U, 0x53U, 0x30U, 0x00U, 0x01U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x01U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x05U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x06U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x07U,
    0x01U, 0x40U, 0x00U, 0xf0U, 0x00U, 0x01U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x05U, 0x00U, 0x00U, 0x00U, 0x05U, 0x00U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU, 0x10U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x19U, 0x1aU, 0x1bU, 0x1cU, 0x1dU, 0x1eU, 0x1fU, 0x20U,
    0x00U, 0x40U, 0x00U, 0x30U, 0x00U, 0x02U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U, 0x18U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
};

static void put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static float float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void fill_bytes(uint8_t *output, size_t size, uint8_t first)
{
    size_t index;
    for (index = 0U; index < size; ++index) {
        output[index] = (uint8_t)(first + (uint8_t)index);
    }
}

static MeisoContentId content_id(uint8_t first)
{
    MeisoContentId value;
    memset(&value, 0, sizeof(value));
    value.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    value.digest_size = MEISO_SHA256_DIGEST_SIZE;
    fill_bytes(value.digest, MEISO_SHA256_DIGEST_SIZE, first);
    return value;
}

static MeisoCompositionScenePresentationPlan example_plan(void)
{
    MeisoCompositionScenePresentationPlan value;
    memset(&value, 0, sizeof(value));
    value.flags = MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME;
    value.plan_generation = UINT64_C(0x0102030405060708);
    value.capability_generation = UINT64_C(0x1112131415161718);
    memcpy(value.candidate_digest, PLAN_DIGEST_GOLDEN,
           sizeof(value.candidate_digest));
    value.scene_binding = *meiso_composition_scene_scene_endpoint();
    value.source = MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE;
    value.geometry = MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION;
    value.target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    value.target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    value.color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    value.depth_format = MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    value.max_result_hz = MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ;
    value.offline_lease_ms = 1000U;
    value.prepared_token_echo_present = true;
    fill_bytes(value.prepared_token_echo,
               sizeof(value.prepared_token_echo), 0x21U);
    return value;
}

static MeisoCompositionScenePresentationStatus example_presentation_status(void)
{
    MeisoCompositionScenePresentationStatus value;
    memset(&value, 0, sizeof(value));
    value.capability_generation = UINT64_C(0x0102030405060708);
    value.candidate_state = MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED;
    value.candidate_plan_generation = UINT64_C(0x1112131415161718);
    memcpy(value.candidate_digest, PLAN_DIGEST_GOLDEN,
           sizeof(value.candidate_digest));
    value.active_state = MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE;
    value.active_plan_generation = UINT64_C(0x2122232425262728);
    fill_bytes(value.active_digest, sizeof(value.active_digest), 0x21U);
    value.reason = MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE;
    value.prepared_token_present = true;
    fill_bytes(value.prepared_token, sizeof(value.prepared_token), 0x41U);
    value.scene_binding = *meiso_composition_scene_scene_endpoint();
    value.target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    value.target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    value.color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    value.depth_format = MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    return value;
}

static MeisoCompositionSceneDesired example_desired(void)
{
    MeisoCompositionSceneDesired value;
    MeisoCompositionSceneObject *object;
    memset(&value, 0, sizeof(value));
    value.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    value.camera.space_epoch = UINT64_C(0x0102030405060708);
    value.camera.source_time = (MeisoTimePoint){
        UINT64_C(0x1112131415161718),
        UINT64_C(0x2122232425262728)
    };
    value.camera.position[0] = 0.0F;
    value.camera.position[1] = 1.0F;
    value.camera.position[2] = -1.0F;
    value.camera.orientation_xyzw[3] = 1.0F;
    value.camera.projection = MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    value.camera.vertical_fov_degrees = 90.0F;
    value.camera.near_z = 0.5F;
    value.camera.far_z = 100.0F;
    value.camera.viewport_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    value.camera.viewport_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    value.object_count = 1U;
    object = &value.objects[0];
    object->id.high = UINT64_C(0x3132333435363738);
    object->id.low = UINT64_C(0x4142434445464748);
    object->mesh_content_id = content_id(1U);
    object->space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    object->space_epoch = value.camera.space_epoch;
    object->transform.position[0] = 1.0F;
    object->transform.position[1] = 2.0F;
    object->transform.position[2] = 3.0F;
    object->transform.orientation_xyzw[3] = 1.0F;
    object->transform.scale[0] = 1.0F;
    object->transform.scale[1] = 2.0F;
    object->transform.scale[2] = 0.5F;
    object->flags = MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE;
    return value;
}

static MeisoCompositionSceneStatus example_scene_status(void)
{
    MeisoCompositionSceneStatus value;
    memset(&value, 0, sizeof(value));
    value.reason = MEISO_COMPOSITION_SCENE_REASON_READY;
    value.active_plan_generation = 1U;
    value.world_generation = 2U;
    value.active_object_count = 2U;
    value.unique_mesh_count = 1U;
    value.published_blob_count = 1U;
    value.published_blob_bytes = 304U;
    value.resident_mesh_bytes = 512U;
    value.target_slots_used = 1U;
    value.target_bytes = 614400U;
    value.render_jobs_in_flight = 1U;
    value.result_queue_depth = 2U;
    value.last_result_stream_epoch = 3U;
    value.last_result_sequence = 4U;
    return value;
}

static MeisoCompositionSceneRenderResult example_render_result(void)
{
    MeisoCompositionSceneRenderResult value;
    memset(&value, 0, sizeof(value));
    value.applied_scene_owner_incarnation = 1U;
    value.applied_scene_state_version = 2U;
    value.active_plan_generation = 3U;
    value.world_generation = 4U;
    value.result_code = MEISO_COMPOSITION_SCENE_RESULT_COMPLETE;
    value.evidence_flags =
        MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE |
        MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED;
    value.object_count = 1U;
    value.unique_mesh_count = 1U;
    value.source_clock_epoch = 5U;
    value.render_start_monotonic_ns = 6U;
    value.render_end_monotonic_ns = 7U;
    fill_bytes(value.full_target_sha256,
               sizeof(value.full_target_sha256), 1U);
    value.target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    value.target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    value.color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    value.depth_format = MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    value.logical_color_row_bytes =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    value.logical_depth_row_bytes =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    value.thumbnail_width = MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH;
    value.thumbnail_height = MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT;
    value.thumbnail_format = MEISO_COMPOSITION_SCENE_COLOR_RGB565;
    value.thumbnail_stride_bytes =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH *
        MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTES_PER_PIXEL;
    value.thumbnail_byte_size = MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE;
    return value;
}

static bool test_profile_contract(void)
{
    static const uint64_t schema_low[MEISO_COMPOSITION_SCENE_RESOURCE_COUNT] = {
        UINT64_C(0x505245535f504c4e), /* PRES_PLN */
        UINT64_C(0x505245535f535441), /* PRES_STA */
        UINT64_C(0x5343454e5f444553), /* SCEN_DES */
        UINT64_C(0x5343454e5f535441), /* SCEN_STA */
        UINT64_C(0x4d4553485f464d54), /* MESH_FMT */
        UINT64_C(0x52454e445f524553)  /* REND_RES */
    };
    const MeisoEndpointId *presentation =
        meiso_composition_scene_presentation_endpoint();
    const MeisoEndpointId *scene = meiso_composition_scene_scene_endpoint();
    const MeisoSchemaRef *presentation_capability =
        meiso_composition_scene_presentation_capability_schema();
    const MeisoSchemaRef *scene_capability =
        meiso_composition_scene_capability_schema();
    const MeisoResourceDescriptor *resources;
    MeisoByteView capability;
    MeisoRuntimeEndpointDescriptor endpoints[2];
    MeisoRuntimeRouteDescriptor routes[6];
    MeisoRuntimeEndpointDescriptor wifi_endpoint;
    MeisoRuntimeRouteDescriptor wifi_route;
    const MeisoResourceDescriptor *wifi_resources;
    size_t count = 0U;
    size_t wifi_count = 0U;
    size_t index;
    size_t other;

    CHECK(presentation->high == UINT64_C(0x434f4d504f53455f));
    CHECK(presentation->low == UINT64_C(0x50524553454e545f));
    CHECK(scene->high == UINT64_C(0x434f4d504f53455f));
    CHECK(scene->low == UINT64_C(0x5343454e455f5f5f));
    CHECK(!meiso_endpoint_id_equal(*presentation, *scene));
    CHECK(presentation_capability->id.high ==
              UINT64_C(0x434f4d505f534348) &&
          presentation_capability->id.low ==
              UINT64_C(0x505245535f434150) &&
          presentation_capability->version == 1U);
    CHECK(scene_capability->id.high == UINT64_C(0x434f4d505f534348) &&
          scene_capability->id.low == UINT64_C(0x5343454e5f434150) &&
          scene_capability->version == 1U);

    capability = meiso_composition_scene_presentation_capability();
    CHECK(capability.size == sizeof(PRESENTATION_CAPABILITY_GOLDEN));
    CHECK(memcmp(capability.data, PRESENTATION_CAPABILITY_GOLDEN,
                 capability.size) == 0);
    capability = meiso_composition_scene_capability();
    CHECK(capability.size == sizeof(SCENE_CAPABILITY_GOLDEN));
    CHECK(memcmp(capability.data, SCENE_CAPABILITY_GOLDEN,
                 capability.size) == 0);

    resources = meiso_composition_scene_resources(&count);
    CHECK(resources != NULL && count == 6U);
    CHECK(meiso_composition_scene_resources(NULL) == resources);
    for (index = 0U; index < count; ++index) {
        CHECK(meiso_composition_scene_resource(
                  (MeisoCompositionSceneResource)index) == &resources[index]);
        CHECK(meiso_resource_descriptor_validate(&resources[index]) ==
              MEISO_DESCRIPTOR_VALID);
        CHECK(resources[index].id.high == UINT64_C(0x434f4d504f53455f));
        CHECK(resources[index].schema.id.high ==
                  UINT64_C(0x434f4d505f534348) &&
              resources[index].schema.id.low == schema_low[index] &&
              resources[index].schema.version == 1U);
        for (other = index + 1U; other < count; ++other) {
            CHECK(!meiso_resource_id_equal(resources[index].id,
                                           resources[other].id));
        }
    }
    CHECK(meiso_composition_scene_resource(6U) == NULL);
    CHECK(resources[0].kind == MEISO_RESOURCE_KIND_STATE &&
          resources[0].owner == MEISO_OWNER_ROLE_HOST &&
          resources[0].max_payload_size == 136U);
    CHECK(resources[1].kind == MEISO_RESOURCE_KIND_STATE &&
          resources[1].owner == MEISO_OWNER_ROLE_EDGE &&
          resources[1].max_payload_size == 216U);
    CHECK(resources[2].kind == MEISO_RESOURCE_KIND_STATE &&
          resources[2].owner == MEISO_OWNER_ROLE_HOST &&
          resources[2].max_payload_size == 3704U);
    CHECK(resources[3].kind == MEISO_RESOURCE_KIND_STATE &&
          resources[3].owner == MEISO_OWNER_ROLE_EDGE &&
          resources[3].max_payload_size == 168U);
    CHECK(resources[4].kind == MEISO_RESOURCE_KIND_BLOB &&
          resources[4].owner == MEISO_OWNER_ROLE_HOST &&
          resources[4].max_payload_size == 262144U);
    CHECK(resources[5].kind == MEISO_RESOURCE_KIND_STREAM &&
          resources[5].owner == MEISO_OWNER_ROLE_EDGE &&
          resources[5].max_payload_size == 6304U);

    memset(endpoints, 0xa5, sizeof(endpoints));
    CHECK(meiso_composition_scene_build_endpoints(
              11U, 12U, endpoints, 2U, &count) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(count == 2U && endpoints[0].incarnation == 11U &&
          endpoints[1].incarnation == 12U);
    CHECK(meiso_endpoint_id_equal(endpoints[0].id, *presentation));
    CHECK(meiso_endpoint_id_equal(endpoints[1].id, *scene));
    CHECK(endpoints[0].capability.size == 32U &&
          endpoints[1].capability.size == 52U);
    memset(routes, 0xa5, sizeof(routes));
    CHECK(meiso_composition_scene_build_routes(
              11U, 12U, routes, 6U, &count) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(count == 6U);
    CHECK(routes[0].facet == MEISO_RUNTIME_FACET_DESIRED);
    CHECK(routes[1].facet == MEISO_RUNTIME_FACET_ACTUAL);
    CHECK(routes[2].facet == MEISO_RUNTIME_FACET_DESIRED);
    CHECK(routes[3].facet == MEISO_RUNTIME_FACET_ACTUAL);
    CHECK(routes[4].facet == MEISO_RUNTIME_FACET_IMMUTABLE);
    CHECK(routes[5].facet == MEISO_RUNTIME_FACET_DATA);
    for (index = 0U; index < count; ++index) {
        CHECK(meiso_resource_descriptor_equal(
            &routes[index].resource, &resources[index]));
    }

    count = 99U;
    CHECK(meiso_composition_scene_build_endpoints(
              0U, 1U, endpoints, 2U, &count) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE && count == 0U);
    count = 99U;
    CHECK(meiso_composition_scene_build_routes(
              1U, 2U, routes, 5U, &count) ==
          MEISO_COMPOSITION_SCENE_CODEC_CAPACITY && count == 0U);
    CHECK(meiso_composition_scene_build_endpoints(
              1U, 2U, endpoints, 2U, NULL) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT);

    memset(&wifi_endpoint, 0, sizeof(wifi_endpoint));
    wifi_endpoint.id = *meiso_wifi_telemetry_endpoint();
    wifi_endpoint.incarnation = 21U;
    wifi_endpoint.capability_schema =
        *meiso_wifi_telemetry_capability_schema();
    wifi_resources = meiso_wifi_telemetry_resources(&wifi_count);
    CHECK(wifi_resources != NULL && wifi_count == 3U);
    memset(&wifi_route, 0, sizeof(wifi_route));
    wifi_route.endpoint_id = wifi_endpoint.id;
    wifi_route.endpoint_incarnation = wifi_endpoint.incarnation;
    wifi_route.resource = wifi_resources[0];
    wifi_route.facet = MEISO_RUNTIME_FACET_DESIRED;
    CHECK(meiso_composition_scene_catalog_identifiers_disjoint(
        &wifi_endpoint, 1U, &wifi_route, 1U));
    wifi_endpoint.id = *scene;
    CHECK(!meiso_composition_scene_catalog_identifiers_disjoint(
        &wifi_endpoint, 1U, NULL, 0U));
    wifi_endpoint.id = *meiso_wifi_telemetry_endpoint();
    wifi_route.resource.id = resources[4].id;
    CHECK(!meiso_composition_scene_catalog_identifiers_disjoint(
        NULL, 0U, &wifi_route, 1U));
    wifi_route.resource = wifi_resources[0];
    wifi_route.resource.schema = resources[5].schema;
    CHECK(!meiso_composition_scene_catalog_identifiers_disjoint(
        NULL, 0U, &wifi_route, 1U));
    CHECK(!meiso_composition_scene_catalog_identifiers_disjoint(
        NULL, 1U, NULL, 0U));
    return true;
}

static bool test_presentation_plan_codec(void)
{
    static const size_t reserved[] = {86U, 87U, 92U, 93U, 94U, 95U};
    uint8_t wire[129];
    uint8_t wire_before[129];
    uint8_t mutated[128];
    MeisoCompositionScenePresentationPlan value = example_plan();
    MeisoCompositionScenePresentationPlan decoded;
    MeisoCompositionScenePresentationPlan before;
    size_t size = 999U;
    size_t index;

    CHECK(meiso_composition_scene_presentation_plan_encoded_size() == 128U);
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(size == sizeof(PLAN_GOLDEN));
    CHECK(memcmp(wire, PLAN_GOLDEN, size) == 0 && wire[size] == 0xa5U);
    CHECK(memcmp(value.candidate_digest, PLAN_DIGEST_GOLDEN,
                 sizeof(PLAN_DIGEST_GOLDEN)) == 0);
    CHECK(PLAN_DIGEST_PREIMAGE[75] == 0U &&
          memcmp(PLAN_DIGEST_PREIMAGE + 24U,
                 "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                 "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
                 MEISO_SHA256_DIGEST_SIZE) == 0);
    CHECK(meiso_composition_scene_presentation_plan_decode(
              (MeisoByteView){PLAN_GOLDEN, sizeof(PLAN_GOLDEN)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    for (index = 0U; index < sizeof(PLAN_GOLDEN); ++index) {
        CHECK(meiso_composition_scene_presentation_plan_decode(
                  (MeisoByteView){PLAN_GOLDEN, index}, &decoded) ==
              MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED);
    }
    memcpy(wire, PLAN_GOLDEN, sizeof(PLAN_GOLDEN));
    wire[sizeof(PLAN_GOLDEN)] = 0U;
    CHECK(meiso_composition_scene_presentation_plan_decode(
              (MeisoByteView){wire, sizeof(wire)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    for (index = 0U; index < sizeof(reserved) / sizeof(reserved[0]); ++index) {
        memcpy(mutated, PLAN_GOLDEN, sizeof(mutated));
        mutated[reserved[index]] = 1U;
        CHECK(meiso_composition_scene_presentation_plan_decode(
                  (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
              MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    }
    memset(&decoded, 0xa5, sizeof(decoded));
    before = decoded;
    memcpy(mutated, PLAN_GOLDEN, sizeof(mutated));
    mutated[0] = (uint8_t)'X';
    CHECK(meiso_composition_scene_presentation_plan_decode(
              (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);

    value = example_plan();
    value.max_result_hz = 1U;
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(wire[84] == 0U && wire[85] == 1U);
    CHECK(meiso_composition_scene_presentation_plan_decode(
              (MeisoByteView){wire, size}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded.max_result_hz == 1U);
    value.max_result_hz = 0U;
    size = 999U;
    memset(wire, 0xa5, sizeof(wire));
    memcpy(wire_before, wire, sizeof(wire));
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    CHECK(size == 0U &&
          memcmp(wire, wire_before, sizeof(wire)) == 0);
    value.max_result_hz = MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ + 1U;
    size = 999U;
    memset(wire, 0xa5, sizeof(wire));
    memcpy(wire_before, wire, sizeof(wire));
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    CHECK(size == 0U &&
          memcmp(wire, wire_before, sizeof(wire)) == 0);
    value.max_result_hz = UINT16_MAX;
    size = 999U;
    memset(wire, 0xa5, sizeof(wire));
    memcpy(wire_before, wire, sizeof(wire));
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    CHECK(size == 0U &&
          memcmp(wire, wire_before, sizeof(wire)) == 0);
    memcpy(mutated, PLAN_GOLDEN, sizeof(mutated));
    mutated[84] = 0U;
    mutated[85] = 0U;
    memset(&decoded, 0xa5, sizeof(decoded));
    before = decoded;
    CHECK(meiso_composition_scene_presentation_plan_decode(
              (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);
    mutated[85] = (uint8_t)(MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ + 1U);
    CHECK(meiso_composition_scene_presentation_plan_decode(
              (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);
    mutated[84] = UINT8_MAX;
    mutated[85] = UINT8_MAX;
    CHECK(meiso_composition_scene_presentation_plan_decode(
              (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);

    value = example_plan();
    size = 999U;
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, 127U}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_CAPACITY);
    CHECK(size == 0U && wire[0] == 0xa5U);
    value.offline_lease_ms = MEISO_COMPOSITION_SCENE_MAX_OFFLINE_LEASE_MS;
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    value.offline_lease_ms++;
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_plan();
    value.prepared_token_echo_present = false;
    memset(value.prepared_token_echo, 0, sizeof(value.prepared_token_echo));
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    value.prepared_token_echo[0] = 1U;
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_plan();
    value.flags |= UINT16_C(2);
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    return true;
}

static bool test_presentation_status_codec(void)
{
    static const size_t reserved[] = {6U, 7U, 101U, 102U, 103U};
    uint8_t wire[161];
    uint8_t mutated[160];
    MeisoCompositionScenePresentationStatus value =
        example_presentation_status();
    MeisoCompositionScenePresentationStatus decoded;
    size_t size = 999U;
    size_t index;

    CHECK(meiso_composition_scene_presentation_status_encoded_size() == 160U);
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_presentation_status_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(size == sizeof(PRESENTATION_STATUS_GOLDEN));
    CHECK(memcmp(wire, PRESENTATION_STATUS_GOLDEN, size) == 0 &&
          wire[size] == 0xa5U);
    CHECK(meiso_composition_scene_presentation_status_decode(
              (MeisoByteView){PRESENTATION_STATUS_GOLDEN,
                              sizeof(PRESENTATION_STATUS_GOLDEN)},
              &decoded) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    for (index = 0U; index < sizeof(PRESENTATION_STATUS_GOLDEN); ++index) {
        CHECK(meiso_composition_scene_presentation_status_decode(
                  (MeisoByteView){PRESENTATION_STATUS_GOLDEN, index},
                  &decoded) == MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED);
    }
    memcpy(wire, PRESENTATION_STATUS_GOLDEN,
           sizeof(PRESENTATION_STATUS_GOLDEN));
    wire[sizeof(PRESENTATION_STATUS_GOLDEN)] = 0U;
    CHECK(meiso_composition_scene_presentation_status_decode(
              (MeisoByteView){wire, sizeof(wire)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    for (index = 0U; index < sizeof(reserved) / sizeof(reserved[0]); ++index) {
        memcpy(mutated, PRESENTATION_STATUS_GOLDEN, sizeof(mutated));
        mutated[reserved[index]] = 1U;
        CHECK(meiso_composition_scene_presentation_status_decode(
                  (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
              MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    }

    size = 999U;
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_presentation_status_encode(
              &value, (MeisoMutableBuffer){wire, 159U}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_CAPACITY);
    CHECK(size == 0U && wire[0] == 0xa5U);
    value.candidate_state =
        MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING;
    CHECK(meiso_composition_scene_presentation_status_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    value.prepared_token_present = false;
    memset(value.prepared_token, 0, sizeof(value.prepared_token));
    CHECK(meiso_composition_scene_presentation_status_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_presentation_status();
    value.candidate_state = MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED;
    value.reason = MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN;
    value.prepared_token_present = false;
    memset(value.prepared_token, 0, sizeof(value.prepared_token));
    CHECK(meiso_composition_scene_presentation_status_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    value.reason = MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE;
    CHECK(meiso_composition_scene_presentation_status_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    return true;
}

static bool test_mesh_codec(void)
{
    static const size_t reserved[] = {
        28U, 29U, 30U, 31U, 56U, 57U, 58U, 59U,
        60U, 61U, 62U, 63U
    };
    static const uint32_t bad_floats[] = {
        UINT32_C(0x7fc00000), UINT32_C(0x7f800000),
        UINT32_C(0xff800000), UINT32_C(0x80000000),
        UINT32_C(0x00000001)
    };
    static MeisoCompositionSceneMeshVertex max_vertices[
        TEST_MAX_MESH_VERTICES
    ];
    static uint8_t max_wire[MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE + 1U];
    static const uint32_t max_indices[3] = {0U, 0U, 0U};
    const MeisoCompositionSceneMesh *cube =
        meiso_composition_scene_canonical_cube();
    MeisoCompositionSceneMesh max_mesh;
    MeisoCompositionSceneMesh invalid;
    MeisoCompositionSceneMeshVertex invalid_vertices[8];
    MeisoCompositionSceneMeshView view;
    MeisoCompositionSceneMeshVertex vertex;
    uint8_t wire[305];
    uint8_t mutated[304];
    uint32_t vertex_index = 99U;
    size_t size = 999U;
    size_t index;
    size_t bad;

    CHECK(cube != NULL && !cube->has_vertex_color &&
          cube->vertex_count == 8U && cube->index_count == 36U);
    CHECK(meiso_composition_scene_mesh_encoded_size(
              8U, false, 36U, &size) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
          size == sizeof(CUBE_GOLDEN));
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_mesh_encode(
              cube, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(size == sizeof(CUBE_GOLDEN));
    CHECK(memcmp(wire, CUBE_GOLDEN, size) == 0 && wire[size] == 0xa5U);
    CHECK(meiso_composition_scene_mesh_decode(
              (MeisoByteView){CUBE_GOLDEN, sizeof(CUBE_GOLDEN)}, &view) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(view.vertex_count == 8U && view.index_count == 36U &&
          !view.has_vertex_color);
    CHECK(meiso_composition_scene_mesh_view_vertex(
              &view, 7U, &vertex) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(vertex.position[0] == -0.5F && vertex.position[1] == 0.5F &&
          vertex.position[2] == 0.5F);
    CHECK(meiso_composition_scene_mesh_view_index(
              &view, 35U, &vertex_index) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
          vertex_index == 5U);
    CHECK(meiso_composition_scene_mesh_view_vertex(
              &view, 8U, &vertex) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    CHECK(meiso_composition_scene_mesh_view_index(
              &view, 36U, &vertex_index) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);

    for (index = 0U; index < sizeof(CUBE_GOLDEN); ++index) {
        CHECK(meiso_composition_scene_mesh_decode(
                  (MeisoByteView){CUBE_GOLDEN, index}, &view) ==
              MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED);
    }
    memcpy(wire, CUBE_GOLDEN, sizeof(CUBE_GOLDEN));
    wire[sizeof(CUBE_GOLDEN)] = 0U;
    CHECK(meiso_composition_scene_mesh_decode(
              (MeisoByteView){wire, sizeof(wire)}, &view) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    for (index = 0U; index < sizeof(reserved) / sizeof(reserved[0]); ++index) {
        memcpy(mutated, CUBE_GOLDEN, sizeof(mutated));
        mutated[reserved[index]] = 1U;
        CHECK(meiso_composition_scene_mesh_decode(
                  (MeisoByteView){mutated, sizeof(mutated)}, &view) ==
              MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    }
    for (bad = 0U; bad < sizeof(bad_floats) / sizeof(bad_floats[0]); ++bad) {
        for (index = 32U; index < 56U; index += 4U) {
            memcpy(mutated, CUBE_GOLDEN, sizeof(mutated));
            put_u32(mutated + index, bad_floats[bad]);
            CHECK(meiso_composition_scene_mesh_decode(
                      (MeisoByteView){mutated, sizeof(mutated)}, &view) ==
                  MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
        }
        for (index = 64U; index < 160U; index += 4U) {
            memcpy(mutated, CUBE_GOLDEN, sizeof(mutated));
            put_u32(mutated + index, bad_floats[bad]);
            CHECK(meiso_composition_scene_mesh_decode(
                      (MeisoByteView){mutated, sizeof(mutated)}, &view) ==
                  MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
        }
    }
    memcpy(mutated, CUBE_GOLDEN, sizeof(mutated));
    put_u32(mutated + 160U, 8U);
    CHECK(meiso_composition_scene_mesh_decode(
              (MeisoByteView){mutated, sizeof(mutated)}, &view) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    memcpy(mutated, CUBE_GOLDEN, sizeof(mutated));
    put_u32(mutated + 32U, UINT32_C(0xbe800000));
    CHECK(meiso_composition_scene_mesh_decode(
              (MeisoByteView){mutated, sizeof(mutated)}, &view) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);

    invalid = *cube;
    memcpy(invalid_vertices, cube->vertices, sizeof(invalid_vertices));
    invalid.vertices = invalid_vertices;
    invalid_vertices[0].position[0] =
        float_from_bits(UINT32_C(0x7fc00000));
    CHECK(meiso_composition_scene_mesh_encode(
              &invalid, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    invalid_vertices[0].position[0] = -0.5F;
    invalid.indices = NULL;
    CHECK(meiso_composition_scene_mesh_encode(
              &invalid, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);

    memset(max_vertices, 0, sizeof(max_vertices));
    max_mesh.has_vertex_color = false;
    max_mesh.vertex_count = TEST_MAX_MESH_VERTICES;
    max_mesh.vertices = max_vertices;
    max_mesh.index_count = 3U;
    max_mesh.indices = max_indices;
    CHECK(meiso_composition_scene_mesh_max_encoded_size() == 262144U);
    CHECK(meiso_composition_scene_mesh_encoded_size(
              TEST_MAX_MESH_VERTICES, false, 3U, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK && size == 262144U);
    memset(max_wire, 0xa5, sizeof(max_wire));
    CHECK(meiso_composition_scene_mesh_encode(
              &max_mesh,
              (MeisoMutableBuffer){max_wire, sizeof(max_wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(size == 262144U && max_wire[size] == 0xa5U);
    CHECK(meiso_composition_scene_mesh_decode(
              (MeisoByteView){max_wire, size}, &view) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(meiso_composition_scene_mesh_encoded_size(
              TEST_MAX_MESH_VERTICES + 1U, false, 3U, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE && size == 0U);
    return true;
}

static bool test_desired_codec(void)
{
    static const size_t reserved[] = {
        12U, 13U, 14U, 15U,
        18U, 19U, 20U, 21U, 22U, 23U,
        96U, 97U, 98U, 99U, 100U, 101U, 102U, 103U,
        104U, 105U, 106U, 107U, 108U, 109U, 110U, 111U,
        131U, 166U, 167U, 216U, 217U, 218U, 219U,
        220U, 221U, 222U, 223U
    };
    static const size_t float_offsets[] = {
        48U, 52U, 56U, 60U, 64U, 68U, 72U, 76U, 80U, 84U,
        176U, 180U, 184U, 188U, 192U, 196U, 200U,
        204U, 208U, 212U
    };
    static const uint32_t bad_floats[] = {
        UINT32_C(0x7fc00000), UINT32_C(0x7f800000),
        UINT32_C(0xff800000), UINT32_C(0x80000000),
        UINT32_C(0x00000001)
    };
    static uint8_t max_wire[
        MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE + 1U
    ];
    MeisoCompositionSceneDesired value = example_desired();
    MeisoCompositionSceneDesired decoded;
    MeisoCompositionSceneDesired before;
    uint8_t wire[225];
    uint8_t mutated[224];
    size_t size = 999U;
    size_t index;
    size_t bad;

    CHECK(meiso_composition_scene_desired_encoded_size(1U, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK && size == 224U);
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(size == sizeof(DESIRED_GOLDEN));
    CHECK(memcmp(wire, DESIRED_GOLDEN, size) == 0 && wire[size] == 0xa5U);
    CHECK(meiso_composition_scene_desired_decode(
              (MeisoByteView){DESIRED_GOLDEN, sizeof(DESIRED_GOLDEN)},
              &decoded) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded.object_count == 1U &&
          decoded.objects[0].id.high == value.objects[0].id.high &&
          decoded.camera.space_epoch == value.camera.space_epoch);
    for (index = MEISO_SHA256_DIGEST_SIZE;
         index < MEISO_CONTENT_DIGEST_MAX_SIZE; ++index) {
        CHECK(decoded.objects[0].mesh_content_id.digest[index] == 0U);
    }
    for (index = 0U; index < sizeof(DESIRED_GOLDEN); ++index) {
        CHECK(meiso_composition_scene_desired_decode(
                  (MeisoByteView){DESIRED_GOLDEN, index}, &decoded) ==
              MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED);
    }
    memcpy(wire, DESIRED_GOLDEN, sizeof(DESIRED_GOLDEN));
    wire[sizeof(DESIRED_GOLDEN)] = 0U;
    CHECK(meiso_composition_scene_desired_decode(
              (MeisoByteView){wire, sizeof(wire)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    for (index = 0U; index < sizeof(reserved) / sizeof(reserved[0]); ++index) {
        memcpy(mutated, DESIRED_GOLDEN, sizeof(mutated));
        mutated[reserved[index]] = 1U;
        CHECK(meiso_composition_scene_desired_decode(
                  (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
              MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    }
    for (bad = 0U; bad < sizeof(bad_floats) / sizeof(bad_floats[0]); ++bad) {
        for (index = 0U;
             index < sizeof(float_offsets) / sizeof(float_offsets[0]);
             ++index) {
            memcpy(mutated, DESIRED_GOLDEN, sizeof(mutated));
            put_u32(mutated + float_offsets[index], bad_floats[bad]);
            CHECK(meiso_composition_scene_desired_decode(
                      (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
                  MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
        }
    }

    memset(&decoded, 0xa5, sizeof(decoded));
    before = decoded;
    memcpy(mutated, DESIRED_GOLDEN, sizeof(mutated));
    mutated[0] = (uint8_t)'X';
    CHECK(meiso_composition_scene_desired_decode(
              (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);

    size = 999U;
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, 223U}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_CAPACITY);
    CHECK(size == 0U && wire[0] == 0xa5U);
    for (bad = 0U; bad < sizeof(bad_floats) / sizeof(bad_floats[0]); ++bad) {
        value = example_desired();
        value.camera.position[0] = float_from_bits(bad_floats[bad]);
        CHECK(meiso_composition_scene_desired_encode(
                  &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
              MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    }

    value = example_desired();
    value.camera.orientation_xyzw[0] =
        float_from_bits(UINT32_C(0x3c800000));
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    value.camera.orientation_xyzw[0] =
        float_from_bits(UINT32_C(0x3c800001));
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_desired();
    value.camera.orientation_xyzw[2] = 1.0F;
    value.camera.orientation_xyzw[3] = 0.0F;
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    value.camera.orientation_xyzw[2] = -1.0F;
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);

    value = example_desired();
    value.camera.vertical_fov_degrees = 1.0F;
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    value.camera.vertical_fov_degrees = 179.0F;
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    value.camera.vertical_fov_degrees = 180.0F;
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_desired();
    value.camera.near_z = value.camera.far_z;
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_desired();
    value.objects[0].space_epoch++;
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_desired();
    value.objects[0].transform.scale[0] = 0.0F;
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);

    value = example_desired();
    memset(value.objects[0].mesh_content_id.digest + MEISO_SHA256_DIGEST_SIZE,
           0xa5, MEISO_CONTENT_DIGEST_MAX_SIZE - MEISO_SHA256_DIGEST_SIZE);
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(memcmp(wire, DESIRED_GOLDEN, sizeof(DESIRED_GOLDEN)) == 0);
    value.objects[0].mesh_content_id.algorithm = 2U;
    CHECK(meiso_composition_scene_desired_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);

    value = example_desired();
    for (index = 0U; index < MEISO_COMPOSITION_SCENE_MAX_OBJECTS; ++index) {
        value.objects[index] = value.objects[0];
        value.objects[index].id.high = 0U;
        value.objects[index].id.low = index + 1U;
        value.objects[index].mesh_content_id =
            content_id((uint8_t)(1U + index % 8U));
    }
    value.object_count = MEISO_COMPOSITION_SCENE_MAX_OBJECTS;
    CHECK(meiso_composition_scene_desired_max_encoded_size() == 3696U);
    memset(max_wire, 0xa5, sizeof(max_wire));
    CHECK(meiso_composition_scene_desired_encode(
              &value,
              (MeisoMutableBuffer){max_wire, sizeof(max_wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(size == 3696U && max_wire[size] == 0xa5U);
    CHECK(meiso_composition_scene_desired_decode(
              (MeisoByteView){max_wire, size}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK && decoded.object_count == 32U);
    value.objects[31].id.low = value.objects[0].id.low;
    CHECK(meiso_composition_scene_desired_encode(
              &value,
              (MeisoMutableBuffer){max_wire, sizeof(max_wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_desired();
    for (index = 0U; index < 9U; ++index) {
        value.objects[index] = value.objects[0];
        value.objects[index].id.low += index;
        value.objects[index].mesh_content_id = content_id((uint8_t)index);
    }
    value.object_count = 9U;
    CHECK(meiso_composition_scene_desired_encode(
              &value,
              (MeisoMutableBuffer){max_wire, sizeof(max_wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    CHECK(meiso_composition_scene_desired_encoded_size(33U, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE && size == 0U);
    return true;
}

static bool test_scene_status_codec(void)
{
    uint8_t wire[113];
    uint8_t mutated[112];
    MeisoCompositionSceneStatus value = example_scene_status();
    MeisoCompositionSceneStatus decoded;
    MeisoCompositionSceneStatus max_value;
    size_t size = 999U;
    size_t index;

    CHECK(meiso_composition_scene_status_encoded_size() == 112U);
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_status_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(size == sizeof(SCENE_STATUS_GOLDEN));
    CHECK(memcmp(wire, SCENE_STATUS_GOLDEN, size) == 0 &&
          wire[size] == 0xa5U);
    CHECK(meiso_composition_scene_status_decode(
              (MeisoByteView){SCENE_STATUS_GOLDEN,
                              sizeof(SCENE_STATUS_GOLDEN)},
              &decoded) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    for (index = 0U; index < sizeof(SCENE_STATUS_GOLDEN); ++index) {
        CHECK(meiso_composition_scene_status_decode(
                  (MeisoByteView){SCENE_STATUS_GOLDEN, index}, &decoded) ==
              MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED);
    }
    memcpy(wire, SCENE_STATUS_GOLDEN, sizeof(SCENE_STATUS_GOLDEN));
    wire[sizeof(SCENE_STATUS_GOLDEN)] = 0U;
    CHECK(meiso_composition_scene_status_decode(
              (MeisoByteView){wire, sizeof(wire)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    for (index = 88U; index < 112U; ++index) {
        memcpy(mutated, SCENE_STATUS_GOLDEN, sizeof(mutated));
        mutated[index] = 1U;
        CHECK(meiso_composition_scene_status_decode(
                  (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
              MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    }
    size = 999U;
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_status_encode(
              &value, (MeisoMutableBuffer){wire, 111U}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_CAPACITY);
    CHECK(size == 0U && wire[0] == 0xa5U);

    memset(&max_value, 0, sizeof(max_value));
    max_value.reason = MEISO_COMPOSITION_SCENE_REASON_RENDERER_FAILED;
    max_value.active_plan_generation = 1U;
    max_value.world_generation = 1U;
    max_value.active_object_count = 32U;
    max_value.unique_mesh_count = 8U;
    max_value.published_blob_count = 16U;
    max_value.published_blob_bytes = UINT64_C(16) * 262144U;
    max_value.resident_mesh_bytes = UINT64_C(4) * 1024U * 1024U;
    max_value.target_slots_used = 2U;
    max_value.target_bytes = UINT64_C(2) * 320U * 240U * 4U * 2U;
    max_value.render_jobs_in_flight = 2U;
    max_value.result_queue_depth = 4U;
    max_value.last_result_stream_epoch = 1U;
    max_value.last_result_sequence = 1U;
    CHECK(meiso_composition_scene_status_encode(
              &max_value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    max_value.active_object_count++;
    CHECK(meiso_composition_scene_status_encode(
              &max_value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    max_value.active_object_count = 32U;
    max_value.published_blob_bytes++;
    CHECK(meiso_composition_scene_status_encode(
              &max_value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    max_value.published_blob_bytes--;
    max_value.target_bytes++;
    CHECK(meiso_composition_scene_status_encode(
              &max_value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_scene_status();
    value.last_result_sequence = 0U;
    CHECK(meiso_composition_scene_status_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_scene_status();
    value.unique_mesh_count = value.active_object_count + 1U;
    CHECK(meiso_composition_scene_status_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    return true;
}

static bool test_render_result_codec(void)
{
    uint8_t wire[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE + 1U];
    uint8_t mutated[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE];
    MeisoCompositionSceneRenderResult value = example_render_result();
    MeisoCompositionSceneRenderResult decoded;
    MeisoCompositionSceneRenderResult before;
    size_t size = 999U;
    size_t index;

    CHECK(meiso_composition_scene_render_result_encoded_size() == 6304U);
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_render_result_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(size == sizeof(RENDER_RESULT_GOLDEN));
    CHECK(memcmp(wire, RENDER_RESULT_GOLDEN, size) == 0 &&
          wire[size] == 0xa5U);
    CHECK(meiso_composition_scene_render_result_decode(
              (MeisoByteView){RENDER_RESULT_GOLDEN,
                              sizeof(RENDER_RESULT_GOLDEN)},
              &decoded) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded.thumbnail_byte_size == 6144U &&
          decoded.logical_color_row_bytes == 1280U &&
          decoded.logical_depth_row_bytes == 1280U);
    for (index = 0U; index < sizeof(RENDER_RESULT_GOLDEN); ++index) {
        CHECK(meiso_composition_scene_render_result_decode(
                  (MeisoByteView){RENDER_RESULT_GOLDEN, index}, &decoded) ==
              MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED);
    }
    memcpy(wire, RENDER_RESULT_GOLDEN, sizeof(RENDER_RESULT_GOLDEN));
    wire[sizeof(RENDER_RESULT_GOLDEN)] = 0U;
    CHECK(meiso_composition_scene_render_result_decode(
              (MeisoByteView){wire, sizeof(wire)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    for (index = 126U; index < 128U; ++index) {
        memcpy(mutated, RENDER_RESULT_GOLDEN, sizeof(mutated));
        mutated[index] = 1U;
        CHECK(meiso_composition_scene_render_result_decode(
                  (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
              MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    }
    for (index = 136U; index < 160U; ++index) {
        memcpy(mutated, RENDER_RESULT_GOLDEN, sizeof(mutated));
        mutated[index] = 1U;
        CHECK(meiso_composition_scene_render_result_decode(
                  (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
              MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    }
    memset(&decoded, 0xa5, sizeof(decoded));
    before = decoded;
    memcpy(mutated, RENDER_RESULT_GOLDEN, sizeof(mutated));
    mutated[0] = (uint8_t)'X';
    CHECK(meiso_composition_scene_render_result_decode(
              (MeisoByteView){mutated, sizeof(mutated)}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_MALFORMED);
    CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);

    size = 999U;
    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_composition_scene_render_result_encode(
              &value, (MeisoMutableBuffer){wire, 6303U}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_CAPACITY);
    CHECK(size == 0U && wire[0] == 0xa5U);
    value.object_count = 32U;
    value.unique_mesh_count = 8U;
    value.thumbnail_rgb565[0] = 0x12U;
    value.thumbnail_rgb565[6143] = 0x34U;
    CHECK(meiso_composition_scene_render_result_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(wire[160] == 0x12U && wire[6303] == 0x34U);
    CHECK(meiso_composition_scene_render_result_decode(
              (MeisoByteView){wire, size}, &decoded) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded.thumbnail_rgb565[0] == 0x12U &&
          decoded.thumbnail_rgb565[6143] == 0x34U);

    value = example_render_result();
    value.unique_mesh_count = value.object_count + 1U;
    CHECK(meiso_composition_scene_render_result_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_render_result();
    value.render_end_monotonic_ns = value.render_start_monotonic_ns - 1U;
    CHECK(meiso_composition_scene_render_result_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_render_result();
    value.evidence_flags |=
        MEISO_COMPOSITION_SCENE_EVIDENCE_DISPLAY_COMMIT_ACCEPTED;
    CHECK(meiso_composition_scene_render_result_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value.evidence_flags |= MEISO_COMPOSITION_SCENE_EVIDENCE_FENCE_COMPLETE;
    CHECK(meiso_composition_scene_render_result_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_OK);
    value = example_render_result();
    value.thumbnail_byte_size--;
    CHECK(meiso_composition_scene_render_result_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    value = example_render_result();
    memset(value.full_target_sha256, 0, sizeof(value.full_target_sha256));
    CHECK(meiso_composition_scene_render_result_encode(
              &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE);
    return true;
}

static bool test_invalid_arguments(void)
{
    uint8_t wire[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE];
    MeisoCompositionSceneDesired desired = example_desired();
    MeisoCompositionSceneRenderResult result = example_render_result();
    MeisoCompositionSceneMeshView view;
    size_t size = 99U;

    CHECK(meiso_composition_scene_desired_encode(
              NULL, (MeisoMutableBuffer){wire, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT && size == 0U);
    size = 99U;
    CHECK(meiso_composition_scene_desired_encode(
              &desired, (MeisoMutableBuffer){NULL, sizeof(wire)}, &size) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT && size == 0U);
    CHECK(meiso_composition_scene_desired_encode(
              &desired, (MeisoMutableBuffer){wire, sizeof(wire)}, NULL) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT);
    CHECK(meiso_composition_scene_desired_decode(
              (MeisoByteView){NULL, 0U}, &desired) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT);
    CHECK(meiso_composition_scene_desired_decode(
              (MeisoByteView){wire, sizeof(DESIRED_GOLDEN)}, NULL) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT);
    CHECK(meiso_composition_scene_mesh_decode(
              (MeisoByteView){NULL, 0U}, &view) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT);
    CHECK(meiso_composition_scene_render_result_encode(
              &result, (MeisoMutableBuffer){wire, sizeof(wire)}, NULL) ==
          MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT);
    return true;
}

int main(void)
{
    if (!test_profile_contract() || !test_presentation_plan_codec() ||
        !test_presentation_status_codec() || !test_mesh_codec() ||
        !test_desired_codec() || !test_scene_status_codec() ||
        !test_render_result_codec() || !test_invalid_arguments()) {
        return 1;
    }
    (void)puts("composition scene profile tests passed");
    return 0;
}
