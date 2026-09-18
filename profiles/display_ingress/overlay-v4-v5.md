# Display Ingress v4/v5 overlay contracts

> `VERSIONED PROFILE CONTRACT`。旧版显式选择及被后续版本继承的规则保留于此，
> 不是实验归档，也不改变任何 wire 或行为。当前入口见[Display Ingress](README.md)。
> v6/v7 沿用本页的层顺序、State 激活、证据及像素预算；v9/v10 的覆盖项以其明确选择为准。

## Historical v4: compressed video with a native screen layer

`DISPLAY_/INGRESS_:v4` explicitly selects two ordered `VIEW Quad` layers: the
unchanged v2 H.264 video layer, then one Edge-native screen region. It uses the
same MSL0/Runtime v4 connection, the four existing video routes, and a second
endpoint with Host PresentationPlan Desired, Edge PresentationStatus Actual,
and an independent Edge page-flip evidence Stream. v1/v2 graphs stay unchanged.
The second endpoint is `DISPLAY_/HUD_END_`, with `DISP_SCH/HUD_CAP_:v2`
capability. Its ordered Resource IDs are `DISPLAY_/HUD_PLAN`, `HUD_STAT`,
and `HUD_PRES`; schemas use the corresponding suffix under `DISP_SCH`: plan v2,
status/evidence v1. v4 replaces the experimental v3 overlay selection; the
Session profile mismatch rejects mixed versions before catalog exchange.
All three routes are inline; the first two are State Desired/Actual, the
last is Stream Data. This exact graph uses nine registry entries.
State delivery generation is 9; HUD evidence uses Stream epoch 1 with
increasing sequence across HUD production epochs within this session.

The complete PresentationPlan selects this fixed layer order, references the
same Host owner's video Desired version, names a nonzero immutable plan
generation, and supplies the native region and label. The referenced video
configuration must already be active and remain unchanged in this session.
Canvas dimensions must match the advertised physical output. A later plan
may replace the native content without restarting the video production epoch.
This selects direct latest for the independently updated native layer; there
is no prepared token or cross-layer atomic frame promise. The Host keeps one offered plan until its matching physical evidence or terminal failure,
and one coalesced latest candidate before publication. The Edge retains old
content until the pending flip; a second unresolved wire plan is a Profile
violation. This bounded State activation policy does not gate video frames.
Each applied HUD plan advances only the HUD production epoch, because Runtime
binds an active epoch to one applied Desired configuration.
Session termination invalidates both layers; no offline lease is selected.

The native renderer is a bounded frame/fill plus one line of built-in 5x7 text
(`A-Z`, `0-9`, space, `-`, `.`, `:`, `/`). This first capability does not claim
arbitrary fonts, Unicode subtitles, world tracking, or optical correction.
Coordinates are physical output pixels. Layout 0 draws one region; layout 1
repeats the same region at x + canvas_width/2 for a side-by-side output. For
layout 1, x and width must fit one eye, the canvas width must be even, and
the native surface spans both copies with a transparent gap. Its pixel cost
is (canvas_width/2 + width) × height, including that gap. Layout selection
requires the corresponding advertised capability bit. Region width and spanning
surface width must both satisfy the advertised width alignment. This is screen-space
repetition, not stereo depth or optical calibration. Colors are straight `0xAARRGGBB`;
text fits inside the border at integer scale 1–8. Hidden content keeps the
renderer enabled and presents transparent pixels, so hiding has its own
physical evidence without disabling the renderer. The next plan uses a new HUD epoch; the video epoch stays fixed.

All integers below are big endian; reserved and unused text bytes are zero.
HUD capability is 32 bytes: output width/height, maximum pixel count (at most
65536), maximum label bytes (at most 48), maximum updates per second (1–60),
then minimum width/height and width/height alignment (four u16), and layout mask:u32 (bit 0 single, bit 1 side-by-side; no unknown bits,
at least one selected). K230D selects minima 16×16 and alignment 2×1 because Canaan
DRM has a 16-pixel framebuffer minimum and VO uses 8-byte stride units. Plan suffix is 112 bytes: generation:u64, video
Desired version:u64, canvas width/height:u32, x/y/width/height:u32,
border/fill/text ARGB:u32, border width:u16, text scale:u8, visible:u8,
text length:u8, layout:u8, six reserved bytes, and 48 label bytes. Visible content
must fit the canvas, pixel budget and text extent; hidden content has zero
geometry, colors, border, scale, text and layout. The Runtime Desired stays enabled.

RUNNING HUD Actual has an 8-byte applied plan generation suffix. Its observed
and applied Desired references retain Runtime meaning; activation status is
not physical proof. The separate 56-byte evidence payload is evidence
class:u8 (DRM page flip=2), seven reserved bytes, plan generation:u64,
Host Desired version:u64, and SHA-256 of the exact 112-byte plan suffix.
The containing Stream binds the Edge owner/production epoch and supplies
the actual page-flip clock/time. Receipts must match this full identity.
Skipping a superseded plan does not manufacture a receipt. Control ACKs and
HUD physical receipts never gate the next video frame.

Acceptance vectors cover exact catalog selection, nonzero generations,
canonical text/reserved bytes, off-canvas and oversized regions, distinct
video/HUD identities, stereo eye bounds, spanning pixel/stride budgets and
transparent gap rasterization, latest replacement, visible/move/hide feedback, and
unchanged video production with bounded memory on the target board.

## Historical v5: side-by-side video with native screen content

`DISPLAY_/INGRESS_:v5` explicitly selects a left/right video atlas. It reuses
v4's exact two-endpoint/seven-route graph and all its resource schemas/codecs;
v1/v2/v4 selections keep their existing behavior. The Session Profile version
makes the different video placement explicit before accepting the catalog.
The video content is two ordered `VIEW Quad` regions of the same decoded frame,
followed by the independently updated native screen content. Both video views
belong to one frame/production epoch and one atomic display commit on the same
output; its physical receipt covers that combined video update.

For input W×H and advertised physical output O×P, W and O are even, W≤O and
H≤P. Each source view is W/2×H: left starts at (0,0), right at (W/2,0).
They are displayed without scaling, centered in each output eye. The left
origin is (floor((O/2-W/2)/2), floor((P-H)/2)); the right origin adds O/2 to x.
The unused area stays outside these views. This preserves the complete images
at native pixel size; it does not claim full-screen upscaling or calibrated
optics. Host rejects a catalog whose output cannot contain the requested pair.
The native screen coordinates continue to refer to O×P, independently of W×H.

K230D uses the two nonrotating NV12 planes, one shared framebuffer and the
existing decoder surfaces. Each frame retains both views until their common
page flip; no additional pixel surfaces or CPU repacking are introduced.
Its selected input width must be divisible by 16 for two 8-pixel-aligned crops.
The matching kernel must implement video source crops and framebuffer stride;
unsupported scaling, crop alignment and unequal Y/UV pitches are rejected.
Normal close restores both video planes before releasing the shared buffers.
