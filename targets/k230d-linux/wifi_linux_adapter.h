#ifndef MEISO_WIFI_LINUX_ADAPTER_H
#define MEISO_WIFI_LINUX_ADAPTER_H

#include "wifi_telemetry.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY 16U

typedef struct {
    char interface_name[MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY];
    MeisoWifiTelemetryFieldMask allowed_fields;
} MeisoWifiLinuxAdapterConfig;

typedef struct {
    char interface_name[MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY];
    MeisoWifiTelemetryFieldMask allowed_fields;
    uint32_t magic;
} MeisoWifiLinuxAdapter;

typedef uint8_t MeisoWifiLinuxAdapterStatus;
enum {
    MEISO_WIFI_LINUX_ADAPTER_OK = 0,
    MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE = 1,
    MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT = 2,
    MEISO_WIFI_LINUX_ADAPTER_INVALID_CONFIG = 3
};

/*
 * The interface name is a private Edge locator. It is copied into the
 * adapter, is never placed in a Profile payload, and must not be logged.
 */
MeisoWifiLinuxAdapterStatus meiso_wifi_linux_adapter_init(
    MeisoWifiLinuxAdapter *adapter,
    const MeisoWifiLinuxAdapterConfig *config
);

/*
 * First requires the controlled Linux sysfs `wireless` directory, then probes
 * only the configured interface's link/carrier, signal and allowlisted
 * counter files. A field enters the catalog only when both config and this
 * probe support it. No identity, address or association metadata is read.
 */
MeisoWifiLinuxAdapterStatus meiso_wifi_linux_adapter_probe(
    const MeisoWifiLinuxAdapter *adapter,
    MeisoWifiTelemetryFieldMask *out_supported_fields
);

/*
 * Reads each selected field independently. Missing fields remain canonical
 * zero with their present bit clear. UNAVAILABLE means the configured source
 * itself could not be observed; it does not return a materialized sample.
 */
MeisoWifiLinuxAdapterStatus meiso_wifi_linux_adapter_sample(
    const MeisoWifiLinuxAdapter *adapter,
    MeisoWifiTelemetryFieldMask selected_fields,
    MeisoWifiTelemetrySample *out_sample
);

#ifdef __cplusplus
}
#endif

#endif
