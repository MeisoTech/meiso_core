#ifndef MEISO_DISPLAY_INGRESS_SCENE_H
#define MEISO_DISPLAY_INGRESS_SCENE_H
#include <meiso/profiles/display_ingress/overlay.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Bounded native-scene payload building block. Application Profile v9 does
 * not select this payload; it must not be sent on the legacy HUD_PLAN route. */
enum { MEISO_DISPLAY_SCENE_MAX_ITEMS=16, MEISO_DISPLAY_SCENE_CAPABILITY_BYTES=36,
    MEISO_DISPLAY_SCENE_HEADER_BYTES=32, MEISO_DISPLAY_SCENE_ITEM_BYTES=88,
    MEISO_DISPLAY_SCENE_MAX_PLAN_BYTES=1440 };
typedef struct {
    uint32_t output_width, output_height, max_pixels, max_text, max_updates_per_second;
    uint16_t min_width, min_height, width_alignment, height_alignment;
    uint32_t max_items, fonts;
} MeisoDisplaySceneCapability;
typedef struct {
    uint32_t x,y,width,height,border_argb,fill_argb,text_argb;
    uint16_t border_width;
    uint8_t text_scale,font,text_size;
    char text[MEISO_DISPLAY_OVERLAY_MAX_TEXT];
} MeisoDisplaySceneItem;
typedef struct {
    uint64_t generation,video_desired_version;
    uint32_t canvas_width,canvas_height,item_count;
    MeisoDisplaySceneItem items[MEISO_DISPLAY_SCENE_MAX_ITEMS];
} MeisoDisplayScenePlan;
typedef struct { uint32_t x,y,width,height; } MeisoDisplaySceneRegion;
/* Absolute output coordinates; each eye is explicit. Count zero clears.
 * Items paint in order using straight-alpha source-over. The single backing
 * region includes transparent gaps, alignment and minimum-size padding. */
size_t meiso_display_scene_plan_bytes(const MeisoDisplayScenePlan *);
bool meiso_display_scene_capability_encode(const MeisoDisplaySceneCapability *,MeisoMutableBuffer);
bool meiso_display_scene_capability_decode(MeisoByteView,MeisoDisplaySceneCapability *);
bool meiso_display_scene_plan_encode(const MeisoDisplayScenePlan *,MeisoMutableBuffer);
bool meiso_display_scene_plan_decode(MeisoByteView,MeisoDisplayScenePlan *);
/* Failure leaves all outputs untouched. Codecs require disjoint input/output.
 * Region is required and must be disjoint from capability and plan. */
bool meiso_display_scene_supported(const MeisoDisplaySceneCapability *,const MeisoDisplayScenePlan *,
    uint64_t active_video_desired_version,MeisoDisplaySceneRegion *);
#ifdef __cplusplus
}
#endif
#endif
