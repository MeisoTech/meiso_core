#include <meiso/profiles/display_ingress/planes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

typedef union { MeisoMaxAlign align; uint8_t bytes[65536]; } Arena;
static Arena session_memory[2], runtime_memory;

/* Two planes Sessions through the handshake; the Edge offers peer_registry (23 now, 19 before §11,
 * 14 before §10). */
static int handshake(uint32_t peer_registry, MeisoSession **host, MeisoSession **edge)
{
    MeisoSession *s[2];
    for (int i = 0; i < 2; ++i) {
        MeisoSessionConfig cfg = {0};
        cfg.local_role = i ? MEISO_SESSION_ROLE_EDGE : MEISO_SESSION_ROLE_HOST;
        cfg.local_peer_id = (MeisoPeerId){i ? 2 : 1, 3}; cfg.expected_peer_id = (MeisoPeerId){i ? 1 : 2, 3};
        cfg.local_incarnation = 4U + (uint64_t)i; cfg.local_nonce = (MeisoSessionNonce){6U + (uint64_t)i, 8};
        cfg.protocol = (MeisoProtocolVersion){1, 0};
        CHECK(meiso_display_planes_configure_session(&cfg) && cfg.limits.max_registry_entries == MEISO_DISPLAY_PLANES_AUDIO_REGISTRY);
        if (i) cfg.limits.max_registry_entries = peer_registry;
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

/* Each older peer still gets a Session; the Runtime is sized to what it can hold. */
static int session_sizing(void)
{
    static const struct { uint32_t registry; bool tracking, input; uint32_t endpoints, routes; } peers[] = {
        {23, true, true, 5, 18}, {19, true, false, 4, 15}, {14, false, false, 3, 11}};
    for (unsigned i = 0; i < 3; ++i) {
        MeisoSession *host, *edge;
        CHECK(handshake(peers[i].registry, &host, &edge) == 0);
        CHECK(meiso_display_planes_session_allows_input(host) == peers[i].input &&
              meiso_display_planes_session_allows_input(edge) == peers[i].input &&
              meiso_display_planes_session_allows_tracking(host) == peers[i].tracking);
        MeisoRuntimeV4Config rc;
        MeisoRuntime *runtime;
        CHECK(meiso_display_planes_configure_runtime(&rc, host, 65536));
        CHECK(rc.base.max_endpoints == peers[i].endpoints && rc.base.max_routes == peers[i].routes);
        CHECK(meiso_runtime_v4_init(runtime_memory.bytes, sizeof(runtime_memory.bytes), &rc, &runtime) == MEISO_RUNTIME_INIT_OK);
    }
    return 0;
}

static union { MeisoMaxAlign align; uint8_t bytes[1 << 20]; } runtime_pair[2];

/* One Session message from one peer to the other; a Runtime message comes back as its event. */
static int deliver(MeisoSession *from, MeisoSession *to, MeisoRuntime *runtime, MeisoRuntimeStatus *status, MeisoRuntimeEvent *event)
{
    MeisoByteView bytes;
    uint8_t copy[8192];
    MeisoSessionRuntimeMessage message;
    CHECK(meiso_session_next_outbound(from, &bytes) && bytes.size <= sizeof(copy));
    memcpy(copy, bytes.data, bytes.size);
    CHECK(meiso_session_mark_outbound_sent(from) == MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(to, (MeisoByteView){copy, bytes.size}, &message) == MEISO_SESSION_RECEIVE_RUNTIME);
    *status = meiso_runtime_receive(runtime, &message, event);
    return 0;
}

typedef struct {
    MeisoSession *hs, *es;
    MeisoRuntime *hr, *er;
    MeisoRuntimeRouteKey key[3];
    uint64_t epoch, actual_version;
} Pair;

static const MeisoOwnerScope host_scope = {MEISO_OWNER_ROLE_HOST, 4}, edge_scope = {MEISO_OWNER_ROLE_EDGE, 5};

/* The Edge publishes an input Actual under the next production epoch; the Host commits and accepts it. */
static int publish_actual(Pair *p, MeisoDisplayInputActual state)
{
    uint8_t profile[16], wire[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE + 16];
    MeisoByteView payload;
    MeisoRuntimeStatus status;
    MeisoRuntimeEvent event;
    MeisoRuntimeActualState a;
    memset(&a, 0, sizeof(a));
    a.observed_desired = (MeisoRuntimeDesiredReference){true, host_scope, 1};
    a.applied_desired = a.observed_desired;
    a.apply_result = MEISO_RUNTIME_APPLY_APPLIED; a.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    a.effective_enabled = true; a.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING; a.production_epoch = ++p->epoch;
    a.profile = (MeisoByteView){profile, 16};
    CHECK(meiso_display_input_actual_encode(&state, (MeisoMutableBuffer){profile, 16}) &&
          meiso_runtime_actual_state_encode(&a, (MeisoMutableBuffer){wire, sizeof(wire)}, &payload));
    const MeisoStateOffer actual = {{MEISO_DISPLAY_INPUT_ACTUAL_GENERATION}, {edge_scope, ++p->actual_version, payload}};
    CHECK(meiso_runtime_send_state_offer(p->er, p->key[MEISO_DISPLAY_INPUT_ROUTE_ACTUAL], &actual) == MEISO_RUNTIME_OK);
    CHECK(deliver(p->es, p->hs, p->hr, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    MeisoDisplayInputActual got;
    CHECK(meiso_display_input_actual_decode((MeisoByteView){profile, 16}, &got) && got.locked == state.locked);
    CHECK(meiso_runtime_commit_state_receive(p->hr, event.route, &event.state_offer, MEISO_STATE_RECEIVE_NEW) == MEISO_RUNTIME_OK &&
          meiso_runtime_send_state_accept(p->hr, event.route, actual.generation, edge_scope, p->actual_version) == MEISO_RUNTIME_OK);
    CHECK(deliver(p->hs, p->es, p->er, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT);
    return 0;
}

/* One key event from the Edge under the current epoch; *decoded tells whether the Host could read it. */
static int send_key(Pair *p, const MeisoDisplayInputCapability *c, const MeisoDisplayInputKeyEvent *k, uint64_t sequence,
    bool corrupt, bool *decoded, MeisoDisplayInputKeyEvent *got)
{
    uint8_t bytes[MEISO_DISPLAY_INPUT_KEY_BYTES];
    MeisoRuntimeStatus status;
    MeisoRuntimeEvent event;
    CHECK(meiso_display_input_key_encode(c, k, (MeisoMutableBuffer){bytes, sizeof(bytes)}));
    if (corrupt) bytes[10] = 1;
    const MeisoStreamSample sample = {{edge_scope, MEISO_DISPLAY_INPUT_KEY_STREAM_EPOCH, sequence}, {4, k->time_ns},
        {0}, {NULL, 0}, {bytes, sizeof(bytes)}};
    CHECK(meiso_runtime_send_stream_sample(p->er, p->key[MEISO_DISPLAY_INPUT_ROUTE_KEY], p->epoch, &sample) == MEISO_RUNTIME_OK);
    CHECK(deliver(p->es, p->hs, p->hr, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE);
    *decoded = meiso_display_input_key_decode(c, event.stream_sample.payload, got);
    return 0;
}

/* §11 through both Runtimes: keys arrive in order; locking and unlocking each change the production
 * epoch and later keys go on under the new one; a bad event is the Host's to drop. */
static int keys_through_runtimes(const MeisoDisplayPlanesCatalogConfig *cc)
{
    Pair p;
    memset(&p, 0, sizeof(p));
    MeisoRuntimeV4Config rc;
    MeisoRuntimeStatus status;
    MeisoRuntimeEvent event;
    CHECK(handshake(MEISO_DISPLAY_PLANES_INPUT_REGISTRY, &p.hs, &p.es) == 0);
    CHECK(meiso_display_planes_configure_runtime(&rc, p.hs, cc->planes.max_image_bytes) &&
          meiso_runtime_v4_init(runtime_pair[0].bytes, sizeof(runtime_pair[0].bytes), &rc, &p.hr) == MEISO_RUNTIME_INIT_OK);
    CHECK(meiso_display_planes_configure_runtime(&rc, p.es, cc->planes.max_image_bytes) &&
          meiso_runtime_v4_init(runtime_pair[1].bytes, sizeof(runtime_pair[1].bytes), &rc, &p.er) == MEISO_RUNTIME_INIT_OK);
    uint8_t cap[MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor ep[MEISO_DISPLAY_PLANES_MAX_ENDPOINTS];
    MeisoRuntimeV4RouteDescriptor rt[MEISO_DISPLAY_PLANES_MAX_ROUTES];
    CHECK(!cc->camera_enabled && cc->tracking_enabled && cc->input_enabled);
    CHECK(meiso_display_planes_build_catalog(cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 4, rt, 15));
    CHECK(meiso_runtime_v4_offer_catalog(p.er, 1, ep, 4, rt, 15) == MEISO_RUNTIME_OK);
    CHECK(deliver(p.es, p.hs, p.hr, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED);
    CHECK(meiso_runtime_accept_catalog(p.hr) == MEISO_RUNTIME_OK);
    CHECK(deliver(p.hs, p.es, p.er, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED);
    const size_t base = meiso_display_planes_input_route_base(cc);
    for (unsigned i = 0; i < 3; ++i) {
        const MeisoRuntimeRouteDescriptor *r = &rt[base + i].route;
        p.key[i] = (MeisoRuntimeRouteKey){1, r->endpoint_id, r->endpoint_incarnation, r->resource.id, r->facet};
    }

    /* the Host wants keys */
    uint8_t profile[16], wire[MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE + 16];
    MeisoByteView payload;
    CHECK(meiso_display_input_desired_encode(&(MeisoDisplayInputDesired){true}, (MeisoMutableBuffer){profile, 16}) &&
          meiso_runtime_desired_state_encode(&(MeisoRuntimeDesiredState){true, {profile, 16}}, (MeisoMutableBuffer){wire, sizeof(wire)}, &payload));
    const MeisoStateOffer desired = {{MEISO_DISPLAY_INPUT_DESIRED_GENERATION}, {host_scope, 1, payload}};
    CHECK(meiso_runtime_send_state_offer(p.hr, p.key[MEISO_DISPLAY_INPUT_ROUTE_DESIRED], &desired) == MEISO_RUNTIME_OK);
    CHECK(deliver(p.hs, p.es, p.er, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    CHECK(meiso_runtime_commit_state_receive(p.er, event.route, &event.state_offer, MEISO_STATE_RECEIVE_NEW) == MEISO_RUNTIME_OK &&
          meiso_runtime_send_state_accept(p.er, event.route, desired.generation, host_scope, 1) == MEISO_RUNTIME_OK);
    CHECK(deliver(p.es, p.hs, p.hr, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT);
    CHECK(publish_actual(&p, (MeisoDisplayInputActual){MEISO_DISPLAY_INPUT_KEYS_RUNNING, false}) == 0);

    /* SELECT pressed on plane 7 of generation 3, released; then a corrupt DOWN, dropped; the Session goes on */
    MeisoDisplayInputKeyEvent k = {1000000000ULL, MEISO_DISPLAY_INPUT_KEY_SELECT, MEISO_DISPLAY_INPUT_PRESS, 7, 3}, got;
    bool decoded;
    uint64_t sequence = 0;
    CHECK(send_key(&p, &cc->input, &k, ++sequence, false, &decoded, &got) == 0 && decoded && got.key == MEISO_DISPLAY_INPUT_KEY_SELECT &&
          got.action == MEISO_DISPLAY_INPUT_PRESS && got.target_plane == 7 && got.target_generation == 3 && got.time_ns == k.time_ns);
    k.action = MEISO_DISPLAY_INPUT_RELEASE; k.time_ns += 120000000ULL;
    CHECK(send_key(&p, &cc->input, &k, ++sequence, false, &decoded, &got) == 0 && decoded && got.action == MEISO_DISPLAY_INPUT_RELEASE &&
          got.time_ns == 1120000000ULL);
    k = (MeisoDisplayInputKeyEvent){1200000000ULL, MEISO_DISPLAY_INPUT_KEY_DOWN, MEISO_DISPLAY_INPUT_PRESS, 0, 0};
    CHECK(send_key(&p, &cc->input, &k, ++sequence, true, &decoded, &got) == 0 && !decoded);

    /* locked: a new epoch; unlocked: another; the next key goes on under it */
    CHECK(publish_actual(&p, (MeisoDisplayInputActual){MEISO_DISPLAY_INPUT_KEYS_RUNNING, true}) == 0 && p.epoch == 2);
    CHECK(publish_actual(&p, (MeisoDisplayInputActual){MEISO_DISPLAY_INPUT_KEYS_RUNNING, false}) == 0 && p.epoch == 3);
    k = (MeisoDisplayInputKeyEvent){3000000000ULL, MEISO_DISPLAY_INPUT_KEY_SYSTEM, MEISO_DISPLAY_INPUT_PRESS, 0, 0};
    CHECK(send_key(&p, &cc->input, &k, ++sequence, false, &decoded, &got) == 0 && decoded && got.key == MEISO_DISPLAY_INPUT_KEY_SYSTEM);
    /* a key under the old epoch is not the Edge's to send any more */
    uint8_t bytes[MEISO_DISPLAY_INPUT_KEY_BYTES];
    CHECK(meiso_display_input_key_encode(&cc->input, &k, (MeisoMutableBuffer){bytes, sizeof(bytes)}));
    const MeisoStreamSample stale = {{edge_scope, MEISO_DISPLAY_INPUT_KEY_STREAM_EPOCH, ++sequence}, {4, k.time_ns},
        {0}, {NULL, 0}, {bytes, sizeof(bytes)}};
    CHECK(meiso_runtime_send_stream_sample(p.er, p.key[MEISO_DISPLAY_INPUT_ROUTE_KEY], 2, &stale) != MEISO_RUNTIME_OK);
    MeisoSessionSnapshot snapshot;
    CHECK(meiso_session_snapshot(p.hs, &snapshot) && snapshot.state == MEISO_SESSION_STATE_READY);
    CHECK(meiso_session_snapshot(p.es, &snapshot) && snapshot.state == MEISO_SESSION_STATE_READY);
    return 0;
}

/* planes-v12.md §11: the input endpoint. */
int main(void)
{
    uint8_t b[32];

    /* capability: the whole control board, head-ray reticle */
    const MeisoDisplayInputCapability c = {0xfe, MEISO_DISPLAY_INPUT_POINTER_HEAD_RAY};
    MeisoDisplayInputCapability cd, bad;
    CHECK(meiso_display_input_capability_encode(&c, (MeisoMutableBuffer){b, 16}));
    CHECK(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0xfe && b[4] == 1 && b[15] == 0);
    CHECK(meiso_display_input_capability_decode((MeisoByteView){b, 16}, &cd) && cd.keys == 0xfe && cd.pointer == 1);
    b[5] = 1; CHECK(!meiso_display_input_capability_decode((MeisoByteView){b, 16}, &cd)); b[5] = 0;
    b[3] = 0xff; CHECK(!meiso_display_input_capability_decode((MeisoByteView){b, 16}, &cd)); b[3] = 0xfe; /* bit 0 */
    b[2] = 0x01; CHECK(!meiso_display_input_capability_decode((MeisoByteView){b, 16}, &cd)); b[2] = 0;    /* bit 8 */
    CHECK(!meiso_display_input_capability_decode((MeisoByteView){b, 15}, &cd));
    bad = c; bad.keys = 0; CHECK(!meiso_display_input_capability_encode(&bad, (MeisoMutableBuffer){b, 16}));
    bad = c; bad.pointer = 2; CHECK(!meiso_display_input_capability_encode(&bad, (MeisoMutableBuffer){b, 16}));
    /* five-way only, no reticle */
    const MeisoDisplayInputCapability plain = {0x3e, MEISO_DISPLAY_INPUT_POINTER_NONE};
    CHECK(meiso_display_input_capability_encode(&plain, (MeisoMutableBuffer){b, 16}) && b[3] == 0x3e && b[4] == 0);

    /* desired and actual */
    MeisoDisplayInputDesired d = {true}, dd;
    CHECK(meiso_display_input_desired_encode(&d, (MeisoMutableBuffer){b, 16}) && b[0] == 1 && b[15] == 0);
    CHECK(meiso_display_input_desired_decode((MeisoByteView){b, 16}, &dd) && dd.keys_enabled);
    b[0] = 2; CHECK(!meiso_display_input_desired_decode((MeisoByteView){b, 16}, &dd)); b[0] = 1;
    b[1] = 1; CHECK(!meiso_display_input_desired_decode((MeisoByteView){b, 16}, &dd)); b[1] = 0;
    MeisoDisplayInputActual a = {MEISO_DISPLAY_INPUT_KEYS_RUNNING, true}, ad;
    CHECK(meiso_display_input_actual_encode(&a, (MeisoMutableBuffer){b, 16}) && b[0] == 1 && b[1] == 1 && b[2] == 0);
    CHECK(meiso_display_input_actual_decode((MeisoByteView){b, 16}, &ad) && ad.keys_state == MEISO_DISPLAY_INPUT_KEYS_RUNNING && ad.locked);
    b[1] = 2; CHECK(!meiso_display_input_actual_decode((MeisoByteView){b, 16}, &ad)); b[1] = 1;
    b[0] = 2; CHECK(!meiso_display_input_actual_decode((MeisoByteView){b, 16}, &ad)); b[0] = 1;
    b[7] = 1; CHECK(!meiso_display_input_actual_decode((MeisoByteView){b, 16}, &ad)); b[7] = 0;
    a.keys_state = 2; CHECK(!meiso_display_input_actual_encode(&a, (MeisoMutableBuffer){b, 16}));

    /* key event: SELECT pressed with plane 0x01020304 of generation 9 under the reticle */
    const MeisoDisplayInputKeyEvent k = {0x0102030405060708ULL, MEISO_DISPLAY_INPUT_KEY_SELECT, MEISO_DISPLAY_INPUT_PRESS,
        0x01020304, 9};
    MeisoDisplayInputKeyEvent kd, x;
    CHECK(meiso_display_input_key_encode(&c, &k, (MeisoMutableBuffer){b, 24}));
    CHECK(b[0] == 1 && b[7] == 8 && b[8] == 5 && b[9] == 1 && b[10] == 0 && b[11] == 0 && b[12] == 1 && b[15] == 4 && b[23] == 9);
    CHECK(meiso_display_input_key_decode(&c, (MeisoByteView){b, 24}, &kd) && kd.time_ns == k.time_ns && kd.key == k.key &&
          kd.action == k.action && kd.target_plane == k.target_plane && kd.target_generation == 9);
    b[11] = 1; CHECK(!meiso_display_input_key_decode(&c, (MeisoByteView){b, 24}, &kd)); b[11] = 0;
    CHECK(!meiso_display_input_key_decode(&c, (MeisoByteView){b, 23}, &kd));
    CHECK(!meiso_display_input_key_decode(&plain, (MeisoByteView){b, 24}, &kd)); /* a target without the reticle */
    x = k; x.key = MEISO_DISPLAY_INPUT_KEY_SYSTEM; x.target_plane = 0; x.target_generation = 0;
    CHECK(meiso_display_input_key_encode(&c, &x, (MeisoMutableBuffer){b, 24}));
    CHECK(!meiso_display_input_key_encode(&plain, &x, (MeisoMutableBuffer){b, 24})); /* no Claire key on that board */
    x = k; x.key = 0; CHECK(!meiso_display_input_key_encode(&c, &x, (MeisoMutableBuffer){b, 24}));
    x = k; x.key = 8; CHECK(!meiso_display_input_key_encode(&c, &x, (MeisoMutableBuffer){b, 24}));
    x = k; x.action = 3; CHECK(!meiso_display_input_key_encode(&c, &x, (MeisoMutableBuffer){b, 24}));
    x = k; x.time_ns = 0; CHECK(!meiso_display_input_key_encode(&c, &x, (MeisoMutableBuffer){b, 24}));
    x = k; x.target_plane = 0; CHECK(!meiso_display_input_key_encode(&c, &x, (MeisoMutableBuffer){b, 24}));     /* generation left */
    x = k; x.target_generation = 0; CHECK(!meiso_display_input_key_encode(&c, &x, (MeisoMutableBuffer){b, 24})); /* plane without it */
    CHECK(!meiso_display_input_key_encode(&c, &k, (MeisoMutableBuffer){b, 23}));

    /* catalog: display + planes [+ camera] [+ tracking] + input */
    MeisoDisplayPlanesCatalogConfig cc, got;
    memset(&cc, 0, sizeof(cc));
    cc.video = (MeisoDisplayIngressCatalogConfig){1, {1280, 720, 30000, 131072, 1024,
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_DRM_KMS_PAGE_FLIP, MEISO_STREAM_DEADLINE_UNSUPPORTED, MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    cc.planes = (MeisoDisplayPlanesCapability){1920, 1080, MEISO_DISPLAY_PLANES_VIEW_MONO, MEISO_DISPLAY_PLANES_TRACKING_NONE,
        16, 4, 262144, 2048, 2048, -0.4363f, 0.4363f, 0.2531f, -0.2531f, 0.0f, 30, 0 /* motion */};
    cc.camera = (MeisoCompositionMediaCapability){.source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        .pixel_formats = MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888, .codecs = MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        .min_width = 320, .min_height = 240, .max_width = 320, .max_height = 240, .max_fps_millihz = 30000,
        .max_access_unit_bytes = 307200, .max_fragment_payload_bytes = 4096};
    cc.tracking = (MeisoDisplayTrackingCapability){200000, 156.9064f, 34.9066f, 8, MEISO_DISPLAY_TRACKING_CORRECTION_NONE, 100};
    cc.input_enabled = true;
    cc.input = c;
    uint8_t cap[MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor ep[MEISO_DISPLAY_PLANES_MAX_ENDPOINTS];
    MeisoRuntimeV4RouteDescriptor rt[MEISO_DISPLAY_PLANES_MAX_ROUTES];
    for (unsigned mask = 0; mask < 4; ++mask) {
        const unsigned camera = mask & 1, tracking = mask >> 1;
        cc.camera_enabled = camera; cc.tracking_enabled = tracking;
        cc.planes.tracking = tracking ? MEISO_DISPLAY_PLANES_TRACKING_POSE : MEISO_DISPLAY_PLANES_TRACKING_NONE;
        const size_t ec = 3 + camera + tracking, rc = 11 + 3 * camera + 4 * tracking, base = meiso_display_planes_input_route_base(&cc);
        const size_t at = 96 + 64 * camera + 32 * tracking;
        CHECK(base == 8 + 3 * camera + 4 * tracking);
        CHECK(meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, MEISO_DISPLAY_PLANES_MAX_ENDPOINTS,
                                                 rt, MEISO_DISPLAY_PLANES_MAX_ROUTES));
        CHECK(meiso_endpoint_id_equal(ep[ec - 1].id, *meiso_display_input_endpoint()) && ep[ec - 1].capability.size == 16 &&
              ep[ec - 1].capability.data == cap + at && cap[at + 3] == 0xfe);
        CHECK(rt[base].route.resource.owner == MEISO_OWNER_ROLE_HOST && rt[base].route.facet == MEISO_RUNTIME_FACET_DESIRED &&
              rt[base].route.resource.kind == MEISO_RESOURCE_KIND_STATE);
        CHECK(rt[base + 1].route.resource.owner == MEISO_OWNER_ROLE_EDGE && rt[base + 1].route.facet == MEISO_RUNTIME_FACET_ACTUAL);
        CHECK(rt[base + 2].route.resource.owner == MEISO_OWNER_ROLE_EDGE && rt[base + 2].route.resource.kind == MEISO_RESOURCE_KIND_STREAM &&
              rt[base + 2].route.facet == MEISO_RUNTIME_FACET_DATA &&
              rt[base + 2].route.resource.max_payload_size == MEISO_DISPLAY_INPUT_KEY_BYTES);
        CHECK(meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
        CHECK(got.input_enabled && got.input.keys == 0xfe && got.input.pointer == 1 && got.camera_enabled == (camera != 0) &&
              got.tracking_enabled == (tracking != 0));
        CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc - 1, &got));
        CHECK(!meiso_display_planes_validate_catalog(ep, ec - 1, rt, rc, &got));
        rt[base + 2].route.resource.owner = MEISO_OWNER_ROLE_HOST;
        CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
        rt[base + 2].route.resource.owner = MEISO_OWNER_ROLE_EDGE;
        cap[at + 8] = 1; CHECK(!meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got)); cap[at + 8] = 0;
        CHECK(meiso_display_planes_validate_catalog(ep, ec, rt, rc, &got));
    }
    /* Input comes after tracking. */
    const MeisoRuntimeEndpointDescriptor swapped = ep[3];
    ep[3] = ep[4]; ep[4] = swapped;
    CHECK(!meiso_display_planes_validate_catalog(ep, 5, rt, 18, &got));
    ep[4] = ep[3]; ep[3] = swapped;
    CHECK(meiso_display_planes_validate_catalog(ep, 5, rt, 18, &got));
    /* A catalog without input still validates, with input_enabled false. */
    cc.input_enabled = false;
    CHECK(meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 5, rt, 18));
    CHECK(meiso_display_planes_validate_catalog(ep, 4, rt, 15, &got) && !got.input_enabled && got.tracking_enabled);
    cc.input_enabled = true; cc.input.keys = 0;
    CHECK(!meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 5, rt, 18));
    cc.input = c;
    CHECK(session_sizing() == 0);
    cc.camera_enabled = false;
    CHECK(keys_through_runtimes(&cc) == 0);
    printf("display ingress input v12: ok\n");
    return 0;
}
