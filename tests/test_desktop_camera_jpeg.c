#include "../adapters/desktop/camera_jpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void)
{
    static uint8_t rgba[320*240*4],jpeg[30720];
    for(unsigned i=0;i<320*240;++i) {rgba[i*4]=240;rgba[i*4+1]=30;rgba[i*4+2]=20;rgba[i*4+3]=255;}
    CHECK(!meiso_desktop_camera_jpeg(rgba,jpeg,16));
    const size_t size=meiso_desktop_camera_jpeg(rgba,jpeg,sizeof(jpeg));
    CHECK(size>4 && size<=sizeof(jpeg));
    struct jpeg_decompress_struct d={0};struct jpeg_error_mgr error;
    d.err=jpeg_std_error(&error);jpeg_create_decompress(&d);jpeg_mem_src(&d,jpeg,size);
    CHECK(jpeg_read_header(&d,TRUE)==JPEG_HEADER_OK && d.image_width==320 && d.image_height==240);
    CHECK(!d.progressive_mode && d.data_precision==8 && d.num_components==3);
    d.out_color_space=JCS_RGB;CHECK(jpeg_start_decompress(&d));
    unsigned char row[320*3];JSAMPROW rows[1]={row};
    while(d.output_scanline<240) {
        CHECK(jpeg_read_scanlines(&d,rows,1)==1);
        for(unsigned x=0;x<320;++x) {CHECK(abs(row[x*3]-240)<5);CHECK(abs(row[x*3+1]-30)<5);CHECK(abs(row[x*3+2]-20)<5);}
    }
    CHECK(jpeg_finish_decompress(&d));jpeg_destroy_decompress(&d);
    /* A bounded noisy frame is dropped, never emitted truncated or queued. */
    uint32_t seed=19;
    for(size_t i=0;i<sizeof(rgba);++i) {seed=seed*1664525U+1013904223U;rgba[i]=(uint8_t)(seed>>24);}
    CHECK(!meiso_desktop_camera_jpeg(rgba,jpeg,sizeof(jpeg)));
    memset(rgba,255,sizeof(rgba));CHECK(meiso_desktop_camera_jpeg(rgba,jpeg,sizeof(jpeg))>0);
    puts("JPEG dimensions/colors, bounded destination, oversize drop and recovery passed");
    return 0;
}
