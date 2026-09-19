/* PRODUCT: Display Ingress runtime; reference runners reuse the common service. */
#define _POSIX_C_SOURCE 200809L
#include "service_private.h"
#include "../../profiles/display_ingress/display_ingress_profile_private.h"
#ifdef MEISO_DISPLAY_INGRESS_VIDEO_EDGE
#include <meiso/integrations/display_camera_channel.h>
#ifndef MEISO_DISPLAY_INGRESS_DESKTOP_EDGE
#include <meiso/edge/linux/camera_v4l2.h>
#endif
#endif
#ifdef MEISO_DISPLAY_INGRESS_VIDEO_EDGE
#include "../../targets/k230d-linux/display_ingress_overlay_raster.h"
#endif
#include <meiso/profiles/display_ingress/projection.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef MEISO_DISPLAY_INGRESS_VIDEO_EDGE
#include "video_backend.h"
bool service_overlay_initialize(Service *, MeisoIngressVideo *);
bool service_overlay_consume(Service *, const MeisoRuntimeEvent *);
bool service_overlay_poll(Service *);
void service_overlay_finish(void);
static bool continuous_edge;
#endif

/* One controlled video workload; Session, Catalog, State and DIFR remain shared. */
static Service service;
static uint64_t frame_limit = 1800U;
static uint64_t evidence_count, last_evidence_sequence;
static uint64_t submitted, presented, rejected, ledger_count;
static volatile sig_atomic_t interrupted;
static void on_signal(int signal_number) { (void)signal_number; interrupted = 1; }

#ifndef MEISO_DISPLAY_INGRESS_VIDEO_EDGE
typedef struct { size_t offset, size; uint8_t digest[32]; } SourceUnit;
static uint64_t last_evidence_sample;
static SourceUnit units[4096];
static size_t unit_count, source_size;
static uint8_t *source;

static bool add_unit(size_t begin, size_t end)
{
    if (end <= begin || unit_count == sizeof(units)/sizeof(units[0]) ||
        end - begin > SERVICE_FRAME_BYTES) return false;
    SourceUnit *u = &units[unit_count];
    u->offset = begin; u->size = end - begin;
    MeisoByteView bytes = {source + begin, u->size};
    if (!meiso_display_ingress_h264_idr_access_unit_valid(bytes) ||
        !meiso_display_ingress_private_payload_sha256(bytes, u->digest)) return false;
    ++unit_count;
    return true;
}

/* Independent Annex B pictures start with optional AUD followed by repeated SPS. */
static bool open_source(const char *path)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 128 * 1024 * 1024) { close(fd); return false; }
    source_size = (size_t)st.st_size;
    source = mmap(NULL, source_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (source == MAP_FAILED) { source = NULL; return false; }
    size_t begin = 0;
    bool found = false, have_sps = false;
    for (size_t i = 0; i + 4U < source_size;) {
        size_t n = 0;
        if (source[i] == 0 && source[i+1] == 0) {
            if (source[i+2] == 1) n = 3;
            else if (source[i+2] == 0 && source[i+3] == 1) n = 4;
        }
        if (n != 0) {
            const unsigned type=source[i+n] & 31U;
            if (type==9U || type==7U) {
                if (!found && i != 0) return false;
                if (found && (type==9U || have_sps)) {
                    if (!add_unit(begin, i)) return false;
                    begin=i; have_sps=false;
                }
                found=true;
                if (type==7U) have_sps=true;
            }
            i += n + 1U;
        } else ++i;
    }
    return found && add_unit(begin, source_size);
}

bool service_video_host_event(Service *s, const MeisoRuntimeEvent *event)
{
    if (event->type == MEISO_RUNTIME_EVENT_STATE_ACCEPT)
        return service_consume_optional_accept(s, event);
    MeisoDisplayIngressPresentationEvidence evidence;
    MeisoRuntimeRouteKey expected = service_route_key(s, MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE);
    const MeisoRuntimeRouteKey got = event->route;
    if (event->type != MEISO_RUNTIME_EVENT_STREAM_SAMPLE ||
        got.catalog_generation != expected.catalog_generation ||
        !meiso_endpoint_id_equal(got.endpoint_id, expected.endpoint_id) ||
        got.endpoint_incarnation != expected.endpoint_incarnation ||
        !meiso_resource_id_equal(got.resource_id, expected.resource_id) || got.facet != expected.facet ||
        event->production_epoch != s->production_epoch ||
        !meiso_owner_scope_equal(event->stream_sample.key.producer, s->edge_owner) ||
        event->stream_sample.key.epoch != SERVICE_EVIDENCE_STREAM_EPOCH ||
        event->stream_sample.key.sequence <= last_evidence_sample ||
        !meiso_time_point_valid(event->stream_sample.source_time) ||
        event->stream_sample.source_time.clock_epoch != SERVICE_EVIDENCE_CLOCK_EPOCH ||
        event->stream_sample.sender_deadline.present || event->stream_sample.dependency.size != 0 ||
        meiso_display_ingress_evidence_decode(event->stream_sample.payload, &evidence) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        !meiso_display_ingress_evidence_supported(&s->capability, &evidence, s->production_epoch) ||
        evidence.evidence_class != SERVICE_EVIDENCE_CLASS ||
        evidence.media_stream_epoch != s->media_stream_epoch ||
        evidence.displayed_frame_sequence <= last_evidence_sequence ||
        evidence.displayed_frame_sequence > submitted ||
        memcmp(evidence.input_frame_sha256,
            units[(evidence.displayed_frame_sequence - 1U) % unit_count].digest, 32) != 0)
        return service_fail(s, "application", "video-evidence");
    last_evidence_sequence = evidence.displayed_frame_sequence;
    last_evidence_sample = event->stream_sample.key.sequence;
    ++evidence_count;
    if (evidence_count == 1 || evidence_count % 300U == 0)
        fprintf(stderr, "PRESENTATION frame=%" PRIu64 " flip_ns=%" PRIu64 " output=%ux%u\n",
            last_evidence_sequence, event->stream_sample.source_time.monotonic_ns,
            evidence.output_width, evidence.output_height);
    return true;
}

static bool send_batch(Service *s)
{
    for (unsigned i = 0; i < 64; ++i) {
        MeisoDisplayIngressMediaPosixUdpPrepareStatus p = meiso_display_ingress_media_posix_udp_prepare_outbound(s->udp);
        if (p == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_IDLE) return true;
        if (p != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY) break;
        MeisoDisplayIngressMediaPosixUdpWriteStatus w = meiso_display_ingress_media_posix_udp_on_writable(s->udp);
        if (w == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_WOULD_BLOCK ||
            w == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_INTERRUPTED) return true;
        if (w != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_PACKET_SENT) break;
        if (i == 63) return true;
    }
    return service_fail(s, "media", "video-send");
}

bool service_run_host_media(Service *s)
{
    uint64_t start = service_now_ns();
    const uint64_t end = start + frame_limit * UINT64_C(1000000000) / 30U;
    const uint64_t deadline = end + UINT64_C(3000000000);
    while (!interrupted && service_now_ns() < deadline) {
        uint64_t now = service_now_ns();
        if (submitted < frame_limit && now >= start + submitted * UINT64_C(1000000000) / 30U) {
            const SourceUnit *u = &units[submitted % unit_count];
            MeisoDisplayIngressMediaFrame frame;
            memset(&frame, 0, sizeof(frame));
            frame.frame_sequence = submitted + 1U;
            frame.source_time = (MeisoTimePoint){1U, now};
            frame.access_unit = (MeisoByteView){source + u->offset, u->size};
            MeisoDisplayIngressMediaTransportStatus status = meiso_display_ingress_media_transport_submit(s->media, &frame, NULL);
            if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK && status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_LATEST_REPLACED)
                return service_fail(s, "media", "video-submit");
            ++submitted;
        }
        if (!send_batch(s)) return false;
        MeisoRuntimeEvent event;
        ServiceControlStep step = service_control_step(s, true, 0, &event);
        if (step == SERVICE_CONTROL_EVENT && !service_video_host_event(s, &event)) return false;
        if (step == SERVICE_CONTROL_FAILED || step == SERVICE_CONTROL_PEER_CLOSED)
            return service_fail(s, "control", "video-transfer");
        /* At most 1 ms idle, without gating video on any presentation receipt. */
        struct pollfd p = {s->control_fd, POLLIN, 0};
        (void)poll(&p, 1, 1);
    }
    if (interrupted || submitted != frame_limit || evidence_count == 0)
        return service_fail(s, "application", "video-incomplete");
    return service_terminate_media(s, MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE) &&
        service_finish_local_close(s);
}
bool service_run_edge_media(Service *s) { return service_fail(s, "application", "host-only"); }
#else
static MeisoIngressVideo *video;
static MeisoDisplayCameraChannel *camera;
#include "../../adapters/media/camera_jpeg.h"
static bool capture_jpeg;
static uint8_t capture_encoded[30720];
#ifdef MEISO_DISPLAY_INGRESS_DESKTOP_EDGE
static uint8_t capture_pixels[320*240*4];
static size_t capture_used;
static bool capture_started;
static uint64_t capture_sequence;
#else
static bool capture_bt601_known;
static MeisoCameraV4l2Adapter *capture;
static void *capture_arena;
static bool capture_stop(void)
{
    if(!capture) return true;
    const MeisoCameraV4l2StopStatus status=meiso_camera_v4l2_stop(capture);
    MeisoCameraV4l2Snapshot state;
    if(meiso_camera_v4l2_snapshot(capture,&state))
        fprintf(stderr,"CAMERA_CAPTURE stopped=%u mapped=%"PRIu64" stream_on=%d borrowed=%d cleanup_stage=%u errno=%d\n",
            status,state.mapped_bytes,state.stream_on,state.frame_borrowed,state.cleanup_failure_stage,state.cleanup_error);
    return status==MEISO_CAMERA_V4L2_STOP_STOPPED || status==MEISO_CAMERA_V4L2_STOP_UNCHANGED;
}
static bool capture_open(const char *path)
{
    MeisoCameraV4l2Config config;
    uint64_t epoch=0;
    do { if(!service_random_bytes(&epoch,sizeof(epoch))) return false; } while(!epoch);
    meiso_camera_v4l2_default_config(epoch,&config);
    config.device_path=path;
    const size_t bytes=meiso_camera_v4l2_memory_required();
    if(posix_memalign(&capture_arena,meiso_camera_v4l2_alignment_required(),bytes)!=0) return false;
    if(meiso_camera_v4l2_init(capture_arena,bytes,&config,&capture)!=MEISO_CAMERA_V4L2_INIT_OK) return false;
    const MeisoCameraV4l2StartStatus status=meiso_camera_v4l2_start(capture);
    MeisoCameraV4l2Snapshot snapshot;
    if(!meiso_camera_v4l2_snapshot(capture,&snapshot)) return false;
    fprintf(stderr,"CAMERA_CAPTURE start=%u arena=%zu mapped=%"PRIu64" stage=%u errno=%d\n",
        status,bytes,snapshot.mapped_bytes,snapshot.primary_failure_stage,snapshot.primary_error);
    return status==MEISO_CAMERA_V4L2_START_STARTED;
}
#endif
static uint64_t capture_last,capture_next,camera_deadline,camera_eof_deadline;
static bool capture_halted;
static bool camera_tick(Service *s)
{
    if(!camera) return true;
    const uint64_t now=service_now_ns();
    if(s->link_enabled && meiso_display_residence_expired(&s->leases[MEISO_DISPLAY_LINK_LEASE_CAMERA],now/UINT64_C(1000000))) {
#ifndef MEISO_DISPLAY_INGRESS_DESKTOP_EDGE
        if(!capture_stop()) return service_fail(s,"camera","lease-capture-stop");
#endif
        capture_halted=true;s->leases[MEISO_DISPLAY_LINK_LEASE_CAMERA].active=false;
        if(!meiso_display_camera_channel_stop(camera,MEISO_DISPLAY_LINK_LEASE_EXPIRED)) return service_fail(s,"camera","lease-stop");
        fprintf(stderr,"CAMERA_LEASE_EXPIRED time_ns=%"PRIu64"\n",now);
    }
    if(camera_eof_deadline) return now<camera_eof_deadline || service_fail(s,"camera","control-close-timeout");
    if(!meiso_display_camera_channel_poll(camera)) {
        if(!meiso_display_camera_channel_peer_eof(camera)) return service_fail(s,"camera","transport");
        /* Independent carriers can deliver camera EOF before Session CLOSE.
         * Stop capture/submit now; only a real control close can finish cleanly. */
        camera_eof_deadline=service_deadline_after_ms(SERVICE_TIMEOUT_MS);
#ifndef MEISO_DISPLAY_INGRESS_DESKTOP_EDGE
        if(!capture_stop()) return service_fail(s,"camera","capture-stop");
#endif
        return true;
    }
    if(capture_halted || meiso_display_camera_channel_stopped(camera)) return true;
    if(!meiso_display_camera_channel_ready(camera))
        return now<camera_deadline || service_fail(s,"camera","negotiation-timeout");
    if(s->link_enabled && !s->leases[MEISO_DISPLAY_LINK_LEASE_CAMERA].desired_version)
        (void)meiso_display_residence_accept(&s->leases[MEISO_DISPLAY_LINK_LEASE_CAMERA],1,now/UINT64_C(1000000));
#ifdef MEISO_DISPLAY_INGRESS_DESKTOP_EDGE
    if(!capture_started) { capture_started=true;capture_last=now; }
    const ssize_t n=read(s->camera_input_fd,capture_pixels+capture_used,sizeof(capture_pixels)-capture_used);
    if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) return service_fail(s,"camera","capture-read");
    if(n==0) return service_fail(s,"camera",capture_used?"partial-frame-eof":"capture-eof");
    if(n>0) capture_used+=(size_t)n;
    if(capture_used==sizeof(capture_pixels)) {
        capture_used=0; ++capture_sequence; capture_last=now;
        if(now>=capture_next) {
            const MeisoCompositionMediaDesired *d=meiso_display_camera_channel_effective(camera);
            if(!d) return service_fail(s,"camera","capture-desired");
            MeisoByteView payload={capture_pixels,sizeof(capture_pixels)};
            if(d->codec==MEISO_COMPOSITION_MEDIA_CODEC_JPEG) {
                const size_t size=meiso_camera_jpeg_rgba(capture_pixels,capture_encoded,sizeof(capture_encoded));
                if(!size) {
                    capture_next=now+UINT64_C(1000000000000)/d->fps_millihz;
                    fprintf(stderr,"CAMERA_DROPPED frame=%"PRIu64" reason=jpeg-invalid-or-oversize\n",capture_sequence);
                    return true;
                }
                payload=(MeisoByteView){capture_encoded,size};
            }
            if(!d || !meiso_display_camera_channel_submit(camera,capture_sequence,(MeisoTimePoint){1,now},
                payload)) return service_fail(s,"camera","capture-submit");
            capture_next=now+UINT64_C(1000000000000)/d->fps_millihz;
            fprintf(stderr,"CAMERA_SUBMITTED frame=%"PRIu64" source_ns=%"PRIu64" bytes=%zu codec=%u exposure=unknown\n",capture_sequence,now,payload.size,d->codec);
            if(d->codec==MEISO_COMPOSITION_MEDIA_CODEC_JPEG) {
                uint8_t digest[32];char hex[65];
                if(!meiso_display_ingress_private_payload_sha256(payload,digest)) return service_fail(s,"camera","digest");
                for(unsigned i=0;i<32;++i)(void)snprintf(hex+2*i,3,"%02x",digest[i]);
                fprintf(stderr,"CAMERA_JPEG frame=%"PRIu64" sha256=%s\n",capture_sequence,hex);
            }
        }
    }
#else
    MeisoCameraV4l2FrameView frame;
    const MeisoCameraV4l2PollStatus status=meiso_camera_v4l2_poll_frame(capture,&frame);
    if(status==MEISO_CAMERA_V4L2_POLL_FRAME) {
        bool ok=true;
        capture_last=now;
        if(now>=capture_next) {
            const MeisoCompositionMediaDesired *d=meiso_display_camera_channel_effective(camera);
            MeisoByteView payload=frame.bytes;
            const uint64_t encode_started=service_now_ns();
            bool dropped=false;
            if(d && d->codec==MEISO_COMPOSITION_MEDIA_CODEC_JPEG) {
                if(frame.bytes.size!=320U*240U*3U/2U) ok=false;
                else {
                    const size_t size=meiso_camera_jpeg_nv12_bt601(frame.bytes.data,capture_encoded,sizeof(capture_encoded));
                    payload=(MeisoByteView){capture_encoded,size};dropped=!size;
                }
            }
            ok=ok && d && (dropped || meiso_display_camera_channel_submit(camera,frame.ref.driver_sequence,frame.source_time,payload));
            if(ok) {
                capture_next=now+UINT64_C(1000000000000)/d->fps_millihz;
                if(dropped) fprintf(stderr,"CAMERA_DROPPED frame=%u reason=jpeg-invalid-or-oversize\n",frame.ref.driver_sequence);
                else {
                    fprintf(stderr,"CAMERA_SUBMITTED frame=%u source_ns=%"PRIu64" bytes=%zu codec=%u encode_submit_ns=%"PRIu64" exposure=unknown timestamp_provenance=%u sequence_provenance=%u\n",
                        frame.ref.driver_sequence,frame.source_time.monotonic_ns,payload.size,d->codec,service_now_ns()-encode_started,frame.timestamp_provenance,frame.sequence_provenance);
                    if(d->codec==MEISO_COMPOSITION_MEDIA_CODEC_JPEG) {
                        uint8_t digest[32];char hex[65];
                        ok=meiso_display_ingress_private_payload_sha256(payload,digest);
                        if(ok) {
                            for(unsigned i=0;i<32;++i)(void)snprintf(hex+2*i,3,"%02x",digest[i]);
                            fprintf(stderr,"CAMERA_JPEG frame=%u sha256=%s\n",frame.ref.driver_sequence,hex);
                        }
                    }
                }
            }
        }
        if(meiso_camera_v4l2_release_frame(capture,&frame.ref)!=MEISO_CAMERA_V4L2_RELEASE_RELEASED)
            return service_fail(s,"camera","capture-release");
        if(!ok) return service_fail(s,"camera","capture-submit");
    } else if(status!=MEISO_CAMERA_V4L2_POLL_WOULD_BLOCK && status!=MEISO_CAMERA_V4L2_POLL_INTERRUPTED)
        return service_fail(s,"camera","capture-poll");
#endif
    return now-capture_last<UINT64_C(3000000000) || service_fail(s,"camera","capture-stalled");
}
static struct { uint64_t sequence; uint8_t digest[32];
    bool projection_present; MeisoDisplayProjection projection; } ledger[16];
static MeisoDisplayIngressPresentationEvidence pending;
static uint64_t pending_time, evidence_sequence;
static bool pending_valid;

bool service_video_host_event(Service *s, const MeisoRuntimeEvent *event)
{ (void)event; return service_fail(s, "application", "edge-only"); }
bool service_run_host_media(Service *s) { return service_fail(s, "application", "edge-only"); }

/* v9/v10 stop remains terminal. v11 can explicitly start a newer production
 * after confirmed removal, without touching the scene adapter or Session. */
static struct { uint64_t version, deadline, restart_version; uint32_t reason;
    bool requested, finished, failed, published; } video_stop;

static bool request_video_stop(Service *s, uint64_t version, uint32_t reason)
{
    if(!video_stop.requested || (s->link_enabled && video_stop.published && video_stop.failed && reason==0)) {
        if(video_stop.requested && meiso_ingress_video_retry_clear(video)) return service_fail(s,"video","retry-clear");
        video_stop.requested=true;video_stop.deadline=service_now_ns()+(uint64_t)(s->clear_grace_ms?s->clear_grace_ms:750)*UINT64_C(1000000);
        if(!s->media_terminated && !service_terminate_media(s,MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE)) return false;
        const MeisoIngressVideoClearResult result=meiso_ingress_video_clear(video);
        video_stop.finished=result!=MEISO_INGRESS_VIDEO_CLEAR_PENDING;
        video_stop.failed=result==MEISO_INGRESS_VIDEO_CLEAR_ERROR;
    }
    s->leases[MEISO_DISPLAY_LINK_LEASE_VIDEO].active=false;
    video_stop.version=version;video_stop.reason=reason;video_stop.published=false;
    fprintf(stderr,"VIDEO_STOP_REQUESTED version=%"PRIu64" geometry=%"PRIu64" reason=%u time_ns=%"PRIu64"\n",
        version,s->video_geometry_desired_version,reason,service_now_ns());
    return true;
}

static int consume_video_stop(Service *s, const MeisoRuntimeEvent *event)
{
    const MeisoRuntimeRouteKey r=service_route_key(s,MEISO_DISPLAY_INGRESS_ROUTE_DESIRED);
    if(event->type!=MEISO_RUNTIME_EVENT_STATE_OFFER ||
        !meiso_resource_id_equal(event->route.resource_id,r.resource_id)) return 0;
    const MeisoStateOffer *offer=&event->state_offer;
    MeisoRuntimeDesiredState desired;
    if(event->route.catalog_generation!=r.catalog_generation || event->route.facet!=r.facet ||
        event->route.endpoint_incarnation!=r.endpoint_incarnation ||
        !meiso_endpoint_id_equal(event->route.endpoint_id,r.endpoint_id) ||
        !meiso_owner_scope_equal(offer->state.owner,s->host_owner) ||
        offer->generation.value!=SERVICE_DESIRED_DELIVERY_GENERATION ||
        !meiso_runtime_desired_state_decode(offer->state.payload,&desired) ||
        desired.profile.size!=sizeof(s->applied_desired_profile) ||
        memcmp(desired.profile.data,s->applied_desired_profile,desired.profile.size)) {
        (void)meiso_runtime_commit_state_receive(s->runtime,event->route,offer,MEISO_STATE_RECEIVE_INVALID);
        return -1;
    }
    const uint64_t version=offer->state.version;
    MeisoStateView previous;
    const bool repeated=meiso_state_replica_current(s->state_replica,&previous) && version<=previous.version;
    if(!repeated && (version<=s->video_geometry_desired_version ||
        (desired.enabled && (!s->link_enabled || !video_stop.published || !video_stop.finished || video_stop.failed)))) {
        (void)meiso_runtime_commit_state_receive(s->runtime,event->route,offer,MEISO_STATE_RECEIVE_INVALID);
        return -1;
    }
    const MeisoStateReceiveStatus received=meiso_state_replica_receive(s->state_replica,
        offer->state.owner,version,offer->state.payload);
    if(meiso_runtime_commit_state_receive(s->runtime,event->route,offer,received)!=MEISO_RUNTIME_OK) return -1;
    if(received==MEISO_STATE_RECEIVE_DUPLICATE || received==MEISO_STATE_RECEIVE_STALE) return 1;
    if(received!=MEISO_STATE_RECEIVE_NEW) return -1;
    if(desired.enabled) {
        video_stop.restart_version=version;
        return 1;
    }
    return request_video_stop(s,version,MEISO_DISPLAY_LINK_TERMINAL_NONE)?1:-1;
}

static bool poll_video_stop(Service *s)
{
    if(!video_stop.requested || video_stop.published) return true;
    if(!video_stop.finished) {
        const MeisoIngressVideoClearResult result=meiso_ingress_video_clear(video);
        video_stop.finished=result!=MEISO_INGRESS_VIDEO_CLEAR_PENDING;
        video_stop.failed=result==MEISO_INGRESS_VIDEO_CLEAR_ERROR;
        if(!video_stop.finished && service_deadline_expired(video_stop.deadline)) {
            video_stop.finished=true; video_stop.failed=true;
        }
    }
    /* Send any actual last-frame evidence before ending its production epoch. */
    if(!video_stop.finished || pending_valid || !service_session_tx_idle(s)) return true;
    MeisoRuntimeActualState actual={0};
    actual.observed_desired=(MeisoRuntimeDesiredReference){true,s->host_owner,video_stop.version};
    actual.applied_desired=(MeisoRuntimeDesiredReference){true,s->host_owner,
        (video_stop.failed || video_stop.reason==MEISO_DISPLAY_LINK_LEASE_EXPIRED)?s->video_geometry_desired_version:video_stop.version};
    actual.apply_result=video_stop.failed?MEISO_RUNTIME_APPLY_FAILED:MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability=MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.production_status=video_stop.failed?MEISO_RUNTIME_PRODUCTION_FAILED:MEISO_RUNTIME_PRODUCTION_STOPPED;
    actual.reason=video_stop.failed?MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED:
        video_stop.reason==MEISO_DISPLAY_LINK_LEASE_EXPIRED?MEISO_RUNTIME_PRODUCTION_REASON_NONE:MEISO_RUNTIME_PRODUCTION_REASON_DISABLED;
    uint8_t profile[8],wire[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE+8];MeisoByteView bytes;
    if(s->link_enabled) {
        if(!meiso_display_link_terminal_encode(video_stop.reason,(MeisoMutableBuffer){profile,8}))
            return service_fail(s,"application","video-stop-reason");
        actual.profile=(MeisoByteView){profile,8};
    }
    MeisoDisplayIngressActualStateView view;
    if(meiso_display_application_validate_video_actual(service_profile_version(s),&s->capability,&s->applied_desired,&actual,&view)!=MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        !meiso_runtime_actual_state_encode(&actual,(MeisoMutableBuffer){wire,sizeof(wire)},&bytes))
        return service_fail(s,"application","video-stop-codec");
    const MeisoStateOffer offer={{SERVICE_ACTUAL_DELIVERY_GENERATION},{s->edge_owner,s->actual_version+1,bytes}};
    const MeisoRuntimeStatus sent=meiso_runtime_send_state_offer(s->runtime,
        service_route_key(s,MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL),&offer);
    if(sent==MEISO_RUNTIME_TX_BUSY) return true;
    if(sent!=MEISO_RUNTIME_OK) return service_fail(s,"control","video-stop-actual");
    ++s->actual_version;video_stop.published=true;
    if(!video_stop.failed && video_stop.reason!=MEISO_DISPLAY_LINK_LEASE_EXPIRED) {
        s->applied_desired.enabled=false;s->applied_desired_version=video_stop.version;
    }
    fprintf(stderr,"VIDEO_STOP_RESULT version=%"PRIu64" cleared=%d evidence=%u unobserved=%"PRIu64" time_ns=%"PRIu64"\n",
        video_stop.version,!video_stop.failed,SERVICE_EVIDENCE_CLASS,ledger_count,service_now_ns());
    /* Local decoder correlation retirement is not a negative frame receipt. */
    if(!video_stop.failed) {memset(ledger,0,sizeof(ledger));ledger_count=0;}
    return true;
}

static bool poll_video_restart(Service *s)
{
    if(!video_stop.restart_version || !service_session_tx_idle(s)) return true;
    const uint64_t version=video_stop.restart_version;video_stop.restart_version=0;
    if(meiso_ingress_video_reactivate(video)) {
        video_stop.version=version;video_stop.failed=true;video_stop.published=false;video_stop.reason=0;
        return true; /* FAILED/APPLY_FAILED preserves the last enabled identity. */
    }
    s->media=NULL;s->udp=NULL;s->binding_ready=false;
    s->applied_desired.enabled=true;s->applied_desired_version=version;s->video_geometry_desired_version=version;
    if(!service_queue_running_actual_and_arm(s)) return false;
    memset(&video_stop,0,sizeof(video_stop));
    memset(ledger,0,sizeof(ledger));ledger_count=0;last_evidence_sequence=0;
    (void)meiso_display_residence_accept(&s->leases[MEISO_DISPLAY_LINK_LEASE_VIDEO],version,service_now_ns()/UINT64_C(1000000));
    fprintf(stderr,"VIDEO_REACTIVATED version=%"PRIu64" epoch=%"PRIu64" session_unchanged=true\n",version,s->production_epoch);
    return true;
}

static bool receive_batch(Service *s)
{
    for (unsigned i = 0; i < 64; ++i) {
        MeisoDisplayIngressMediaPosixUdpReadStatus r = meiso_display_ingress_media_posix_udp_on_readable(s->udp, NULL);
        if (r == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_WOULD_BLOCK ||
            r == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INTERRUPTED) return true;
        if (r == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_PACKET_REJECTED ||
            r == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRUNCATED) ++rejected;
        else if (r != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_PACKET_ACCEPTED &&
            r != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED &&
            r != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED &&
            r != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_REASSEMBLY_REPLACED)
            return service_fail(s, "media", "video-receive");
    }
    return true;
}

static bool decode_latest(Service *s)
{
    MeisoDisplayIngressMediaLatestView latest;
    MeisoDisplayIngressMediaTransportStatus status = meiso_display_ingress_media_transport_latest(s->media, &latest);
    if (status == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_FRAME) return true;
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) return service_fail(s, "media", "video-latest");
    unsigned slot = 0;
    while (slot < 16 && ledger[slot].sequence != 0) ++slot;
    if (slot == 16) return true;
    bool projection_present=false;
    MeisoDisplayProjection projection={0};
    if (meiso_display_projection_sei_read(latest.frame.access_unit,&projection_present,&projection)
        != MEISO_DISPLAY_INGRESS_PROFILE_OK) return service_fail(s,"media","projection-sei-invalid");
    MeisoIngressVideoSubmitResult r = meiso_ingress_video_submit(video, latest.frame.frame_sequence,
        latest.frame.access_unit.data, latest.frame.access_unit.size);
    if (r == MEISO_INGRESS_VIDEO_WOULD_BLOCK) return true;
    if (r != MEISO_INGRESS_VIDEO_ACCEPTED) return service_fail(s, "decoder", "submit");
    ledger[slot].sequence = latest.frame.frame_sequence;
    ledger[slot].projection_present=projection_present;
    ledger[slot].projection=projection;
    if (!meiso_display_ingress_private_payload_sha256(latest.frame.access_unit, ledger[slot].digest))
        return service_fail(s, "application", "video-digest");
    ++ledger_count; ++submitted;
    return meiso_display_ingress_media_transport_release_latest(s->media, &latest.ref) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK ||
        service_fail(s, "media", "video-release");
}

static void observe_memory(void)
{
    char text[2048];
    int fd=open("/proc/meminfo",O_RDONLY|O_CLOEXEC);
    if(fd<0) return;
    ssize_t n=read(fd,text,sizeof(text)-1); close(fd);
    if(n<=0) return;
    text[n]=0;
    const char *available=strstr(text,"MemAvailable:");
    const char *cma=strstr(text,"CmaFree:");
    if(available && cma) fprintf(stderr,"MEMORY_PROGRESS available_kib=%lu cma_free_kib=%lu\n",
        strtoul(available+13,NULL,10),strtoul(cma+8,NULL,10));
}

static bool poll_presentation(Service *s)
{
    MeisoIngressVideoPresentation p;
    MeisoIngressVideoPollResult r = meiso_ingress_video_poll(video, &p);
    if (r == MEISO_INGRESS_VIDEO_NONE) return true;
    if (r != MEISO_INGRESS_VIDEO_PRESENTED) return service_fail(s, "display", "poll");
    unsigned slot = 0;
    while (slot < 16 && ledger[slot].sequence != p.sequence) ++slot;
    if (slot == 16 || p.sequence <= last_evidence_sequence || p.page_flip_monotonic_ns == 0)
        return service_fail(s, "display", "frame-identity");
    memset(&pending, 0, sizeof(pending));
    pending.evidence_class = SERVICE_EVIDENCE_CLASS;
    pending.connector_kind = p.connector_kind == MEISO_INGRESS_VIDEO_CONNECTOR_DSI ? MEISO_DISPLAY_INGRESS_CONNECTOR_DSI :
        p.connector_kind == MEISO_INGRESS_VIDEO_CONNECTOR_HDMI ? MEISO_DISPLAY_INGRESS_CONNECTOR_HDMI : MEISO_DISPLAY_INGRESS_CONNECTOR_UNSPECIFIED;
    pending.output_width = p.output_width; pending.output_height = p.output_height;
    pending.media_stream_epoch = s->media_stream_epoch;
    pending.displayed_frame_sequence = p.sequence;
    memcpy(pending.input_frame_sha256, ledger[slot].digest, 32);
    if (ledger[slot].projection_present && (presented==0 || presented%30==0)) {
        const MeisoDisplayProjection *v=&ledger[slot].projection;
        fprintf(stderr,"PROJECTION_PRESENTED frame=%"PRIu64" views=%u space=%u/%"PRIu64"/%"PRIu64
            " render_target=%"PRIu64"/%"PRIu64" view_x=%g,%g first_fov=%g,%g,%g,%g correction=none\n",
            p.sequence,v->view_count,v->space_class,v->source_space_id,v->source_space_epoch,
            v->target_time.clock_epoch,v->target_time.monotonic_ns,
            (double)v->views[0].position_meters[0],(double)v->views[1].position_meters[0],
            (double)v->views[0].fov_left_right_up_down[0],(double)v->views[0].fov_left_right_up_down[1],
            (double)v->views[0].fov_left_right_up_down[2],(double)v->views[0].fov_left_right_up_down[3]);
        for (unsigned i=0;i<v->view_count;++i) {
            const MeisoDisplayProjectionView *w=&v->views[i];
            fprintf(stderr,"PROJECTION_VIEW frame=%"PRIu64" view=%u position=%.9g,%.9g,%.9g"
                " orientation=%.9g,%.9g,%.9g,%.9g fov=%.9g,%.9g,%.9g,%.9g\n",p.sequence,i,
                (double)w->position_meters[0],(double)w->position_meters[1],(double)w->position_meters[2],
                (double)w->orientation_xyzw[0],(double)w->orientation_xyzw[1],
                (double)w->orientation_xyzw[2],(double)w->orientation_xyzw[3],
                (double)w->fov_left_right_up_down[0],(double)w->fov_left_right_up_down[1],
                (double)w->fov_left_right_up_down[2],(double)w->fov_left_right_up_down[3]);
        }
    }
    ledger[slot].sequence = 0; --ledger_count;
    last_evidence_sequence = p.sequence; ++presented;
    if (s->overlay_enabled && (presented==1 || presented%30==0)) {
        observe_memory();
        fprintf(stderr,"VIDEO_PROGRESS frame=%"PRIu64" epoch=%"PRIu64" presented=%"PRIu64" flip_ns=%"PRIu64"\n",
            p.sequence,s->production_epoch,presented,p.page_flip_monotonic_ns);
    }
    pending_time = p.page_flip_monotonic_ns; pending_valid = true;
    return true;
}

static bool queue_presentation(Service *s)
{
    if (!pending_valid || !service_session_tx_idle(s)) return true;
    uint8_t payload[MEISO_DISPLAY_INGRESS_PROFILE_EVIDENCE_WIRE_SIZE];
    size_t size;
    if (!meiso_display_ingress_evidence_supported(&s->capability, &pending, s->production_epoch) ||
        meiso_display_ingress_evidence_encode(&pending, (MeisoMutableBuffer){payload,sizeof(payload)}, &size) != MEISO_DISPLAY_INGRESS_PROFILE_OK)
        return service_fail(s, "application", "video-evidence");
    MeisoStreamSample sample;
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){s->edge_owner, SERVICE_EVIDENCE_STREAM_EPOCH, evidence_sequence + 1U};
    sample.source_time = (MeisoTimePoint){SERVICE_EVIDENCE_CLOCK_EPOCH, pending_time};
    sample.payload = (MeisoByteView){payload, size};
    MeisoRuntimeStatus r = meiso_runtime_send_stream_sample(s->runtime,
        service_route_key(s, MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE), s->production_epoch, &sample);
    if (r == MEISO_RUNTIME_TX_BUSY) return true;
    if (r != MEISO_RUNTIME_OK) return service_fail(s, "control", "video-evidence");
    ++evidence_sequence; ++evidence_count; pending_valid = false;
    return true;
}

bool service_run_edge_media(Service *s)
{
    if (!service_overlay_initialize(s, video)) return false;
    if(s->link_enabled) {
        s->link=meiso_display_link_channel_open(s->session,s->runtime,NULL);
        if(!s->link) return service_fail(s,"link","open");
        (void)meiso_display_residence_accept(&s->leases[0],s->applied_desired_version,service_now_ns()/UINT64_C(1000000));
    }
    if(s->camera_enabled) {
        MeisoDisplayCameraChannelConfig config={.session=s->session,.runtime=s->runtime,.media_fd=s->camera_media_fd,
            .application_version=service_profile_version(s)};
        if(!service_random_bytes(&config.edge_nonce,sizeof(config.edge_nonce))) return false;
        camera=meiso_display_camera_channel_open(&config);
        if(!camera) return service_fail(s,"camera","open");
        capture_last=service_now_ns(); camera_deadline=capture_last+UINT64_C(10000000000);
    }
    const uint64_t deadline = service_now_ns() + frame_limit * UINT64_C(1000000000) / 30U + UINT64_C(15000000000);
    uint64_t last_progress = service_now_ns(), previous_presented = 0;
    while (!interrupted && (continuous_edge || service_now_ns() < deadline)) {
        if(s->link_enabled && meiso_display_residence_expired(&s->leases[0],service_now_ns()/UINT64_C(1000000)) &&
            !request_video_stop(s,s->applied_desired_version,MEISO_DISPLAY_LINK_LEASE_EXPIRED)) return false;
        if ((!video_stop.requested && (!receive_batch(s) || !decode_latest(s))) ||
            (!video_stop.published && !video_stop.failed && !poll_presentation(s)) || !service_overlay_poll(s)) return false;
        if(!camera_tick(s)) return false;
        MeisoRuntimeEvent event;
        ServiceControlStep step = service_control_step(s, true, 0, &event);
        if (step == SERVICE_CONTROL_PEER_CLOSED) {
            if (presented == 0 && !s->overlay_enabled)
                return service_fail(s, "application", "video-no-presentation");
            return (s->media_terminated || service_terminate_media(s, MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_CONTROL_SESSION)) && service_finish_peer_close(s);
        }
        if (step == SERVICE_CONTROL_FAILED) return false;
        if (step == SERVICE_CONTROL_EVENT) {
            int consumed=0;
            if(s->link) consumed=meiso_display_link_channel_consume(s->link,&event);
            if(!consumed) consumed=consume_video_stop(s,&event);
            if(!consumed && camera) consumed=meiso_display_camera_channel_consume(camera,&event);
            if(consumed<0 || (!consumed && !service_overlay_consume(s,&event))) return service_fail(s,"camera","control");
        }
        if(s->link) {
            MeisoDisplayLinkDesired settings;
            const uint64_t version=meiso_display_link_channel_effective(s->link,&settings);
            if(version && version!=s->link_version) {
                for(unsigned i=0;i<MEISO_DISPLAY_LINK_LEASE_COUNT;++i)
                    meiso_display_residence_renew(&s->leases[i],settings.lease_ms[i],service_now_ns()/UINT64_C(1000000));
                s->link_version=version;s->clear_grace_ms=settings.clear_grace_ms;
            }
            if(!meiso_display_link_channel_poll(s->link,service_now_ns()/UINT64_C(1000000))) return service_fail(s,"link","heartbeat");
        }
        if (!queue_presentation(s) || !poll_video_stop(s) || !poll_video_restart(s)) return false;
        if (presented != previous_presented || ledger_count == 0) last_progress = service_now_ns();
        previous_presented = presented;
        if (!video_stop.requested && ledger_count != 0 && service_now_ns() - last_progress > UINT64_C(5000000000))
            return service_fail(s, "decoder", "no-output");
        struct pollfd p[2] = {{s->control_fd, POLLIN, 0}, {video_stop.requested ? -1 : s->media_fd, POLLIN, 0}};
        (void)poll(p, 2, 1);
    }
    return service_fail(s, "application", "video-timeout");
}
#endif

static bool number(const char *text, uint64_t max, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    if (text == NULL || *text < '0' || *text > '9') return false;
    unsigned long long n = strtoull(text, &end, 10);
    if (errno != 0 || *end != 0 || n > max) return false;
    *out = (uint64_t)n; return true;
}

int main(int argc, char **argv)
{
    /* Caller owns connecting TCP/UDP; this executable assumes no deployment address. */
    const char *input = NULL, *video_path = "/dev/video0", *drm_path = "/dev/dri/card0";
#if defined(MEISO_DISPLAY_INGRESS_VIDEO_EDGE) && !defined(MEISO_DISPLAY_INGRESS_DESKTOP_EDGE)
    const char *camera_path="/dev/video1";
#endif
    memset(&service, 0, sizeof(service));
    service.control_fd = service.media_fd = service.frame_fd = -1;
    service.camera_input_fd=service.camera_media_fd=-1;
#ifdef MEISO_DISPLAY_INGRESS_VIDEO_EDGE
    service.role = MEISO_SESSION_ROLE_EDGE; service.role_name = "edge";
#else
    service.role = MEISO_SESSION_ROLE_HOST; service.role_name = "host";
#endif
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) return 2;
        uint64_t n;
        if (strcmp(argv[i], "--control-fd") == 0 && number(argv[i+1], 1048575U, &n)) service.control_fd = (int)n;
        else if (strcmp(argv[i], "--local-peer") == 0 && service_parse_peer_id(argv[i+1], &service.local_peer)) { }
        else if (strcmp(argv[i], "--expected-peer") == 0 && service_parse_peer_id(argv[i+1], &service.expected_peer)) { }
        else if (strcmp(argv[i], "--media-fd") == 0 && number(argv[i+1], 1048575U, &n)) service.media_fd = (int)n;
        else if (strcmp(argv[i], "--frames") == 0 && number(argv[i+1], 18000U, &n) && n != 0) frame_limit = n;
        else if (strcmp(argv[i], "--role") == 0 && strcmp(argv[i+1], service.role_name) == 0) { /* Explicit role must match this build. */ }
        else if (strcmp(argv[i], "--width") == 0 && number(argv[i+1], 1920U, &n) && n >= 16 && n%8 == 0) service.selected_input_width=(uint32_t)n;
        else if (strcmp(argv[i], "--height") == 0 && number(argv[i+1], 1080U, &n) && n >= 16 && n%2 == 0) service.selected_input_height=(uint32_t)n;
        else if (strcmp(argv[i], "--source") == 0) input = argv[i+1];
        else if (strcmp(argv[i], "--video-device") == 0) video_path = argv[i+1];
        else if (strcmp(argv[i], "--drm-device") == 0) drm_path = argv[i+1];
#ifdef MEISO_DISPLAY_INGRESS_DESKTOP_EDGE
        else if (strcmp(argv[i], "--output-width") == 0 && number(argv[i+1],4096U,&n) && n) service.output_width=(uint32_t)n;
        else if (strcmp(argv[i], "--output-height") == 0 && number(argv[i+1],4096U,&n) && n) service.output_height=(uint32_t)n;
        else if (strcmp(argv[i], "--output-directory") == 0) drm_path = argv[i+1];
        else if (strcmp(argv[i], "--camera-input-fd") == 0 && number(argv[i+1],1048575U,&n)) service.camera_input_fd=(int)n;
#endif
#ifdef MEISO_DISPLAY_INGRESS_VIDEO_EDGE
        else if (strcmp(argv[i], "--link-control") == 0 && strcmp(argv[i+1], "on") == 0) service.link_enabled=true;
        else if (strcmp(argv[i], "--camera-codec") == 0 && strcmp(argv[i+1],"jpeg")==0) capture_jpeg=true;
        else if (strcmp(argv[i], "--camera-media-fd") == 0 && number(argv[i+1],1048575U,&n)) service.camera_media_fd=(int)n;
#ifndef MEISO_DISPLAY_INGRESS_DESKTOP_EDGE
        else if (strcmp(argv[i], "--camera-device") == 0) camera_path=argv[i+1];
        else if (strcmp(argv[i], "--camera-colorimetry") == 0 && strcmp(argv[i+1],"bt601-limited")==0) capture_bt601_known=true;
#endif
        else if (strcmp(argv[i], "--continuous") == 0 && strcmp(argv[i+1], "on") == 0) continuous_edge=true;
        else if (strcmp(argv[i], "--video-layout") == 0 &&
            (strcmp(argv[i+1], "native") == 0 || strcmp(argv[i+1], "side-by-side") == 0))
            service.stereo_video=strcmp(argv[i+1], "side-by-side") == 0;
        else if (strcmp(argv[i], "--native-overlay") == 0 && strcmp(argv[i+1], "on") == 0) service.overlay_enabled=true;
        else if (strcmp(argv[i], "--native-overlay") == 0 && strcmp(argv[i+1], "scene") == 0) service.overlay_enabled=service.scene_enabled=true;
#endif
        else return 2;
    }
    const bool local_set = meiso_peer_id_valid(service.local_peer);
    const bool expected_set = meiso_peer_id_valid(service.expected_peer);
    if (local_set != expected_set || (local_set && meiso_peer_id_equal(service.local_peer, service.expected_peer))) {
        fprintf(stderr, "local/expected peer IDs must be supplied together and differ\n");
        return 2;
    }
#ifdef MEISO_DISPLAY_INGRESS_VIDEO_EDGE
    if(service.link_enabled && service.overlay_enabled && !service.scene_enabled) {
        fprintf(stderr,"v11 link control requires native-overlay scene when HUD is selected\n");return 2;
    }
    service.camera_enabled=service.camera_media_fd>=0;
    if(service.camera_enabled && (service.camera_media_fd==service.control_fd ||
            service.camera_media_fd==service.media_fd)) {
        fprintf(stderr,"camera requires a separate media descriptor\n");
        return 2;
    }
#ifdef MEISO_DISPLAY_INGRESS_DESKTOP_EDGE
    if ((service.camera_input_fd>=0)!=(service.camera_media_fd>=0)) return 2;
    service.camera_enabled=service.camera_input_fd>=0;
    if(capture_jpeg && !service.camera_enabled) {fprintf(stderr,"JPEG mode requires camera input\n");return 2;}
    if (service.camera_enabled) {
        const int fds[]={service.control_fd,service.media_fd,service.camera_input_fd,service.camera_media_fd};
        for(unsigned i=0;i<4;++i) for(unsigned j=i+1;j<4;++j) if(fds[i]==fds[j]) return 2;
        int flags=fcntl(service.camera_input_fd,F_GETFL);
        if(flags<0 || fcntl(service.camera_input_fd,F_SETFL,flags|O_NONBLOCK)<0) return 2;
        service.camera_capability=(MeisoCompositionMediaCapability){.source=MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
            .pixel_formats=MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888,.codecs=MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
            .min_width=320,.min_height=240,.max_width=320,.max_height=240,.max_fps_millihz=5000,
            .max_access_unit_bytes=320*240*4,.max_fragment_payload_bytes=4096};
        if(capture_jpeg) {
            service.camera_capability.codecs=MEISO_COMPOSITION_MEDIA_CODEC_MASK_JPEG;
            service.camera_capability.max_fps_millihz=500;
            service.camera_capability.max_access_unit_bytes=sizeof(capture_encoded);
            service.camera_capability.max_bitrate_bps=150000;
        }
    }
    if (strcmp(drm_path,"/dev/dri/card0")==0) {
        fprintf(stderr,"desktop Edge requires an explicit new --output-directory\n"); return 2;
    }
    fprintf(stderr,"DESKTOP_EDGE evidence=SIMULATED physical_presentation=false backend=ffmpeg-offscreen\n");
#else
    if(service.camera_enabled) service.camera_capability=(MeisoCompositionMediaCapability){
        .source=MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,.pixel_formats=MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        .codecs=MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,.min_width=320,.min_height=240,.max_width=320,.max_height=240,
        .max_fps_millihz=5000,.max_access_unit_bytes=320*240*3/2,.max_fragment_payload_bytes=4096};
    if(capture_jpeg) {
        if(!service.camera_enabled || !capture_bt601_known) {
            fprintf(stderr,"JPEG mode requires camera and explicit --camera-colorimetry bt601-limited\n");return 2;
        }
        service.camera_capability.pixel_formats=MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888;
        service.camera_capability.codecs=MEISO_COMPOSITION_MEDIA_CODEC_MASK_JPEG;
        service.camera_capability.max_fps_millihz=500;
        service.camera_capability.max_access_unit_bytes=sizeof(capture_encoded);
        service.camera_capability.max_bitrate_bps=150000;
    }
#endif
    if (continuous_edge && !local_set) {
        fprintf(stderr, "continuous Edge requires --local-peer and --expected-peer\n");
        return 2;
    }
#endif
    signal(SIGPIPE, SIG_IGN); signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (service.control_fd < 0 || service.media_fd < 0 || service.control_fd == service.media_fd) return 2;
#ifdef MEISO_DISPLAY_INGRESS_VIDEO_EDGE
    (void)input;
    video = meiso_ingress_video_open(video_path, drm_path, service_frame_width(&service), service_frame_height(&service));
    if (video == NULL) { fprintf(stderr, "video backend open failed\n"); return 1; }
#ifdef MEISO_DISPLAY_INGRESS_DESKTOP_EDGE
    if ((service.output_width==0)!=(service.output_height==0) ||
        (service.output_width && meiso_ingress_video_set_output_size(video,service.output_width,service.output_height))) {
        meiso_ingress_video_close(video); return 2;
    }
#endif
    if(meiso_ingress_video_output_size(video,&service.output_width,&service.output_height)) {
        meiso_ingress_video_close(video); return 1;
    }
    if (service.stereo_video && meiso_ingress_video_set_stereo(video)) {
        fprintf(stderr, "stereo video output unavailable\n");
        meiso_ingress_video_close(video); return 1;
    }
    if (service.overlay_enabled) {
        /* Canaan DRM framebuffers are at least 16x16; VO stride is in 8-byte
         * units, hence an even ARGB width. Publish these actual constraints. */
        service.overlay_capability=(MeisoDisplayOverlayCapability){0,0,65536,48,10,16,16,2,1,3,1};
        const char *font_path=getenv("MEISO_OVERLAY_FONT");
        if(font_path && *font_path) {
            if(!meiso_k230d_overlay_font_open(font_path)) {
                fprintf(stderr,"invalid or unavailable CJK16 font asset\n");
                meiso_ingress_video_close(video); return 2;
            }
            service.overlay_capability.fonts|=2;
        }
        if(service.scene_enabled && meiso_ingress_video_scene_prepare(video,service.overlay_capability.fonts,&service.scene_capability)) {
            fprintf(stderr,"native scene output/resource budget unavailable\n");meiso_ingress_video_close(video);return 1;
        }
        if (meiso_ingress_video_output_size(video,&service.overlay_capability.output_width,
                &service.overlay_capability.output_height)) { meiso_ingress_video_close(video); return 1; }
    }
#else
    (void)video_path; (void)drm_path;
    if (input == NULL || !open_source(input)) { fprintf(stderr, "invalid H264 IDR source\n"); return 2; }
    fprintf(stderr, "SOURCE units=%zu bytes=%zu\n", unit_count, source_size);
#endif
    if(service.link_enabled) {
        service.link_capability=(MeisoDisplayLinkCapability){7,{1000,20000,100000},{1000,20000,100000},
            service_frame_width(&service)*service_frame_height(&service),
            service.scene_enabled?service.scene_capability.max_pixels:service.output_width*service.output_height};
    }
    bool ok=true;
#if defined(MEISO_DISPLAY_INGRESS_VIDEO_EDGE) && !defined(MEISO_DISPLAY_INGRESS_DESKTOP_EDGE)
    if(service.camera_enabled) ok=capture_open(camera_path);
#endif
    if(ok) ok=service_run(&service);
    MeisoDisplayIngressMediaTransportSnapshot media_state;
    memset(&media_state, 0, sizeof(media_state));
    if (service.media != NULL) (void)meiso_display_ingress_media_transport_snapshot(service.media, &media_state);
    int restored = 1;
#ifdef MEISO_DISPLAY_INGRESS_VIDEO_EDGE
    service_overlay_finish();
    meiso_display_link_channel_close(service.link);
    meiso_display_camera_channel_close(camera);
    service_close_fd(&service.camera_media_fd); service_close_fd(&service.camera_input_fd);
#ifndef MEISO_DISPLAY_INGRESS_DESKTOP_EDGE
    if(!capture_stop()) ok=false;
    free(capture_arena);
#endif
    meiso_k230d_overlay_font_close();
    restored = meiso_ingress_video_close(video) == 0;
#else
    if (source != NULL) munmap(source, source_size);
#endif
    service_close_fd(&service.control_fd); service_close_fd(&service.media_fd);
    printf("VIDEO_RESULT role=%s ok=%d submitted=%" PRIu64 " presented=%" PRIu64
        " evidence=%" PRIu64 " last_frame=%" PRIu64 " rejected=%" PRIu64 " pending=%" PRIu64
        " transport_drops=%" PRIu64 " reassembly_drops=%" PRIu64 " latest_drops=%" PRIu64
        " restored=%d close_clean=%d failure=%s/%s evidence_class=%u\n",
        service.role_name, ok && restored, submitted, presented, evidence_count, last_evidence_sequence,
        rejected, ledger_count, media_state.transport_whole_frames_dropped, media_state.reassembly_frames_dropped,
        media_state.unread_latest_frames_dropped, restored, service.close_clean,
        service.failure_plane ? service.failure_plane : "none", service.failure_stage ? service.failure_stage : "none",
        (unsigned)SERVICE_EVIDENCE_CLASS);
    return ok && restored ? 0 : 1;
}
