# K230D Linux four-kind lab endpoint

This target builds the bounded native Edge/Host fixture selected by
[`four-kind-lab-v0`](../../profiles/four-kind-lab-v0/README.md). It is a manual lab executable, not
a daemon, boot service, board image, product Runtime, discovery mechanism, or security transport.

## Build

Build the macOS Host with the repository CMake target:

```sh
cmake -S . -B build
cmake --build build --target meiso_four_kind_lab
```

Build the K230D Linux Edge with the official Canaan/Xuantie glibc compiler. The compiler path is
provided at invocation time and is never recorded in the repository:

```sh
MEISO_K230_LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
  targets/k230d-linux/build_four_kind_lab.sh /tmp/meiso_four_kind_lab-k230d
```

The script deliberately does not build a root filesystem, flash storage, install files, or create
autostart state. Deployment over a maintenance surface is separate from the Meiso Link evidence.

## Run

Start one bounded Edge attempt on the board:

```sh
./meiso_four_kind_lab-k230d --edge --port 27640 --sessions 1
```

Copy [`direct-ip-lab.example`](../../config/direct-ip-lab.example) to an ignored local location. Put
only the numeric IPv4 locator in the fixed environment variable; the executable validates the
tracked Profile label and port and never emits the address:

```sh
MEISO_LAB_EDGE_ADDRESS='<numeric-ipv4>' \
  ./build/meiso_four_kind_lab --host --config /path/to/local-config --test all
```

`--test state|stream|action|blob|all` selects requested NDJSON verdicts. Every selection still runs
the full Registry, State, Stream, Action and Blob transcript because the four distinctions are the
evidence boundary. Output is a fixed, dashboard-safe NDJSON projection; it excludes locator,
interface name, SSID, BSSID, MAC, credentials, nonces, complete identifiers, sysfs paths, raw thermal
values and digests.

## Platform observations

- Wi-Fi input is limited to carrier/operstate, cumulative RX/TX counters and `/proc/net/wireless`
  signal. It is network telemetry, not a physical sensor.
- The only current physical sensor input is the K230 on-die thermal source. A separate child gets at
  most 1000 ms before `listen` to validate the thermal-zone type, register/code guards and vendor
  conversion; the Session path uses only the cached result. Timeout, absence or rejection stays
  absent. Only corrected milli-Celsius and fixed untrimmed `3000 mC` uncertainty cross the wire.
- Action resets only the executable's counter baseline/window. It does not change the interface,
  driver, route, thermal hardware, Session, kernel counters, or persistent state.
- Plain TCP and fixed public lab PeerIds provide exact Profile/role matching, not authentication,
  confidentiality or integrity. Use only on a controlled lab network.
