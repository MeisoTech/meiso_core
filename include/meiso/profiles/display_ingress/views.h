#ifndef MEISO_DISPLAY_INGRESS_VIEWS_H
#define MEISO_DISPLAY_INGRESS_VIEWS_H
#include <meiso/runtime/runtime.h>
#ifdef __cplusplus
extern "C" {
#endif
/* v12 section 14: immutable independent views; no aggregate output raster.
 * Recommended dimensions are budget inputs, not a required allocation. */
enum { MEISO_DISPLAY_VIEWS_MAX_COUNT = 2, MEISO_DISPLAY_VIEWS_CAPABILITY_BYTES = 144 };
typedef struct {
    uint32_t recommended_width, recommended_height;
    float fov_left, fov_right, fov_up, fov_down;
    float position[3]; /* head-relative metres */
    float orientation[4]; /* view -> head, unit quaternion x y z w */
} MeisoDisplayView;
typedef struct {
    uint8_t count;
    MeisoDisplayView views[MEISO_DISPLAY_VIEWS_MAX_COUNT];
} MeisoDisplayViewsCapability;
bool meiso_display_views_capability_encode(const MeisoDisplayViewsCapability *, MeisoMutableBuffer);
bool meiso_display_views_capability_decode(MeisoByteView, MeisoDisplayViewsCapability *);
const MeisoEndpointId *meiso_display_views_endpoint(void);
#ifdef __cplusplus
}
#endif
#endif
