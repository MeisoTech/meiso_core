#ifndef MEISO_K230D_DISPLAY_INGRESS_VIDEO_BACKEND_H
#define MEISO_K230D_DISPLAY_INGRESS_VIDEO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* Private K230D Linux adapter.  The owning service decides admission and drop policy. */
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
    uint64_t page_flip_monotonic_ns;
    uint32_t output_width;
    uint32_t output_height;
    MeisoIngressVideoConnectorKind connector_kind;
} MeisoIngressVideoPresentation;

/* Opens one decoder/empty NV12 plane, two input DMA buffers and provisional capture.
 * Full-size NV12 surfaces are allocated after the first source-change event. */
MeisoIngressVideo *meiso_ingress_video_open(const char *video_path,
                                            const char *drm_path,
                                            uint32_t width,
                                            uint32_t height);

/* Select two centered native-size views of one left/right atlas. Call once
 * before submitting any input. Requires the matching Canaan crop/stride kernel,
 * two empty nonrotating NV12 planes and input width divisible by 16. */
int meiso_ingress_video_set_stereo(MeisoIngressVideo *video);

/* AU must be one <=128 KiB H.264 Baseline IDR access unit with SPS/PPS. */
MeisoIngressVideoSubmitResult meiso_ingress_video_submit(MeisoIngressVideo *video,
                                                          uint64_t sequence,
                                                          const uint8_t *au,
                                                          size_t au_size);

/* Performs at most one bounded (~33 ms) real DRM event wait. */
MeisoIngressVideoPollResult meiso_ingress_video_poll(MeisoIngressVideo *video,
                                                      MeisoIngressVideoPresentation *presentation);

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

/* Nonblocking observation of the separate overlay page flip. Keep calling the
 * regular video poll too; it serializes both planes on their shared CRTC.
 * sequence=the exact overlay update, not the decoded video frame identity. */
MeisoIngressVideoPollResult meiso_ingress_video_overlay_poll(
    MeisoIngressVideo *video, MeisoIngressVideoPresentation *presentation);

/* Restores the selected plane, stops MVX, releases all exported DMA-BUFs and closes fds.
 * Returns 0 only when all requested recovery operations succeeded. */
int meiso_ingress_video_close(MeisoIngressVideo *video);

#endif
