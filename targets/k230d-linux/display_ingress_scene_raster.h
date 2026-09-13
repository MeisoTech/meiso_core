#ifndef MEISO_DISPLAY_INGRESS_SCENE_RASTER_H
#define MEISO_DISPLAY_INGRESS_SCENE_RASTER_H
#include <meiso/profiles/display_ingress/scene.h>
/* Direct straight ARGB4444 output, no full-frame staging allocation. Capacity
 * is uint16_t pixels. Empty scene permits NULL/zero. All outputs must be
 * disjoint; invalid geometry, font or capacity leaves them untouched. */
bool meiso_k230d_scene_raster(const MeisoDisplaySceneCapability *,const MeisoDisplayScenePlan *,
    uint16_t *pixels,size_t capacity,MeisoDisplaySceneRegion *);
#endif
