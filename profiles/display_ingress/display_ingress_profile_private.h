#ifndef MEISO_DISPLAY_INGRESS_PROFILE_PRIVATE_H
#define MEISO_DISPLAY_INGRESS_PROFILE_PRIVATE_H

#include <meiso/profiles/display_ingress/profile.h>

#if defined(__clang__) || defined(__GNUC__)
#define MEISO_DISPLAY_INGRESS_PRIVATE_API \
    __attribute__((visibility("hidden")))
#else
#define MEISO_DISPLAY_INGRESS_PRIVATE_API
#endif

MEISO_DISPLAY_INGRESS_PRIVATE_API MeisoRuntimeEndpointDescriptor
meiso_display_overlay_private_endpoint(uint64_t incarnation);
MEISO_DISPLAY_INGRESS_PRIVATE_API MeisoRuntimeV4RouteDescriptor
meiso_display_overlay_private_route(unsigned index,uint64_t incarnation);

MEISO_DISPLAY_INGRESS_PRIVATE_API bool
meiso_display_ingress_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);
MEISO_DISPLAY_INGRESS_PRIVATE_API bool
meiso_display_ingress_private_bytes_zero(
    const uint8_t *bytes,
    size_t size
);
MEISO_DISPLAY_INGRESS_PRIVATE_API void
meiso_display_ingress_private_put_u32(uint8_t *out, uint32_t value);
MEISO_DISPLAY_INGRESS_PRIVATE_API void
meiso_display_ingress_private_put_u64(uint8_t *out, uint64_t value);
MEISO_DISPLAY_INGRESS_PRIVATE_API uint32_t
meiso_display_ingress_private_get_u32(const uint8_t *in);
MEISO_DISPLAY_INGRESS_PRIVATE_API uint64_t
meiso_display_ingress_private_get_u64(const uint8_t *in);
MEISO_DISPLAY_INGRESS_PRIVATE_API bool
meiso_display_ingress_private_capability_valid(
    const MeisoDisplayIngressCapability *value
);
MEISO_DISPLAY_INGRESS_PRIVATE_API bool
meiso_display_ingress_private_desired_valid(
    const MeisoDisplayIngressDesired *value
);
MEISO_DISPLAY_INGRESS_PRIVATE_API bool
meiso_display_ingress_private_actual_valid(
    const MeisoDisplayIngressActual *value
);
MEISO_DISPLAY_INGRESS_PRIVATE_API bool
meiso_display_ingress_private_evidence_valid(
    const MeisoDisplayIngressPresentationEvidence *value
);

/* Build-private shared digest; not part of the installed Profile API. */
bool meiso_display_ingress_private_payload_sha256(
    MeisoByteView payload,
    uint8_t out_digest[MEISO_SHA256_DIGEST_SIZE]
);

#endif
