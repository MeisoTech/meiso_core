/* PRODUCT: K230D Display Ingress platform implementation. */
#include "display_ingress_scene_raster.h"
#include "display_ingress_overlay_raster.h"
#include <string.h>
#include <stdint.h>
static bool overlap(const void *a,size_t an,const void *b,size_t bn)
{
    if(!an || !bn) return false;
    const uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y?y-x<an:x-y<bn;
}
/* Quantize to four bits only after composing each primitive. Retain straight
 * alpha for the DRM plane; transparent source is a no-op. */
static uint16_t over(uint16_t dst,uint32_t src)
{
    unsigned sa=src>>24,da=(dst>>12)*17U;
    if(!sa) return dst;
    const unsigned alpha=sa*255U+da*(255U-sa);
    unsigned result=((alpha+2167U)/4335U)<<12;
    for(unsigned c=0;c<3;++c) {
        const unsigned s=(src>>(8*c))&255U,d=((dst>>(4*c))&15U)*17U;
        const unsigned value=(s*sa*255U+d*da*(255U-sa)+alpha/2)/alpha;
        result|=((value+8)/17)<<(4*c);
    }
    return (uint16_t)result;
}
bool meiso_k230d_scene_raster(const MeisoDisplaySceneCapability *cap,const MeisoDisplayScenePlan *p,
    uint16_t *pixels,size_t capacity,MeisoDisplaySceneRegion *out)
{
    MeisoDisplaySceneRegion r;
    if(!p || !out || !meiso_display_scene_supported(cap,p,p->video_desired_version,&r) ||
        overlap(out,sizeof(*out),cap,sizeof(*cap)) || overlap(out,sizeof(*out),p,sizeof(*p))) return false;
    const size_t count=(size_t)r.width*r.height,bytes=count*sizeof(*pixels);
    if(count>capacity || (count && !pixels) || overlap(pixels,bytes,p,sizeof(*p)) ||
        overlap(pixels,bytes,cap,sizeof(*cap)) || overlap(pixels,bytes,out,sizeof(*out))) return false;
    /* At most 24 KiB of glyph rows, independent of the output resolution. Load
     * before touching DMA so missing/truncated font files cannot partially draw. */
    uint8_t rows[16][48][32],advances[16][48],heights[16][48],counts[16]={0};
    for(uint32_t i=0;i<p->item_count;++i) {
        const MeisoDisplaySceneItem *v=&p->items[i];
        MeisoByteView text={(const uint8_t *)v->text,v->text_size};
        while(text.size) {
            uint32_t cp;
            if(v->font==MEISO_DISPLAY_OVERLAY_FONT_CJK16) {
                if(!meiso_display_overlay_cjk_next(&text,&cp)) return false;
            } else { cp=*text.data++;--text.size; }
            const unsigned g=counts[i]++;
            if(!meiso_k230d_overlay_glyph(v->font,cp,rows[i][g],&advances[i][g],&heights[i][g])) return false;
        }
    }
    if(bytes) memset(pixels,0,bytes);
    for(uint32_t i=0;i<p->item_count;++i) {
        const MeisoDisplaySceneItem *v=&p->items[i];const uint32_t ox=v->x-r.x,oy=v->y-r.y;
        for(uint32_t y=0;y<v->height;++y) for(uint32_t x=0;x<v->width;++x) {
            uint16_t *d=pixels+(oy+y)*r.width+ox+x;
            const uint32_t color=(x<v->border_width || y<v->border_width ||
                x>=v->width-v->border_width || y>=v->height-v->border_width)?v->border_argb:v->fill_argb;
            *d=over(*d,color);
        }
        unsigned advance=0;
        for(unsigned g=0;g<counts[i];++g) {
            const unsigned gw=v->font==MEISO_DISPLAY_OVERLAY_FONT_LABEL?5:advances[i][g];
            for(unsigned y=0;y<heights[i][g];++y) for(unsigned x=0;x<gw;++x)
                if(rows[i][g][2*y+x/8]&(0x80U>>(x%8)))
                    for(unsigned yy=0;yy<v->text_scale;++yy) for(unsigned xx=0;xx<v->text_scale;++xx) {
                        uint16_t *d=pixels+(oy+v->border_width+y*v->text_scale+yy)*r.width+
                            ox+v->border_width+(advance+x)*v->text_scale+xx;
                        *d=over(*d,v->text_argb);
                    }
            advance+=advances[i][g];
        }
    }
    *out=r;return true;
}
