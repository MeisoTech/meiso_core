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
static uint8_t channel(int value) {return (uint8_t)(value<0?0:value>255?255:value);}
static size_t encode(const uint8_t *pixels,int nv12,uint8_t *out,size_t capacity)
{
    if(!pixels || !out || !capacity || capacity>30720) return 0;
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
        const size_t y=e->codec.next_scanline;
        if(nv12) {
            const uint8_t *luma=pixels+y*320,*chroma=pixels+320*240+(y/2)*320;
            for(unsigned x=0;x<320;++x) {
                const int c=(int)luma[x]-16,d=(int)chroma[x&~1U]-128,f=(int)chroma[(x&~1U)+1]-128;
                e->row[x*3]=channel((298*c+409*f+128)>>8);
                e->row[x*3+1]=channel((298*c-100*d-208*f+128)>>8);
                e->row[x*3+2]=channel((298*c+516*d+128)>>8);
            }
        } else {
            const uint8_t *src=pixels+y*320*4;
            for(unsigned x=0;x<320;++x) for(unsigned k=0;k<3;++k)e->row[x*3+k]=src[x*4+k];
        }
        JSAMPROW rows[1]={e->row};(void)jpeg_write_scanlines(&e->codec,rows,1);
    }
    jpeg_finish_compress(&e->codec);
    const size_t size=capacity-e->destination.free_in_buffer;
    jpeg_destroy_compress(&e->codec);free(e);return size;
}
size_t meiso_camera_jpeg_rgba(const uint8_t *rgba,uint8_t *out,size_t capacity)
{return encode(rgba,0,out,capacity);}
size_t meiso_camera_jpeg_nv12_bt601(const uint8_t *nv12,uint8_t *out,size_t capacity)
{return encode(nv12,1,out,capacity);}
