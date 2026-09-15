#include "../adapters/media/camera_jpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void check_color(uint8_t *jpeg,size_t size,int r,int g,int b)
{
    struct jpeg_decompress_struct d={0};struct jpeg_error_mgr error;
    d.err=jpeg_std_error(&error);jpeg_create_decompress(&d);jpeg_mem_src(&d,jpeg,size);
    CHECK(jpeg_read_header(&d,TRUE)==JPEG_HEADER_OK && d.image_width==320 && d.image_height==240);
    CHECK(!d.progressive_mode && d.data_precision==8 && d.num_components==3);
    d.out_color_space=JCS_RGB;CHECK(jpeg_start_decompress(&d));
    unsigned char row[320*3];JSAMPROW rows[1]={row};
    while(d.output_scanline<240) {
        CHECK(jpeg_read_scanlines(&d,rows,1)==1);
        for(unsigned x=0;x<320;++x) {CHECK(abs(row[x*3]-r)<5);CHECK(abs(row[x*3+1]-g)<5);CHECK(abs(row[x*3+2]-b)<5);}
    }
    CHECK(jpeg_finish_decompress(&d));jpeg_destroy_decompress(&d);
}
int main(void)
{
    static uint8_t rgba[320*240*4],jpeg[30720];
    for(unsigned i=0;i<320*240;++i) {rgba[i*4]=240;rgba[i*4+1]=30;rgba[i*4+2]=20;rgba[i*4+3]=255;}
    CHECK(!meiso_camera_jpeg_rgba(rgba,jpeg,16));
    const size_t size=meiso_camera_jpeg_rgba(rgba,jpeg,sizeof(jpeg));
    CHECK(size>4 && size<=sizeof(jpeg));
    check_color(jpeg,size,240,30,20);
    /* A bounded noisy frame is dropped, never emitted truncated or queued. */
    uint32_t seed=19;
    for(size_t i=0;i<sizeof(rgba);++i) {seed=seed*1664525U+1013904223U;rgba[i]=(uint8_t)(seed>>24);}
    CHECK(!meiso_camera_jpeg_rgba(rgba,jpeg,sizeof(jpeg)));
    memset(rgba,255,sizeof(rgba));CHECK(meiso_camera_jpeg_rgba(rgba,jpeg,sizeof(jpeg))>0);
    static uint8_t nv12[320*240*3/2];
    const int cases[][6]={{16,128,128,0,0,0},{235,128,128,255,255,255},{81,90,240,255,0,0},{41,240,110,0,0,255}};
    for(unsigned k=0;k<4;++k) {
        memset(nv12,cases[k][0],320*240);
        for(unsigned i=320*240;i<sizeof(nv12);i+=2){nv12[i]=(uint8_t)cases[k][1];nv12[i+1]=(uint8_t)cases[k][2];}
        const size_t encoded=meiso_camera_jpeg_nv12_bt601(nv12,jpeg,sizeof(jpeg));CHECK(encoded>0);
        check_color(jpeg,encoded,cases[k][3],cases[k][4],cases[k][5]);
    }
    puts("JPEG dimensions/colors, bounded destination, oversize drop and recovery passed");
    return 0;
}
