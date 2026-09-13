#include <meiso/profiles/display_ingress/scene.h>
#include "display_ingress_profile_private.h"
#include <string.h>
#define put32 meiso_display_ingress_private_put_u32
#define put64 meiso_display_ingress_private_put_u64
#define get32 meiso_display_ingress_private_get_u32
#define get64 meiso_display_ingress_private_get_u64
#define zero meiso_display_ingress_private_bytes_zero
#define overlap meiso_display_ingress_private_ranges_overlap
static bool dimension(uint32_t n) { return n && n<=4096; }
static bool alignment(uint32_t n) { return dimension(n) && !(n&(n-1)); }
static bool cap_valid(const MeisoDisplaySceneCapability *c)
{
    return c && dimension(c->output_width) && dimension(c->output_height) &&
        c->max_pixels && c->max_pixels<=UINT32_C(4096)*4096 &&
        c->max_text<=48 && c->max_updates_per_second && c->max_updates_per_second<=60 &&
        c->max_items && c->max_items<=16 && c->fonts && !(c->fonts&~3U) &&
        dimension(c->min_width) && dimension(c->min_height) &&
        c->min_width<=c->output_width && c->min_height<=c->output_height &&
        alignment(c->width_alignment) && alignment(c->height_alignment) &&
        !(c->min_width%c->width_alignment) && !(c->min_height%c->height_alignment) &&
        (uint32_t)c->min_width*c->min_height<=c->max_pixels;
}
static bool plan_valid(const MeisoDisplayScenePlan *p)
{
    if(!p || !p->generation || !p->video_desired_version || !dimension(p->canvas_width) ||
        !dimension(p->canvas_height) || p->item_count>16) return false;
    for(uint32_t i=0;i<p->item_count;++i) {
        const MeisoDisplaySceneItem *v=&p->items[i];uint32_t tw,th;
        if(!dimension(v->width) || !dimension(v->height) || v->width>p->canvas_width ||
            v->height>p->canvas_height || v->x>p->canvas_width-v->width || v->y>p->canvas_height-v->height ||
            v->border_width>v->width/2 || v->border_width>v->height/2 || !v->text_scale ||
            v->text_scale>8 || v->text_size>48 ||
            !zero((const uint8_t *)v->text+v->text_size,48-v->text_size) ||
            !meiso_display_overlay_text_extent(v->font,(MeisoByteView){(const uint8_t *)v->text,v->text_size},&tw,&th) ||
            tw*v->text_scale>v->width-2*v->border_width || th*v->text_scale>v->height-2*v->border_width) return false;
    }
    return true;
}
size_t meiso_display_scene_plan_bytes(const MeisoDisplayScenePlan *p)
{ return plan_valid(p)?32+88*p->item_count:0; }
bool meiso_display_scene_capability_encode(const MeisoDisplaySceneCapability *c,MeisoMutableBuffer out)
{
    if(!cap_valid(c) || !out.data || out.capacity<36 || overlap(c,sizeof(*c),out.data,36)) return false;
    uint8_t b[36]={0};put32(b,c->output_width);put32(b+4,c->output_height);put32(b+8,c->max_pixels);
    put32(b+12,c->max_text);put32(b+16,c->max_updates_per_second);
    const uint16_t n[]={c->min_width,c->min_height,c->width_alignment,c->height_alignment};
    for(unsigned i=0;i<4;++i) { b[20+2*i]=(uint8_t)(n[i]>>8);b[21+2*i]=(uint8_t)n[i]; }
    put32(b+28,c->max_items);put32(b+32,c->fonts);memcpy(out.data,b,36);return true;
}
bool meiso_display_scene_capability_decode(MeisoByteView in,MeisoDisplaySceneCapability *out)
{
    if(!in.data || in.size!=36 || !out || overlap(in.data,in.size,out,sizeof(*out))) return false;
    const uint8_t *b=in.data;
    MeisoDisplaySceneCapability c={get32(b),get32(b+4),get32(b+8),get32(b+12),get32(b+16),
        (uint16_t)(b[20]*256U+b[21]),(uint16_t)(b[22]*256U+b[23]),
        (uint16_t)(b[24]*256U+b[25]),(uint16_t)(b[26]*256U+b[27]),get32(b+28),get32(b+32)};
    if(!cap_valid(&c)) return false;
    *out=c;return true;
}
bool meiso_display_scene_plan_encode(const MeisoDisplayScenePlan *p,MeisoMutableBuffer out)
{
    const size_t n=meiso_display_scene_plan_bytes(p);
    if(!n || !out.data || out.capacity<n || overlap(p,sizeof(*p),out.data,n)) return false;
    uint8_t b[MEISO_DISPLAY_SCENE_MAX_PLAN_BYTES]={0};put64(b,p->generation);put64(b+8,p->video_desired_version);
    put32(b+16,p->canvas_width);put32(b+20,p->canvas_height);b[24]=(uint8_t)p->item_count;
    for(uint32_t i=0;i<p->item_count;++i) {
        const MeisoDisplaySceneItem *v=&p->items[i];uint8_t *s=b+32+88*i;
        put32(s,v->x);put32(s+4,v->y);put32(s+8,v->width);put32(s+12,v->height);
        put32(s+16,v->border_argb);put32(s+20,v->fill_argb);put32(s+24,v->text_argb);
        s[28]=(uint8_t)(v->border_width>>8);s[29]=(uint8_t)v->border_width;
        s[30]=v->text_scale;s[31]=v->font;s[32]=v->text_size;memcpy(s+40,v->text,v->text_size);
    }
    memcpy(out.data,b,n);return true;
}
bool meiso_display_scene_plan_decode(MeisoByteView in,MeisoDisplayScenePlan *out)
{
    if(!in.data || in.size<32 || !out || overlap(in.data,in.size,out,sizeof(*out)) ||
        in.data[24]>16 || in.size!=(size_t)32+88*in.data[24] || !zero(in.data+25,7)) return false;
    const uint8_t *b=in.data;MeisoDisplayScenePlan p={0};
    p.generation=get64(b);p.video_desired_version=get64(b+8);p.canvas_width=get32(b+16);
    p.canvas_height=get32(b+20);p.item_count=b[24];
    for(uint32_t i=0;i<p.item_count;++i) {
        const uint8_t *s=b+32+88*i;MeisoDisplaySceneItem *v=&p.items[i];
        if(!zero(s+33,7)) return false;
        v->x=get32(s);v->y=get32(s+4);v->width=get32(s+8);v->height=get32(s+12);
        v->border_argb=get32(s+16);v->fill_argb=get32(s+20);v->text_argb=get32(s+24);
        v->border_width=(uint16_t)(s[28]*256U+s[29]);v->text_scale=s[30];v->font=s[31];v->text_size=s[32];
        memcpy(v->text,s+40,48);
    }
    if(!plan_valid(&p)) return false;
    *out=p;return true;
}
bool meiso_display_scene_supported(const MeisoDisplaySceneCapability *c,const MeisoDisplayScenePlan *p,
    uint64_t version,MeisoDisplaySceneRegion *out)
{
    if(!cap_valid(c) || !plan_valid(p) || !out || overlap(out,sizeof(*out),c,sizeof(*c)) ||
        overlap(out,sizeof(*out),p,sizeof(*p)) || version!=p->video_desired_version ||
        p->canvas_width!=c->output_width || p->canvas_height!=c->output_height || p->item_count>c->max_items) return false;
    MeisoDisplaySceneRegion r={0};
    if(p->item_count) {
        uint32_t left=p->canvas_width,top=p->canvas_height,right=0,bottom=0;
        for(uint32_t i=0;i<p->item_count;++i) {
            const MeisoDisplaySceneItem *v=&p->items[i];
            if(v->text_size>c->max_text || (v->text_size && !(c->fonts&(1U<<v->font)))) return false;
            if(v->x<left) left=v->x;
            if(v->y<top) top=v->y;
            if(v->x+v->width>right) right=v->x+v->width;
            if(v->y+v->height>bottom) bottom=v->y+v->height;
        }
        r.width=right-left;r.height=bottom-top;
        if(r.width<c->min_width) r.width=c->min_width;
        if(r.height<c->min_height) r.height=c->min_height;
        r.width=(r.width+c->width_alignment-1)&~(uint32_t)(c->width_alignment-1);
        r.height=(r.height+c->height_alignment-1)&~(uint32_t)(c->height_alignment-1);
        if(r.width>c->output_width || r.height>c->output_height || (uint64_t)r.width*r.height>c->max_pixels) return false;
        r.x=left<c->output_width-r.width?left:c->output_width-r.width;
        r.y=top<c->output_height-r.height?top:c->output_height-r.height;
    }
    *out=r;return true;
}
