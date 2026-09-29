/* Built against the actual pre-extension installed SDK, never the new decoder.
 * Literal values keep the consumer independent of the new headers. */
#include <meiso/profiles/display_ingress/planes.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
_Static_assert(MEISO_DISPLAY_PLANES_MAX_ENDPOINTS == 5, "Requires old v12 headers");
int main(void)
{
    uint8_t b[80], legacy[80]; size_t n;
    const MeisoDisplayPlanesCapability pc = {1920, 1080, 1, 0, 16, 4, 262144, 2048, 2048,
        -0.5f, 0.5f, 0.5f, -0.5f, 0, 30};
    MeisoDisplayPlanesCapability pd;
    CHECK(meiso_display_planes_capability_encode(&pc, (MeisoMutableBuffer){b, 64})); memcpy(legacy, b, 64);
    CHECK(meiso_display_planes_capability_decode((MeisoByteView){b, 64}, &pd));
    b[9] = 3; CHECK(!meiso_display_planes_capability_decode((MeisoByteView){b, 64}, &pd)); /* EDGE */
    memcpy(b, legacy, 64); b[8] = 3;
    CHECK(!meiso_display_planes_capability_decode((MeisoByteView){b, 64}, &pd)); /* RUNTIME_VIEWS */
    memset(b, 0, 8); memset(b + 20, 0, 20);
    CHECK(!meiso_display_planes_capability_decode((MeisoByteView){b, 64}, &pd)); /* canonical new layout */

    MeisoDisplayIngressCapability vc = {1280, 720, 90000, 131072, 1024, 2,
        MEISO_STREAM_DEADLINE_UNSUPPORTED, MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}, vd;
    CHECK(meiso_display_ingress_capability_encode(&vc, (MeisoMutableBuffer){b, 32}, &n) == 0);
    CHECK(meiso_display_ingress_capability_decode((MeisoByteView){b, 32}, &vd) == 0);
    b[23] = 4; CHECK(meiso_display_ingress_capability_decode((MeisoByteView){b, 32}, &vd) != 0);

    MeisoDisplayPlanesModel m = {0}, md; m.generation = 7; m.focused_app = 255;
    CHECK(meiso_display_planes_model_encode(&m, (MeisoMutableBuffer){b, 32}));
    CHECK(meiso_display_planes_model_decode((MeisoByteView){b, 32}, &md));
    b[27] = 1; CHECK(!meiso_display_planes_model_decode((MeisoByteView){b, 32}, &md));

    MeisoDisplayPlanesEvidence pe = {2, 1, 2, 7, 9, {0}}, ped;
    CHECK(meiso_display_planes_evidence_encode(&pe, (MeisoMutableBuffer){b, 64}));
    CHECK(meiso_display_planes_evidence_decode((MeisoByteView){b, 64}, &ped));
    b[0] = 3; CHECK(!meiso_display_planes_evidence_decode((MeisoByteView){b, 64}, &ped));
    b[63] = 1; CHECK(!meiso_display_planes_evidence_decode((MeisoByteView){b, 64}, &ped));

    MeisoDisplayIngressPresentationEvidence ve = {0}, ved;
    ve.evidence_class = 2; ve.connector_kind = 1; ve.output_width = 1920; ve.output_height = 1080;
    ve.media_stream_epoch = 11; ve.displayed_frame_sequence = 13;
    CHECK(meiso_display_ingress_evidence_encode(&ve, (MeisoMutableBuffer){b, 64}, &n) == 0 && n == 64);
    CHECK(meiso_display_ingress_evidence_decode((MeisoByteView){b, 64}, &ved) == 0);
    b[0] = 3; CHECK(meiso_display_ingress_evidence_decode((MeisoByteView){b, 64}, &ved) != 0);
    b[1] = 0; memset(b + 4, 0, 8); memset(b + 64, 0, 8); b[71] = 1;
    CHECK(meiso_display_ingress_evidence_decode((MeisoByteView){b, 72}, &ved) != 0);
    puts("pre-extension v12: old values accepted; new enums, mask, epoch and XR evidence rejected"); return 0;
}
