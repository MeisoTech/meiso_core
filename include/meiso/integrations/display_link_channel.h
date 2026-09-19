#ifndef MEISO_INTEGRATIONS_DISPLAY_LINK_CHANNEL_H
#define MEISO_INTEGRATIONS_DISPLAY_LINK_CHANNEL_H
#include <meiso/profiles/display_ingress/link.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MeisoDisplayLinkChannel MeisoDisplayLinkChannel;
/* Serialized with Runtime I/O by caller. No socket, thread or platform clock.
 * Open only after a validated application-v11 Catalog containing LINK_END.
 * desired is required on Host and ignored on Edge. */
MeisoDisplayLinkChannel *meiso_display_link_channel_open(MeisoSession *, MeisoRuntime *, const MeisoDisplayLinkDesired *desired);
/* 0 unrelated, 1 consumed, -1 invalid/failed. State ACK alone never establishes
 * peer liveness. A repeated State neither renews leases nor changes effective. */
int meiso_display_link_channel_consume(MeisoDisplayLinkChannel *, const MeisoRuntimeEvent *);
/* now_ms is caller's monotonic clock. Host miss limit invokes transport_lost;
 * Edge never ends the Session because LINK_DES is absent. */
bool meiso_display_link_channel_poll(MeisoDisplayLinkChannel *, uint64_t now_ms);
/* Version advances on accepted new LINK_DES (Edge) or observed LINK_ACT (Host).
 * Edge caller compares it with the last serviced version to renew active leases
 * exactly once, using the acceptance clock, after processing due expiries. */
uint64_t meiso_display_link_channel_effective(const MeisoDisplayLinkChannel *, MeisoDisplayLinkDesired *);
void meiso_display_link_channel_close(MeisoDisplayLinkChannel *);
#ifdef __cplusplus
}
#endif
#endif
