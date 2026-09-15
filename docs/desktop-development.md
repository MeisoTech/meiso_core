# Desktop Host / Edge development

Host and Edge are roles. Desktop validation uses the production Session,
Catalog, Desired/Actual, Display Ingress H.264 profile and bounded TCP/UDP
adapters. Only the output backend is replaced. No K230D or camera is required.

## Build and run a local pair

Install CMake, pkg-config and FFmpeg development libraries (`libavcodec`,
`libavutil`, `libswscale`), then build:

```sh
cmake -S . -B build-desktop -DMEISO_BUILD_DESKTOP_EDGE=ON
cmake --build build-desktop --target meiso_desktop_host meiso_desktop_edge
cmake --install build-desktop --component DesktopRuntime --prefix "$PWD/desktop-install"
```

Prepare a local 1280×720 H.264 Annex B file, one Baseline IDR slice per access
unit, with repeated SPS/PPS and optional AUD. The same 128 KiB AU limit applies
as on K230D. For example, with an explicitly supplied local video:

```sh
ffmpeg -i input.mp4 -frames:v 60 -c:v libx264 -threads 1 \
  -preset ultrafast -tune zerolatency -profile:v baseline -pix_fmt yuv420p \
  -x264-params keyint=1:min-keyint=1:scenecut=0:repeat-headers=1:aud=1:slices=1 \
  -b:v 1500k -f h264 source.h264
python3 devtools/desktop/pair.py \
  --host-service build-desktop/meiso_desktop_host \
  --edge-service build-desktop/meiso_desktop_edge \
  --source source.h264 --output-directory runs/desktop-pair
```

`pair.py` creates real connected TCP and UDP sockets, passes the descriptors to
the two independent executables, and checks their exits. It contains no Meiso
wire codec. The output directory must be new. The Edge decodes into a single
owned decoded RGBA raster and output canvas, and returns **SIMULATED** evidence with the actual accepted AU
sequence/digest. `first.ppm`, `latest.ppm`, `last.ppm` allow inspection; the
received elementary stream is recorded up to 64 MiB, then recording stops while
decoding continues. This is a finite debugging capture, not platform history.
When the backend closes, `last.ppm` preserves its final presented image for
diagnosis and `latest.ppm` becomes black, including after a control disconnect.
That cleanup does not create a frame or application presentation receipt.
The installed commands are `meiso-desktop-pair`, `meiso-desktop-launch`,
`meiso_desktop_host` and `meiso_desktop_edge`; the same pair invocation works
with their installed paths. FFmpeg shared libraries remain a runtime dependency.

The reference Host here replays prepared encoded input. It validates the shared
SDK/transport but does not replace a Looking Glass HostRuntime application test.
Software completion is not a DRM page flip, physical visibility, K230D memory
performance or a measurement of capture-to-photon delay.

## Mixed devices

`devtools/desktop/launch.py --help` supplies explicit bind/peer/control/media
ports and execs the same service on macOS or Linux. Connected descriptors remain
the deployment boundary; tunnels/relays do not enter the product protocol.
It can connect the desktop Edge to Looking Glass's real HostRuntime when that
Host explicitly selects simulated evidence. The default physical Host must
reject this capability; it must never silently weaken its evidence requirement.
Use distinct configured peer identities, sockets and output directories for
continuous mixed-device development.

For a routed LAN, start the Looking Glass Host with its configured Edge address
and media port, `--presentation-evidence simulated --native-overlay off`. Start
the Edge with `meiso-desktop-launch --role edge`, the corresponding bind/peer
addresses and TCP/UDP ports. Pass `--continuous on --output-directory NEW_DIR`
and both configured peer identities after the launcher's `--` separator.
Then run the normal native application against the Host's IPC socket with
`--overlay off`. The application and its cleanup use the same HostRuntime API.
If a deployment uses a NAT or tunnel, its UDP return mapping must remain valid
while the application is idle; a connected TCP control stream alone does not
keep that mapping alive. This is a deployment responsibility.

The v9 application profile supports video, optional camera and optional native
HUD in one Session. To compose stereo video and independent labels, select
`--native-overlay on --video-layout side-by-side` on both services and add
`--output-width 1920 --output-height 1080` to the desktop Edge service options.
The 1280×720 video is placed per eye within that output canvas. Leave the
application's overlay enabled (or select its `--message`); HUD show, move and
hide recompose from the decoded video without retaining old pixels. Video and
HUD receipts remain SIMULATED. Output dimensions are parameters, not a physical
screen claim. Parameter snapshots for offline optics
are separate from real device calibration. Actual supported dimensions and AU
budgets come from each peer's Catalog rather than the desktop's memory size.

For independent per-eye rectangles and source-bound recognition, select
`--native-overlay scene` at both peers (application v10). The installed Looking
Glass SDK keeps its ordinary screen-message API and additionally accepts native
scenes; use its static-scene adapter for source/optical binding. A v10 installation
does not supply measured optical calibration. On the K230D init deployment,
`MEISO_NATIVE_OVERLAY=scene` selects this same mode; omission retains `on`/v9.

## Optional local camera uplink

Pass `--camera-codec jpeg` in the Edge service arguments (after `--`) to select
320×240 JPEG keyframes at no more than 0.5 Hz / 30 KiB each. The matched Host must
request JPEG. The Edge encodes baseline JPEG at quality 75 and drops an oversize
frame; it does not accumulate retries or lower the source cadence by queuing old
images. Capture still runs at the desktop provider rate; this limits transmission,
not camera power. Omitting the option retains the existing RAW capability.

The Edge launcher accepts `--camera-device DEVICE --camera-port PORT` before
the service arguments. On macOS DEVICE is an explicitly selected FFmpeg
AVFoundation video device (enumerate with `ffmpeg -f avfoundation -list_devices
true -i ''`); on Linux it is a V4L2 path. No audio is opened. It captures
640x480/30, reduces to 320x240 RGBA/5 Hz, and publishes at the Host-requested
rate (initially 2 Hz). Missing devices, capture stalls or partial-frame EOF fail
the run and reclaim its children. Stopping the launcher stops its camera.
Camera TCP EOF immediately revokes that source. Because media and control can
use independent carriers, the Edge waits at most five seconds for the existing
normal Session close; EOF alone never makes a run successful.

The matching Looking Glass `meiso-host-serve` selects `--camera-port PORT`,
`--camera-fixture off`; native HUD may remain enabled. Both sides select Display
Ingress v9, composing video, optional HUD and CameraMedia in one Session. The camera port
is a separate TCP media connection using the existing CameraMedia framing,
not another control Session. The normal video media ports still use UDP.
The Host publishes owned RGBA snapshots through its installed camera SDK;
Locate's `configs/sdk-camera-people.json` demonstrates explicit real CPU person
detection. Source exposure and optical calibration remain unknown.

Capture buffers and FFmpeg memory are desktop costs. This path does not
establish simultaneous K230D ISP/decoder/display memory or power budgets.

The installed launcher is an explicit foreground development command, not a
login camera service. After a connection ends, close the previous application
and launch a new Session with a **new** output directory, then restart the app
against the Host IPC socket. Do not reuse old handles or replay their content.
The Linux Host's existing systemd service can rebuild its connection; its
optional camera/evidence configuration is described in the
[paired installation guide](host-edge-quickstart.md#安装并连接).
