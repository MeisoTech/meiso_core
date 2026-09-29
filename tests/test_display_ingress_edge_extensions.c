#include <meiso/profiles/display_ingress/planes.h>
#include <meiso/profiles/display_ingress/camera.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static MeisoDisplayPlanesCatalogConfig config(void)
{
    MeisoDisplayPlanesCatalogConfig c = {0};
    c.video = (MeisoDisplayIngressCatalogConfig){1, {1280, 720, 90000, 131072, 1024,
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_XR_FRAME_SUBMITTED, MEISO_STREAM_DEADLINE_UNSUPPORTED,
        MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    c.planes = (MeisoDisplayPlanesCapability){0, 0, MEISO_DISPLAY_PLANES_VIEW_RUNTIME_VIEWS,
        MEISO_DISPLAY_PLANES_TRACKING_EDGE, 14, 4, 262144, 2048, 2048, 0, 0, 0, 0, 0, 30};
    c.local_pose_enabled = c.views_enabled = c.input_enabled = true;
    c.local_pose.max_samples_per_second = 200;
    c.input = (MeisoDisplayInputCapability){0xfe, MEISO_DISPLAY_INPUT_POINTER_HEAD_RAY};
    c.views.count = 2;
    for (unsigned i = 0; i < 2; ++i) {
        c.views.views[i] = (MeisoDisplayView){6356, 5104, -0.5f, 0.5f, 0.5f, -0.5f,
            {i ? 0.03125f : -0.03125f, 0, 0}, {0, 0, 0, 1}};
    }
    c.camera = (MeisoCompositionMediaCapability){.source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        .pixel_formats = MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888, .codecs = MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        .min_width = 320, .min_height = 240, .max_width = 320, .max_height = 240, .max_fps_millihz = 5000,
        .max_access_unit_bytes = 307200, .max_fragment_payload_bytes = 4096};
    c.tracking = (MeisoDisplayTrackingCapability){200000, 156.9064f, 34.9066f, 8, MEISO_DISPLAY_TRACKING_CORRECTION_NONE, 100};
    return c;
}

static int codecs(void)
{
    MeisoDisplayPlanesCatalogConfig c = config();
    uint8_t b[192], saved[192];
    static const uint8_t plane_cap[64] = {
        [8] = 3, [9] = 3, [10] = 14, [11] = 4, [13] = 4, [16] = 8, [18] = 8, [41] = 30
    };
    CHECK(meiso_display_planes_capability_encode(&c.planes, (MeisoMutableBuffer){b, 64}) && !memcmp(b, plane_cap, 64));
    MeisoDisplayPlanesCapability pc;
    CHECK(meiso_display_planes_capability_decode((MeisoByteView){b, 64}, &pc) && pc.view_layout == 3 && pc.tracking == 3);
    c.planes.output_width = 6356; CHECK(!meiso_display_planes_capability_encode(&c.planes, (MeisoMutableBuffer){b, 64}));
    c.planes.output_width = 0; c.planes.fov_left = -0.0f;
    CHECK(!meiso_display_planes_capability_encode(&c.planes, (MeisoMutableBuffer){b, 64})); c = config();

    static const uint8_t pose_cap[16] = {[1] = 200};
    MeisoDisplayLocalPoseCapability lc = {0};
    CHECK(meiso_display_local_pose_capability_encode(&c.local_pose, (MeisoMutableBuffer){b, 16}) && !memcmp(b, pose_cap, 16));
    CHECK(meiso_display_local_pose_capability_decode((MeisoByteView){b, 16}, &lc) && lc.max_samples_per_second == 200);
    for (unsigned i = 2; i < 16; ++i) {
        b[i] = 1; CHECK(!meiso_display_local_pose_capability_decode((MeisoByteView){b, 16}, &lc) && lc.max_samples_per_second == 200); b[i] = 0;
    }
    b[1] = 201; CHECK(!meiso_display_local_pose_capability_decode((MeisoByteView){b, 16}, &lc));
    b[1] = 0; CHECK(!meiso_display_local_pose_capability_decode((MeisoByteView){b, 16}, &lc));
    MeisoDisplayLocalPoseDesired d = {true}, dd = {false};
    MeisoDisplayLocalPoseActual a = {MEISO_DISPLAY_LOCAL_POSE_FAILED}, ad = {0};
    static const uint8_t desired[16] = {1}, actual[16] = {2};
    CHECK(meiso_display_local_pose_desired_encode(&d, (MeisoMutableBuffer){b, 16}) && !memcmp(b, desired, 16));
    CHECK(meiso_display_local_pose_desired_decode((MeisoByteView){b, 16}, &dd) && dd.enabled);
    b[0] = 2; CHECK(!meiso_display_local_pose_desired_decode((MeisoByteView){b, 16}, &dd));
    CHECK(meiso_display_local_pose_actual_encode(&a, (MeisoMutableBuffer){b, 16}) && !memcmp(b, actual, 16));
    CHECK(meiso_display_local_pose_actual_decode((MeisoByteView){b, 16}, &ad) && ad.state == 2);
    b[0] = 3; CHECK(!meiso_display_local_pose_actual_decode((MeisoByteView){b, 16}, &ad) && ad.state == 2);
    for (unsigned i = 1; i < 16; ++i) {
        memcpy(b, desired, 16); b[i] = 1; CHECK(!meiso_display_local_pose_desired_decode((MeisoByteView){b, 16}, &dd));
        memcpy(b, actual, 16); b[i] = 1; CHECK(!meiso_display_local_pose_actual_decode((MeisoByteView){b, 16}, &ad));
    }
    MeisoDisplayLocalPose pose = {0}, pd = {0}; pose.time_ns = 9; pose.world_epoch = 0x01020304;
    pose.quality = MEISO_DISPLAY_TRACKING_TRACKING; pose.orientation[3] = 1;
    static const uint8_t pose_golden[72] = {[7] = 9, [8] = 1, [9] = 2, [10] = 3, [11] = 4, [12] = 1, [40] = 0x3f, [41] = 0x80};
    CHECK(meiso_display_local_pose_encode(&pose, (MeisoMutableBuffer){b, 72}) && !memcmp(b, pose_golden, 72));
    CHECK(meiso_display_local_pose_decode((MeisoByteView){b, 72}, &pd) && pd.world_epoch == pose.world_epoch);
    b[13] = 1; CHECK(!meiso_display_local_pose_decode((MeisoByteView){b, 72}, &pd));
    pose.position[0] = NAN; CHECK(!meiso_display_local_pose_encode(&pose, (MeisoMutableBuffer){b, 72}));
    pose.position[0] = 0; pose.quality = MEISO_DISPLAY_TRACKING_LOST;
    CHECK(meiso_display_local_pose_encode(&pose, (MeisoMutableBuffer){b, 72}) && b[12] == 2);

    static const uint8_t view_golden[144] = {
        [0] = 2, [18] = 0x18, [19] = 0xd4, [22] = 0x13, [23] = 0xf0,
        [24] = 0xbf, [28] = 0x3f, [32] = 0x3f, [36] = 0xbf, [40] = 0xbd, [64] = 0x3f, [65] = 0x80,
        [82] = 0x18, [83] = 0xd4, [86] = 0x13, [87] = 0xf0,
        [88] = 0xbf, [92] = 0x3f, [96] = 0x3f, [100] = 0xbf, [104] = 0x3d, [128] = 0x3f, [129] = 0x80
    };
    MeisoDisplayViewsCapability vd;
    CHECK(meiso_display_views_capability_encode(&c.views, (MeisoMutableBuffer){b, 144}) && !memcmp(b, view_golden, 144));
    CHECK(meiso_display_views_capability_decode((MeisoByteView){b, 144}, &vd) && vd.views[1].recommended_width == 6356);
    for (unsigned i = 0; i < 144; ++i) if ((i >= 1 && i <= 15) || (i >= 68 && i < 80) || i >= 132) {
        b[i] = 1; CHECK(!meiso_display_views_capability_decode((MeisoByteView){b, 144}, &vd)); b[i] = 0;
    }
    b[0] = 3; CHECK(!meiso_display_views_capability_decode((MeisoByteView){b, 144}, &vd));
    b[0] = 0; CHECK(!meiso_display_views_capability_decode((MeisoByteView){b, 144}, &vd));
    b[0] = 1; CHECK(!meiso_display_views_capability_decode((MeisoByteView){b, 144}, &vd)); /* unused record */
    memset(b + 80, 0, 64); CHECK(meiso_display_views_capability_decode((MeisoByteView){b, 144}, &vd) && vd.count == 1);
    c.views.views[0].fov_left = c.views.views[0].fov_right;
    memset(b, 0xa5, sizeof(b)); memcpy(saved, b, sizeof(b));
    CHECK(!meiso_display_views_capability_encode(&c.views, (MeisoMutableBuffer){b, 144}) && !memcmp(b, saved, sizeof(b)));
    c = config(); c.views.views[0].orientation[3] = 0;
    CHECK(!meiso_display_views_capability_encode(&c.views, (MeisoMutableBuffer){b, 144}));
    c = config(); c.views.views[0].position[1] = INFINITY;
    CHECK(!meiso_display_views_capability_encode(&c.views, (MeisoMutableBuffer){b, 144}));
    c = config(); c.views.views[0].position[1] = 1.01f;
    CHECK(!meiso_display_views_capability_encode(&c.views, (MeisoMutableBuffer){b, 144}));
    c = config(); c.views.views[0].recommended_height = 0;
    CHECK(!meiso_display_views_capability_encode(&c.views, (MeisoMutableBuffer){b, 144}));
    c = config();
    CHECK(!meiso_display_views_capability_encode(&c.views, (MeisoMutableBuffer){(uint8_t *)&c.views, 144}));
    CHECK(!meiso_display_views_capability_decode((MeisoByteView){(uint8_t *)&vd, 144}, &vd));

    MeisoDisplayPlanesModel m = {0}, md = {0}; m.generation = 7; m.focused_app = 255; m.world_epoch = 0x01020304;
    static const uint8_t model_golden[32] = {[7] = 7, [18] = 255, [24] = 1, [25] = 2, [26] = 3, [27] = 4};
    CHECK(meiso_display_planes_model_encode(&m, (MeisoMutableBuffer){b, 32}) && !memcmp(b, model_golden, 32));
    CHECK(meiso_display_planes_model_decode((MeisoByteView){b, 32}, &md) && md.world_epoch == m.world_epoch);
    CHECK(meiso_display_planes_model_supported(&c.planes, &m, 0, 0, 0));
    m.world_epoch = 0; CHECK(!meiso_display_planes_model_supported(&c.planes, &m, 0, 0, 0));
    c.planes.tracking = MEISO_DISPLAY_PLANES_TRACKING_NONE; CHECK(meiso_display_planes_model_supported(&c.planes, &m, 0, 0, 0));
    m.world_epoch = 1; CHECK(!meiso_display_planes_model_supported(&c.planes, &m, 0, 0, 0));
    for (unsigned i = 19; i < 32; ++i) if (i < 24 || i >= 28) {
        b[i] = 1; CHECK(!meiso_display_planes_model_decode((MeisoByteView){b, 32}, &md)); b[i] = 0;
    }
    return 0;
}

static int evidence(void)
{
    MeisoDisplayPlanesCatalogConfig c = config();
    uint8_t b[80], saved[80]; size_t n = 0;
    MeisoDisplayPlanesEvidence e = {MEISO_DISPLAY_INGRESS_EVIDENCE_XR_FRAME_SUBMITTED, 1, 2, 7, 9, {0}, UINT64_C(0x0102030405060708)}, ed;
    static const uint8_t plane_evidence[64] = {[0] = 3, [1] = 1, [2] = 2, [15] = 7, [23] = 9,
        [56] = 1, [57] = 2, [58] = 3, [59] = 4, [60] = 5, [61] = 6, [62] = 7, [63] = 8};
    CHECK(meiso_display_planes_evidence_encode(&e, (MeisoMutableBuffer){b, 64}) && !memcmp(b, plane_evidence, 64));
    CHECK(meiso_display_planes_evidence_decode((MeisoByteView){b, 64}, &ed) && ed.predicted_display_time_ns == e.predicted_display_time_ns);
    b[0] = 2; CHECK(!meiso_display_planes_evidence_decode((MeisoByteView){b, 64}, &ed));
    b[0] = 3; memset(b + 56, 0, 8); CHECK(!meiso_display_planes_evidence_decode((MeisoByteView){b, 64}, &ed));
    e.predicted_display_time_ns = 0; CHECK(!meiso_display_planes_evidence_encode(&e, (MeisoMutableBuffer){b, 64}));
    e.evidence_class = 2; CHECK(meiso_display_planes_evidence_encode(&e, (MeisoMutableBuffer){b, 64})); /* old class stays 64 */

    MeisoDisplayIngressPresentationEvidence v = {0}, vd;
    v.evidence_class = MEISO_DISPLAY_INGRESS_EVIDENCE_XR_FRAME_SUBMITTED; v.media_stream_epoch = 11;
    v.displayed_frame_sequence = 13; v.predicted_display_time_ns = UINT64_C(0x0102030405060708);
    static const uint8_t video_evidence[72] = {[0] = 3, [23] = 11, [31] = 13,
        [64] = 1, [65] = 2, [66] = 3, [67] = 4, [68] = 5, [69] = 6, [70] = 7, [71] = 8};
    CHECK(meiso_display_ingress_evidence_encode(&v, (MeisoMutableBuffer){b, sizeof(b)}, &n) == 0 && n == 72 && !memcmp(b, video_evidence, 72));
    CHECK(meiso_display_ingress_evidence_decode((MeisoByteView){b, 72}, &vd) == 0 && vd.predicted_display_time_ns == v.predicted_display_time_ns);
    CHECK(meiso_display_ingress_evidence_supported(&c.video.capability, &vd, 11));
    CHECK(!meiso_display_ingress_evidence_supported(&c.video.capability, &vd, 12));
    c.video.capability.evidence_classes = 2;
    CHECK(!meiso_display_ingress_evidence_supported(&c.video.capability, &vd, 11)); c = config();
    CHECK(meiso_display_ingress_evidence_decode((MeisoByteView){b, 64}, &vd) != 0);
    CHECK(meiso_display_ingress_evidence_decode((MeisoByteView){b, 73}, &vd) != 0);
    b[0] = 2; CHECK(meiso_display_ingress_evidence_decode((MeisoByteView){b, 72}, &vd) != 0); b[0] = 3;
    memset(b + 64, 0, 8); CHECK(meiso_display_ingress_evidence_decode((MeisoByteView){b, 72}, &vd) != 0);
    memset(b, 0xa5, sizeof(b)); memcpy(saved, b, sizeof(b));
    CHECK(meiso_display_ingress_evidence_encode(&v, (MeisoMutableBuffer){b, 64}, &n) != 0 && !n && !memcmp(b, saved, sizeof(b)));
    v.output_width = 1; CHECK(meiso_display_ingress_evidence_encode(&v, (MeisoMutableBuffer){b, sizeof(b)}, &n) != 0);
    v.output_width = 0; v.connector_kind = MEISO_DISPLAY_INGRESS_CONNECTOR_HDMI;
    CHECK(meiso_display_ingress_evidence_encode(&v, (MeisoMutableBuffer){b, sizeof(b)}, &n) != 0);
    v.evidence_class = 2; v.output_width = v.output_height = 1280; v.predicted_display_time_ns = 0;
    CHECK(meiso_display_ingress_evidence_encode(&v, (MeisoMutableBuffer){b, sizeof(b)}, &n) == 0 && n == 64);
    CHECK(meiso_display_ingress_evidence_decode((MeisoByteView){b, 64}, &vd) == 0 && !vd.predicted_display_time_ns);
    CHECK(meiso_display_ingress_capability_encode(&c.video.capability, (MeisoMutableBuffer){b, 32}, &n) == 0 && b[23] == 4);
    for (uint32_t mask = 5; mask < 8; ++mask) {
        c.video.capability.evidence_classes = mask;
        CHECK(meiso_display_ingress_capability_encode(&c.video.capability, (MeisoMutableBuffer){b, 32}, &n) != 0);
    }
    c = config(); c.video.capability.frame_format = MEISO_DISPLAY_INGRESS_FRAME_RGBA8888;
    CHECK(meiso_display_ingress_capability_encode(&c.video.capability, (MeisoMutableBuffer){b, 32}, &n) != 0);
    return 0;
}

static int catalogs(void)
{
    MeisoDisplayPlanesCatalogConfig c = config(), got;
    uint8_t cap[352]; MeisoRuntimeEndpointDescriptor ep[6]; MeisoRuntimeV4RouteDescriptor rt[18];
    for (unsigned mask = 0; mask < 8; ++mask) {
        c.camera_enabled = (mask & 1) != 0; c.input_enabled = (mask & 2) != 0;
        c.tracking_enabled = (mask & 4) != 0; c.local_pose_enabled = !c.tracking_enabled;
        c.planes.tracking = c.local_pose_enabled ? 3 : 2;
        const size_t ec = 4U + c.camera_enabled + c.input_enabled;
        const size_t rc = 11U + c.tracking_enabled + 3U * c.camera_enabled + 3U * c.input_enabled;
        CHECK(meiso_display_planes_build_catalog(&c, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 6, rt, 18));
        CHECK(meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
        CHECK(got.local_pose_enabled == c.local_pose_enabled && got.views_enabled && got.views.count == 2);
        CHECK(ep[ec - 1].capability.data + ep[ec - 1].capability.size == cap + 256 + 16U * c.tracking_enabled +
            64U * c.camera_enabled + 16U * c.input_enabled);
        CHECK(rt[3].route.resource.max_payload_size == 72);
        const size_t base = meiso_display_planes_tracking_route_base(&c);
        CHECK(rt[base + 2].route.resource.owner == MEISO_OWNER_ROLE_EDGE);
        if (c.local_pose_enabled) {
            CHECK(rt[base + 2].route.resource.id.low == UINT64_C(0x4c50535f504f5345) &&
                rt[base + 2].route.resource.max_payload_size == 72 && rt[base + 2].route.resource.schema.version == 1);
            rt[base + 2].route.resource.owner = MEISO_OWNER_ROLE_HOST;
            CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got)); rt[base + 2].route.resource.owner = MEISO_OWNER_ROLE_EDGE;
        }
        rt[base].route.resource.schema.version = 2;
        CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got)); rt[base].route.resource.schema.version = 1;
        MeisoRuntimeEndpointDescriptor temp = ep[ec - 1]; ep[ec - 1] = ep[1]; ep[1] = temp;
        CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got)); ep[1] = ep[ec - 1]; ep[ec - 1] = temp;
        CHECK(!meiso_display_planes_validate_catalog(ep, ec - 1, rt, rc, &got));
        CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, 3, &got));
        rt[3].route.resource.max_payload_size = 64; CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
    }
    c = config(); c.tracking_enabled = true;
    CHECK(!meiso_display_planes_build_catalog(&c, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 6, rt, 18));
    c = config(); c.views_enabled = false;
    CHECK(!meiso_display_planes_build_catalog(&c, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 6, rt, 18));
    c = config(); c.planes.view_layout = 1; c.planes.output_width = 1280; c.planes.output_height = 720;
    c.planes.fov_left = c.planes.fov_down = -0.5f; c.planes.fov_right = c.planes.fov_up = 0.5f;
    CHECK(!meiso_display_planes_build_catalog(&c, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 6, rt, 18));
    c.views_enabled = false; CHECK(meiso_display_planes_build_catalog(&c, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 6, rt, 18));
    CHECK(meiso_display_planes_validate_catalog(ep, 4, rt, 14, &got));
    c = config(); CHECK(meiso_display_planes_build_catalog(&c, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 6, rt, 18));
    MeisoDisplayIngressCapability video;
    CHECK(meiso_display_ingress_validate_catalog(ep, 1, rt, 4, &video) != 0); /* old Profiles cannot opt in */
    CHECK(meiso_display_ingress_build_catalog(&c.video, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 6, rt, 18) != 0);
    CHECK(!meiso_display_camera_build_catalog(&c.video, &c.camera, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 6, rt, 18));
    return 0;
}

/* A real pair of Sessions/Runtimes: no transport or pose-service stand-in. */
static union { MeisoMaxAlign align; uint8_t bytes[1 << 20]; } arena[4];
typedef struct { MeisoSession *s[2]; MeisoRuntime *r[2]; MeisoRuntimeRouteKey key[3]; } Pair;
static const MeisoOwnerScope host_scope = {MEISO_OWNER_ROLE_HOST, 4}, edge_scope = {MEISO_OWNER_ROLE_EDGE, 5};
static int deliver(Pair *p, unsigned from, MeisoRuntimeEvent *e)
{
    MeisoByteView b; MeisoSessionRuntimeMessage m; uint8_t copy[8192];
    CHECK(meiso_session_next_outbound(p->s[from], &b) && b.size <= sizeof(copy)); memcpy(copy, b.data, b.size);
    CHECK(meiso_session_mark_outbound_sent(p->s[from]) == MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(p->s[1 - from], (MeisoByteView){copy, b.size}, &m) == MEISO_SESSION_RECEIVE_RUNTIME);
    CHECK(meiso_runtime_receive(p->r[1 - from], &m, e) == MEISO_RUNTIME_OK); return 0;
}
static int setup(Pair *p, uint32_t peer_registry)
{
    memset(p, 0, sizeof(*p));
    for (unsigned i = 0; i < 2; ++i) {
        MeisoSessionConfig c = {0}; c.local_role = i ? MEISO_SESSION_ROLE_EDGE : MEISO_SESSION_ROLE_HOST;
        c.local_peer_id = (MeisoPeerId){i ? 2 : 1, 3}; c.expected_peer_id = (MeisoPeerId){i ? 1 : 2, 3};
        c.local_incarnation = 4U + i; c.local_nonce = (MeisoSessionNonce){6U + i, 8}; c.protocol = (MeisoProtocolVersion){1, 0};
        CHECK(meiso_display_planes_configure_session(&c)); if (i) c.limits.max_registry_entries = peer_registry;
        CHECK(meiso_session_init(arena[i].bytes, sizeof(arena[i].bytes), &c, &p->s[i]) == MEISO_SESSION_INIT_OK);
        CHECK(meiso_session_start(p->s[i]) == MEISO_SESSION_START_STARTED);
    }
    for (unsigned round = 0; round < 4; ++round) {
        const unsigned from = round % 2; MeisoByteView b; uint8_t copy[512]; MeisoSessionRuntimeMessage m;
        if (!meiso_session_next_outbound(p->s[from], &b)) continue;
        CHECK(b.size <= sizeof(copy)); memcpy(copy, b.data, b.size);
        CHECK(meiso_session_mark_outbound_sent(p->s[from]) == MEISO_SESSION_OUTBOUND_SENT);
        CHECK(meiso_session_receive(p->s[1 - from], (MeisoByteView){copy, b.size}, &m) == MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS);
    }
    for (unsigned i = 0; i < 2; ++i) {
        MeisoRuntimeV4Config c;
        CHECK(meiso_display_planes_session_allows_edge_extensions(p->s[i]) == (peer_registry >= 24));
        CHECK(meiso_display_planes_configure_runtime(&c, p->s[i], 262144));
        CHECK(c.base.max_capability_bytes == (peer_registry >= 24 ? 352U : 208U));
        CHECK(meiso_runtime_v4_init(arena[i + 2].bytes, sizeof(arena[i + 2].bytes), &c, &p->r[i]) == MEISO_RUNTIME_INIT_OK);
    }
    return 0;
}
static int state(Pair *p, bool actual, MeisoRuntimeProductionStatus production, uint64_t version)
{
    uint8_t b[128], profile[16]; MeisoByteView payload; MeisoRuntimeEvent e;
    if (actual) {
        MeisoRuntimeActualState a = {0};
        a.observed_desired = a.applied_desired = (MeisoRuntimeDesiredReference){true, host_scope, 1};
        a.apply_result = MEISO_RUNTIME_APPLY_APPLIED; a.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
        a.effective_enabled = production == MEISO_RUNTIME_PRODUCTION_RUNNING; a.production_status = production;
        a.production_epoch = a.effective_enabled ? version : 0;
        const MeisoDisplayLocalPoseActual pa = {a.effective_enabled ? MEISO_DISPLAY_LOCAL_POSE_RUNNING : MEISO_DISPLAY_LOCAL_POSE_FAILED};
        CHECK(meiso_display_local_pose_actual_encode(&pa, (MeisoMutableBuffer){profile, 16}));
        a.profile = (MeisoByteView){profile, 16}; CHECK(meiso_runtime_actual_state_encode(&a, (MeisoMutableBuffer){b, sizeof(b)}, &payload));
    } else {
        CHECK(meiso_display_local_pose_desired_encode(&(MeisoDisplayLocalPoseDesired){true}, (MeisoMutableBuffer){profile, 16}));
        CHECK(meiso_runtime_desired_state_encode(&(MeisoRuntimeDesiredState){true, {profile, 16}}, (MeisoMutableBuffer){b, sizeof(b)}, &payload));
    }
    const unsigned from = actual ? 1 : 0, route = actual ? 1 : 0;
    const MeisoOwnerScope owner = actual ? edge_scope : host_scope;
    const MeisoStateOffer offer = {{actual ? 20 : 19}, {owner, version, payload}};
    CHECK(meiso_runtime_send_state_offer(p->r[from], p->key[route], &offer) == MEISO_RUNTIME_OK);
    CHECK(deliver(p, from, &e) == 0 && e.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    CHECK(meiso_runtime_commit_state_receive(p->r[1 - from], e.route, &e.state_offer, MEISO_STATE_RECEIVE_NEW) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_state_accept(p->r[1 - from], e.route, offer.generation, owner, version) == MEISO_RUNTIME_OK);
    CHECK(deliver(p, 1 - from, &e) == 0 && e.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT); return 0;
}
static int runtime(void)
{
    Pair p; MeisoRuntimeEvent e;
    const uint32_t registries[] = {14, 19, 23, 24};
    for (unsigned i = 0; i < 4; ++i) CHECK(setup(&p, registries[i]) == 0);
    MeisoDisplayPlanesCatalogConfig c = config();
    uint8_t cap[352]; MeisoRuntimeEndpointDescriptor ep[6]; MeisoRuntimeV4RouteDescriptor rt[18];
    CHECK(meiso_display_planes_build_catalog(&c, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 6, rt, 18));
    CHECK(meiso_runtime_v4_offer_catalog(p.r[1], 1, ep, 5, rt, 14) == MEISO_RUNTIME_OK);
    CHECK(deliver(&p, 1, &e) == 0 && e.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED);
    CHECK(meiso_runtime_accept_catalog(p.r[0]) == MEISO_RUNTIME_OK);
    CHECK(deliver(&p, 0, &e) == 0 && e.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED);
    for (unsigned i = 0; i < 3; ++i) {
        const MeisoRuntimeRouteDescriptor *r = &rt[8 + i].route;
        p.key[i] = (MeisoRuntimeRouteKey){1, r->endpoint_id, r->endpoint_incarnation, r->resource.id, r->facet};
    }
    CHECK(state(&p, false, MEISO_RUNTIME_PRODUCTION_STOPPED, 1) == 0);
    CHECK(state(&p, true, MEISO_RUNTIME_PRODUCTION_RUNNING, 1) == 0);
    MeisoDisplayLocalPose pose = {0}; pose.time_ns = 1000; pose.world_epoch = 7; pose.quality = 1; pose.orientation[3] = 1;
    uint8_t b[72]; CHECK(meiso_display_local_pose_encode(&pose, (MeisoMutableBuffer){b, sizeof(b)}));
    MeisoStreamSample sample = {{edge_scope, 1, 1}, {4, 1000}, {0}, {NULL, 0}, {b, sizeof(b)}};
    for (unsigned i = 0; i < 3; ++i) {
        sample.key.sequence = i + 1U; b[13] = i == 1 ? 1 : 0;
        CHECK(meiso_runtime_send_stream_sample(p.r[1], p.key[2], 1, &sample) == MEISO_RUNTIME_OK);
        CHECK(deliver(&p, 1, &e) == 0 && e.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE);
        MeisoDisplayLocalPose got;
        CHECK(meiso_display_local_pose_decode(e.stream_sample.payload, &got) == (i != 1));
        CHECK(e.stream_sample.source_time.clock_epoch == 4 && e.stream_sample.source_time.monotonic_ns == pose.time_ns);
    }
    sample.key.sequence = 4;
    CHECK(meiso_runtime_send_stream_sample(p.r[1], p.key[2], 2, &sample) != MEISO_RUNTIME_OK); /* wrong production */
    sample.key.producer = host_scope;
    CHECK(meiso_runtime_send_stream_sample(p.r[1], p.key[2], 1, &sample) != MEISO_RUNTIME_OK); sample.key.producer = edge_scope;
    CHECK(state(&p, true, MEISO_RUNTIME_PRODUCTION_FAILED, 2) == 0);
    CHECK(meiso_runtime_send_stream_sample(p.r[1], p.key[2], 1, &sample) != MEISO_RUNTIME_OK);
    CHECK(state(&p, true, MEISO_RUNTIME_PRODUCTION_RUNNING, 3) == 0);
    CHECK(meiso_runtime_send_stream_sample(p.r[1], p.key[2], 3, &sample) == MEISO_RUNTIME_OK);
    CHECK(deliver(&p, 1, &e) == 0 && e.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE);
    for (unsigned i = 0; i < 2; ++i) { MeisoSessionSnapshot s; CHECK(meiso_session_snapshot(p.s[i], &s) && s.state == MEISO_SESSION_STATE_READY); }
    return 0;
}
int main(void)
{
    CHECK(codecs() == 0); CHECK(evidence() == 0); CHECK(catalogs() == 0); CHECK(runtime() == 0);
    puts("display ingress v12 Edge extensions: ok"); return 0;
}
