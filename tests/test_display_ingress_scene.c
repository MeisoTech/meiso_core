#define _POSIX_C_SOURCE 200809L
#include <meiso/profiles/display_ingress/scene.h>
#include "../targets/k230d-linux/display_ingress_scene_raster.h"
#include "../targets/k230d-linux/display_ingress_overlay_raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1; } } while(0)
int main(void)
{
    MeisoDisplaySceneCapability cap={1920,1080,1600*720,48,10,16,16,8,1,16,3},c;
    MeisoDisplayScenePlan p={0},d;
    p.generation=501;p.video_desired_version=17;p.canvas_width=1920;p.canvas_height=1080;p.item_count=2;
    p.items[0]=(MeisoDisplaySceneItem){160,180,320,360,0xff00ff00,0,0xffffffff,4,1,0,0,{0}};
    p.items[1]=(MeisoDisplaySceneItem){1131,193,400,410,0xffffff00,0,0xffffffff,6,1,0,0,{0}};
    uint8_t wire[1441],canonical[1440];memset(wire,0xa5,sizeof(wire));
    CHECK(meiso_display_scene_capability_encode(&cap,(MeisoMutableBuffer){wire,sizeof(wire)}));
    CHECK(meiso_display_scene_capability_decode((MeisoByteView){wire,36},&c));
    CHECK(c.max_pixels==1152000 && c.max_items==16 && c.width_alignment==8);
    const size_t n=meiso_display_scene_plan_bytes(&p);CHECK(n==208);
    CHECK(meiso_display_scene_plan_encode(&p,(MeisoMutableBuffer){wire,sizeof(wire)}));
    CHECK(wire[n]==0xa5 && wire[6]==1 && wire[7]==245 && wire[15]==17 && wire[24]==2);
    memcpy(canonical,wire,n);
    CHECK(meiso_display_scene_plan_decode((MeisoByteView){wire,n},&d));
    CHECK(d.items[1].x==1131 && d.items[1].height==410);
    MeisoDisplaySceneRegion r={0};CHECK(meiso_display_scene_supported(&cap,&d,17,&r));
    CHECK(r.x==160 && r.y==180 && r.width==1376 && r.height==423);
    r.x=99;CHECK(!meiso_display_scene_supported(&cap,&d,18,&r) && r.x==99);
    c=cap;c.max_pixels=1376*423-1;
    CHECK(!meiso_display_scene_supported(&c,&p,17,&r)); // gap + alignment are charged
    c=cap;c.max_items=1;CHECK(!meiso_display_scene_supported(&c,&p,17,&r));
    for(size_t i=0;i<n;++i) CHECK(!meiso_display_scene_plan_decode((MeisoByteView){wire,i},&d));
    CHECK(!meiso_display_scene_plan_decode((MeisoByteView){wire,n+1},&d));
    const size_t bad[]={25,31,32+33,32+39,32+87};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        memcpy(wire,canonical,n);wire[bad[i]]=1;d.generation=999;
        CHECK(!meiso_display_scene_plan_decode((MeisoByteView){wire,n},&d) && d.generation==999);
    }
    memcpy(wire,canonical,n);wire[24]=17;CHECK(!meiso_display_scene_plan_decode((MeisoByteView){wire,n},&d));
    d=p;d.items[1].x=UINT32_MAX;memset(wire,0xa5,sizeof(wire));
    CHECK(!meiso_display_scene_plan_encode(&d,(MeisoMutableBuffer){wire,sizeof(wire)}) && wire[0]==0xa5);
    d=p;d.items[1].height=1080;CHECK(!meiso_display_scene_plan_bytes(&d));
    CHECK(!meiso_display_scene_plan_encode(&p,(MeisoMutableBuffer){(uint8_t *)&p,sizeof(p)}));
    CHECK(!meiso_display_scene_supported(&cap,&p,17,(MeisoDisplaySceneRegion *)&p));

    const size_t capacity=1600*720;uint16_t *pixels=malloc((capacity+1)*2);CHECK(pixels);
    for(size_t i=0;i<=capacity;++i) pixels[i]=0xa55a;
    CHECK(meiso_k230d_scene_raster(&cap,&p,pixels,capacity,&r));
    CHECK(pixels[r.width*r.height]==0xa55a && pixels[0]==0xf0f0);
    CHECK(pixels[13*r.width+971]==0xfff0); // independent right eye, not a repeated left copy
    CHECK(pixels[20*r.width+700]==0 && pixels[20*r.width+20]==0);
    pixels[0]=0x1234;r.x=99;
    CHECK(!meiso_k230d_scene_raster(&cap,&p,pixels,100,&r) && pixels[0]==0x1234 && r.x==99);

    // Transparent later interiors preserve earlier borders; half-alpha blue
    // over opaque red yields the expected opaque purple after quantization.
    p.item_count=2;p.items[0]=(MeisoDisplaySceneItem){0,0,32,32,0xffff0000,0xffff0000,0,1,1,0,0,{0}};
    p.items[1]=(MeisoDisplaySceneItem){8,8,16,16,0xff00ff00,0,0,1,1,0,0,{0}};
    CHECK(meiso_k230d_scene_raster(&cap,&p,pixels,capacity,&r));
    CHECK(pixels[10*r.width+10]==0xff00 && pixels[8*r.width+8]==0xf0f0);
    p.items[1].fill_argb=0x800000ff;CHECK(meiso_k230d_scene_raster(&cap,&p,pixels,capacity,&r));
    CHECK(pixels[10*r.width+10]==0xf708);

    // Padding near the output edge shifts the backing surface, not the item.
    p.item_count=1;p.items[0]=(MeisoDisplaySceneItem){1915,1075,5,5,0xffffffff,0,0,1,1,0,0,{0}};
    CHECK(meiso_k230d_scene_raster(&cap,&p,pixels,capacity,&r));
    CHECK(r.x==1904 && r.y==1064 && r.width==16 && r.height==16);
    CHECK(pixels[11*16+11]==0xffff && !pixels[0]);

    // Text is validated and loaded before pixels change, including a missing
    // CJK bank. Synthetic glyphs exercise the same reader as the physical path.
    p.items[0]=(MeisoDisplaySceneItem){0,0,64,32,0,0,0xffffffff,2,1,1,3,{0}};
    memcpy(p.items[0].text,"你",3);pixels[0]=0x1234;r.x=99;
    CHECK(!meiso_k230d_scene_raster(&cap,&p,pixels,capacity,&r) && pixels[0]==0x1234 && r.x==99);
    char path[]="/tmp/meiso-scene-font-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);
    CHECK(ftruncate(fd,8+65536*32)==0 && pwrite(fd,"MCJK16\r\n",8,0)==8);
    const uint8_t stroke[2]={0x80,0};CHECK(pwrite(fd,stroke,2,8+(off_t)0x4f60*32)==2);
    CHECK(meiso_k230d_overlay_font_open(path));CHECK(unlink(path)==0);
    CHECK(meiso_k230d_scene_raster(&cap,&p,pixels,capacity,&r) && pixels[2*r.width+2]==0xffff);
    CHECK(ftruncate(fd,8)==0);pixels[0]=0x1234;
    CHECK(!meiso_k230d_scene_raster(&cap,&p,pixels,capacity,&r) && pixels[0]==0x1234);
    CHECK(close(fd)==0);meiso_k230d_overlay_font_close();
    p.items[0].font=0;p.items[0].text_size=1;memset(p.items[0].text,0,48);p.items[0].text[0]='H';
    CHECK(meiso_k230d_scene_raster(&cap,&p,pixels,capacity,&r) && pixels[2*r.width+2]==0xffff);
    p.item_count=16;for(unsigned i=1;i<16;++i) p.items[i]=p.items[0];
    CHECK(meiso_display_scene_plan_bytes(&p)==1440);
    CHECK(meiso_display_scene_plan_encode(&p,(MeisoMutableBuffer){wire,sizeof(wire)}));
    CHECK(meiso_display_scene_plan_decode((MeisoByteView){wire,1440},&d) && d.item_count==16);
    CHECK(meiso_k230d_scene_raster(&cap,&d,pixels,capacity,&r));
    p.item_count=0;CHECK(meiso_display_scene_plan_bytes(&p)==32);
    CHECK(meiso_k230d_scene_raster(&cap,&p,NULL,0,&r) && !r.width && !r.height);
    free(pixels);puts("Native scene geometry, codecs, alpha and bounded raster passed");return 0;
}
