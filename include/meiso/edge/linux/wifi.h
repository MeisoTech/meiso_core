#ifndef MEISO_EDGE_LINUX_WIFI_H
#define MEISO_EDGE_LINUX_WIFI_H

#include <meiso/profiles/wifi_telemetry.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY 16U
#define MEISO_WIFI_LINUX_LOCATOR_CAPACITY 192U

typedef struct {
    char interface_name[MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY];
    MeisoWifiTelemetryFieldMask allowed_fields;
    /* NULL selects /sys/class/net and /proc/net/wireless respectively. */
    const char *sys_class_net_root;
    const char *proc_net_wireless_path;
} MeisoWifiLinuxAdapterConfig;

typedef struct {
    char interface_name[MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY];
    char sys_class_net_root[MEISO_WIFI_LINUX_LOCATOR_CAPACITY];
    char proc_net_wireless_path[MEISO_WIFI_LINUX_LOCATOR_CAPACITY];
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
 * The interface name and optional Linux roots are private Edge locators. init
 * copies them into the adapter; they are never placed in a Profile payload and
 * must not be logged. Each non-NULL locator must be an absolute path with a
 * NUL inside LOCATOR_CAPACITY. Adapter storage must be disjoint from config
 * and both actual locator string ranges. Every failure leaves adapter bytes
 * unchanged. Locators are trusted procfs/sysfs-like regular-file roots; FIFO,
 * socket and device leaves are rejected before open. No heap allocation
 * occurs.
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
 * The wireless table scan is capped at 256 complete lines and 32768 bytes.
 * Adapter/output must be disjoint; every failure leaves output unchanged.
 */
MeisoWifiLinuxAdapterStatus meiso_wifi_linux_adapter_probe(
    const MeisoWifiLinuxAdapter *adapter,
    MeisoWifiTelemetryFieldMask *out_supported_fields
);

/*
 * Reads each selected field independently. Missing fields remain canonical
 * zero with their present bit clear. UNAVAILABLE means the configured source
 * itself could not be observed; it does not return a materialized sample.
 * Adapter/output must be disjoint; every failure leaves output unchanged.
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
