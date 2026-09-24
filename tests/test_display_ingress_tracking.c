#include <meiso/profiles/display_ingress/planes.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

typedef union { max_align_t align; uint8_t bytes[65536]; } Arena;
static Arena session_memory[2], runtime_memory;

/* Two planes Sessions through the handshake; old_peer negotiates the registry of a peer from
 * before tracking (3 endpoints + 11 routes). */
static int handshake(bool old_peer, MeisoSession **host, MeisoSession **edge)
{
    MeisoSession *s[2];
    for (int i = 0; i < 2; ++i) {
        MeisoSessionConfig cfg = {0};
        cfg.local_role = i ? MEISO_SESSION_ROLE_EDGE : MEISO_SESSION_ROLE_HOST;
        cfg.local_peer_id = (MeisoPeerId){i ? 2 : 1, 3}; cfg.expected_peer_id = (MeisoPeerId){i ? 1 : 2, 3};
        cfg.local_incarnation = 4U + (uint64_t)i; cfg.local_nonce = (MeisoSessionNonce){6U + (uint64_t)i, 8};
        cfg.protocol = (MeisoProtocolVersion){1, 0};
        CHECK(meiso_display_planes_configure_session(&cfg) && cfg.limits.max_registry_entries == 19);
        if (old_peer && i) cfg.limits.max_registry_entries = 14;
        CHECK(meiso_session_init(session_memory[i].bytes, sizeof(session_memory[i].bytes), &cfg, &s[i]) == MEISO_SESSION_INIT_OK);
        CHECK(meiso_session_start(s[i]) == MEISO_SESSION_START_STARTED);
    }
    for (int round = 0; round < 4; ++round) {
        MeisoSession *from = s[round % 2], *to = s[1 - round % 2];
        MeisoByteView bytes;
        uint8_t copy[512];
        MeisoSessionRuntimeMessage message;
        if (!meiso_session_next_outbound(from, &bytes)) continue;
        CHECK(bytes.size <= sizeof(copy));
        memcpy(copy, bytes.data, bytes.size);
        CHECK(meiso_session_mark_outbound_sent(from) == MEISO_SESSION_OUTBOUND_SENT);
        CHECK(meiso_session_receive(to, (MeisoByteView){copy, bytes.size}, &message) == MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS);
    }
    *host = s[0]; *edge = s[1];
    return 0;
}

/* A peer from before tracking still gets a Session: the Runtime is sized without the tracking
 * endpoint, and says so. */
static int session_sizing(void)
{
    for (int old_peer = 0; old_peer < 2; ++old_peer) {
        MeisoSession *host, *edge;
        CHECK(handshake(old_peer, &host, &edge) == 0);
        MeisoSessionSnapshot snapshot;
        CHECK(meiso_session_snapshot(host, &snapshot) && snapshot.state == MEISO_SESSION_STATE_READY);
        CHECK(meiso_display_planes_session_allows_tracking(host) == !old_peer &&
              meiso_display_planes_session_allows_tracking(edge) == !old_peer);
        MeisoRuntimeV4Config rc;
        MeisoRuntime *runtime;
        CHECK(meiso_display_planes_configure_runtime(&rc, host, 65536));
        CHECK(rc.base.max_endpoints == (old_peer ? 3U : 4U) && rc.base.max_routes == (old_peer ? 11U : 15U));
        CHECK(meiso_runtime_v4_init(runtime_memory.bytes, sizeof(runtime_memory.bytes), &rc, &runtime) == MEISO_RUNTIME_INIT_OK);
    }
    return 0;
}

/* planes-v12.md §10: the tracking endpoint. */
int main(void)
{
    uint8_t b[MEISO_DISPLAY_TRACKING_MAX_IMU_BYTES];

    /* capability: 200 Hz, ±16 g, ±2000 dps, batches of 8, velocity extrapolation, 100 ms */
    const MeisoDisplayTrackingCapability c = {200000, 156.9064f, 34.9066f, 8, MEISO_DISPLAY_TRACKING_CORRECTION_NONE, 100};
    MeisoDisplayTrackingCapability cd, bad;
    CHECK(meiso_display_tracking_capability_encode(&c, (MeisoMutableBuffer){b, 32}));
    CHECK(b[0] == 0 && b[1] == 0x03 && b[2] == 0x0d && b[3] == 0x40 && b[4] == 0x43 && b[5] == 0x1c && b[12] == 8 && b[13] == 0 && b[14] == 0 && b[15] == 100);
    CHECK(meiso_display_tracking_capability_decode((MeisoByteView){b, 32}, &cd) && cd.imu_rate_millihz == 200000 &&
          cd.accel_range_mps2 == c.accel_range_mps2 && cd.max_prediction_ms == 100);
    b[20] = 1; CHECK(!meiso_display_tracking_capability_decode((MeisoByteView){b, 32}, &cd)); b[20] = 0;
    CHECK(!meiso_display_tracking_capability_decode((MeisoByteView){b, 31}, &cd));
    bad = c; bad.max_imu_batch = 0; CHECK(!meiso_display_tracking_capability_encode(&bad, (MeisoMutableBuffer){b, 32}));
    bad = c; bad.max_imu_batch = 33; CHECK(!meiso_display_tracking_capability_encode(&bad, (MeisoMutableBuffer){b, 32}));
    bad = c; bad.max_prediction_ms = 201; CHECK(!meiso_display_tracking_capability_encode(&bad, (MeisoMutableBuffer){b, 32}));
    bad = c; bad.correction = 2; CHECK(!meiso_display_tracking_capability_encode(&bad, (MeisoMutableBuffer){b, 32}));
    bad = c; bad.imu_rate_millihz = 40000; CHECK(!meiso_display_tracking_capability_encode(&bad, (MeisoMutableBuffer){b, 32}));
    bad = c; bad.gyro_range_rps = NAN; CHECK(!meiso_display_tracking_capability_encode(&bad, (MeisoMutableBuffer){b, 32}));
    /* pose only: no IMU, so no ranges and no IMU correction */
    const MeisoDisplayTrackingCapability pose_only = {0, 0.0f, 0.0f, 1, MEISO_DISPLAY_TRACKING_CORRECTION_NONE, 50};
    CHECK(meiso_display_tracking_capability_encode(&pose_only, (MeisoMutableBuffer){b, 32}));
    bad = pose_only; bad.accel_range_mps2 = 1.0f; CHECK(!meiso_display_tracking_capability_encode(&bad, (MeisoMutableBuffer){b, 32}));
    bad = pose_only; bad.correction = MEISO_DISPLAY_TRACKING_CORRECTION_IMU;
    CHECK(!meiso_display_tracking_capability_encode(&bad, (MeisoMutableBuffer){b, 32}));

    /* desired and actual */
    MeisoDisplayTrackingDesired d = {true, true}, dd;
    CHECK(meiso_display_tracking_desired_encode(&d, (MeisoMutableBuffer){b, 16}) && b[0] == 1 && b[1] == 1 && b[15] == 0);
    CHECK(meiso_display_tracking_desired_decode((MeisoByteView){b, 16}, &dd) && dd.imu_enabled && dd.pose_enabled);
    b[0] = 2; CHECK(!meiso_display_tracking_desired_decode((MeisoByteView){b, 16}, &dd)); b[0] = 1;
    b[9] = 1; CHECK(!meiso_display_tracking_desired_decode((MeisoByteView){b, 16}, &dd)); b[9] = 0;
    CHECK(meiso_display_tracking_desired_supported(&c, &d) && !meiso_display_tracking_desired_supported(&pose_only, &d));
    d.imu_enabled = false; CHECK(meiso_display_tracking_desired_supported(&pose_only, &d));
    MeisoDisplayTrackingActual a = {MEISO_DISPLAY_TRACKING_IMU_FAILED, MEISO_DISPLAY_TRACKING_POSE_APPLYING}, ad;
    CHECK(meiso_display_tracking_actual_encode(&a, (MeisoMutableBuffer){b, 16}) && b[0] == 2 && b[1] == 1);
    CHECK(meiso_display_tracking_actual_decode((MeisoByteView){b, 16}, &ad) && ad.imu_state == MEISO_DISPLAY_TRACKING_IMU_FAILED &&
          ad.pose_state == MEISO_DISPLAY_TRACKING_POSE_APPLYING);
    b[1] = 2; CHECK(!meiso_display_tracking_actual_decode((MeisoByteView){b, 16}, &ad));
    a.imu_state = 3; CHECK(!meiso_display_tracking_actual_encode(&a, (MeisoMutableBuffer){b, 16}));

    /* IMU record: three samples 5 ms apart */
    MeisoDisplayTrackingImu m, md, x;
    memset(&m, 0, sizeof(m));
    m.first_sequence = 41; m.count = 3;
    for (int i = 0; i < 3; ++i) {
        m.samples[i].time_ns = 1000000000ULL + 5000000ULL * (uint64_t)i;
        m.samples[i].accel_mps2[2] = 9.80665f; m.samples[i].gyro_rps[1] = 0.01f * (float)i;
    }
    CHECK(meiso_display_tracking_imu_encode(&c, &m, (MeisoMutableBuffer){b, sizeof(b)}) == 16 + 3 * 32);
    CHECK(b[7] == 41 && b[8] == 3 && b[16 + 7] == 0x00 && b[16 + 4] == 0x3b && b[48 + 5] == 0xe7);
    CHECK(meiso_display_tracking_imu_decode(&c, (MeisoByteView){b, 112}, &md) && md.count == 3 && md.first_sequence == 41 &&
          md.samples[2].time_ns == 1010000000ULL && md.samples[2].gyro_rps[1] == 0.02f);
    CHECK(!meiso_display_tracking_imu_decode(&c, (MeisoByteView){b, 111}, &md));
    CHECK(!meiso_display_tracking_imu_decode(&c, (MeisoByteView){b, 80}, &md)); /* count says 3 */
    b[10] = 1; CHECK(!meiso_display_tracking_imu_decode(&c, (MeisoByteView){b, 112}, &md)); b[10] = 0;
    CHECK(!meiso_display_tracking_imu_decode(&pose_only, (MeisoByteView){b, 112}, &md));
    x = m; x.samples[2].time_ns = x.samples[1].time_ns; CHECK(!meiso_display_tracking_imu_encode(&c, &x, (MeisoMutableBuffer){b, sizeof(b)}));
    x = m; x.samples[0].accel_mps2[0] = 180.0f; CHECK(!meiso_display_tracking_imu_encode(&c, &x, (MeisoMutableBuffer){b, sizeof(b)}));
    x = m; x.samples[1].gyro_rps[2] = NAN; CHECK(!meiso_display_tracking_imu_encode(&c, &x, (MeisoMutableBuffer){b, sizeof(b)}));
    x = m; x.first_sequence = 0; CHECK(!meiso_display_tracking_imu_encode(&c, &x, (MeisoMutableBuffer){b, sizeof(b)}));
    x = m; x.count = 9; CHECK(!meiso_display_tracking_imu_encode(&c, &x, (MeisoMutableBuffer){b, sizeof(b)})); /* over the batch */
    CHECK(!meiso_display_tracking_imu_encode(&c, &m, (MeisoMutableBuffer){b, 100}));

    /* pose */
    MeisoDisplayTrackingPose p = {2000000000ULL, 3, MEISO_DISPLAY_TRACKING_TRACKING, {0.1f, 1.6f, -0.2f},
        {0.0f, 0.3826834f, 0.0f, 0.9238795f}, {0.5f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, pd, y;
    CHECK(meiso_display_tracking_pose_encode(&p, (MeisoMutableBuffer){b, 72}) && b[11] == 3 && b[12] == 1 && b[71] == 0);
    CHECK(meiso_display_tracking_pose_decode((MeisoByteView){b, 72}, &pd) && pd.time_ns == p.time_ns && pd.world_epoch == 3 &&
          pd.orientation[1] == p.orientation[1] && pd.angular[1] == 1.0f && pd.velocity[0] == 0.5f);
    b[13] = 1; CHECK(!meiso_display_tracking_pose_decode((MeisoByteView){b, 72}, &pd)); b[13] = 0;
    b[70] = 1; CHECK(!meiso_display_tracking_pose_decode((MeisoByteView){b, 72}, &pd)); b[70] = 0;
    y = p; y.world_epoch = 0; CHECK(!meiso_display_tracking_pose_encode(&y, (MeisoMutableBuffer){b, 72}));
    y = p; y.time_ns = 0; CHECK(!meiso_display_tracking_pose_encode(&y, (MeisoMutableBuffer){b, 72}));
    y = p; y.quality = 3; CHECK(!meiso_display_tracking_pose_encode(&y, (MeisoMutableBuffer){b, 72}));
    y = p; y.orientation[3] = 0.8f; CHECK(!meiso_display_tracking_pose_encode(&y, (MeisoMutableBuffer){b, 72}));
    y = p; y.velocity[1] = 21.0f; CHECK(!meiso_display_tracking_pose_encode(&y, (MeisoMutableBuffer){b, 72}));
    y = p; y.position[0] = INFINITY; CHECK(!meiso_display_tracking_pose_encode(&y, (MeisoMutableBuffer){b, 72}));

    /* catalog: display + planes [+ camera] + tracking */
    MeisoDisplayPlanesCatalogConfig cc, got;
    memset(&cc, 0, sizeof(cc));
    cc.video = (MeisoDisplayIngressCatalogConfig){1, {1280, 720, 30000, 131072, 1024,
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_DRM_KMS_PAGE_FLIP, MEISO_STREAM_DEADLINE_UNSUPPORTED, MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    cc.planes = (MeisoDisplayPlanesCapability){1920, 1080, MEISO_DISPLAY_PLANES_VIEW_MONO, MEISO_DISPLAY_PLANES_TRACKING_POSE,
        16, 4, 262144, 2048, 2048, -0.4363f, 0.4363f, 0.2531f, -0.2531f, 0.0f, 30};
    cc.camera = (MeisoCompositionMediaCapability){.source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        .pixel_formats = MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888, .codecs = MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        .min_width = 320, .min_height = 240, .max_width = 320, .max_height = 240, .max_fps_millihz = 30000,
        .max_access_unit_bytes = 307200, .max_fragment_payload_bytes = 4096};
    cc.tracking_enabled = true;
    cc.tracking = c;
    uint8_t cap[MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor ep[MEISO_DISPLAY_PLANES_MAX_ENDPOINTS];
    MeisoRuntimeV4RouteDescriptor rt[MEISO_DISPLAY_PLANES_MAX_ROUTES];
    for (unsigned camera = 0; camera < 2; ++camera) {
        cc.camera_enabled = camera != 0;
        const size_t ec = 3 + camera, rc = 12 + 3 * camera, base = meiso_display_planes_tracking_route_base(&cc);
        CHECK(base == 8 + 3 * camera);
        CHECK(meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 4, rt, 15));
        CHECK(meiso_endpoint_id_equal(ep[ec - 1].id, *meiso_display_tracking_endpoint()) && ep[ec - 1].capability.size == 32);
        CHECK(rt[base].route.resource.owner == MEISO_OWNER_ROLE_HOST && rt[base].route.facet == MEISO_RUNTIME_FACET_DESIRED);
        CHECK(rt[base + 1].route.resource.owner == MEISO_OWNER_ROLE_EDGE && rt[base + 1].route.facet == MEISO_RUNTIME_FACET_ACTUAL);
        CHECK(rt[base + 2].route.resource.owner == MEISO_OWNER_ROLE_EDGE && rt[base + 2].route.resource.kind == MEISO_RESOURCE_KIND_STREAM &&
              rt[base + 2].route.resource.max_payload_size == MEISO_DISPLAY_TRACKING_MAX_IMU_BYTES);
        CHECK(rt[base + 3].route.resource.owner == MEISO_OWNER_ROLE_HOST && rt[base + 3].route.facet == MEISO_RUNTIME_FACET_DATA &&
              rt[base + 3].route.resource.max_payload_size == MEISO_DISPLAY_TRACKING_POSE_BYTES);
        CHECK(meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
        CHECK(got.tracking_enabled && got.camera_enabled == cc.camera_enabled && got.tracking.max_imu_batch == 8 &&
              got.planes.tracking == MEISO_DISPLAY_PLANES_TRACKING_POSE);
        CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc - 1, &got));
        rt[base + 3].route.resource.owner = MEISO_OWNER_ROLE_EDGE;
        CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
        rt[base + 3].route.resource.owner = MEISO_OWNER_ROLE_HOST;
        cap[96 + 64 * camera + 20] = 1; CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
        cap[96 + 64 * camera + 20] = 0;
    }
    /* Tracking must come after the camera. */
    const MeisoRuntimeEndpointDescriptor swapped = ep[2];
    ep[2] = ep[3]; ep[3] = swapped;
    CHECK(!meiso_display_planes_validate_catalog(ep, 4, rt, 15, &got));
    /* Planes capability and the tracking endpoint agree: POSE with it, NONE without. */
    cc.planes.tracking = MEISO_DISPLAY_PLANES_TRACKING_NONE;
    CHECK(!meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 4, rt, 15));
    cc.tracking_enabled = false;
    CHECK(meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 4, rt, 15));
    cc.planes.tracking = MEISO_DISPLAY_PLANES_TRACKING_POSE;
    CHECK(!meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 4, rt, 15));
    cc.tracking_enabled = true; cc.tracking.max_imu_batch = 0;
    CHECK(!meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 4, rt, 15));
    CHECK(session_sizing() == 0);
    printf("display ingress tracking v12: ok\n");
    return 0;
}
