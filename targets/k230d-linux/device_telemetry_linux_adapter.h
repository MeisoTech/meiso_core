#ifndef MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_H
#define MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_H

#include "device_telemetry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY 192U
#define MEISO_DEVICE_TELEMETRY_LINUX_TEMPERATURE_UNCERTAINTY_MILLIC 3000U

typedef struct {
    const char *proc_stat_path;
    const char *proc_meminfo_path;
    const char *thermal_type_path;
    const char *thermal_value_path;
    MeisoDeviceTelemetryFieldMask allowed_fields;
} MeisoDeviceTelemetryLinuxAdapterConfig;

typedef struct {
    bool present;
    int32_t temperature_millic;
    uint32_t uncertainty_millic;
} MeisoDeviceTelemetryLinuxTemperature;

typedef struct {
    char proc_stat_path[MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY];
    char proc_meminfo_path[MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY];
    char thermal_type_path[MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY];
    char thermal_value_path[MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY];
    MeisoDeviceTelemetryFieldMask allowed_fields;
    MeisoDeviceTelemetryFieldMask supported_fields;
    uint64_t previous_cpu_total;
    uint64_t previous_cpu_idle;
    bool cpu_baseline_valid;
    bool probed;
    uint32_t magic;
} MeisoDeviceTelemetryLinuxAdapter;

typedef uint8_t MeisoDeviceTelemetryLinuxAdapterStatus;
enum {
    MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK = 0,
    MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNAVAILABLE = 1,
    MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT = 2,
    MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_CONFIG = 3,
    MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_NOT_PROBED = 4,
    MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNSUPPORTED = 5
};

/*
 * Populates the normal K230D Linux procfs/sysfs locators. They are private
 * platform locators: neither this adapter nor the DeviceTelemetry Profile
 * serializes or logs them. Tests and alternate Linux layouts may supply
 * different absolute regular-file paths through init. These are trusted
 * local procfs/sysfs-like sources; FIFOs and sockets are rejected so the
 * synchronous CPU/memory reads cannot be redirected to an unbounded source.
 */
void meiso_device_telemetry_linux_adapter_default_config(
    MeisoDeviceTelemetryFieldMask allowed_fields,
    MeisoDeviceTelemetryLinuxAdapterConfig *out_config
);

/*
 * Destination adapter storage is disjoint from config and every borrowed path
 * range. Every path must contain a NUL within PATH_CAPACITY. Failure leaves
 * adapter byte-exact unchanged. No heap allocation occurs.
 */
MeisoDeviceTelemetryLinuxAdapterStatus
meiso_device_telemetry_linux_adapter_init(
    MeisoDeviceTelemetryLinuxAdapter *adapter,
    const MeisoDeviceTelemetryLinuxAdapterConfig *config
);

/*
 * Probe intersects policy with observable Linux sources and returns the exact
 * capability to place in the catalog. It reads only the thermal type, never
 * the potentially slow thermal value. A successful re-probe resets the CPU
 * delta baseline. Failure leaves adapter and output unchanged.
 */
MeisoDeviceTelemetryLinuxAdapterStatus
meiso_device_telemetry_linux_adapter_probe(
    MeisoDeviceTelemetryLinuxAdapter *adapter,
    MeisoDeviceTelemetryCapability *out_capability
);

/*
 * Clears the CPU delta baseline at endpoint disable, cadence suspension or
 * any producer discontinuity. The first later CPU sample only establishes a
 * new baseline. A caller that stops invoking sample() must call this once.
 */
MeisoDeviceTelemetryLinuxAdapterStatus
meiso_device_telemetry_linux_adapter_reset_sampling(
    MeisoDeviceTelemetryLinuxAdapter *adapter
);

/*
 * Direct thermal access is intentionally separate from sample(). On K230D it
 * must run in a caller-owned bounded worker, never synchronously inside the
 * Session/Runtime/media event loop. A readable but currently invalid raw code
 * returns OK with present=false; I/O failure returns UNAVAILABLE and leaves
 * output unchanged.
 */
MeisoDeviceTelemetryLinuxAdapterStatus
meiso_device_telemetry_linux_adapter_read_temperature_direct(
    const MeisoDeviceTelemetryLinuxAdapter *adapter,
    MeisoDeviceTelemetryLinuxTemperature *out_temperature
);

/*
 * Reads CPU and memory synchronously from procfs and merges only a fresh result
 * obtained by the bounded thermal worker for this exact sample period. NULL or
 * present=false means no fresh temperature result. The first valid CPU read
 * establishes a baseline and therefore has no CPU present bit; later reads
 * report the aggregate busy delta in parts per million. Individual unavailable
 * selected fields remain canonical zero. Failure leaves output unchanged.
 */
MeisoDeviceTelemetryLinuxAdapterStatus
meiso_device_telemetry_linux_adapter_sample(
    MeisoDeviceTelemetryLinuxAdapter *adapter,
    MeisoDeviceTelemetryFieldMask selected_fields,
    const MeisoDeviceTelemetryLinuxTemperature *bounded_temperature,
    MeisoDeviceTelemetrySample *out_sample
);

#ifdef __cplusplus
}
#endif

#endif
