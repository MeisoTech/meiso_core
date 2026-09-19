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
static int dimensions(const char *root)
{
    char dir[256],path[512];
    snprintf(dir,sizeof(dir),"%s/cropped",root);
    MeisoIngressVideo *v=meiso_ingress_video_open(NULL,dir,30,18);CHECK(v);
    CHECK(!meiso_ingress_video_set_output_size(v,64,32));
    CHECK(meiso_ingress_video_submit(v,1,cropped_h264,sizeof(cropped_h264))==MEISO_INGRESS_VIDEO_ACCEPTED);
    MeisoIngressVideoPresentation p;
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED);
    unsigned char got[64*32*3];CHECK(pixels(dir,got));
    CHECK(got[(17*64+29)*3]>240 && got[(17*64+29)*3+2]<10);
    CHECK(got[(17*64+30)*3]==0 && got[(18*64)*3]==0);
    /* Reject both a larger and a smaller visible frame after successful decode.
     * A rejected frame must not advance sequence or produce a receipt. */
    CHECK(meiso_ingress_video_submit(v,2,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_NONE);
    CHECK(meiso_ingress_video_submit(v,2,cropped_h264,sizeof(cropped_h264))==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==2);
    CHECK(!meiso_ingress_video_close(v));
    const char *files[]={"latest.ppm","first.ppm","last.ppm","received.h264"};
    for(unsigned i=0;i<4;++i){snprintf(path,sizeof(path),"%s/%s",dir,files[i]);CHECK(!unlink(path));}
    CHECK(!rmdir(dir));

    /* Wider-than-negotiated, same-area/different-aspect and smaller frames all
     * fail exact visible-size admission even when within the padding budget. */
    const unsigned widths[]={16,16,64},heights[]={16,32,32};
    for(unsigned i=0;i<3;++i) {
        snprintf(dir,sizeof(dir),"%s/reject-%u",root,i);
        v=meiso_ingress_video_open(NULL,dir,widths[i],heights[i]);CHECK(v);
        CHECK(meiso_ingress_video_submit(v,1,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
        CHECK(meiso_ingress_video_submit(v,1,large_h264,sizeof(large_h264))==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
        CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_NONE);
        CHECK(!meiso_ingress_video_close(v));
        snprintf(path,sizeof(path),"%s/received.h264",dir);
        FILE *f=fopen(path,"rb");CHECK(f);CHECK(fgetc(f)==EOF);CHECK(!fclose(f));
        CHECK(!unlink(path));CHECK(!rmdir(dir)); /* No displayed-frame artifacts. */
    }
    return 0;
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
    CHECK(pixels(dir,got));
    for(unsigned i=0;i<sizeof(got);++i)CHECK(got[i]==0);
    const char *files[]={"latest.ppm","first.ppm","last.ppm","received.h264"};
    for(unsigned i=0;i<4;++i){char path[512];snprintf(path,sizeof(path),"%s/%s",dir,files[i]);CHECK(!unlink(path));}
    CHECK(!rmdir(dir));snprintf(dir,sizeof(dir),"%s/scene",root);
    v=meiso_ingress_video_open(NULL,dir,32,16);CHECK(v);
    CHECK(!meiso_ingress_video_set_output_size(v,64,32));CHECK(!meiso_ingress_video_set_stereo(v));
    MeisoDisplaySceneCapability cap;CHECK(!meiso_ingress_video_scene_prepare(v,1,&cap));
    CHECK(meiso_ingress_video_set_output_size(v,32,16)!=0);
    MeisoDisplayScenePlan scene={0};scene.generation=1;scene.video_desired_version=1;scene.canvas_width=64;scene.canvas_height=32;
    CHECK(meiso_ingress_video_scene_submit(v,1,&scene)==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==1);
    CHECK(meiso_ingress_video_submit(v,1,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED);CHECK(pixels(dir,original));
    scene.generation=2;scene.item_count=2;
    scene.items[0]=(MeisoDisplaySceneItem){8,8,8,8,0xff00ff00,0,0,2,1,0,0,{0}};
    scene.items[1]=(MeisoDisplaySceneItem){43,13,8,8,0xffffff00,0,0,2,1,0,0,{0}};
    CHECK(meiso_ingress_video_scene_submit(v,2,&scene)==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_scene_submit(v,3,&scene)==MEISO_INGRESS_VIDEO_WOULD_BLOCK);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==2);
    CHECK(pixels(dir,got) && got[(8*64+8)*3+1]==255 && got[(13*64+43)*3]==255 && got[(13*64+43)*3+1]==255);
    CHECK(got[(12*64+12)*3]>240); // transparent interior preserves video
    scene.item_count=0;scene.generation=3;
    CHECK(meiso_ingress_video_scene_submit(v,3,&scene)==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==3);
    CHECK(pixels(dir,got) && !memcmp(got,original,sizeof(got)));
    CHECK(meiso_ingress_video_scene_submit(v,3,&scene)==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
    CHECK(meiso_ingress_video_overlay_submit(v,4,8,8,16,16,green,sizeof(green))==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
    // Stop during in-flight video and scene: observe them with their true
    // identities, then clear both eyes while preserving native scene pixels.
    scene.item_count=2;scene.generation=4;
    CHECK(meiso_ingress_video_scene_submit(v,4,&scene)==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_submit(v,2,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_clear(v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING);
    CHECK(meiso_ingress_video_reactivate(v)!=0); // pending removal is not a release proof
    CHECK(meiso_ingress_video_submit(v,3,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
    CHECK(meiso_ingress_video_scene_submit(v,5,&scene)==MEISO_INGRESS_VIDEO_WOULD_BLOCK);
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==2);
    CHECK(meiso_ingress_video_clear(v)==MEISO_INGRESS_VIDEO_CLEAR_PENDING);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==4);
    CHECK(meiso_ingress_video_clear(v)==MEISO_INGRESS_VIDEO_CLEARED);
    CHECK(meiso_ingress_video_clear(v)==MEISO_INGRESS_VIDEO_CLEARED); // idempotent
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_NONE);
    CHECK(meiso_ingress_video_submit(v,3,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
    CHECK(pixels(dir,got));
    CHECK(got[(8*64+8)*3+1]==255 && got[(13*64+43)*3]==255 && got[(13*64+43)*3+1]==255);
    for(unsigned c=0;c<3;++c) CHECK(got[(12*64+12)*3+c]==0 && got[(12*64+44)*3+c]==0);
    scene.generation=5;scene.items[0].x=20;
    CHECK(meiso_ingress_video_scene_submit(v,5,&scene)==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==5);
    CHECK(pixels(dir,got) && got[(8*64+8)*3+1]==0 && got[(8*64+20)*3+1]==255);
    CHECK(!meiso_ingress_video_reactivate(v));
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_NONE); // flushing is not a new receipt
    CHECK(meiso_ingress_video_submit(v,1,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==1);
    scene.generation=6;
    CHECK(meiso_ingress_video_scene_submit(v,6,&scene)==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==6);
    CHECK(pixels(dir,got) && got[(12*64+12)*3]>240 && got[(12*64+44)*3+2]>240);
    CHECK(got[(8*64+20)*3+1]==255); // independent scene survived video restart
    // Closing an active scene still clears all remaining current output.
    CHECK(!meiso_ingress_video_close(v));
    CHECK(pixels(dir,got));
    for(unsigned i=0;i<sizeof(got);++i)CHECK(got[i]==0);
    for(unsigned i=0;i<4;++i){char path[512];snprintf(path,sizeof(path),"%s/%s",dir,files[i]);CHECK(!unlink(path));}
    CHECK(!rmdir(dir));CHECK(!dimensions(root));
    // Output failure is not a clear acknowledgement, even after the path is
    // repaired. The adapter remains video-stopped; native scene stays usable.
    snprintf(dir,sizeof(dir),"%s/failed-clear",root);
    v=meiso_ingress_video_open(NULL,dir,32,16);CHECK(v);
    CHECK(!meiso_ingress_video_set_output_size(v,64,32));
    CHECK(meiso_ingress_video_submit(v,1,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED);
    char moved[256];snprintf(moved,sizeof(moved),"%s/moved",root);CHECK(!rename(dir,moved));
    CHECK(meiso_ingress_video_clear(v)==MEISO_INGRESS_VIDEO_CLEAR_ERROR);
    CHECK(meiso_ingress_video_reactivate(v)!=0); // failed removal is never enough
    CHECK(!rename(moved,dir));
    CHECK(meiso_ingress_video_clear(v)==MEISO_INGRESS_VIDEO_CLEAR_ERROR);
    CHECK(meiso_ingress_video_submit(v,2,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_SUBMIT_ERROR);
    CHECK(meiso_ingress_video_overlay_submit(v,1,8,8,16,16,green,sizeof(green))==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_overlay_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED);
    CHECK(!meiso_ingress_video_retry_clear(v)); // explicit newer disabled Desired
    CHECK(meiso_ingress_video_clear(v)==MEISO_INGRESS_VIDEO_CLEARED);
    CHECK(!meiso_ingress_video_reactivate(v));
    CHECK(meiso_ingress_video_submit(v,2,stereo_h264,sizeof(stereo_h264))==MEISO_INGRESS_VIDEO_ACCEPTED);
    CHECK(meiso_ingress_video_poll(v,&p)==MEISO_INGRESS_VIDEO_PRESENTED && p.sequence==2);
    CHECK(!meiso_ingress_video_close(v));
    for(unsigned i=0;i<4;++i){char path[512];snprintf(path,sizeof(path),"%s/%s",dir,files[i]);CHECK(!unlink(path));}
    CHECK(!rmdir(dir));CHECK(!rmdir(root));
    puts("desktop dimensions, stereo, native HUD move/hide and completion identities passed");return 0;
}
