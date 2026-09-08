#include <meiso/profiles/display_ingress/projection.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x); return 1; } } while (0)
static const uint8_t golden[]={0x00,0x00,0x00,0x01,0x06,0x05,0x90,0xd4,0xe0,0xea,0x40,0x60,0x84,0x4f,0x31,0xbb,0x01,0x96,0xbe,0xd4,0x9b,0xf4,0xa1,0x01,0x02,0x01,0x02,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x05,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x07,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x09,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x3f,0x80,0x00,0x00,0xbd,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0xbf,0x00,0x00,0x03,0x00,0x3f,0x00,0x00,0x03,0x00,0x3f,0x00,0x00,0x03,0x00,0xbf,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x3f,0x80,0x00,0x00,0x3d,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0xbf,0x00,0x00,0x03,0x00,0x3f,0x00,0x00,0x03,0x00,0x3f,0x00,0x00,0x03,0x00,0xbf,0x00,0x00,0x03,0x00,0x80};
static const uint8_t au[]={0,0,0,1,9,0xf0,0,0,0,1,0x67,66,0,30,0x80,0,0,1,0x68,0x80,0,0,1,0x65,0x80};
int main(void) {
    MeisoDisplayProjection p={.view_count=2,.space_class=2,.source_space_id=3,.source_space_epoch=5,
        .target_time_present=true,.target_time={7,9}};
    for (unsigned i=0;i<2;++i) {
        p.views[i].orientation_xyzw[3]=1;
        p.views[i].position_meters[0]=i ? .03125F : -.03125F;
        const float f[]={-.5F,.5F,.5F,-.5F}; memcpy(p.views[i].fov_left_right_up_down,f,sizeof(f));
    }
    uint8_t bytes[1024],saved[1024]; size_t n=777; bool present=false;
    MeisoDisplayProjection out={0},prior=out;
    CHECK(meiso_display_projection_sei_write(&p,(MeisoMutableBuffer){bytes,sizeof(bytes)},&n)==0);
    CHECK(n==sizeof(golden) && !memcmp(bytes,golden,n));
    CHECK(meiso_display_projection_sei_read((MeisoByteView){golden,sizeof(golden)},&present,&out)==0 && present);
    CHECK(out.view_count==2 && out.source_space_id==3 && out.source_space_epoch==5 &&
        out.target_time.clock_epoch==7 && out.target_time.monotonic_ns==9 && out.target_time_present);
    CHECK(!memcmp(out.views,p.views,sizeof(p.views)));
    CHECK(meiso_display_projection_sei_read((MeisoByteView){au,sizeof(au)},&present,&out)==0 && !present);
    prior=out;
    memcpy(bytes,golden,sizeof(golden)); bytes[7]^=1;
    CHECK(meiso_display_projection_sei_read((MeisoByteView){bytes,sizeof(golden)},&present,&out)==0 && !present);
    memcpy(bytes,golden,sizeof(golden)); bytes[23]=2; present=true;
    CHECK(meiso_display_projection_sei_read((MeisoByteView){bytes,sizeof(golden)},&present,&out)!=0 && present);
    CHECK(!memcmp(&prior,&out,sizeof(out)));
    memcpy(bytes,golden,sizeof(golden)); bytes[6]=145; /* Known payload has the wrong length. */
    CHECK(meiso_display_projection_sei_read((MeisoByteView){bytes,sizeof(golden)},&present,&out)!=0);
    memcpy(bytes,golden,sizeof(golden));
    size_t quaternion=0;
    for (size_t i=23;i+1<sizeof(golden);++i) if (bytes[i]==0x3f && bytes[i+1]==0x80) { quaternion=i; break; }
    CHECK(quaternion!=0); bytes[quaternion]=0x7f; /* Unit w=1 becomes infinity on the wire. */
    CHECK(meiso_display_projection_sei_read((MeisoByteView){bytes,sizeof(golden)},&present,&out)!=0);
    memcpy(bytes,golden,sizeof(golden)); memcpy(bytes+sizeof(golden),golden,sizeof(golden));
    CHECK(meiso_display_projection_sei_read((MeisoByteView){bytes,2*sizeof(golden)},&present,&out)!=0);
    CHECK(meiso_display_projection_sei_read((MeisoByteView){golden,sizeof(golden)-1},&present,&out)!=0);
    for (size_t size=1;size<sizeof(golden);++size)
        CHECK(meiso_display_projection_sei_read((MeisoByteView){golden,size},&present,&out)!=0);
    CHECK(meiso_display_ingress_h264_idr_access_unit_valid((MeisoByteView){au,sizeof(au)}));
    CHECK(meiso_display_projection_sei_insert((MeisoByteView){au,sizeof(au)},&p,
        (MeisoMutableBuffer){bytes,sizeof(bytes)},&n)==0);
    CHECK(n==sizeof(au)+sizeof(golden) && meiso_display_ingress_h264_idr_access_unit_valid((MeisoByteView){bytes,n}));
    CHECK(!memcmp(bytes,au,20) && !memcmp(bytes+20,golden,sizeof(golden)) && !memcmp(bytes+n-5,au+20,5));
    CHECK(meiso_display_projection_sei_read((MeisoByteView){bytes,n},&present,&out)==0 && present);
    memcpy(saved,bytes,n); size_t m=n;
    CHECK(meiso_display_projection_sei_insert((MeisoByteView){bytes,n},&p,(MeisoMutableBuffer){bytes,sizeof(bytes)},&m)!=0);
    CHECK(m==n && !memcmp(bytes,saved,n));
    memcpy(bytes,au,sizeof(au));
    CHECK(meiso_display_projection_sei_insert((MeisoByteView){bytes,sizeof(au)},&p,(MeisoMutableBuffer){bytes,sizeof(bytes)},&m)==0);
    CHECK(m==n && !memcmp(bytes,saved,n));
    memset(bytes,0xa5,sizeof(bytes)); memcpy(saved,bytes,sizeof(bytes)); n=777;
    CHECK(meiso_display_projection_sei_write(&p,(MeisoMutableBuffer){bytes,1},&n)==MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY);
    CHECK(n==777 && !memcmp(bytes,saved,sizeof(bytes)));
    CHECK(meiso_display_projection_sei_insert((MeisoByteView){au,sizeof(au)},&p,(MeisoMutableBuffer){bytes,sizeof(au)},&n)==MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY);
    CHECK(n==777 && !memcmp(bytes,saved,sizeof(bytes)));
    p.views[0].orientation_xyzw[3]=NAN; CHECK(!meiso_display_projection_valid(&p));
    p.views[0].orientation_xyzw[3]=2; CHECK(!meiso_display_projection_valid(&p));
    p.views[0].orientation_xyzw[3]=1; p.views[0].position_meters[0]=INFINITY; CHECK(!meiso_display_projection_valid(&p));
    p.views[0].position_meters[0]=0; p.target_time_present=false; CHECK(!meiso_display_projection_valid(&p));
    p.target_time=(MeisoTimePoint){0}; p.view_count=1; CHECK(!meiso_display_projection_valid(&p));
    memset(&p.views[1],0,sizeof(p.views[1])); CHECK(meiso_display_projection_valid(&p));
    CHECK(meiso_display_projection_sei_write(&p,(MeisoMutableBuffer){bytes,sizeof(bytes)},&n)==0);
    CHECK(meiso_display_projection_sei_read((MeisoByteView){bytes,n},&present,&out)==0 && present && out.view_count==1 && !out.target_time_present);
    puts("DISPLAY_PROJECTION_SEI_CODEC_OK"); return 0;
}
