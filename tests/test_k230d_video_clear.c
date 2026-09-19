/* Exercise the product adapter's video/scene clear paths with injected DRM syscalls. No
 * second scanout implementation, target device or public test hook. */
#define _POSIX_C_SOURCE 200809L
#include <poll.h>
#include <stdarg.h>
int test_poll(struct pollfd *,nfds_t,int);
int test_ioctl(int,unsigned long,...);
#define drmModeAtomicAddProperty test_add_property
#define drmModeAtomicCommit test_commit
#define drmModeObjectGetProperties test_get_properties
#define drmModeGetProperty test_get_property
#define drmHandleEvent test_handle_event
#define poll test_poll
#define ioctl test_ioctl
#include "../targets/k230d-linux/display_ingress_video_backend.c"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static unsigned added, commits, event_ready, bad_plane, reject_commit, wrong_crtc;
static unsigned scene_case, scene_active, syncs, zero_time, bad_binding, read_error;
static uint64_t scene_values[NPROPS];
static void *event_cookie;
static int bad_operation;
int test_add_property(drmModeAtomicReqPtr req,uint32_t object,uint32_t property,uint64_t value)
{
    (void)req;
    if(scene_case) {
        if(object==7) {
            if(!scene_active || property!=11 || value!=1)bad_operation=1;
        } else if(object!=51 || !property || property>NPROPS || value!=scene_values[property-1])
            bad_operation=1;
    } else if((object!=31 && object!=41) || !property || property>NPROPS || value) bad_operation=1;
    ++added;return 0;
}
int test_commit(int fd,const drmModeAtomicReqPtr req,uint32_t flags,void *cookie)
{
    (void)req;
    if(fd!=9 || added!=(scene_case?NPROPS+scene_active:20) || (flags!=DRM_MODE_ATOMIC_TEST_ONLY &&
        flags!=(DRM_MODE_ATOMIC_NONBLOCK|DRM_MODE_PAGE_FLIP_EVENT))) bad_operation=1;
    added=0;++commits;
    if(reject_commit) {errno=EIO;return -1;}
    if(flags!=DRM_MODE_ATOMIC_TEST_ONLY) event_cookie=cookie;
    return 0;
}
drmModeObjectPropertiesPtr test_get_properties(int fd,uint32_t object,uint32_t type)
{
    if(fd!=9 || (scene_case?object!=51:(object!=31 && object!=41)) || type!=DRM_MODE_OBJECT_PLANE) bad_operation=1;
    if(read_error)return NULL;
    drmModeObjectPropertiesPtr p=calloc(1,sizeof(*p));
    p->count_props=NPROPS;p->props=calloc(NPROPS,sizeof(*p->props));p->prop_values=calloc(NPROPS,sizeof(*p->prop_values));
    for(unsigned i=0;i<NPROPS;++i)p->props[i]=i+1;
    if(bad_plane==object)p->prop_values[0]=52;
    if(bad_binding)p->prop_values[1]=7;
    return p;
}
drmModePropertyPtr test_get_property(int fd,uint32_t property)
{
    if(fd!=9 || !property || property>NPROPS) {bad_operation=1;return NULL;}
    drmModePropertyPtr p=calloc(1,sizeof(*p));p->prop_id=property;
    strcpy(p->name,property_names[property-1]);return p;
}
int test_poll(struct pollfd *fds,nfds_t count,int timeout)
{
    if(count!=1 || fds[0].fd!=9 || timeout!=0)bad_operation=1;
    fds[0].revents=event_ready?POLLIN:0;return event_ready?1:0;
}
int test_handle_event(int fd,drmEventContextPtr context)
{
    context->page_flip_handler2(fd,1,zero_time?0:1,zero_time?0:2,wrong_crtc?99:7,event_cookie);
    event_ready=0;return 0;
}
int test_ioctl(int fd,unsigned long request,...)
{
    va_list args;va_start(args,request);
    struct dma_buf_sync *sync=va_arg(args,struct dma_buf_sync *);va_end(args);
    if((fd!=20 && fd!=21) || request!=DMA_BUF_IOCTL_SYNC ||
       (sync->flags!=(DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE) &&
        sync->flags!=(DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE)))bad_operation=1;
    ++syncs;return 0;
}
static struct MeisoIngressVideo adapter(void)
{
    struct MeisoIngressVideo v={0};v.drm_fd=9;v.crtc=7;v.plane=31;v.right.plane=41;
    v.state=READY;v.shown_fb=52;v.held_capture=3;v.shown_handles[0]=42;v.shown_handles[1]=43;
    for(unsigned i=0;i<NPROPS;++i)v.prop_ids[i]=v.right.props[i]=i+1;
    added=commits=event_ready=bad_plane=reject_commit=wrong_crtc=0;bad_operation=0;event_cookie=NULL;
    scene_case=scene_active=syncs=zero_time=bad_binding=read_error=0;
    memset(scene_values,0,sizeof(scene_values));
    return v;
}
static struct MeisoIngressVideo scene_adapter(void)
{
    static uint16_t pixels[2][256];
    struct MeisoIngressVideo v=adapter();scene_case=scene_active=1;
    v.scene_prepared=1;v.scene_crtc_active_property=11;
    v.output_w=1920;v.output_h=1080;v.overlay.plane=51;
    v.overlay.front=v.overlay.target=-1;
    v.scene_capability=(MeisoDisplaySceneCapability){1920,1080,256,48,10,16,16,8,1,16,1};
    for(unsigned i=0;i<NPROPS;++i)v.overlay.props[i]=i+1;
    for(unsigned i=0;i<2;++i) {
        v.overlay.surfaces[i]=(struct surface){20+(int)i,71+i,pixels[i],sizeof(pixels[i])};
        v.overlay.fb[i]=61+i;v.overlay.width[i]=v.overlay.height[i]=16;
    }
    return v;
}
int main(void)
{
    struct MeisoIngressVideo v=adapter();
    CHECK(meiso_ingress_video_reactivate(&v)==-1 && errno==EBUSY && !bad_operation);
    CHECK(meiso_ingress_video_retry_clear(&v)==-1);
    v.flip.pending=1;v.overlay.pending=1;
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING && commits==0);
    const uint8_t au[]={0};
    CHECK(meiso_ingress_video_submit(&v,4,au,sizeof(au))==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
    /* Existing poll owns these completions; a stop must not erase either. */
    CHECK(v.flip.pending && v.overlay.pending);v.flip.pending=0;
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING && commits==0);
    v.overlay.pending=0;
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING && commits==2);
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING); // no event: no proof
    wrong_crtc=1;event_ready=1;
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING);
    wrong_crtc=0;event_ready=1;
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEARED && !bad_operation);
    CHECK(v.shown_fb==52 && v.shown_handles[0]==42 && v.held_capture==3); // resources retained
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEARED && commits==2);
    MeisoIngressVideoPresentation p;
    CHECK(meiso_ingress_video_poll(&v,&p)==MEISO_INGRESS_VIDEO_NONE); // no resurrection/no V4L ioctl
    v=adapter();reject_commit=1;
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_ERROR && v.shown_fb==52);
    reject_commit=0;CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_ERROR && commits==1);
    CHECK(meiso_ingress_video_reactivate(&v)==-1 && errno==EBUSY);
    CHECK(meiso_ingress_video_retry_clear(&v)==0 && !v.clear.failed && !v.clear.completed);
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING && commits==3);
    event_ready=1;CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEARED && !bad_operation);
    /* A timed-out clear may still own an event: retry must retain its cookie and
     * cannot accept a new decoder pool while that event is outstanding. */
    v=adapter();CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING);
    v.clear.failed=1;void *retained_cookie=event_cookie;
    CHECK(meiso_ingress_video_retry_clear(&v)==0 && v.clear.pending && event_cookie==retained_cookie);
    CHECK(meiso_ingress_video_reactivate(&v)==-1 && errno==EBUSY);
    event_ready=1;CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEARED && commits==2 && !bad_operation);
    bad_plane=41;CHECK(meiso_ingress_video_reactivate(&v)==-1 && errno==EBUSY && !bad_operation);
    for(unsigned plane=31;plane<=41;plane+=10) {
        v=adapter();CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING);
        event_ready=1;bad_plane=plane;
        CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_ERROR && v.shown_fb==52 && !bad_operation);
    }
    v=adapter();v.shown_fb=0;
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEARED && commits==0 && !bad_operation);
    /* Empty even before first rendering, and repeated empty, have their own
     * CRTC event. No fake completion, pixel writes or video-plane property edits. */
    v=scene_adapter();
    MeisoDisplayScenePlan plan={0};
    plan.generation=1;plan.video_desired_version=17;plan.canvas_width=1920;plan.canvas_height=1080;
    CHECK(meiso_ingress_video_scene_submit(&v,1,&plan)==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_overlay_poll(&v,&p)==MEISO_INGRESS_VIDEO_NONE);
    wrong_crtc=1;event_ready=1;
    CHECK(meiso_ingress_video_overlay_poll(&v,&p)==MEISO_INGRESS_VIDEO_NONE);
    wrong_crtc=0;event_ready=1;
    CHECK(meiso_ingress_video_overlay_poll(&v,&p)==MEISO_INGRESS_VIDEO_PRESENTED);
    CHECK(p.sequence==1 && p.page_flip_monotonic_ns==1000002000 && v.overlay.front==-1 && !syncs);
    CHECK(meiso_ingress_video_scene_submit(&v,1,&plan)==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
    CHECK(meiso_ingress_video_scene_submit(&v,2,&plan)==MEISO_INGRESS_VIDEO_ACCEPTED);
    event_ready=1;CHECK(meiso_ingress_video_overlay_poll(&v,&p)==MEISO_INGRESS_VIDEO_PRESENTED);
    CHECK(p.sequence==2 && !syncs && !bad_operation);
    /* Re-display through the public scene entry, then remove it without
     * destroying the reserved buffers or interfering with active video. */
    plan.item_count=1;
    plan.items[0]=(MeisoDisplaySceneItem){0,0,16,16,0xffffffff,0xffffffff,0,1,1,0,0,{0}};
    scene_active=0;
    const uint64_t shown[NPROPS]={61,7,0,0,16U<<16,16U<<16,0,0,16,16};
    memcpy(scene_values,shown,sizeof(shown));
    CHECK(meiso_ingress_video_scene_submit(&v,3,&plan)==MEISO_INGRESS_VIDEO_ACCEPTED);
    event_ready=1;CHECK(meiso_ingress_video_overlay_poll(&v,&p)==MEISO_INGRESS_VIDEO_PRESENTED);
    CHECK(v.overlay.front==0 && syncs==2 && !bad_operation);
    plan.item_count=0;scene_active=1;memset(scene_values,0,sizeof(scene_values));
    CHECK(meiso_ingress_video_scene_submit(&v,4,&plan)==MEISO_INGRESS_VIDEO_ACCEPTED);
    event_ready=1;CHECK(meiso_ingress_video_overlay_poll(&v,&p)==MEISO_INGRESS_VIDEO_PRESENTED);
    CHECK(p.sequence==4 && v.overlay.front==-1 && syncs==2 && !bad_operation);
    CHECK(v.shown_fb==52 && v.shown_handles[0]==42 && v.held_capture==3 && v.state==READY);
    CHECK(v.overlay.fb[0]==61 && v.overlay.surfaces[0].fd==20);
    /* An event without a usable timestamp or inactive-plane readback is not
     * evidence. Preserve unknown ownership on every failure. */
    for(unsigned failure=0;failure<4;++failure) {
        v=scene_adapter();v.overlay.front=0;
        CHECK(meiso_ingress_video_scene_submit(&v,5,&plan)==MEISO_INGRESS_VIDEO_ACCEPTED);
        zero_time=failure==0;bad_plane=failure==1?51:0;bad_binding=failure==2;read_error=failure==3;
        event_ready=1;CHECK(meiso_ingress_video_overlay_poll(&v,&p)==MEISO_INGRESS_VIDEO_POLL_ERROR);
        CHECK(v.overlay.front==0 && v.overlay.pending && v.shown_fb==52 && !bad_operation);
    }
    v=scene_adapter();reject_commit=1;
    CHECK(meiso_ingress_video_scene_submit(&v,6,&plan)==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
    CHECK(!v.overlay.pending && !v.overlay.last_sequence && !bad_operation);
    v=scene_adapter();v.flip.pending=1;
    CHECK(meiso_ingress_video_scene_submit(&v,7,&plan)==MEISO_INGRESS_VIDEO_WOULD_BLOCK && !commits);
    puts("K230D video/scene clear: events, readback, repeated empty, redisplay and retained ownership passed (injected DRM)");
    return 0;
}
