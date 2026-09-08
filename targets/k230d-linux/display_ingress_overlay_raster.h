#ifndef MEISO_K230D_OVERLAY_RASTER_H
#define MEISO_K230D_OVERLAY_RASTER_H
#include <meiso/profiles/display_ingress/overlay.h>
/* Optional local CJK16 bank. Fixed size/header are checked once; glyph reads
 * use one descriptor and at most 1536 bytes of stack, with no font-wide heap. */
bool meiso_k230d_overlay_font_open(const char *path);
void meiso_k230d_overlay_font_close(void);
/* CPU renderer shared by the board and raster checks. No allocation; invalid
 * geometry/capacity leaves pixels and dimensions untouched. Capacity is pixels. */
bool meiso_k230d_overlay_raster(const MeisoDisplayOverlayCapability *,
    const MeisoDisplayOverlayPlan *, uint32_t *pixels, size_t capacity,
    uint32_t *width, uint32_t *height);
#endif
