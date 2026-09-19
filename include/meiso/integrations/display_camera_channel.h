#ifndef MEISO_INTEGRATIONS_DISPLAY_CAMERA_CHANNEL_H
#define MEISO_INTEGRATIONS_DISPLAY_CAMERA_CHANNEL_H
#include <meiso/adapters/posix/composition_media_tcp.h>
#include <meiso/profiles/display_ingress/camera.h>
#include <meiso/profiles/display_ingress/link.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MeisoDisplayCameraChannel MeisoDisplayCameraChannel;
/* POSIX assembly for v8/v9, once the combined Catalog is active. The caller
 * serializes calls with Runtime/control I/O, and owns all descriptors.
 * No blocking I/O, thread, camera driver or control socket is owned here. */
typedef struct {
    MeisoSession *session;
    MeisoRuntime *runtime;
    int media_fd;
    MeisoCompositionMediaDesired desired; /* Host request; Edge checks capability. */
    MeisoSessionNonce edge_nonce; /* Edge-generated random nonce; ignored on Host. */
    uint32_t application_version; /* 11 selects lease terminal suffix; zero retains legacy. */
} MeisoDisplayCameraChannelConfig;
MeisoDisplayCameraChannel *meiso_display_camera_channel_open(const MeisoDisplayCameraChannelConfig *);
/* 0 unrelated, 1 consumed, -1 invalid/terminal. */
int meiso_display_camera_channel_consume(MeisoDisplayCameraChannel *, const MeisoRuntimeEvent *);
bool meiso_display_camera_channel_poll(MeisoDisplayCameraChannel *);
bool meiso_display_camera_channel_ready(const MeisoDisplayCameraChannel *);
/* v11 Edge expiry: revoke media now; poll publishes STOPPED after disposal.
 * Caller stops its physical capture as part of the same serialized operation.
 * The channel retains its control identity and never closes caller's fd. */
bool meiso_display_camera_channel_stop(MeisoDisplayCameraChannel *, uint32_t terminal_reason);
bool meiso_display_camera_channel_stopped(const MeisoDisplayCameraChannel *);
/* EOF revokes camera data immediately. It is not proof of normal Session close;
 * callers may await that independent control result within a bounded grace. */
bool meiso_display_camera_channel_peer_eof(const MeisoDisplayCameraChannel *);
bool meiso_display_camera_channel_submit(MeisoDisplayCameraChannel *, uint64_t sequence,
    MeisoTimePoint source_time, MeisoByteView pixels);
bool meiso_display_camera_channel_latest(MeisoDisplayCameraChannel *, MeisoCompositionMediaLatestView *);
bool meiso_display_camera_channel_release(MeisoDisplayCameraChannel *, const MeisoCompositionMediaFrameRef *);
const MeisoCompositionMediaDesired *meiso_display_camera_channel_effective(const MeisoDisplayCameraChannel *);
void meiso_display_camera_channel_close(MeisoDisplayCameraChannel *);
#ifdef __cplusplus
}
#endif
#endif
