/* Exercise the product adapter's clear path with injected DRM syscalls. No
 * second scanout implementation, target device or public test hook. */
#define _POSIX_C_SOURCE 200809L
#include <poll.h>
int test_poll(struct pollfd *,nfds_t,int);
#define drmModeAtomicAddProperty test_add_property
#define drmModeAtomicCommit test_commit
#define drmModeObjectGetProperties test_get_properties
#define drmModeGetProperty test_get_property
#define drmHandleEvent test_handle_event
#define poll test_poll
#include "../targets/k230d-linux/display_ingress_video_backend.c"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static unsigned added, commits, event_ready, bad_plane, reject_commit, wrong_crtc;
static void *event_cookie;
static int bad_operation;
int test_add_property(drmModeAtomicReqPtr req,uint32_t object,uint32_t property,uint64_t value)
{
    (void)req;
    if((object!=31 && object!=41) || !property || property>NPROPS || value) bad_operation=1;
    ++added;return 0;
}
int test_commit(int fd,const drmModeAtomicReqPtr req,uint32_t flags,void *cookie)
{
    (void)req;
    if(fd!=9 || added!=20 || (flags!=DRM_MODE_ATOMIC_TEST_ONLY &&
        flags!=(DRM_MODE_ATOMIC_NONBLOCK|DRM_MODE_PAGE_FLIP_EVENT))) bad_operation=1;
    added=0;++commits;
    if(reject_commit) {errno=EIO;return -1;}
    if(flags!=DRM_MODE_ATOMIC_TEST_ONLY) event_cookie=cookie;
    return 0;
}
drmModeObjectPropertiesPtr test_get_properties(int fd,uint32_t object,uint32_t type)
{
    if(fd!=9 || (object!=31 && object!=41) || type!=DRM_MODE_OBJECT_PLANE) bad_operation=1;
    drmModeObjectPropertiesPtr p=calloc(1,sizeof(*p));
    p->count_props=NPROPS;p->props=calloc(NPROPS,sizeof(*p->props));p->prop_values=calloc(NPROPS,sizeof(*p->prop_values));
    for(unsigned i=0;i<NPROPS;++i)p->props[i]=i+1;
    if(bad_plane==object)p->prop_values[0]=52;
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
    context->page_flip_handler2(fd,1,1,2,wrong_crtc?99:7,event_cookie);
    event_ready=0;return 0;
}
static struct MeisoIngressVideo adapter(void)
{
    struct MeisoIngressVideo v={0};v.drm_fd=9;v.crtc=7;v.plane=31;v.right.plane=41;
    v.state=READY;v.shown_fb=52;v.held_capture=3;v.shown_handles[0]=42;v.shown_handles[1]=43;
    for(unsigned i=0;i<NPROPS;++i)v.prop_ids[i]=v.right.props[i]=i+1;
    added=commits=event_ready=bad_plane=reject_commit=wrong_crtc=0;bad_operation=0;event_cookie=NULL;
    return v;
}
int main(void)
{
    struct MeisoIngressVideo v=adapter();v.flip.pending=1;v.overlay.pending=1;
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
    for(unsigned plane=31;plane<=41;plane+=10) {
        v=adapter();CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING);
        event_ready=1;bad_plane=plane;
        CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEAR_ERROR && v.shown_fb==52 && !bad_operation);
    }
    v=adapter();v.shown_fb=0;
    CHECK(meiso_ingress_video_clear(&v)==MEISO_INGRESS_VIDEO_CLEARED && commits==0 && !bad_operation);
    puts("K230D video clear: dual-plane commit/readback, missing evidence, retained ownership passed (injected DRM)");
    return 0;
}
