#include <meiso/host/display_ingress_fixture_input.h>
#include <meiso/profiles/display_ingress/local_pose.h>
#include <meiso/profiles/display_ingress/views.h>

#include <type_traits>
static_assert(std::is_standard_layout<MeisoDisplayLocalPose>::value);
static_assert(std::is_standard_layout<MeisoDisplayViewsCapability>::value);

static_assert(
    std::is_standard_layout<MeisoDisplayIngressHostFixtureInput>::value,
    "DIFI typed input must remain consumable from C++17"
);

extern "C" int meiso_difi_cpp17_sha256(
    const uint8_t *payload,
    size_t payload_size,
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    if (MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_VERSION != 1U) {
        return 1;
    }
    return meiso_display_ingress_host_fixture_input_sha256(
        MeisoByteView{payload, payload_size}, digest
    ) ? 0 : 1;
}
