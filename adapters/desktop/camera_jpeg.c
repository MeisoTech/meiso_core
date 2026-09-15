#include "camera_jpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <jpeglib.h>

typedef struct {
    struct jpeg_compress_struct codec;
    struct jpeg_error_mgr error;
    struct jpeg_destination_mgr destination;
    jmp_buf jump;
    uint8_t row[320*3];
} Encoder;
static void fail(j_common_ptr c) {longjmp(((Encoder*)c->client_data)->jump,1);}
static void init_destination(j_compress_ptr c) {(void)c;}
static boolean full(j_compress_ptr c) {fail((j_common_ptr)c);return FALSE;}
static void finish_destination(j_compress_ptr c) {(void)c;}
size_t meiso_desktop_camera_jpeg(const uint8_t *rgba,uint8_t *out,size_t capacity)
{
    if(!rgba || !out || !capacity || capacity>30720) return 0;
    Encoder *e=calloc(1,sizeof(*e));if(!e)return 0;
    e->codec.err=jpeg_std_error(&e->error);e->error.error_exit=fail;e->codec.client_data=e;
    if(setjmp(e->jump)) {jpeg_destroy_compress(&e->codec);free(e);return 0;}
    jpeg_create_compress(&e->codec);
    e->destination=(struct jpeg_destination_mgr){out,capacity,init_destination,full,finish_destination};
    e->codec.dest=&e->destination;
    e->codec.image_width=320;e->codec.image_height=240;
    e->codec.input_components=3;e->codec.in_color_space=JCS_RGB;
    jpeg_set_defaults(&e->codec);jpeg_set_quality(&e->codec,75,TRUE);jpeg_start_compress(&e->codec,TRUE);
    while(e->codec.next_scanline<240) {
        const uint8_t *src=rgba+(size_t)e->codec.next_scanline*320*4;
        for(unsigned x=0;x<320;++x) for(unsigned k=0;k<3;++k)e->row[x*3+k]=src[x*4+k];
        JSAMPROW rows[1]={e->row};(void)jpeg_write_scanlines(&e->codec,rows,1);
    }
    jpeg_finish_compress(&e->codec);
    const size_t size=capacity-e->destination.free_in_buffer;
    jpeg_destroy_compress(&e->codec);free(e);return size;
}
