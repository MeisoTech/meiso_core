#ifndef MEISO_DISPLAY_INGRESS_APPLICATION_H
#define MEISO_DISPLAY_INGRESS_APPLICATION_H
#include <meiso/profiles/display_ingress/overlay.h>
#include <meiso/profiles/display_ingress/scene.h>
#include <meiso/profiles/display_ingress/camera.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One bounded graph: display, optional HUD, optional CameraMedia, in that order.
 * Layout and output geometry are capabilities, not Profile variants. */
enum { MEISO_DISPLAY_APPLICATION_PROFILE_VERSION=9, MEISO_DISPLAY_SCENE_APPLICATION_PROFILE_VERSION=10,
    MEISO_DISPLAY_APPLICATION_MAX_ENDPOINTS=3, MEISO_DISPLAY_APPLICATION_MAX_ROUTES=10,
    MEISO_DISPLAY_APPLICATION_MAX_CAPABILITY_BYTES=148,
    MEISO_DISPLAY_APPLICATION_VIDEO_CAPABILITY_BYTES=48,
    MEISO_DISPLAY_APPLICATION_VIDEO_NATIVE=0, MEISO_DISPLAY_APPLICATION_VIDEO_SIDE_BY_SIDE=1 };
typedef struct {
    MeisoDisplayIngressCatalogConfig video;
    uint32_t output_width, output_height;
    uint8_t video_layout;
    bool overlay_enabled, camera_enabled;
    MeisoDisplayOverlayCapability overlay;
    MeisoCompositionMediaCapability camera;
    MeisoDisplaySceneCapability scene; /* v10's optional native 2D capability */
} MeisoDisplayApplicationCatalogConfig;
const MeisoSchemaRef *meiso_display_application_video_capability_schema(void);
bool meiso_display_application_video_capability_decode(MeisoByteView,
    MeisoDisplayIngressCapability *, uint32_t *, uint32_t *, uint8_t *);
bool meiso_display_application_configure_session(MeisoSessionConfig *);
bool meiso_display_application_configure_runtime(MeisoRuntimeV4Config *, MeisoSession *);
bool meiso_display_application_build_catalog(const MeisoDisplayApplicationCatalogConfig *,
    MeisoMutableBuffer, MeisoRuntimeEndpointDescriptor *, size_t, MeisoRuntimeV4RouteDescriptor *, size_t);
bool meiso_display_application_validate_catalog(const MeisoRuntimeEndpointDescriptor *, size_t,
    const MeisoRuntimeV4RouteDescriptor *, size_t, MeisoDisplayApplicationCatalogConfig *);
/* Explicit Session/Profile selection. Legacy helpers above remain v9. */
bool meiso_display_application_configure_session_for_version(MeisoSessionConfig *,uint32_t);
bool meiso_display_application_configure_runtime_for_version(MeisoRuntimeV4Config *,MeisoSession *,uint32_t);
bool meiso_display_application_build_catalog_for_version(const MeisoDisplayApplicationCatalogConfig *,uint32_t,
    MeisoMutableBuffer,MeisoRuntimeEndpointDescriptor *,size_t,MeisoRuntimeV4RouteDescriptor *,size_t);
bool meiso_display_application_validate_catalog_for_version(const MeisoRuntimeEndpointDescriptor *,size_t,
    const MeisoRuntimeV4RouteDescriptor *,size_t,uint32_t,MeisoDisplayApplicationCatalogConfig *);
/* HUD_PRES v2 explicitly binds software or physical completion. Legacy HUD v1
 * codecs remain physical-only. The expected class must come from the Catalog. */
bool meiso_display_overlay_evidence_encode_for_class(const MeisoDisplayOverlayEvidence *,
    MeisoDisplayIngressEvidenceClass, MeisoMutableBuffer);
bool meiso_display_overlay_evidence_decode_for_class(MeisoByteView,
    MeisoDisplayIngressEvidenceClass, MeisoDisplayOverlayEvidence *);
#ifdef __cplusplus
}
#endif
#endif
