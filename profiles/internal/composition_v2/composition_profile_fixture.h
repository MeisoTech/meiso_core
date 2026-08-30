#ifndef MEISO_COMPOSITION_PROFILE_FIXTURE_H
#define MEISO_COMPOSITION_PROFILE_FIXTURE_H

#include <meiso/core/session.h>

#include <stdint.h>

/*
 * Build-only identities for exercising superseded MRT0 wire versions. They
 * are deliberately unrelated to the installed Composition Profile identity.
 */
static inline MeisoProfileRef
meiso_composition_internal_mrt0_v2_fixture_profile(void)
{
    const MeisoProfileRef profile = {
        {UINT64_C(0x4d5254305f545354), UINT64_C(0x5f50524f46494c45)},
        1U
    };

    return profile;
}

static inline MeisoProfileRef
meiso_composition_internal_mrt0_v3_fixture_profile(void)
{
    const MeisoProfileRef profile = {
        {UINT64_C(0x4d5254305f545354), UINT64_C(0x5f50524f46494c45)},
        2U
    };

    return profile;
}

#endif
