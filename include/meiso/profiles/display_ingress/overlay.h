#ifndef MEISO_PROFILES_DISPLAY_INGRESS_OVERLAY_H
#define MEISO_PROFILES_DISPLAY_INGRESS_OVERLAY_H
#include <meiso/profiles/display_ingress/profile.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    MEISO_DISPLAY_OVERLAY_PROFILE_VERSION = 6,
    MEISO_DISPLAY_STEREO_OVERLAY_PROFILE_VERSION = 7,
    MEISO_DISPLAY_OVERLAY_ENDPOINT_COUNT = 2,
    MEISO_DISPLAY_OVERLAY_ROUTE_COUNT = 7,
    MEISO_DISPLAY_OVERLAY_ROUTE_PLAN = 4,
    MEISO_DISPLAY_OVERLAY_ROUTE_STATUS = 5,
    MEISO_DISPLAY_OVERLAY_ROUTE_EVIDENCE = 6,
    MEISO_DISPLAY_OVERLAY_CAPABILITY_BYTES = 36,
    MEISO_DISPLAY_OVERLAY_PLAN_BYTES = 112,
    MEISO_DISPLAY_OVERLAY_STATUS_BYTES = 8,
    MEISO_DISPLAY_OVERLAY_EVIDENCE_BYTES = 56,
    MEISO_DISPLAY_OVERLAY_MAX_PIXELS = 65536,
    MEISO_DISPLAY_OVERLAY_LAYOUT_SINGLE = 0,
    MEISO_DISPLAY_OVERLAY_LAYOUT_SIDE_BY_SIDE = 1,
    MEISO_DISPLAY_OVERLAY_MAX_TEXT = 48,
    MEISO_DISPLAY_OVERLAY_FONT_LABEL = 0,
    MEISO_DISPLAY_OVERLAY_FONT_CJK16 = 1
};

typedef struct {
    uint32_t output_width, output_height, max_pixels, max_text, max_updates_per_second;
    uint16_t min_width, min_height, width_alignment, height_alignment;
    uint32_t layouts; /* bit 0 single, bit 1 side-by-side repetition */
    uint32_t fonts; /* bit 0 built-in label, bit 1 optional CJK16 glyph bank */
} MeisoDisplayOverlayCapability;

/* Complete fixed two-layer plan; video reference has the containing State's
 * Host owner. Generation identifies immutable content, separately from State
 * version. Pixels are produced on Edge, never carried in this State. For
 * side-by-side layout, x/width describe one eye; the right copy is offset by
 * half the canvas width. The spanning surface, including its transparent gap,
 * consumes the advertised pixel budget and must meet width alignment. */
typedef struct {
    uint64_t generation, video_desired_version;
    uint32_t canvas_width, canvas_height, x, y, width, height;
    uint32_t border_argb, fill_argb, text_argb;
    uint16_t border_width;
    uint8_t text_scale, visible, text_size, layout, font;
    char text[MEISO_DISPLAY_OVERLAY_MAX_TEXT];
} MeisoDisplayOverlayPlan;

typedef struct {
    uint64_t plan_generation, desired_version;
    uint8_t plan_sha256[32];
} MeisoDisplayOverlayEvidence;

/* Fixed-size codecs. Inputs/output are disjoint. Failure leaves output
 * unchanged; unused text/reserved bytes must be zero on the wire. */
bool meiso_display_overlay_capability_encode(const MeisoDisplayOverlayCapability *, MeisoMutableBuffer);
bool meiso_display_overlay_capability_decode(MeisoByteView, MeisoDisplayOverlayCapability *);
bool meiso_display_overlay_plan_encode(const MeisoDisplayOverlayPlan *, MeisoMutableBuffer);
bool meiso_display_overlay_plan_decode(MeisoByteView, MeisoDisplayOverlayPlan *);
bool meiso_display_overlay_evidence_encode(const MeisoDisplayOverlayEvidence *, MeisoMutableBuffer);
bool meiso_display_overlay_evidence_decode(MeisoByteView, MeisoDisplayOverlayEvidence *);
bool meiso_display_overlay_plan_supported(const MeisoDisplayOverlayCapability *,
    const MeisoDisplayOverlayPlan *, uint64_t active_video_desired_version);
/* Strict UTF-8 within the CJK16 repertoire. On failure both outputs are unchanged. */
bool meiso_display_overlay_cjk_next(MeisoByteView *, uint32_t *codepoint);
/* Unscaled pixel extent; validates the selected font's complete bounded text. */
bool meiso_display_overlay_text_extent(uint8_t font, MeisoByteView,
    uint32_t *width, uint32_t *height);

/* Explicit opt-in; v1/v2 helpers and exact graphs are unchanged. */
bool meiso_display_overlay_configure_session(MeisoSessionConfig *);
/* Same graph/codecs, explicitly centered native-size left/right video views. */
bool meiso_display_stereo_overlay_configure_session(MeisoSessionConfig *);
bool meiso_display_overlay_configure_runtime(MeisoRuntimeV4Config *, MeisoSession *);
/* Capability storage holds video's 32 bytes then HUD's 36 bytes; endpoint
 * descriptors borrow it until Runtime copies it. No partial output on error. */
bool meiso_display_overlay_build_catalog(const MeisoDisplayIngressCatalogConfig *,
    const MeisoDisplayOverlayCapability *, MeisoMutableBuffer capability_storage,
    MeisoRuntimeEndpointDescriptor *, size_t endpoint_capacity,
    MeisoRuntimeV4RouteDescriptor *, size_t route_capacity);
bool meiso_display_overlay_validate_catalog(const MeisoRuntimeEndpointDescriptor *, size_t,
    const MeisoRuntimeV4RouteDescriptor *, size_t,
    MeisoDisplayIngressCapability *, MeisoDisplayOverlayCapability *);

#ifdef __cplusplus
}
#endif
#endif
