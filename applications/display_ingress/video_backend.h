#ifndef MEISO_DISPLAY_INGRESS_VIDEO_BACKEND_H
#define MEISO_DISPLAY_INGRESS_VIDEO_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <meiso/profiles/display_ingress/scene.h>

/* Platform adapter boundary. The service owns admission, transport and evidence policy.
 * K230D uses MVX/DRM; desktop validation uses software decoding/offscreen output. */
typedef struct MeisoIngressVideo MeisoIngressVideo;

typedef enum MeisoIngressVideoSubmitResult {
    MEISO_INGRESS_VIDEO_ACCEPTED = 0,
    MEISO_INGRESS_VIDEO_WOULD_BLOCK = 1,
    MEISO_INGRESS_VIDEO_SUBMIT_ERROR = 2
} MeisoIngressVideoSubmitResult;

typedef enum MeisoIngressVideoPollResult {
    MEISO_INGRESS_VIDEO_NONE = 0,
    MEISO_INGRESS_VIDEO_PRESENTED = 1,
    MEISO_INGRESS_VIDEO_POLL_ERROR = 2
} MeisoIngressVideoPollResult;

typedef enum MeisoIngressVideoConnectorKind {
    MEISO_INGRESS_VIDEO_CONNECTOR_UNKNOWN = 0,
    MEISO_INGRESS_VIDEO_CONNECTOR_DSI = 1,
    MEISO_INGRESS_VIDEO_CONNECTOR_HDMI = 2
} MeisoIngressVideoConnectorKind;

typedef struct MeisoIngressVideoPresentation {
    uint64_t sequence;
    /* Legacy field name: adapter completion on its monotonic clock. The
     * selected evidence class, not this timestamp, establishes physical scope. */
    uint64_t page_flip_monotonic_ns;
    uint32_t output_width;
    uint32_t output_height;
    MeisoIngressVideoConnectorKind connector_kind;
} MeisoIngressVideoPresentation;

/* Opens one bounded decoder/output adapter. Paths select platform resources:
 * K230D uses MVX/DRM devices; desktop uses a new output directory and ignores
 * video_path. Platform-specific allocations belong to each implementation. */
MeisoIngressVideo *meiso_ingress_video_open(const char *video_path,
                                            const char *output_path,
                                            uint32_t width,
                                            uint32_t height);

/* Desktop-only output canvas configuration before any submission; the hardware
 * backend reports its actual connector mode through output_size instead. */
#ifdef MEISO_DISPLAY_INGRESS_DESKTOP_VIDEO
int meiso_ingress_video_set_output_size(MeisoIngressVideo *, uint32_t width, uint32_t height);
#endif

/* Select two centered native-size views of one left/right atlas. Call once
 * before submitting any input. Requires the matching Canaan crop/stride kernel,
 * two empty nonrotating NV12 planes and input width divisible by 16. */
int meiso_ingress_video_set_stereo(MeisoIngressVideo *video);

/* AU must be one <=128 KiB H.264 Baseline IDR access unit with SPS/PPS. */
MeisoIngressVideoSubmitResult meiso_ingress_video_submit(MeisoIngressVideo *video,
                                                          uint64_t sequence,
                                                          const uint8_t *au,
                                                          size_t au_size);

/* Performs one bounded completion poll (K230D: at most ~33 ms DRM event wait;
 * desktop: software raster completion). No unbounded frame queue. */
MeisoIngressVideoPollResult meiso_ingress_video_poll(MeisoIngressVideo *video,
                                                      MeisoIngressVideoPresentation *presentation);

/* Stop video admission until explicit reactivation and remove only
 * its video scanout. Call until terminal, while continuing video/overlay polls
 * to retire already submitted flips truthfully. No synthetic frame receipt is
 * produced and native scene remains usable. Legacy callers use a new adapter;
 * v11 calls reactivate only after real deactivation and a newer enabled Desired.
 * One call does bounded work; PENDING is not proof of removal. ERROR preserves
 * unknown scanout ownership until close. Decoder buffers remain owned until
 * close; CLEARED proves output removal, not release of all video resources.
 * This private primitive does not select a wire contract or stale deadline. */
typedef enum MeisoIngressVideoClearResult {
    MEISO_INGRESS_VIDEO_CLEAR_PENDING = 0,
    MEISO_INGRESS_VIDEO_CLEARED = 1,
    MEISO_INGRESS_VIDEO_CLEAR_ERROR = 2
} MeisoIngressVideoClearResult;
MeisoIngressVideoClearResult meiso_ingress_video_clear(MeisoIngressVideo *video);

/* Explicit new production epoch after CLEARED. Flush old decoder work before
 * admitting fresh IDRs; keep the independent scene/CRTC and its reserved buffers.
 * Never called automatically on a timeout. Failure leaves video admission shut. */
int meiso_ingress_video_reactivate(MeisoIngressVideo *video);

int meiso_ingress_video_output_size(const MeisoIngressVideo *video, uint32_t *width, uint32_t *height);

/* Private Edge-rendered screen region, packed native 0xAARRGGBB. This does not
 * add a Host resource or advertise an overlay in Display Ingress v2.
 * At most 256 KiB of pixels and two 256 KiB DMA surfaces, allocated lazily.
 * width=height=0, x=y=bytes=0 and pixels=NULL disables an active region;
 * disabling an already hidden region is rejected (there is no new page flip).
 * Coordinates use output pixels; sequence must increase after each admission.
 * Source pixels are copied during submit; one pending flip, no pixel FIFO. */
MeisoIngressVideoSubmitResult meiso_ingress_video_overlay_submit(
    MeisoIngressVideo *video, uint64_t sequence, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, const uint32_t *pixels, size_t bytes);

/* Select/reserve a native scene before any input/overlay submission. Publishes
 * the adapter's actual bounded output capability on success. Mutually exclusive
 * with the legacy pixel-submit API. Scene pixels are generated directly in the
 * idle platform buffer; caller's plan is not retained. Empty updates remove the
 * scene, including when already empty. K230D detaches its plane and requires a
 * CRTC completion event plus inactive-plane readback; desktop reports software
 * completion only. Reserved buffers remain available for subsequent updates. */
int meiso_ingress_video_scene_prepare(MeisoIngressVideo *,uint32_t available_fonts,MeisoDisplaySceneCapability *);
MeisoIngressVideoSubmitResult meiso_ingress_video_scene_submit(
    MeisoIngressVideo *,uint64_t sequence,const MeisoDisplayScenePlan *);

/* Nonblocking observation of the separate overlay page flip. Keep calling the
 * regular video poll too; it serializes both planes on their shared CRTC.
 * sequence=the exact overlay update, not the decoded video frame identity. */
MeisoIngressVideoPollResult meiso_ingress_video_overlay_poll(
    MeisoIngressVideo *video, MeisoIngressVideoPresentation *presentation);

/* Releases adapter resources and performs its requested output cleanup.
 * Returns 0 only when all requested recovery operations succeeded. */
int meiso_ingress_video_close(MeisoIngressVideo *video);

#endif
