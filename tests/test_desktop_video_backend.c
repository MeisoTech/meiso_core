#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "../applications/display_ingress/video_backend.h"
#include "display_stereo_h264_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int pixels(const char *dir,unsigned char out[64*32*3])
{
    char path[256];snprintf(path,sizeof(path),"%s/latest.ppm",dir);
    FILE *f=fopen(path,"rb");if(!f)return 0;
    unsigned w=0,h=0,m=0;int ok=fscanf(f,"P6\n%u %u\n%u",&w,&h,&m)==3 && fgetc(f)=='\n' && w==64 && h==32 && m==255;
    ok=ok && fread(out,1,64*32*3,f)==64*32*3;fclose(f);return ok;
}
int main(void)
{
    char root[]="/tmp/meiso-desktop-XXXXXX",dir[256];CHECK(mkdtemp(root));
    snprintf(dir,sizeof(dir),"%s/output",root);
    MeisoIngressVideo *v=meiso_ingress_video_open(NULL,dir,32,16);CHECK(v);
    CHECK(!meiso_ingress_video_set_output_size(v,64,32));CHECK(!meiso_ingress_video_set_stereo(v));
    CHECK(meiso_ingress_video_submit(v,1,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_ACCEPTED);
    MeisoIngressVideoPresentation p;
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==1 && p.output_width==64);
    unsigned char original[64*32*3],got[64*32*3];CHECK(pixels(dir,original));
    CHECK(original[(12*64+12)*3]>240 && original[(12*64+12)*3+2]<10);
    CHECK(original[(12*64+44)*3+2]>240 && original[(12*64+44)*3]<10);
    CHECK(original[0]==0 && original[(12*64+30)*3]==0);
    uint32_t green[16*16];for(unsigned i=0;i<256;++i)green[i]=0xff00ff00;
    CHECK(meiso_ingress_video_overlay_submit(v,1,8,8,16,16,green,sizeof(green))==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==1);
    CHECK(pixels(dir,got) && got[(12*64+12)*3+1]==255 && got[(12*64+44)*3+2]>240);
    CHECK(meiso_ingress_video_overlay_submit(v,2,40,8,16,16,green,sizeof(green))==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==2);
    CHECK(pixels(dir,got) && got[(12*64+12)*3]>240 && got[(12*64+44)*3+1]==255);
    CHECK(meiso_ingress_video_overlay_submit(v,3,0,0,0,0,NULL,0)==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==3);
    CHECK(pixels(dir,got) && !memcmp(got,original,sizeof(got)));
    CHECK(meiso_ingress_video_overlay_submit(v,4,0,0,0,0,NULL,0)==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_NONE);
    CHECK(!meiso_ingress_video_close(v));
    const char *files[]={"latest.ppm","first.ppm","last.ppm","received.h264"};
    for(unsigned i=0;i<4;++i){char path[512];snprintf(path,sizeof(path),"%s/%s",dir,files[i]);CHECK(!unlink(path));}
    CHECK(!rmdir(dir) && !rmdir(root));puts("desktop stereo, native HUD move/hide and completion identities passed");return 0;
}
