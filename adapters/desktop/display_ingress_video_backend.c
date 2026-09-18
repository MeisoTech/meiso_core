#define _POSIX_C_SOURCE 200809L
#include "../../applications/display_ingress/video_backend.h"
#include "../../targets/k230d-linux/display_ingress_scene_raster.h"
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
    uint8_t *rgba, *canvas;
    uint32_t *overlay;
    uint16_t *scene_pixels;
    MeisoDisplaySceneCapability scene_capability;
    uint32_t output_width, output_height, overlay_x, overlay_y, overlay_w, overlay_h;
    uint64_t overlay_sequence, overlay_completed;
    bool stereo, overlay_pending, overlay_visible;
    uint32_t width, height;
    uint64_t sequence, completed, capture_bytes;
    bool pending, stopping, cleared, clear_failed;
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
    bool ok=fprintf(f,"P6\n%u %u\n255\n",v->output_width,v->output_height)>0;
    /* Keep conversion bounded; RGB rows need only width*3 scratch bytes. */
    uint8_t row[4096*3];
    for (uint32_t y=0; ok && y<v->output_height; ++y) {
        const uint8_t *in=v->canvas+(size_t)y*v->output_width*4;
        for (uint32_t x=0; x<v->output_width; ++x) memcpy(row+(size_t)x*3,in+(size_t)x*4,3);
        ok=fwrite(row,3,v->output_width,f)==v->output_width;
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
    v->width=width;v->height=height;v->output_width=width;v->output_height=height;v->directory=strdup(output);
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    v->decoder=codec ? avcodec_alloc_context3(codec) : NULL;
    v->frame=av_frame_alloc();v->rgba=calloc((size_t)width*height,4);
    v->canvas=calloc((size_t)width*height,4);
    if (!v->decoder || !v->frame || !v->rgba || !v->canvas || !v->directory) goto fail;
    v->decoder->thread_count=1;
    if (avcodec_open2(v->decoder,codec,NULL)<0) goto fail;
    /* FFmpeg also applies max_pixels to internally aligned dimensions (e.g.
     * ff_get_buffer in 6.1 checks 64x16 for a visible 32x16 frame). Derive a
     * finite allocation guard using its public H.264 alignment API; do not
     * confuse that guard with the exact visible-size check in submit.
     * YUV420P includes H.264 macroblock/chroma padding, including 1080->1088.
     * Restore NONE so this budget calculation does not select a pixel format. */
    int padded_width=(int)width, padded_height=(int)height;
    v->decoder->pix_fmt=AV_PIX_FMT_YUV420P;
    avcodec_align_dimensions(v->decoder,&padded_width,&padded_height);
    v->decoder->pix_fmt=AV_PIX_FMT_NONE;
    v->decoder->max_pixels=(int64_t)padded_width*padded_height;
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
    if (!v || v->stopping || !sequence || sequence<=v->sequence || !au || !size || size>131072)
        return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    if (v->pending) return MEISO_INGRESS_VIDEO_WOULD_BLOCK;
    AVPacket *packet=av_packet_alloc();
    if (!packet || av_new_packet(packet,(int)size)<0) {av_packet_free(&packet);return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;}
    memcpy(packet->data,au,size);
    int result=avcodec_send_packet(v->decoder,packet);av_packet_free(&packet);
    if (result<0 || avcodec_receive_frame(v->decoder,v->frame)<0 ||
        v->frame->width!=(int)v->width || v->frame->height!=(int)v->height) {
        av_frame_unref(v->frame);
        return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    }
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

/* Rebuild from the retained video, so moving/hiding never leaves old HUD pixels.
 * Two native-size eye rectangles mirror the hardware plane placement. */
static void compose(MeisoIngressVideo *v)
{
    memset(v->canvas,0,(size_t)v->output_width*v->output_height*4);
    unsigned eyes=v->stereo?2:1;
    uint32_t width=v->width/eyes, output_eye=v->output_width/eyes;
    for(unsigned eye=0;eye<eyes;++eye) {
        uint32_t x=eye*output_eye+(v->stereo?(output_eye-width)/2:0);
        uint32_t y=v->stereo?(v->output_height-v->height)/2:0;
        for(uint32_t row=0;row<v->height;++row)
            memcpy(v->canvas+((size_t)(row+y)*v->output_width+x)*4,
                v->rgba+((size_t)row*v->width+eye*width)*4,(size_t)width*4);
    }
    if(v->overlay_visible) for(uint32_t y=0;y<v->overlay_h;++y) for(uint32_t x=0;x<v->overlay_w;++x) {
        uint32_t argb;
        if(v->scene_pixels) {
            const uint16_t pixel=v->scene_pixels[(size_t)y*v->overlay_w+x];
            argb=((uint32_t)(pixel>>12)*17U<<24)|((uint32_t)((pixel>>8)&15)*17U<<16)|
                ((uint32_t)((pixel>>4)&15)*17U<<8)|(uint32_t)(pixel&15)*17U;
        } else argb=v->overlay[(size_t)y*v->overlay_w+x];
        const uint32_t alpha=argb>>24;
        uint8_t *out=v->canvas+((size_t)(y+v->overlay_y)*v->output_width+x+v->overlay_x)*4;
        const uint32_t rgb[]={(argb>>16)&255,(argb>>8)&255,argb&255};
        for(unsigned i=0;i<3;++i) out[i]=(uint8_t)((rgb[i]*alpha+out[i]*(255-alpha)+127)/255);
        out[3]=255;
    }
}

MeisoIngressVideoPollResult meiso_ingress_video_poll(MeisoIngressVideo *v,
                                                   MeisoIngressVideoPresentation *p)
{
    if (!v || !p) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    if (!v->pending) return MEISO_INGRESS_VIDEO_NONE;
    compose(v);
    /* Software completion occurs after decoding into the owned output raster.
     * Export first/every 30th/last frame; this is not a physical page flip. */
    if ((!v->completed && save_frame(v,"first.ppm")) ||
        (v->completed%30==0 && save_frame(v,"latest.ppm"))) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    uint64_t now=monotonic_ns();
    if (!now) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    *p=(MeisoIngressVideoPresentation){v->sequence,now,v->output_width,v->output_height,MEISO_INGRESS_VIDEO_CONNECTOR_UNKNOWN};
    ++v->completed;v->pending=false;
    if (v->completed==1 || v->completed%30==0)
        fprintf(stderr,"SOFTWARE_PRESENTED frame=%"PRIu64" decoded=%"PRIu64" physical=false\n",v->sequence,v->completed);
    return MEISO_INGRESS_VIDEO_PRESENTED;
}

MeisoIngressVideoClearResult meiso_ingress_video_clear(MeisoIngressVideo *v)
{
    if (!v || v->clear_failed) return MEISO_INGRESS_VIDEO_CLEAR_ERROR;
    v->stopping=true;
    if (v->cleared) return MEISO_INGRESS_VIDEO_CLEARED;
    if (v->pending || v->overlay_pending) return MEISO_INGRESS_VIDEO_CLEAR_PENDING;
    memset(v->rgba,0,(size_t)v->width*v->height*4);
    compose(v);
    if (save_frame(v,"latest.ppm")) {
        v->clear_failed=true;
        return MEISO_INGRESS_VIDEO_CLEAR_ERROR;
    }
    v->cleared=true;
    return MEISO_INGRESS_VIDEO_CLEARED;
}

int meiso_ingress_video_output_size(const MeisoIngressVideo *v,uint32_t *w,uint32_t *h)
{if(!v || !w || !h)return -1;*w=v->output_width;*h=v->output_height;return 0;}
int meiso_ingress_video_set_output_size(MeisoIngressVideo *v,uint32_t w,uint32_t h)
{
    if(!v || v->scene_pixels || v->sequence || v->overlay_sequence || v->stereo || w<v->width || h<v->height || w>4096 || h>4096) return -1;
    uint8_t *canvas=calloc((size_t)w*h,4);if(!canvas) return -1;
    free(v->canvas);v->canvas=canvas;v->output_width=w;v->output_height=h;return 0;
}
int meiso_ingress_video_set_stereo(MeisoIngressVideo *v)
{
    if(!v || v->scene_pixels || v->sequence || v->overlay_sequence || v->stereo || v->width%16 || v->output_width%2) return -1;
    v->stereo=true;return 0;
}
MeisoIngressVideoSubmitResult meiso_ingress_video_overlay_submit(MeisoIngressVideo *v,uint64_t seq,
    uint32_t x,uint32_t y,uint32_t w,uint32_t h,const uint32_t *pixels,size_t bytes)
{
    if(!v || v->scene_pixels || !seq || seq<=v->overlay_sequence) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    if(v->overlay_pending || (v->stopping && !v->cleared && !v->clear_failed))
        return MEISO_INGRESS_VIDEO_WOULD_BLOCK;
    const bool visible=w!=0 || h!=0;
    if(visible) {
        if(!pixels || w<16 || h<16 || w%2 || w>v->output_width || h>v->output_height ||
            x>v->output_width-w || y>v->output_height-h || (uint64_t)w*h>65536 ||
            bytes!=(size_t)w*h*4) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
        if(!v->overlay) v->overlay=malloc(65536*4);
        if(!v->overlay) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
        memcpy(v->overlay,pixels,bytes);
    } else if(x || y || bytes || !v->overlay_visible) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    v->overlay_x=x;v->overlay_y=y;v->overlay_w=w;v->overlay_h=h;
    v->overlay_visible=visible;v->overlay_sequence=seq;v->overlay_pending=true;
    return MEISO_INGRESS_VIDEO_ACCEPTED;
}
int meiso_ingress_video_scene_prepare(MeisoIngressVideo *v,uint32_t fonts,MeisoDisplaySceneCapability *out)
{
    if(!v || !out || !fonts || (fonts&~3U) || v->sequence || v->overlay_sequence || v->scene_pixels ||
        v->output_width<16 || v->output_height<16) return -1;
    const uint32_t area=v->output_width*v->output_height;
    v->scene_capability=(MeisoDisplaySceneCapability){v->output_width,v->output_height,
        area<1152000?area:1152000,48,10,16,16,8,1,16,fonts};
    v->scene_pixels=malloc((size_t)v->scene_capability.max_pixels*2);
    if(!v->scene_pixels) return -1;
    *out=v->scene_capability;return 0;
}
MeisoIngressVideoSubmitResult meiso_ingress_video_scene_submit(MeisoIngressVideo *v,uint64_t seq,
    const MeisoDisplayScenePlan *plan)
{
    MeisoDisplaySceneRegion r;
    if(!v || !v->scene_pixels || !seq || seq<=v->overlay_sequence || !plan ||
        !meiso_display_scene_supported(&v->scene_capability,plan,plan->video_desired_version,&r))
        return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    if(v->overlay_pending || (v->stopping && !v->cleared && !v->clear_failed))
        return MEISO_INGRESS_VIDEO_WOULD_BLOCK;
    if(!meiso_k230d_scene_raster(&v->scene_capability,plan,v->scene_pixels,v->scene_capability.max_pixels,&r))
        return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    v->overlay_x=r.x;v->overlay_y=r.y;v->overlay_w=r.width;v->overlay_h=r.height;
    v->overlay_visible=r.width!=0;v->overlay_sequence=seq;v->overlay_pending=true;
    return MEISO_INGRESS_VIDEO_ACCEPTED;
}
MeisoIngressVideoPollResult meiso_ingress_video_overlay_poll(MeisoIngressVideo *v,MeisoIngressVideoPresentation *p)
{
    if(!v || !p) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    if(!v->overlay_pending) return MEISO_INGRESS_VIDEO_NONE;
    compose(v);
    if(save_frame(v,"latest.ppm")) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    uint64_t now=monotonic_ns();if(!now) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    *p=(MeisoIngressVideoPresentation){v->overlay_sequence,now,v->output_width,v->output_height,MEISO_INGRESS_VIDEO_CONNECTOR_UNKNOWN};
    v->overlay_pending=false;++v->overlay_completed;
    return MEISO_INGRESS_VIDEO_PRESENTED;
}
int meiso_ingress_video_close(MeisoIngressVideo *v)
{
    if (!v) return 0;
    int result=0;
    if (v->completed || v->overlay_completed) {
        if(save_frame(v,"last.ppm")) result=-1;
        // A diagnostic last image is not a still-active software display.
        // Clear the current canvas on every terminal path without inventing a receipt.
        memset(v->canvas,0,(size_t)v->output_width*v->output_height*4);
        if(save_frame(v,"latest.ppm")) result=-1;
    }
    if (v->capture && fclose(v->capture)) result=-1;
    fprintf(stderr,"DESKTOP_BACKEND_CLOSED decoded=%"PRIu64" capture_bytes=%"PRIu64" physical=false\n",v->completed,v->capture_bytes);
    sws_freeContext(v->scaler);av_frame_free(&v->frame);avcodec_free_context(&v->decoder);
    free(v->rgba);free(v->canvas);free(v->overlay);free(v->scene_pixels);free(v->directory);free(v);return result;
}
