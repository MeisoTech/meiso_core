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

/* AU must be one <=128 KiB H.264 Baseline IDR access unit with SPS/PPS. */
MeisoIngressVideoSubmitResult meiso_ingress_video_submit(MeisoIngressVideo *video,
                                                          uint64_t sequence,
                                                          const uint8_t *au,
                                                          size_t au_size);

/* Performs at most one bounded (~33 ms) real DRM event wait. */
MeisoIngressVideoPollResult meiso_ingress_video_poll(MeisoIngressVideo *video,
                                                      MeisoIngressVideoPresentation *presentation);

/* Restores the selected plane, stops MVX, releases all exported DMA-BUFs and closes fds.
 * Returns 0 only when all requested recovery operations succeeded. */
int meiso_ingress_video_close(MeisoIngressVideo *video);

#endif
