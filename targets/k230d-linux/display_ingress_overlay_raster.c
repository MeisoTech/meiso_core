/* PRODUCT: K230D Display Ingress platform implementation. */
#define _POSIX_C_SOURCE 200809L
#include "display_ingress_overlay_raster.h"
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int font_fd=-1;
bool meiso_k230d_overlay_font_open(const char *path)
{
    if(!path || !*path) return false;
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    struct stat st; uint8_t header[8];
    if(fd<0) return false;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=8+65536*32 ||
        pread(fd,header,8,0)!=8 || memcmp(header,"MCJK16\r\n",8)) {
        close(fd); return false;
    }
    if(font_fd>=0) close(font_fd);
    font_fd=fd; return true;
}
void meiso_k230d_overlay_font_close(void)
{ if(font_fd>=0) close(font_fd); font_fd=-1; }

/* Built-in bitmap glyphs, intentionally limited by the selected capability. */
static const uint8_t font[36][7]={
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
 {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}
};
static unsigned glyph(char c, unsigned row)
{
    if(c>='A' && c<='Z') return font[c-'A'][row];
    if(c>='0' && c<='9') return font[26+c-'0'][row];
    if(c=='-') return row==3?31:0;
    if(c=='.') return row==6?4:0;
    if(c==':') return row==2 || row==5?4:0;
    if(c=='/') return 1U<<(row*4/6);
    return 0;
}
bool meiso_k230d_overlay_glyph(uint8_t selected, uint32_t cp,
    uint8_t out[32], uint8_t *advance, uint8_t *height)
{
    if(!out || !advance || !height || selected>1) return false;
    uint8_t rows[32]={0},a,h;
    if(selected==MEISO_DISPLAY_OVERLAY_FONT_CJK16) {
        if(font_fd<0 || cp>65535 || pread(font_fd,rows,32,8+(off_t)cp*32)!=32) return false;
        a=cp<128?8:16;h=16;
    } else {
        if(cp>127) return false;
        for(unsigned y=0;y<7;++y) rows[2*y]=(uint8_t)(glyph((char)cp,y)<<3);
        a=6;h=7;
    }
    memcpy(out,rows,32);*advance=a;*height=h;return true;
}
bool meiso_k230d_overlay_raster(const MeisoDisplayOverlayCapability *cap,
    const MeisoDisplayOverlayPlan *p, uint32_t *pixels, size_t capacity,
    uint32_t *width, uint32_t *height)
{
    if (!p || !pixels || !width || !height ||
        !meiso_display_overlay_plan_supported(cap,p,p->video_desired_version)) return false;
    const bool stereo=p->visible && p->layout==MEISO_DISPLAY_OVERLAY_LAYOUT_SIDE_BY_SIDE;
    const uint32_t eye=stereo?p->canvas_width/2:0;
    const uint32_t w=p->visible?p->width+eye:16, h=p->visible?p->height:16;
    if ((uint64_t)w*h>capacity) return false;
    uint8_t rows[MEISO_DISPLAY_OVERLAY_MAX_TEXT][32], advances[MEISO_DISPLAY_OVERLAY_MAX_TEXT];
    unsigned count=0;
    if(p->visible && p->text_size && p->font==MEISO_DISPLAY_OVERLAY_FONT_CJK16) {
        if(font_fd<0) return false;
        MeisoByteView text={(const uint8_t *)p->text,p->text_size};
        while(text.size) {
            uint32_t cp;
            if(!meiso_display_overlay_cjk_next(&text,&cp) ||
                pread(font_fd,rows[count],32,8+(off_t)cp*32)!=32) return false;
            advances[count++]=cp<128?8:16;
        }
    }
    memset(pixels,0,(size_t)w*h*sizeof(*pixels));
    if (p->visible) for (unsigned copy=0;copy<(stereo?2U:1U);++copy) {
        const uint32_t offset=copy*eye;
        for(unsigned y=0;y<p->height;++y) for(unsigned x=0;x<p->width;++x)
            pixels[y*w+offset+x]=(x<p->border_width || y<p->border_width ||
                x>=p->width-p->border_width || y>=p->height-p->border_width)?p->border_argb:p->fill_argb;
        if(p->font==MEISO_DISPLAY_OVERLAY_FONT_CJK16) {
            unsigned advance=0;
            for(unsigned g=0;g<count;++g) {
                for(unsigned y=0;y<16;++y) for(unsigned x=0;x<advances[g];++x)
                    if(rows[g][2*y+x/8] & (0x80U>>(x%8)))
                        for(unsigned yy=0;yy<p->text_scale;++yy) for(unsigned xx=0;xx<p->text_scale;++xx)
                            pixels[(p->border_width+y*p->text_scale+yy)*w+offset+
                                p->border_width+(advance+x)*p->text_scale+xx]=p->text_argb;
                advance+=advances[g];
            }
        } else for(unsigned g=0;g<p->text_size;++g) for(unsigned y=0;y<7;++y) for(unsigned x=0;x<5;++x)
            if(glyph(p->text[g],y)&(1U<<(4-x)))
                for(unsigned yy=0;yy<p->text_scale;++yy) for(unsigned xx=0;xx<p->text_scale;++xx)
                    pixels[(p->border_width+y*p->text_scale+yy)*w+offset+
                        p->border_width+(g*6+x)*p->text_scale+xx]=p->text_argb;
    }
    *width=w; *height=h; return true;
}
