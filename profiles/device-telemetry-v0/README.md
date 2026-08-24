# Device telemetry Profile v0

`DEVICE_T/LM_PROF0:v1` declares one Edge device-health endpoint. Host desired
State selects real fields and cadence; Edge actual State reports the effective
selection; the Edge Stream carries bounded samples. It never carries network
locators, credentials, serials, MACs, filesystem paths, or private driver
identity.

The endpoint is `DEVICE_T/LM_ENDP0`. Capability schema is
`DEVICE_S/CAP_V000:v1`. Desired, actual, and data resources are respectively
`DEVICE_T/DES_STAT`, `DEVICE_T/ACT_STAT`, and `DEVICE_T/DATA_STR`; their schema
low words are `DES_V000`, `ACT_V000`, and `SMP_V000` under `DEVICE_S`.

All integers are big-endian and every reserved byte is zero:

- capability (48 bytes): `DTCP`, version, supported mask, min/max/step ms,
  signed temperature range in milli-Celsius, maximum temperature uncertainty,
  fixed CPU maximum `1,000,000 ppm`, reserved 8 bytes;
- desired/effective suffix (8 bytes): selected mask and period ms;
- sample (48 bytes): `DTS0`, version, selected/present masks, signed
  milli-Celsius, uncertainty, CPU ppm, reserved 4 bytes, memory total and
  available bytes.

Fields without a present bit have canonical zero values. Memory available may
not exceed total when both are present. Capability is truthful adapter probe +
policy; unsupported or unavailable fields are never synthesized.
