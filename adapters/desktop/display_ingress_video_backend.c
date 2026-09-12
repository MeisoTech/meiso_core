#define _POSIX_C_SOURCE 200809L
#include "../../applications/display_ingress/video_backend.h"
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <errno.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* One decoded frame plus an output canvas; no decoder FIFO or retained input. */
struct MeisoIngressVideo {
    AVCodecContext *decoder;
    AVFrame *frame;
    struct SwsContext *scaler;
    uint8_t *rgba;
    uint32_t width, height;
    uint64_t sequence, completed, capture_bytes;
    bool pending;
    char *directory;
    FILE *capture;
};

static uint64_t monotonic_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}

static FILE *output_file(MeisoIngressVideo *v, const char *name, const char *mode)
{
    size_t size=strlen(v->directory)+strlen(name)+2;
    char *path=malloc(size);
    if (!path) return NULL;
    (void)snprintf(path,size,"%s/%s",v->directory,name);
    FILE *f=fopen(path,mode); free(path); return f;
}

static int save_frame(MeisoIngressVideo *v, const char *name)
{
    FILE *f=output_file(v,name,"wb");
    if (!f) return -1;
    bool ok=fprintf(f,"P6\n%u %u\n255\n",v->width,v->height)>0;
    /* Keep conversion bounded; RGB rows need only width*3 scratch bytes. */
    uint8_t row[1920*3];
    for (uint32_t y=0; ok && y<v->height; ++y) {
        const uint8_t *in=v->rgba+(size_t)y*v->width*4;
        for (uint32_t x=0; x<v->width; ++x) memcpy(row+(size_t)x*3,in+(size_t)x*4,3);
        ok=fwrite(row,3,v->width,f)==v->width;
    }
    if (fclose(f)) ok=false;
    return ok ? 0 : -1;
}

MeisoIngressVideo *meiso_ingress_video_open(const char *video_path, const char *output,
                                          uint32_t width, uint32_t height)
{
    (void)video_path;
    if (!output || !*output || width<16 || width>1920 || height<16 || height>1080 ||
        mkdir(output,0700)) return NULL; /* Never overwrite another run. */
    MeisoIngressVideo *v=calloc(1,sizeof(*v));
    if (!v) return NULL;
    v->width=width;v->height=height;v->directory=strdup(output);
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    v->decoder=codec ? avcodec_alloc_context3(codec) : NULL;
    v->frame=av_frame_alloc();v->rgba=malloc((size_t)width*height*4);
    if (!v->decoder || !v->frame || !v->rgba || !v->directory) goto fail;
    v->decoder->thread_count=1;
    v->decoder->max_pixels=(int64_t)width*height;
    if (avcodec_open2(v->decoder,codec,NULL)<0) goto fail;
    v->capture=output_file(v,"received.h264","wb");
    if (!v->capture) goto fail;
    fprintf(stderr,"DESKTOP_BACKEND input=%ux%u decoded_rgba_bytes=%zu evidence=SIMULATED\n",
            width,height,(size_t)width*height*4);
    return v;
fail:
    (void)meiso_ingress_video_close(v);return NULL;
}

MeisoIngressVideoSubmitResult meiso_ingress_video_submit(MeisoIngressVideo *v, uint64_t sequence,
                                                       const uint8_t *au, size_t size)
{
    if (!v || !sequence || sequence<=v->sequence || !au || !size || size>131072)
        return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    if (v->pending) return MEISO_INGRESS_VIDEO_WOULD_BLOCK;
    AVPacket *packet=av_packet_alloc();
    if (!packet || av_new_packet(packet,(int)size)<0) {av_packet_free(&packet);return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;}
    memcpy(packet->data,au,size);
    int result=avcodec_send_packet(v->decoder,packet);av_packet_free(&packet);
    if (result<0 || avcodec_receive_frame(v->decoder,v->frame)<0 ||
        v->frame->width!=(int)v->width || v->frame->height!=(int)v->height)
        return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    v->scaler=sws_getCachedContext(v->scaler,v->frame->width,v->frame->height,
        (enum AVPixelFormat)v->frame->format,(int)v->width,(int)v->height,
        AV_PIX_FMT_RGBA,SWS_BILINEAR,NULL,NULL,NULL);
    if (!v->scaler) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    uint8_t *planes[4]={v->rgba,NULL,NULL,NULL};int strides[4]={(int)v->width*4,0,0,0};
    result=sws_scale(v->scaler,(const uint8_t *const *)v->frame->data,v->frame->linesize,0,
                     (int)v->height,planes,strides);
    av_frame_unref(v->frame);
    if (result!=(int)v->height) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    if (v->capture && v->capture_bytes+size<=64U*1024U*1024U) {
        if (fwrite(au,1,size,v->capture)!=size) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
        v->capture_bytes+=size;
    } else if (v->capture) {
        if (fclose(v->capture)) {v->capture=NULL;return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;}
        v->capture=NULL;
        fprintf(stderr,"DESKTOP_CAPTURE_LIMIT bytes=%"PRIu64" decoding_continues=true\n",v->capture_bytes);
    }
    v->sequence=sequence;v->pending=true;
    return MEISO_INGRESS_VIDEO_ACCEPTED;
}

MeisoIngressVideoPollResult meiso_ingress_video_poll(MeisoIngressVideo *v,
                                                   MeisoIngressVideoPresentation *p)
{
    if (!v || !p) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    if (!v->pending) return MEISO_INGRESS_VIDEO_NONE;
    /* Software completion occurs after decoding into the owned output raster.
     * Export first/every 30th/last frame; this is not a physical page flip. */
    if ((!v->completed && save_frame(v,"first.ppm")) ||
        (v->completed%30==0 && save_frame(v,"latest.ppm"))) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    uint64_t now=monotonic_ns();
    if (!now) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    *p=(MeisoIngressVideoPresentation){v->sequence,now,v->width,v->height,MEISO_INGRESS_VIDEO_CONNECTOR_UNKNOWN};
    ++v->completed;v->pending=false;
    if (v->completed==1 || v->completed%30==0)
        fprintf(stderr,"SOFTWARE_PRESENTED frame=%"PRIu64" decoded=%"PRIu64" physical=false\n",v->sequence,v->completed);
    return MEISO_INGRESS_VIDEO_PRESENTED;
}

int meiso_ingress_video_output_size(const MeisoIngressVideo *v,uint32_t *w,uint32_t *h)
{if(!v || !w || !h)return -1;*w=v->width;*h=v->height;return 0;}
int meiso_ingress_video_set_stereo(MeisoIngressVideo *v) {(void)v;return -1;}
MeisoIngressVideoSubmitResult meiso_ingress_video_overlay_submit(MeisoIngressVideo *v,uint64_t seq,
    uint32_t x,uint32_t y,uint32_t w,uint32_t h,const uint32_t *pixels,size_t bytes)
{(void)v;(void)seq;(void)x;(void)y;(void)w;(void)h;(void)pixels;(void)bytes;return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;}
MeisoIngressVideoPollResult meiso_ingress_video_overlay_poll(MeisoIngressVideo *v,MeisoIngressVideoPresentation *p)
{(void)v;(void)p;return MEISO_INGRESS_VIDEO_NONE;}
int meiso_ingress_video_close(MeisoIngressVideo *v)
{
    if (!v) return 0;
    int result=0;
    if (v->completed && save_frame(v,"last.ppm")) result=-1;
    if (v->capture && fclose(v->capture)) result=-1;
    fprintf(stderr,"DESKTOP_BACKEND_CLOSED decoded=%"PRIu64" capture_bytes=%"PRIu64" physical=false\n",v->completed,v->capture_bytes);
    sws_freeContext(v->scaler);av_frame_free(&v->frame);avcodec_free_context(&v->decoder);
    free(v->rgba);free(v->directory);free(v);return result;
}
