#include <meiso/profiles/display_ingress/planes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

typedef union { MeisoMaxAlign align; uint8_t bytes[65536]; } Arena;
static Arena session_memory[2], runtime_memory;

/* Two planes Sessions through the handshake; the Edge offers peer_registry (28 now, 24 before §15). */
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

/* Old peers still get a Session, without voice; the Runtime keeps each older capacity level (§15, §14.4). */
static int session_sizing(void)
{
    static const struct { uint32_t registry; bool audio, extended; uint32_t endpoints, routes, capability; } peers[] = {
        {28, true, true, 7, 21, 368}, {24, false, true, 6, 18, 352}, {23, false, false, 5, 18, 208},
        {19, false, false, 4, 15, 208}, {14, false, false, 3, 11, 208}};
    for (unsigned i = 0; i < sizeof(peers) / sizeof(peers[0]); ++i) {
        MeisoSession *host, *edge;
        CHECK(handshake(peers[i].registry, &host, &edge) == 0);
        CHECK(meiso_display_planes_session_allows_audio(host) == peers[i].audio &&
              meiso_display_planes_session_allows_audio(edge) == peers[i].audio &&
              meiso_display_planes_session_allows_edge_extensions(host) == peers[i].extended);
        MeisoRuntimeV4Config rc;
        MeisoRuntime *runtime;
        CHECK(meiso_display_planes_configure_runtime(&rc, host, 65536));
        CHECK(rc.base.max_endpoints == peers[i].endpoints && rc.base.max_routes == peers[i].routes &&
              rc.base.max_capability_bytes == peers[i].capability);
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
    uint64_t epoch, actual_version, sequence;
} Pair;

static const MeisoOwnerScope host_scope = {MEISO_OWNER_ROLE_HOST, 4}, edge_scope = {MEISO_OWNER_ROLE_EDGE, 5};

/* The Edge publishes an audio Actual under the next production epoch; the Host commits and accepts it. */
static int publish_actual(Pair *p, MeisoDisplayAudioState state)
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
    CHECK(meiso_display_audio_actual_encode(&(MeisoDisplayAudioActual){state}, (MeisoMutableBuffer){profile, 16}) &&
          meiso_runtime_actual_state_encode(&a, (MeisoMutableBuffer){wire, sizeof(wire)}, &payload));
    const MeisoStateOffer actual = {{MEISO_DISPLAY_AUDIO_ACTUAL_GENERATION}, {edge_scope, ++p->actual_version, payload}};
    CHECK(meiso_runtime_send_state_offer(p->er, p->key[MEISO_DISPLAY_AUDIO_ROUTE_ACTUAL], &actual) == MEISO_RUNTIME_OK);
    CHECK(deliver(p->es, p->hs, p->hr, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    CHECK(meiso_runtime_commit_state_receive(p->hr, event.route, &event.state_offer, MEISO_STATE_RECEIVE_NEW) == MEISO_RUNTIME_OK &&
          meiso_runtime_send_state_accept(p->hr, event.route, actual.generation, edge_scope, p->actual_version) == MEISO_RUNTIME_OK);
    CHECK(deliver(p->hs, p->es, p->er, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT);
    return 0;
}

/* One audio record from the Edge under the current epoch; *decoded tells whether the Host could read it. */
static int send_record(Pair *p, const MeisoDisplayAudioRecord *r, bool corrupt, bool *decoded, MeisoDisplayAudioRecord *got)
{
    static uint8_t bytes[MEISO_DISPLAY_AUDIO_MAX_RECORD_BYTES];
    size_t n = 0;
    MeisoRuntimeStatus status;
    MeisoRuntimeEvent event;
    CHECK(meiso_display_audio_record_encode(r, (MeisoMutableBuffer){bytes, sizeof(bytes)}, &n));
    if (corrupt) bytes[17] = 1;
    const MeisoStreamSample sample = {{edge_scope, MEISO_DISPLAY_AUDIO_STREAM_EPOCH, ++p->sequence}, {4, r->time_ns},
        {0}, {NULL, 0}, {bytes, n}};
    CHECK(meiso_runtime_send_stream_sample(p->er, p->key[MEISO_DISPLAY_AUDIO_ROUTE_RECORD], p->epoch, &sample) == MEISO_RUNTIME_OK);
    CHECK(deliver(p->es, p->hs, p->hr, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE);
    *decoded = meiso_display_audio_record_decode(event.stream_sample.payload, got);
    return 0;
}

/* §15 through both Runtimes: an utterance of three records arrives whole and in order, the largest
 * record included; a bad record is the Host's to drop and the Session goes on. */
static int audio_through_runtimes(const MeisoDisplayPlanesCatalogConfig *cc)
{
    Pair p;
    memset(&p, 0, sizeof(p));
    MeisoRuntimeV4Config rc;
    MeisoRuntimeStatus status;
    MeisoRuntimeEvent event;
    CHECK(handshake(MEISO_DISPLAY_PLANES_AUDIO_REGISTRY, &p.hs, &p.es) == 0);
    CHECK(meiso_display_planes_configure_runtime(&rc, p.hs, cc->planes.max_image_bytes) &&
          meiso_runtime_v4_init(runtime_pair[0].bytes, sizeof(runtime_pair[0].bytes), &rc, &p.hr) == MEISO_RUNTIME_INIT_OK);
    CHECK(meiso_display_planes_configure_runtime(&rc, p.es, cc->planes.max_image_bytes) &&
          meiso_runtime_v4_init(runtime_pair[1].bytes, sizeof(runtime_pair[1].bytes), &rc, &p.er) == MEISO_RUNTIME_INIT_OK);
    uint8_t cap[MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor ep[MEISO_DISPLAY_PLANES_MAX_ENDPOINTS];
    MeisoRuntimeV4RouteDescriptor rt[MEISO_DISPLAY_PLANES_MAX_ROUTES];
    CHECK(!cc->camera_enabled && cc->tracking_enabled && cc->input_enabled && !cc->views_enabled && cc->audio_enabled);
    CHECK(meiso_display_planes_build_catalog(cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 5, rt, 18));
    CHECK(meiso_runtime_v4_offer_catalog(p.er, 1, ep, 5, rt, 18) == MEISO_RUNTIME_OK);
    CHECK(deliver(p.es, p.hs, p.hr, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED);
    CHECK(meiso_runtime_accept_catalog(p.hr) == MEISO_RUNTIME_OK);
    CHECK(deliver(p.hs, p.es, p.er, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED);
    const size_t base = meiso_display_planes_audio_route_base(cc);
    for (unsigned i = 0; i < 3; ++i) {
        const MeisoRuntimeRouteDescriptor *r = &rt[base + i].route;
        p.key[i] = (MeisoRuntimeRouteKey){1, r->endpoint_id, r->endpoint_incarnation, r->resource.id, r->facet};
    }

    /* the Host allows push-to-talk */
    uint8_t profile[16], wire[MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE + 16];
    MeisoByteView payload;
    CHECK(meiso_display_audio_desired_encode(&(MeisoDisplayAudioDesired){true}, (MeisoMutableBuffer){profile, 16}) &&
          meiso_runtime_desired_state_encode(&(MeisoRuntimeDesiredState){true, {profile, 16}}, (MeisoMutableBuffer){wire, sizeof(wire)}, &payload));
    const MeisoStateOffer desired = {{MEISO_DISPLAY_AUDIO_DESIRED_GENERATION}, {host_scope, 1, payload}};
    CHECK(meiso_runtime_send_state_offer(p.hr, p.key[MEISO_DISPLAY_AUDIO_ROUTE_DESIRED], &desired) == MEISO_RUNTIME_OK);
    CHECK(deliver(p.hs, p.es, p.er, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    CHECK(meiso_runtime_commit_state_receive(p.er, event.route, &event.state_offer, MEISO_STATE_RECEIVE_NEW) == MEISO_RUNTIME_OK &&
          meiso_runtime_send_state_accept(p.er, event.route, desired.generation, host_scope, 1) == MEISO_RUNTIME_OK);
    CHECK(deliver(p.es, p.hs, p.hr, &status, &event) == 0 && status == MEISO_RUNTIME_OK && event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT);
    CHECK(publish_actual(&p, MEISO_DISPLAY_AUDIO_READY) == 0);

    /* one hold: 20 ms, then a full 60 ms record (after a stall), then the empty LAST after the release */
    static uint8_t pcm[2 * MEISO_DISPLAY_AUDIO_MAX_RECORD_SAMPLES];
    for (size_t i = 0; i < sizeof(pcm); ++i) pcm[i] = (uint8_t)(i * 7U);
    MeisoDisplayAudioRecord r = {1000000000ULL, 1, 0, MEISO_DISPLAY_AUDIO_FIRST, 320, pcm}, got;
    bool decoded;
    CHECK(send_record(&p, &r, false, &decoded, &got) == 0 && decoded && got.utterance == 1 && got.index == 0 &&
          got.flags == MEISO_DISPLAY_AUDIO_FIRST && got.count == 320 && !memcmp(got.samples, pcm, 640));
    r = (MeisoDisplayAudioRecord){1020000000ULL, 1, 1, 0, MEISO_DISPLAY_AUDIO_MAX_RECORD_SAMPLES, pcm};
    CHECK(send_record(&p, &r, false, &decoded, &got) == 0 && decoded && got.index == 1 &&
          got.count == MEISO_DISPLAY_AUDIO_MAX_RECORD_SAMPLES && !memcmp(got.samples, pcm, sizeof(pcm)));
    r = (MeisoDisplayAudioRecord){1080000000ULL, 1, 2, MEISO_DISPLAY_AUDIO_LAST, 0, NULL};
    CHECK(send_record(&p, &r, false, &decoded, &got) == 0 && decoded && got.index == 2 && got.flags == MEISO_DISPLAY_AUDIO_LAST &&
          got.count == 0);
    /* a corrupt record reaches the Host, which drops it; the next one is read */
    r = (MeisoDisplayAudioRecord){2000000000ULL, 2, 0, MEISO_DISPLAY_AUDIO_FIRST, 320, pcm};
    CHECK(send_record(&p, &r, true, &decoded, &got) == 0 && !decoded);
    r.flags |= MEISO_DISPLAY_AUDIO_LAST;
    CHECK(send_record(&p, &r, false, &decoded, &got) == 0 && decoded && got.utterance == 2);
    /* forbidding changes the Actual (a new epoch); records under the old one are not the Edge's to send */
    CHECK(publish_actual(&p, MEISO_DISPLAY_AUDIO_OFF) == 0 && p.epoch == 2);
    uint8_t bytes[64];
    size_t n = 0;
    r = (MeisoDisplayAudioRecord){3000000000ULL, 3, 0, MEISO_DISPLAY_AUDIO_FIRST | MEISO_DISPLAY_AUDIO_LAST, 8, pcm};
    CHECK(meiso_display_audio_record_encode(&r, (MeisoMutableBuffer){bytes, sizeof(bytes)}, &n) && n == 40);
    const MeisoStreamSample stale = {{edge_scope, MEISO_DISPLAY_AUDIO_STREAM_EPOCH, ++p.sequence}, {4, r.time_ns},
        {0}, {NULL, 0}, {bytes, n}};
    CHECK(meiso_runtime_send_stream_sample(p.er, p.key[MEISO_DISPLAY_AUDIO_ROUTE_RECORD], 1, &stale) != MEISO_RUNTIME_OK);
    MeisoSessionSnapshot snapshot;
    CHECK(meiso_session_snapshot(p.hs, &snapshot) && snapshot.state == MEISO_SESSION_STATE_READY);
    CHECK(meiso_session_snapshot(p.es, &snapshot) && snapshot.state == MEISO_SESSION_STATE_READY);
    return 0;
}

static int codecs(void)
{
    uint8_t b[64];

    /* capability: 16 kHz mono S16LE, 30 s */
    const MeisoDisplayAudioCapability c = {16000, 1, MEISO_DISPLAY_AUDIO_FORMAT_S16LE, 30000};
    MeisoDisplayAudioCapability cd, bad;
    static const uint8_t cap_golden[16] = {0, 0, 0x3e, 0x80, 1, 1, 0x75, 0x30};
    CHECK(meiso_display_audio_capability_encode(&c, (MeisoMutableBuffer){b, 16}) && !memcmp(b, cap_golden, 16));
    CHECK(meiso_display_audio_capability_decode((MeisoByteView){b, 16}, &cd) && cd.sample_rate_hz == 16000 && cd.channels == 1 &&
          cd.format == MEISO_DISPLAY_AUDIO_FORMAT_S16LE && cd.max_utterance_ms == 30000);
    b[8] = 1; CHECK(!meiso_display_audio_capability_decode((MeisoByteView){b, 16}, &cd)); b[8] = 0;
    b[15] = 1; CHECK(!meiso_display_audio_capability_decode((MeisoByteView){b, 16}, &cd)); b[15] = 0;
    CHECK(!meiso_display_audio_capability_decode((MeisoByteView){b, 15}, &cd));
    bad = c; bad.sample_rate_hz = 48000; CHECK(!meiso_display_audio_capability_encode(&bad, (MeisoMutableBuffer){b, 16}));
    bad = c; bad.channels = 2; CHECK(!meiso_display_audio_capability_encode(&bad, (MeisoMutableBuffer){b, 16}));
    bad = c; bad.format = 2; CHECK(!meiso_display_audio_capability_encode(&bad, (MeisoMutableBuffer){b, 16}));
    bad = c; bad.max_utterance_ms = 999; CHECK(!meiso_display_audio_capability_encode(&bad, (MeisoMutableBuffer){b, 16}));
    bad = c; bad.max_utterance_ms = 60001; CHECK(!meiso_display_audio_capability_encode(&bad, (MeisoMutableBuffer){b, 16}));
    CHECK(!meiso_display_audio_capability_encode(&c, (MeisoMutableBuffer){b, 15}));
    memset(b, 0xaa, 16);
    CHECK(!meiso_display_audio_capability_encode(&bad, (MeisoMutableBuffer){b, 16}) && b[0] == 0xaa && b[15] == 0xaa);

    /* desired and actual */
    MeisoDisplayAudioDesired dd;
    CHECK(meiso_display_audio_desired_encode(&(MeisoDisplayAudioDesired){true}, (MeisoMutableBuffer){b, 16}) && b[0] == 1 && b[15] == 0);
    CHECK(meiso_display_audio_desired_decode((MeisoByteView){b, 16}, &dd) && dd.allowed);
    b[0] = 2; CHECK(!meiso_display_audio_desired_decode((MeisoByteView){b, 16}, &dd)); b[0] = 1;
    b[1] = 1; CHECK(!meiso_display_audio_desired_decode((MeisoByteView){b, 16}, &dd)); b[1] = 0;
    CHECK(meiso_display_audio_desired_encode(&(MeisoDisplayAudioDesired){false}, (MeisoMutableBuffer){b, 16}) && b[0] == 0);
    CHECK(meiso_display_audio_desired_decode((MeisoByteView){b, 16}, &dd) && !dd.allowed);
    MeisoDisplayAudioActual ad;
    CHECK(meiso_display_audio_actual_encode(&(MeisoDisplayAudioActual){MEISO_DISPLAY_AUDIO_FAILED}, (MeisoMutableBuffer){b, 16}) &&
          b[0] == 2 && b[1] == 0);
    CHECK(meiso_display_audio_actual_decode((MeisoByteView){b, 16}, &ad) && ad.state == MEISO_DISPLAY_AUDIO_FAILED);
    b[0] = 3; CHECK(!meiso_display_audio_actual_decode((MeisoByteView){b, 16}, &ad)); b[0] = 1;
    b[9] = 1; CHECK(!meiso_display_audio_actual_decode((MeisoByteView){b, 16}, &ad)); b[9] = 0;
    CHECK(!meiso_display_audio_actual_encode(&(MeisoDisplayAudioActual){3}, (MeisoMutableBuffer){b, 16}));

    /* record: utterance 3's first record, two samples 0x1234 and 0xabcd */
    static const uint8_t samples[4] = {0x34, 0x12, 0xcd, 0xab};
    static const uint8_t rec_golden[28] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 3, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0,
        0x34, 0x12, 0xcd, 0xab};
    const MeisoDisplayAudioRecord r = {0x0102030405060708ULL, 3, 0, MEISO_DISPLAY_AUDIO_FIRST, 2, samples};
    MeisoDisplayAudioRecord rd, x;
    size_t n = 0;
    CHECK(meiso_display_audio_record_bytes(2) == 28 && meiso_display_audio_record_bytes(960) == MEISO_DISPLAY_AUDIO_MAX_RECORD_BYTES);
    CHECK(meiso_display_audio_record_encode(&r, (MeisoMutableBuffer){b, sizeof(b)}, &n) && n == 28 && !memcmp(b, rec_golden, 28));
    CHECK(meiso_display_audio_record_decode((MeisoByteView){b, 28}, &rd) && rd.time_ns == r.time_ns && rd.utterance == 3 &&
          rd.index == 0 && rd.flags == MEISO_DISPLAY_AUDIO_FIRST && rd.count == 2 && rd.samples == b + 24);
    CHECK(!meiso_display_audio_record_decode((MeisoByteView){b, 27}, &rd));  /* size must match count */
    CHECK(!meiso_display_audio_record_decode((MeisoByteView){b, 30}, &rd));
    CHECK(!meiso_display_audio_record_decode((MeisoByteView){b, 23}, &rd));
    b[17] = 1; CHECK(!meiso_display_audio_record_decode((MeisoByteView){b, 28}, &rd)); b[17] = 0;
    b[21] = 1; CHECK(!meiso_display_audio_record_decode((MeisoByteView){b, 28}, &rd)); b[21] = 0;
    b[16] = 0x11; CHECK(!meiso_display_audio_record_decode((MeisoByteView){b, 28}, &rd)); b[16] = 1; /* unknown flag */
    b[15] = 1; CHECK(!meiso_display_audio_record_decode((MeisoByteView){b, 28}, &rd)); b[15] = 0;   /* FIRST not at index 0 */
    b[16] = 0; CHECK(!meiso_display_audio_record_decode((MeisoByteView){b, 28}, &rd)); b[16] = 1;   /* index 0 without FIRST */
    CHECK(meiso_display_audio_record_decode((MeisoByteView){b, 28}, &rd));
    memset(b, 0xaa, 28);
    CHECK(meiso_display_audio_record_decode((MeisoByteView){rec_golden, 28}, &rd)); rd.count = 99;
    x = rd; CHECK(!meiso_display_audio_record_decode((MeisoByteView){b, 28}, &x) && x.count == 99); /* failure leaves output */
    x = r; x.time_ns = 0; CHECK(!meiso_display_audio_record_encode(&x, (MeisoMutableBuffer){b, sizeof(b)}, &n));
    x = r; x.utterance = 0; CHECK(!meiso_display_audio_record_encode(&x, (MeisoMutableBuffer){b, sizeof(b)}, &n));
    x = r; x.count = 0; CHECK(!meiso_display_audio_record_encode(&x, (MeisoMutableBuffer){b, sizeof(b)}, &n)); /* empty, not LAST */
    x = r; x.count = 961; CHECK(!meiso_display_audio_record_encode(&x, (MeisoMutableBuffer){b, sizeof(b)}, &n));
    x = r; x.flags = MEISO_DISPLAY_AUDIO_FIRST | MEISO_DISPLAY_AUDIO_TRUNCATED;  /* TRUNCATED without LAST */
    CHECK(!meiso_display_audio_record_encode(&x, (MeisoMutableBuffer){b, sizeof(b)}, &n));
    x.flags |= MEISO_DISPLAY_AUDIO_LAST; CHECK(meiso_display_audio_record_encode(&x, (MeisoMutableBuffer){b, sizeof(b)}, &n));
    x.flags |= MEISO_DISPLAY_AUDIO_ABORTED; CHECK(!meiso_display_audio_record_encode(&x, (MeisoMutableBuffer){b, sizeof(b)}, &n));
    x = r; x.index = 4; x.flags = MEISO_DISPLAY_AUDIO_LAST | MEISO_DISPLAY_AUDIO_ABORTED; x.count = 0; x.samples = NULL;
    CHECK(meiso_display_audio_record_encode(&x, (MeisoMutableBuffer){b, sizeof(b)}, &n) && n == 24 && b[16] == 10 && b[15] == 4);
    CHECK(meiso_display_audio_record_decode((MeisoByteView){b, 24}, &rd) && rd.count == 0 && rd.index == 4);
    CHECK(!meiso_display_audio_record_encode(&r, (MeisoMutableBuffer){b, 27}, &n));
    uint8_t in_place[28]; /* samples inside the output */
    memcpy(in_place, rec_golden, 28);
    x = r; x.samples = in_place + 24;
    CHECK(!meiso_display_audio_record_encode(&x, (MeisoMutableBuffer){in_place, 28}, &n));
    return 0;
}

/* The full catalog: display + planes + camera + tracking + input + views + audio (7 endpoints, 21
 * routes, 368 B); audio last, after input's routes. Turning audio off gives back the old bytes. */
static int catalog(MeisoDisplayPlanesCatalogConfig *out)
{
    MeisoDisplayPlanesCatalogConfig cc, got;
    memset(&cc, 0, sizeof(cc));
    cc.video = (MeisoDisplayIngressCatalogConfig){1, {1280, 720, 30000, 131072, 1024,
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_DRM_KMS_PAGE_FLIP, MEISO_STREAM_DEADLINE_UNSUPPORTED, MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    cc.planes = (MeisoDisplayPlanesCapability){0, 0, MEISO_DISPLAY_PLANES_VIEW_RUNTIME_VIEWS, MEISO_DISPLAY_PLANES_TRACKING_POSE,
        16, 4, 262144, 2048, 2048, 0, 0, 0, 0, 0, 30};
    cc.camera_enabled = cc.tracking_enabled = cc.input_enabled = cc.views_enabled = cc.audio_enabled = true;
    cc.camera = (MeisoCompositionMediaCapability){.source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        .pixel_formats = MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888, .codecs = MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        .min_width = 320, .min_height = 240, .max_width = 320, .max_height = 240, .max_fps_millihz = 30000,
        .max_access_unit_bytes = 307200, .max_fragment_payload_bytes = 4096};
    cc.tracking = (MeisoDisplayTrackingCapability){200000, 156.9064f, 34.9066f, 8, MEISO_DISPLAY_TRACKING_CORRECTION_NONE, 100};
    cc.input = (MeisoDisplayInputCapability){0xfe, MEISO_DISPLAY_INPUT_POINTER_HEAD_RAY};
    cc.views.count = 2;
    for (unsigned i = 0; i < 2; ++i)
        cc.views.views[i] = (MeisoDisplayView){6356, 5104, -0.5f, 0.5f, 0.5f, -0.5f, {i ? 0.03125f : -0.03125f, 0, 0}, {0, 0, 0, 1}};
    cc.audio = (MeisoDisplayAudioCapability){16000, 1, MEISO_DISPLAY_AUDIO_FORMAT_S16LE, 30000};

    uint8_t cap[MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES], old_cap[MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor ep[MEISO_DISPLAY_PLANES_MAX_ENDPOINTS], old_ep[MEISO_DISPLAY_PLANES_MAX_ENDPOINTS];
    MeisoRuntimeV4RouteDescriptor rt[MEISO_DISPLAY_PLANES_MAX_ROUTES], old_rt[MEISO_DISPLAY_PLANES_MAX_ROUTES];
    const size_t base = meiso_display_planes_audio_route_base(&cc);
    CHECK(base == 18);
    CHECK(meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 7, rt, 21));
    CHECK(!meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, 367}, ep, 7, rt, 21));
    CHECK(!meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 7, rt, 20));
    CHECK(meiso_endpoint_id_equal(ep[6].id, *meiso_display_audio_endpoint()) && ep[6].capability.data == cap + 352 &&
          ep[6].capability.size == 16 && cap[354] == 0x3e && cap[355] == 0x80);
    CHECK(meiso_endpoint_id_equal(ep[5].id, *meiso_display_views_endpoint()));
    CHECK(rt[base].route.resource.owner == MEISO_OWNER_ROLE_HOST && rt[base].route.facet == MEISO_RUNTIME_FACET_DESIRED &&
          rt[base].route.resource.kind == MEISO_RESOURCE_KIND_STATE && rt[base].delivery == MEISO_RUNTIME_DELIVERY_INLINE);
    CHECK(rt[base + 1].route.resource.owner == MEISO_OWNER_ROLE_EDGE && rt[base + 1].route.facet == MEISO_RUNTIME_FACET_ACTUAL);
    CHECK(rt[base + 2].route.resource.owner == MEISO_OWNER_ROLE_EDGE && rt[base + 2].route.resource.kind == MEISO_RESOURCE_KIND_STREAM &&
          rt[base + 2].route.facet == MEISO_RUNTIME_FACET_DATA &&
          rt[base + 2].route.resource.max_payload_size == MEISO_DISPLAY_AUDIO_MAX_RECORD_BYTES);
    for (unsigned i = 0; i < 3; ++i)
        CHECK(meiso_endpoint_id_equal(rt[base + i].route.endpoint_id, *meiso_display_audio_endpoint()));
    CHECK(meiso_display_planes_validate_catalog(ep, 7, rt, 21, &got));
    CHECK(got.audio_enabled && got.audio.sample_rate_hz == 16000 && got.audio.max_utterance_ms == 30000 && got.views_enabled &&
          got.input_enabled && got.camera_enabled && got.tracking_enabled);
    CHECK(!meiso_display_planes_validate_catalog(ep, 7, rt, 20, &got));
    CHECK(!meiso_display_planes_validate_catalog(ep, 6, rt, 21, &got));
    rt[base + 2].route.resource.owner = MEISO_OWNER_ROLE_HOST;
    CHECK(!meiso_display_planes_validate_catalog(ep, 7, rt, 21, &got));
    rt[base + 2].route.resource.owner = MEISO_OWNER_ROLE_EDGE;
    cap[352 + 8] = 1; CHECK(!meiso_display_planes_validate_catalog(ep, 7, rt, 21, &got)); cap[352 + 8] = 0;
    /* audio comes after views */
    const MeisoRuntimeEndpointDescriptor swapped = ep[5];
    ep[5] = ep[6]; ep[6] = swapped;
    CHECK(!meiso_display_planes_validate_catalog(ep, 7, rt, 21, &got));
    ep[6] = ep[5]; ep[5] = swapped;
    CHECK(meiso_display_planes_validate_catalog(ep, 7, rt, 21, &got));

    /* without audio: the same bytes, endpoints and routes as before §15 */
    cc.audio_enabled = false;
    CHECK(meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){old_cap, 352}, old_ep, 6, old_rt, 18));
    CHECK(!memcmp(old_cap, cap, 352));
    for (unsigned i = 0; i < 6; ++i)
        CHECK(meiso_endpoint_id_equal(old_ep[i].id, ep[i].id) && old_ep[i].capability.size == ep[i].capability.size &&
              old_ep[i].capability.data == old_cap + (ep[i].capability.data - cap));
    for (unsigned i = 0; i < 18; ++i) CHECK(meiso_resource_descriptor_equal(&old_rt[i].route.resource, &rt[i].route.resource));
    CHECK(meiso_display_planes_validate_catalog(old_ep, 6, old_rt, 18, &got) && !got.audio_enabled && got.views_enabled);
    /* a catalog that lists audio's routes but not its endpoint is refused */
    CHECK(!meiso_display_planes_validate_catalog(ep, 6, rt, 21, &got));

    /* audio needs input with SYSTEM */
    cc.audio_enabled = true;
    cc.input.keys = 0x7e;
    CHECK(!meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 7, rt, 21));
    cc.input_enabled = false; cc.input.keys = 0xfe;
    CHECK(!meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 7, rt, 21));
    cc.input_enabled = true;
    cc.audio.channels = 2;
    CHECK(!meiso_display_planes_build_catalog(&cc, (MeisoMutableBuffer){cap, sizeof(cap)}, ep, 7, rt, 21));
    cc.audio.channels = 1;

    /* for the Runtime pair: no camera, no views */
    cc.camera_enabled = cc.views_enabled = false;
    cc.planes = (MeisoDisplayPlanesCapability){1920, 1080, MEISO_DISPLAY_PLANES_VIEW_MONO, MEISO_DISPLAY_PLANES_TRACKING_POSE,
        16, 4, 262144, 2048, 2048, -0.4363f, 0.4363f, 0.2531f, -0.2531f, 0.0f, 30};
    CHECK(meiso_display_planes_audio_route_base(&cc) == 15);
    *out = cc;
    return 0;
}

/* planes-v12.md §15: the audio endpoint. */
int main(void)
{
    MeisoDisplayPlanesCatalogConfig cc;
    CHECK(codecs() == 0);
    CHECK(catalog(&cc) == 0);
    CHECK(session_sizing() == 0);
    CHECK(audio_through_runtimes(&cc) == 0);
    printf("display ingress audio v12: ok\n");
    return 0;
}
