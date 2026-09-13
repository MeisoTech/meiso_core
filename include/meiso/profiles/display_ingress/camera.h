#ifndef MEISO_DISPLAY_INGRESS_CAMERA_H
#define MEISO_DISPLAY_INGRESS_CAMERA_H
#include <meiso/profiles/display_ingress/profile.h>
#include <meiso/profiles/composition/profile.h>
#ifdef __cplusplus
extern "C" {
#endif
/* v8 composes H.264 ingress with the existing CameraMedia resource, in one
 * Session. It does not select a HUD or stereo placement policy. */
enum { MEISO_DISPLAY_CAMERA_PROFILE_VERSION=8, MEISO_DISPLAY_CAMERA_ENDPOINTS=2,
    MEISO_DISPLAY_CAMERA_ROUTES=7, MEISO_DISPLAY_CAMERA_CAPABILITY_BYTES=96 };
bool meiso_display_camera_configure_session(MeisoSessionConfig *);
bool meiso_display_camera_configure_runtime(MeisoRuntimeV4Config *, MeisoSession *);
bool meiso_display_camera_build_catalog(const MeisoDisplayIngressCatalogConfig *,
    const MeisoCompositionMediaCapability *, MeisoMutableBuffer,
    MeisoRuntimeEndpointDescriptor *, size_t, MeisoRuntimeV4RouteDescriptor *, size_t);
bool meiso_display_camera_validate_catalog(const MeisoRuntimeEndpointDescriptor *, size_t,
    const MeisoRuntimeV4RouteDescriptor *, size_t, MeisoDisplayIngressCapability *,
    MeisoCompositionMediaCapability *);
#ifdef __cplusplus
}
#endif
#endif
