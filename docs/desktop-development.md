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
owned RGBA raster and returns **SIMULATED** evidence with the actual accepted AU
sequence/digest. `first.ppm`, `latest.ppm`, `last.ppm` allow inspection; the
received elementary stream is recorded up to 64 MiB, then recording stops while
decoding continues. This is a finite debugging capture, not platform history.
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

Current desktop support is the H.264 video profile with an offscreen output
raster. Native HUD and centered physical-eye crops are not yet
implemented in this adapter. Their capabilities are not advertised; selecting
them fails before the Session starts. Parameter snapshots for offline optics
are separate from real device calibration. Actual supported dimensions and AU
budgets come from each peer's Catalog rather than the desktop's memory size.

## Optional local camera uplink

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
`--camera-fixture off` and `--native-overlay off`. Both sides select Display
Ingress v8, composing video and CameraMedia in one Session. The camera port
is a separate TCP media connection using the existing CameraMedia framing,
not another control Session. The normal video media ports still use UDP.
The Host publishes owned RGBA snapshots through its installed camera SDK;
Locate's `configs/sdk-camera-people.json` demonstrates explicit real CPU person
detection. Source exposure and optical calibration remain unknown.

Capture buffers and FFmpeg memory are desktop costs. This path does not
establish simultaneous K230D ISP/decoder/display memory or power budgets.
