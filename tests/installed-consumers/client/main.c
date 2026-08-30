#include <meiso/host/composition.h>
#include <meiso/profiles/composition/profile.h>

#include <stdint.h>

int main(void)
{
    const MeisoProfileRef *profile = meiso_composition_profile_ref();

    return profile != NULL &&
        profile->id.high == UINT64_C(0x434f4d504f53455f) &&
        profile->id.low == UINT64_C(0x5f50524f46494c45) &&
        profile->version == 1U &&
        MEISO_COMPOSITION_PROFILE_RUNTIME_WIRE_VERSION == 4U &&
        MEISO_HOST_EDGE_API_VERSION == 1U &&
        MEISO_COMPOSITION_EDGE_DESIRED_COUNT == 6U ? 0 : 1;
}
