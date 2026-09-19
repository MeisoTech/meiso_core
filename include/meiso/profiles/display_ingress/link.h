#ifndef MEISO_DISPLAY_INGRESS_LINK_H
#define MEISO_DISPLAY_INGRESS_LINK_H
#include <meiso/profiles/display_ingress/profile.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Application v11 only. No Core wire or carrier assumptions. */
enum {
    MEISO_DISPLAY_LINK_CAPABILITY_BYTES = 40,
    MEISO_DISPLAY_LINK_DESIRED_BYTES = 32,
    MEISO_DISPLAY_LINK_TERMINAL_BYTES = 8,
    MEISO_DISPLAY_LINK_SCENE_STATUS_BYTES = 16,
    MEISO_DISPLAY_LINK_LEASE_VIDEO = 0,
    MEISO_DISPLAY_LINK_LEASE_SCENE = 1,
    MEISO_DISPLAY_LINK_LEASE_CAMERA = 2,
    MEISO_DISPLAY_LINK_LEASE_COUNT = 3,
    MEISO_DISPLAY_LINK_TERMINAL_NONE = 0,
    MEISO_DISPLAY_LINK_LEASE_EXPIRED = 1
};
typedef struct {
    uint32_t tier_mask;
    uint32_t downlink_kbit_s[3], uplink_kbit_s[3];
    uint32_t decode_max_pixels, render_max_pixels;
} MeisoDisplayLinkCapability;
typedef struct {
    uint32_t tier, downlink_kbit_s, uplink_kbit_s;
    uint32_t keepalive_interval_ms, keepalive_miss_limit;
    uint32_t lease_ms[MEISO_DISPLAY_LINK_LEASE_COUNT];
} MeisoDisplayLinkDesired;
bool meiso_display_link_capability_encode(const MeisoDisplayLinkCapability *, MeisoMutableBuffer);
bool meiso_display_link_capability_decode(MeisoByteView, MeisoDisplayLinkCapability *);
bool meiso_display_link_desired_encode(const MeisoDisplayLinkDesired *, MeisoMutableBuffer);
bool meiso_display_link_desired_decode(MeisoByteView, MeisoDisplayLinkDesired *);
bool meiso_display_link_desired_supported(const MeisoDisplayLinkCapability *, const MeisoDisplayLinkDesired *);
bool meiso_display_link_terminal_encode(uint32_t reason, MeisoMutableBuffer);
bool meiso_display_link_terminal_decode(MeisoByteView, uint32_t *reason);
const MeisoEndpointId *meiso_display_link_endpoint_id(void);
MeisoRuntimeEndpointDescriptor meiso_display_link_endpoint(uint64_t incarnation);
/* index=0 Desired, index=1 Actual. Other values return an empty descriptor. */
MeisoRuntimeV4RouteDescriptor meiso_display_link_route(unsigned index, uint64_t incarnation);

/* Monotonic milliseconds supplied by the caller; pure bounded bookkeeping.
 * The caller commits validated State before renewing. Mark inactive only after
 * cleanup; expiry itself is neither presentation evidence nor a STOPPED proof. */
typedef struct {
    uint32_t lease_ms;
    uint64_t renewed_ms, desired_version;
    bool active;
} MeisoDisplayResidenceLease;
bool meiso_display_residence_accept(MeisoDisplayResidenceLease *, uint64_t version, uint64_t now_ms);
void meiso_display_residence_renew(MeisoDisplayResidenceLease *, uint32_t lease_ms, uint64_t now_ms);
bool meiso_display_residence_expired(const MeisoDisplayResidenceLease *, uint64_t now_ms);
typedef struct {
    uint32_t interval_ms, miss_limit;
    uint64_t version, outstanding, sent_ms;
} MeisoDisplayLinkHeartbeat;
bool meiso_display_link_heartbeat_due(const MeisoDisplayLinkHeartbeat *, uint64_t now_ms);
bool meiso_display_link_heartbeat_sent(MeisoDisplayLinkHeartbeat *, uint64_t version, uint64_t now_ms);
bool meiso_display_link_heartbeat_observe(MeisoDisplayLinkHeartbeat *, uint64_t version);
bool meiso_display_link_heartbeat_lost(const MeisoDisplayLinkHeartbeat *, uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
