#include <meiso/profiles/display_ingress/planes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static MeisoDisplayPlanesCapability capability(void)
{
    MeisoDisplayPlanesCapability c = {1920, 1080, MEISO_DISPLAY_PLANES_VIEW_MONO, MEISO_DISPLAY_PLANES_TRACKING_NONE,
        16, 4, 262144, 2048, 2048, -0.4363f, 0.4363f, 0.2531f, -0.2531f, 0.0f, 30};
    return c;
}

static MeisoDisplayPlanesModel model(void)
{
    MeisoDisplayPlanesModel m;
    memset(&m, 0, sizeof(m));
    m.generation = 7; m.video_desired_version = 3; m.plane_count = 2; m.app_count = 2; m.focused_app = 0;
    m.apps[0].app_id = 11; m.apps[0].name_size = 4; memcpy(m.apps[0].name, "Demo", 4);
    m.apps[1].app_id = 12; m.apps[1].name_size = 6; memcpy(m.apps[1].name, "\xe7\x9b\xb8\xe5\x86\x8c", 6); /* 相册 */
    MeisoDisplayPlane *v = &m.planes[0];
    v->plane_id = 1; v->app = 0; v->anchor = MEISO_DISPLAY_PLANES_ANCHOR_WORLD; v->source = MEISO_DISPLAY_PLANES_SOURCE_STREAM;
    v->opacity = 255; v->source_width = 640; v->source_height = 360;
    v->position[0] = -0.3f; v->position[1] = 0.05f; v->position[2] = -1.6f;
    v->orientation[1] = 0.2213f; v->orientation[3] = 0.9752f; v->width_m = 0.96f; v->height_m = 0.54f;
    v = &m.planes[1];
    v->plane_id = 2; v->app = 1; v->anchor = MEISO_DISPLAY_PLANES_ANCHOR_WORLD; v->source = MEISO_DISPLAY_PLANES_SOURCE_IMAGE;
    v->opacity = 255; v->source_width = 512; v->source_height = 384; memset(v->image_sha256, 0xab, 32);
    v->position[0] = 0.42f; v->position[2] = -1.4f; v->orientation[3] = 1.0f; v->width_m = 0.48f; v->height_m = 0.36f;
    return m;
}

int main(void)
{
    uint8_t b[MEISO_DISPLAY_PLANES_MAX_MODEL_BYTES], c2[64];
    /* capability golden fields and round trip */
    MeisoDisplayPlanesCapability c = capability(), cd;
    CHECK(meiso_display_planes_capability_encode(&c, (MeisoMutableBuffer){b, 64}));
    CHECK(b[3] == 0x80 && b[2] == 0x07 && b[7] == 0x38 && b[8] == 1 && b[10] == 16 && b[11] == 4 && b[41] == 30);
    CHECK(b[20] == 0xbe && b[24] == 0x3e); /* -0.4363f, +0.4363f big-endian sign bytes */
    CHECK(meiso_display_planes_capability_decode((MeisoByteView){b, 64}, &cd));
    /* Compare the canonical wire, not unspecified struct padding bytes. */
    CHECK(meiso_display_planes_capability_encode(&cd, (MeisoMutableBuffer){c2, 64}) && !memcmp(b, c2, 64));
    b[50] = 1; CHECK(!meiso_display_planes_capability_decode((MeisoByteView){b, 64}, &cd)); b[50] = 0;
    MeisoDisplayPlanesCapability bad = c; bad.ipd_m = 0.06f;
    CHECK(!meiso_display_planes_capability_encode(&bad, (MeisoMutableBuffer){c2, 64}));
    bad = c; bad.view_layout = MEISO_DISPLAY_PLANES_VIEW_SIDE_BY_SIDE; bad.ipd_m = 0.063f;
    CHECK(meiso_display_planes_capability_encode(&bad, (MeisoMutableBuffer){c2, 64}));
    bad.output_width = 1919; CHECK(!meiso_display_planes_capability_encode(&bad, (MeisoMutableBuffer){c2, 64}));
    bad = c; bad.fov_left = 0.5f; CHECK(!meiso_display_planes_capability_encode(&bad, (MeisoMutableBuffer){c2, 64}));
    bad = c; bad.fov_up = NAN; CHECK(!meiso_display_planes_capability_encode(&bad, (MeisoMutableBuffer){c2, 64}));
    bad = c; bad.max_images = 0; CHECK(!meiso_display_planes_capability_encode(&bad, (MeisoMutableBuffer){c2, 64}));
    bad = c; bad.max_planes = 17; CHECK(!meiso_display_planes_capability_encode(&bad, (MeisoMutableBuffer){c2, 64}));

    /* model golden layout, round trip and failure vectors */
    MeisoDisplayPlanesModel m = model(), md;
    const size_t n = meiso_display_planes_model_bytes(&m);
    CHECK(n == 32 + 2 * 40 + 2 * 96);
    CHECK(meiso_display_planes_model_encode(&m, (MeisoMutableBuffer){b, sizeof(b)}));
    CHECK(b[7] == 7 && b[15] == 3 && b[16] == 2 && b[17] == 2 && b[18] == 0);
    CHECK(b[35] == 11 && b[36] == 4 && !memcmp(b + 40, "Demo", 4) && b[76] == 6);
    const uint8_t *p0 = b + 32 + 80, *p1 = p0 + 96;
    CHECK(p0[3] == 1 && p0[5] == 2 && p0[6] == 1 && p0[7] == 255 && p0[18] == 0x02 && p0[19] == 0x80);
    CHECK(p1[6] == 2 && p1[24] == 0xab && p1[55] == 0xab);
    CHECK(p0[56] == 0xbe && p0[64] == 0xbf); /* x = -0.3, z = -1.6 */
    CHECK(meiso_display_planes_model_decode((MeisoByteView){b, n}, &md));
    CHECK(md.generation == 7 && md.plane_count == 2 && md.planes[0].position[2] == -1.6f &&
          md.planes[1].image_sha256[31] == 0xab && !memcmp(md.apps[1].name, m.apps[1].name, 6));
    CHECK(meiso_display_planes_model_image_count(&m) == 1);
    CHECK(!meiso_display_planes_model_decode((MeisoByteView){b, n - 1}, &md));
    b[20] = 1; CHECK(!meiso_display_planes_model_decode((MeisoByteView){b, n}, &md)); b[20] = 0;
    b[32 + 80 + 94] = 1; CHECK(!meiso_display_planes_model_decode((MeisoByteView){b, n}, &md)); b[32 + 80 + 94] = 0;
    b[32 + 5] = 1; CHECK(!meiso_display_planes_model_decode((MeisoByteView){b, n}, &md)); b[32 + 5] = 0;
    CHECK(meiso_display_planes_model_decode((MeisoByteView){b, n}, &md));

    MeisoDisplayPlanesModel x;
    x = m; x.generation = 0; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.planes[1].plane_id = 1; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.apps[1].app_id = 11; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.planes[0].orientation[3] = 0.5f; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.planes[0].position[0] = INFINITY; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.planes[0].width_m = 0.0f; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.planes[0].opacity = 0; CHECK(meiso_display_planes_model_bytes(&x)); /* hidden (meiso_host #71) */
    x = m; x.planes[0].app = 2; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.planes[0].app = MEISO_DISPLAY_PLANES_NO_APP; CHECK(meiso_display_planes_model_bytes(&x));
    x = m; x.focused_app = 2; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.planes[0].image_sha256[0] = 1; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; memset(x.planes[1].image_sha256, 0, 32); CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.video_desired_version = 0; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.planes[0].source = MEISO_DISPLAY_PLANES_SOURCE_IMAGE; memset(x.planes[0].image_sha256, 1, 32);
    CHECK(!meiso_display_planes_model_bytes(&x)); /* no stream plane but a video reference */
    x.video_desired_version = 0; CHECK(meiso_display_planes_model_bytes(&x) && meiso_display_planes_model_image_count(&x) == 2);
    x = m; x.apps[0].name[1] = '\n'; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; memset(&x.apps[1].name[1], 0xff, 1); CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.apps[0].name[5] = 'x'; CHECK(!meiso_display_planes_model_bytes(&x));
    x = m; x.plane_count = 0; x.video_desired_version = 0; CHECK(meiso_display_planes_model_bytes(&x) == 32 + 80);

    /* support against capability and the active display Desired */
    CHECK(meiso_display_planes_model_supported(&c, &m, 3, 640, 360));
    CHECK(!meiso_display_planes_model_supported(&c, &m, 4, 640, 360));
    CHECK(!meiso_display_planes_model_supported(&c, &m, 3, 320, 360));
    CHECK(!meiso_display_planes_model_supported(&c, &m, 0, 0, 0));
    bad = c; bad.max_planes = 1; CHECK(!meiso_display_planes_model_supported(&bad, &m, 3, 640, 360));
    x = m; x.planes[1].source_width = 2048; x.planes[1].source_x = 1;
    CHECK(!meiso_display_planes_model_supported(&c, &x, 3, 640, 360));

    /* status and evidence */
    MeisoDisplayPlanesStatus s = {7, 1, 1}, sd;
    CHECK(meiso_display_planes_status_encode(&s, (MeisoMutableBuffer){b, 16}) && b[7] == 7 && b[8] == 1 && b[9] == 1);
    CHECK(meiso_display_planes_status_decode((MeisoByteView){b, 16}, &sd) && sd.generation == 7 && sd.waiting == 1);
    b[12] = 1; CHECK(!meiso_display_planes_status_decode((MeisoByteView){b, 16}, &sd));
    s.generation = 0; CHECK(!meiso_display_planes_status_encode(&s, (MeisoMutableBuffer){b, 16}));
    MeisoDisplayPlanesEvidence e = {MEISO_DISPLAY_INGRESS_EVIDENCE_DRM_KMS_PAGE_FLIP, 2, 0, 7, 9, {0}, 0}, ed;
    memset(e.model_sha256, 0x5a, 32);
    CHECK(meiso_display_planes_evidence_encode(&e, (MeisoMutableBuffer){b, 64}) && b[0] == 2 && b[15] == 7 && b[23] == 9 && b[55] == 0x5a);
    CHECK(meiso_display_planes_evidence_decode((MeisoByteView){b, 64}, &ed) && ed.drawn == 2 && ed.desired_version == 9);
    b[60] = 1; CHECK(!meiso_display_planes_evidence_decode((MeisoByteView){b, 64}, &ed)); b[60] = 0;
    b[0] = 3; CHECK(!meiso_display_planes_evidence_decode((MeisoByteView){b, 64}, &ed));

    /* evidence digest: SHA-256("abc") */
    uint8_t digest[32];
    CHECK(meiso_display_planes_sha256((MeisoByteView){(const uint8_t *)"abc", 3}, digest) && digest[0] == 0xba && digest[31] == 0xad);

    /* baseline JPEG header */
    static const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x04, 'J', 'F', 0xff, 0xc0, 0x00, 0x11, 0x08, 0x01, 0x80,
        0x02, 0x00, 0x03, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1, 0xff, 0xda, 0, 2, 0xff, 0xd9};
    uint32_t w = 0, h = 0;
    CHECK(meiso_display_planes_jpeg_header((MeisoByteView){jpeg, sizeof(jpeg)}, &w, &h) && w == 512 && h == 384);
    uint8_t progressive[sizeof(jpeg)];
    memcpy(progressive, jpeg, sizeof(jpeg)); progressive[9] = 0xc2;
    CHECK(!meiso_display_planes_jpeg_header((MeisoByteView){progressive, sizeof(progressive)}, &w, &h));
    CHECK(!meiso_display_planes_jpeg_header((MeisoByteView){jpeg, 12}, &w, &h));

    /* catalog: display + planes, with and without camera */
    MeisoDisplayPlanesCatalogConfig cc, got;
    memset(&cc, 0, sizeof(cc));
    cc.video = (MeisoDisplayIngressCatalogConfig){1, {1280, 720, 30000, 131072, 1024,
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_DRM_KMS_PAGE_FLIP, MEISO_STREAM_DEADLINE_UNSUPPORTED, MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    cc.planes = c;
    cc.camera = (MeisoCompositionMediaCapability){.source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        .pixel_formats = MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888, .codecs = MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        .min_width = 320, .min_height = 240, .max_width = 320, .max_height = 240, .max_fps_millihz = 5000,
        .max_access_unit_bytes = 307200, .max_fragment_payload_bytes = 4096};
    uint8_t cap[MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor ep[3];
    MeisoRuntimeV4RouteDescriptor rt[MEISO_DISPLAY_PLANES_MAX_ROUTES];
    for (unsigned camera = 0; camera < 2; ++camera) {
        cc.camera_enabled = camera != 0;
        const size_t ec = 2 + camera, rc = 8 + 3 * camera;
        CHECK(meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 3, rt, 11));
        CHECK(meiso_endpoint_id_equal(ep[1].id, *meiso_display_planes_endpoint()) && ep[1].capability.size == 64);
        CHECK(rt[7].route.resource.kind == MEISO_RESOURCE_KIND_BLOB && rt[7].route.resource.owner == MEISO_OWNER_ROLE_HOST &&
              rt[7].route.resource.max_payload_size == 262144 && rt[7].route.facet == MEISO_RUNTIME_FACET_IMMUTABLE);
        CHECK(rt[4].route.resource.max_payload_size == MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE + 1888);
        CHECK(meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
        CHECK(got.camera_enabled == cc.camera_enabled && got.planes.max_images == 4 && got.video.capability.max_input_width == 1280);
        CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc - 1, &got));
        cap[32 + 50] = 1; CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got)); cap[32 + 50] = 0;
        rt[5].route.resource.owner = MEISO_OWNER_ROLE_HOST;
        CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
        rt[5].route.resource.owner = MEISO_OWNER_ROLE_EDGE;
        rt[7].route.resource.max_payload_size = 1;
        CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
    }
    MeisoSessionConfig sc;
    memset(&sc, 0, sizeof(sc));
    sc.local_role = MEISO_SESSION_ROLE_EDGE; sc.local_peer_id = (MeisoPeerId){11, 12}; sc.expected_peer_id = (MeisoPeerId){21, 22};
    sc.local_incarnation = 1; sc.local_nonce = (MeisoSessionNonce){3, 4}; sc.protocol = (MeisoProtocolVersion){1, 0};
    CHECK(meiso_display_planes_configure_session(&sc) && sc.profile.version == 12 && sc.limits.max_registry_entries == 28); /* section 15 */
    printf("display ingress planes v12: ok\n");
    return 0;
}
