#include <meiso/profiles/display_ingress/projection.h>
#include "display_ingress_profile_private.h"

#include <float.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "projection wire requires IEEE binary32");
static const uint8_t projection_uuid[16] = {
    0xd4,0xe0,0xea,0x40,0x60,0x84,0x4f,0x31,0xbb,0x01,0x96,0xbe,0xd4,0x9b,0xf4,0xa1
};
enum { PROJECTION_BYTES = 128, PAYLOAD_BYTES = 16 + PROJECTION_BYTES };

static bool overlap(const void *a, size_t n, const void *b, size_t m)
{ return meiso_display_ingress_private_ranges_overlap(a,n,b,m); }
static void put_float(uint8_t *p, float f)
{ uint32_t u; memcpy(&u,&f,4); meiso_display_ingress_private_put_u32(p,u); }
static float get_float(const uint8_t *p)
{ uint32_t u=meiso_display_ingress_private_get_u32(p); float f; memcpy(&f,&u,4); return f; }

bool meiso_display_projection_valid(const MeisoDisplayProjection *v)
{
    if (!v || v->view_count < 1 || v->view_count > 2 ||
        v->space_class < 1 || v->space_class > 3 ||
        !v->source_space_id || !v->source_space_epoch) return false;
    if (v->target_time_present ? (!v->target_time.clock_epoch || !v->target_time.monotonic_ns) :
        (v->target_time.clock_epoch || v->target_time.monotonic_ns)) return false;
    for (unsigned i=0;i<2;++i) {
        const MeisoDisplayProjectionView *w=&v->views[i];
        double norm=0;
        for (unsigned j=0;j<4;++j) {
            const float q=w->orientation_xyzw[j], f=w->fov_left_right_up_down[j];
            if (!isfinite(q) || !isfinite(f) || f<=-1.57079632679F || f>=1.57079632679F) return false;
            if (i>=v->view_count && (q!=0 || f!=0)) return false;
            norm+=(double)q*q;
        }
        for (unsigned j=0;j<3;++j)
            if (!isfinite(w->position_meters[j]) || (i>=v->view_count && w->position_meters[j]!=0)) return false;
        if (i<v->view_count && (norm<0.999 || norm>1.001 ||
            w->fov_left_right_up_down[0]>=w->fov_left_right_up_down[1] ||
            w->fov_left_right_up_down[3]>=w->fov_left_right_up_down[2])) return false;
    }
    return true;
}

static void encode_value(const MeisoDisplayProjection *v, uint8_t *p)
{
    memset(p,0,PROJECTION_BYTES);
    p[0]=1; p[1]=v->view_count; p[2]=v->target_time_present; p[3]=v->space_class;
    meiso_display_ingress_private_put_u64(p+8,v->source_space_id);
    meiso_display_ingress_private_put_u64(p+16,v->source_space_epoch);
    meiso_display_ingress_private_put_u64(p+24,v->target_time.clock_epoch);
    meiso_display_ingress_private_put_u64(p+32,v->target_time.monotonic_ns);
    for (unsigned i=0;i<v->view_count;++i) {
        uint8_t *w=p+40+44*i;
        for (unsigned j=0;j<4;++j) put_float(w+4*j,v->views[i].orientation_xyzw[j]);
        for (unsigned j=0;j<3;++j) put_float(w+16+4*j,v->views[i].position_meters[j]);
        for (unsigned j=0;j<4;++j) put_float(w+28+4*j,v->views[i].fov_left_right_up_down[j]);
    }
}

static bool decode_value(const uint8_t *p, MeisoDisplayProjection *v)
{
    if (p[0]!=1 || p[2]>1 || !meiso_display_ingress_private_bytes_zero(p+4,4) ||
        (p[1]==1 && !meiso_display_ingress_private_bytes_zero(p+84,44))) return false;
    *v=(MeisoDisplayProjection){.view_count=p[1],.space_class=p[3],.target_time_present=p[2]!=0};
    v->source_space_id=meiso_display_ingress_private_get_u64(p+8);
    v->source_space_epoch=meiso_display_ingress_private_get_u64(p+16);
    v->target_time.clock_epoch=meiso_display_ingress_private_get_u64(p+24);
    v->target_time.monotonic_ns=meiso_display_ingress_private_get_u64(p+32);
    for (unsigned i=0;i<2;++i) {
        const uint8_t *w=p+40+44*i;
        for (unsigned j=0;j<4;++j) v->views[i].orientation_xyzw[j]=get_float(w+4*j);
        for (unsigned j=0;j<3;++j) v->views[i].position_meters[j]=get_float(w+16+4*j);
        for (unsigned j=0;j<4;++j) v->views[i].fov_left_right_up_down[j]=get_float(w+28+4*j);
    }
    return meiso_display_projection_valid(v);
}

MeisoDisplayIngressProfileStatus meiso_display_projection_sei_write(
    const MeisoDisplayProjection *v, MeisoMutableBuffer output, size_t *out_size)
{
    uint8_t rbsp[2+PAYLOAD_BYTES+1], nal[MEISO_DISPLAY_PROJECTION_SEI_MAX_BYTES]={0,0,0,1,6};
    size_t n=5; unsigned zeros=0;
    if (!v || !output.data || !out_size || overlap(v,sizeof(*v),output.data,output.capacity) ||
        overlap(out_size,sizeof(*out_size),output.data,output.capacity) || overlap(v,sizeof(*v),out_size,sizeof(*out_size)))
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    if (!meiso_display_projection_valid(v)) return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    rbsp[0]=5; rbsp[1]=PAYLOAD_BYTES; memcpy(rbsp+2,projection_uuid,16);
    encode_value(v,rbsp+18); rbsp[sizeof(rbsp)-1]=0x80;
    for (size_t i=0;i<sizeof(rbsp);++i) {
        if (zeros==2 && rbsp[i]<=3) { nal[n++]=3; zeros=0; }
        nal[n++]=rbsp[i]; zeros=rbsp[i]==0 ? zeros+1 : 0;
    }
    if (output.capacity<n) return MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY;
    memcpy(output.data,nal,n); *out_size=n; return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

static size_t start_code(MeisoByteView u, size_t i)
{
    if (i>u.size || u.size-i<3 || u.data[i] || u.data[i+1]) return 0;
    return u.data[i+2]==1 ? 3 : (u.size-i>=4 && !u.data[i+2] && u.data[i+3]==1 ? 4 : 0);
}

typedef struct { const uint8_t *p; size_t size, at; unsigned zeros; } Reader;
static bool byte(Reader *r, uint8_t *out)
{
    if (r->at==r->size) return false;
    uint8_t b=r->p[r->at++];
    if (r->zeros==2) {
        if (b==3) {
            if (r->at==r->size || r->p[r->at]>3) return false;
            b=r->p[r->at++]; r->zeros=0;
        } else if (b<3) return false;
    }
    r->zeros=b==0 ? r->zeros+1 : 0; *out=b; return true;
}
static bool sei(Reader r, bool *found, MeisoDisplayProjection *value)
{
    while (r.at<r.size) {
        uint8_t b, payload[PAYLOAD_BYTES]; size_t type=0,size=0;
        if (!byte(&r,&b)) return false;
        if (b==0x80 && r.at==r.size) return true;
        do { if (type>SIZE_MAX-b) return false; type+=b; if (b!=255) break; if (!byte(&r,&b)) return false; } while (true);
        do { if (!byte(&r,&b) || size>SIZE_MAX-b) return false; size+=b; } while (b==255);
        if (size>r.size-r.at) return false;
        for (size_t i=0;i<size;++i) {
            if (!byte(&r,&b)) return false;
            if (i<sizeof(payload)) payload[i]=b;
        }
        if (type==5 && size>=16 && !memcmp(payload,projection_uuid,16)) {
            if (*found || size!=PAYLOAD_BYTES || !decode_value(payload+16,value)) return false;
            *found=true;
        }
    }
    return false; /* rbsp_trailing_bits is mandatory. */
}

MeisoDisplayIngressProfileStatus meiso_display_projection_sei_read(
    MeisoByteView u, bool *present, MeisoDisplayProjection *out)
{
    bool found=false; MeisoDisplayProjection value={0}; size_t at=0;
    if (!u.data || !u.size || !present || !out || overlap(u.data,u.size,out,sizeof(*out)) ||
        overlap(u.data,u.size,present,sizeof(*present)) || overlap(out,sizeof(*out),present,sizeof(*present)))
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    while (at<u.size) {
        const size_t prefix=start_code(u,at); size_t end;
        if (!prefix || u.size-at<=prefix) return MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED;
        at+=prefix;
        if (u.data[at]&0x80) return MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED;
        for (end=at+1;end<u.size && !start_code(u,end);++end) {}
        if ((u.data[at]&31)==6) {
            size_t payload_end=end;
            while (payload_end>at+1 && !u.data[payload_end-1]) --payload_end; /* Annex B trailing_zero_8bits */
            if (!sei((Reader){.p=u.data+at+1,.size=payload_end-at-1},&found,&value))
                return MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED;
        }
        at=end;
    }
    if (found) *out=value;
    *present=found; return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_projection_sei_insert(
    MeisoByteView u, const MeisoDisplayProjection *value, MeisoMutableBuffer output, size_t *out_size)
{
    uint8_t nal[MEISO_DISPLAY_PROJECTION_SEI_MAX_BYTES]; size_t n=0,at=0; bool present=false;
    MeisoDisplayProjection prior;
    if (!value || !u.data || !output.data || !out_size ||
        (u.data!=output.data && overlap(u.data,u.size,output.data,output.capacity)) ||
        overlap(value,sizeof(*value),output.data,output.capacity) ||
        overlap(out_size,sizeof(*out_size),output.data,output.capacity) ||
        overlap(out_size,sizeof(*out_size),u.data,u.size) || overlap(out_size,sizeof(*out_size),value,sizeof(*value)))
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    MeisoDisplayIngressProfileStatus status=meiso_display_projection_sei_write(value,(MeisoMutableBuffer){nal,sizeof(nal)},&n);
    if (status!=MEISO_DISPLAY_INGRESS_PROFILE_OK) return status;
    if (!meiso_display_ingress_h264_idr_access_unit_valid(u) ||
        meiso_display_projection_sei_read(u,&present,&prior)!=MEISO_DISPLAY_INGRESS_PROFILE_OK || present)
        return MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED;
    if (u.size>SIZE_MAX-n || output.capacity<u.size+n) return MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY;
    while (at<u.size) {
        size_t prefix=start_code(u,at);
        if ((u.data[at+prefix]&31)==5) break;
        at+=prefix+1;
        while (at<u.size && !start_code(u,at)) ++at;
    }
    memmove(output.data+at+n,u.data+at,u.size-at);
    if (at) memmove(output.data,u.data,at);
    memcpy(output.data+at,nal,n); *out_size=u.size+n;
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}
