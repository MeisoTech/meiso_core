#define _POSIX_C_SOURCE 200809L

#include "device_telemetry_linux_adapter.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    DEVICE_TELEMETRY_LINUX_MAGIC = 0x44544c31U,
    DEVICE_TELEMETRY_LINUX_LINE_CAPACITY = 256,
    DEVICE_TELEMETRY_LINUX_MEMINFO_MAX_ROWS = 256,
    DEVICE_TELEMETRY_LINUX_MEMINFO_MAX_BYTES = 32768,
    DEVICE_TELEMETRY_LINUX_MIN_TEMPERATURE_MILLIC = -40000,
    DEVICE_TELEMETRY_LINUX_MAX_TEMPERATURE_MILLIC = 125000,
    DEVICE_TELEMETRY_LINUX_CPU_MAX_PPM = 1000000
};

static const char DEFAULT_PROC_STAT_PATH[] = "/proc/stat";
static const char DEFAULT_PROC_MEMINFO_PATH[] = "/proc/meminfo";
static const char DEFAULT_THERMAL_TYPE_PATH[] =
    "/sys/class/thermal/thermal_zone0/type";
static const char DEFAULT_THERMAL_VALUE_PATH[] =
    "/sys/class/thermal/thermal_zone0/temp";
static const char K230_THERMAL_TYPE[] = "canaan_thermal_zone";

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    const uintptr_t lhs_begin = (uintptr_t)lhs;
    const uintptr_t rhs_begin = (uintptr_t)rhs;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_begin > UINTPTR_MAX - lhs_size ||
        rhs_begin > UINTPTR_MAX - rhs_size) {
        return true;
    }
    return lhs_begin < rhs_begin + rhs_size &&
        rhs_begin < lhs_begin + lhs_size;
}

static bool mask_valid(MeisoDeviceTelemetryFieldMask value)
{
    return value != 0U &&
        (value & (MeisoDeviceTelemetryFieldMask)
            ~MEISO_DEVICE_TELEMETRY_FIELD_ALL) == 0U;
}

static bool path_length(const char *value, size_t *out_length)
{
    const char *end;

    if (value == NULL || out_length == NULL) {
        return false;
    }
    end = (const char *)memchr(
        value, '\0', MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY
    );
    if (end == NULL || end <= value + 1 || value[0] != '/') {
        return false;
    }
    *out_length = (size_t)(end - value);
    return true;
}

static bool path_valid(const char *value)
{
    size_t length;
    return path_length(value, &length);
}

static bool copy_path(
    char output[MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY],
    const char *input
)
{
    size_t length;

    if (!path_length(input, &length)) {
        return false;
    }
    memcpy(output, input, length + 1U);
    return true;
}

static bool regular_source(const char *path)
{
    struct stat status;
    return path_valid(path) && stat(path, &status) == 0 &&
        S_ISREG(status.st_mode);
}

static bool adapter_valid(const MeisoDeviceTelemetryLinuxAdapter *adapter)
{
    return adapter != NULL &&
        adapter->magic == DEVICE_TELEMETRY_LINUX_MAGIC &&
        mask_valid(adapter->allowed_fields) &&
        path_valid(adapter->proc_stat_path) &&
        path_valid(adapter->proc_meminfo_path) &&
        path_valid(adapter->thermal_type_path) &&
        path_valid(adapter->thermal_value_path);
}

static bool read_text_line(
    const char *path,
    char *output,
    size_t output_capacity
)
{
    FILE *file;
    size_t length;

    if (!path_valid(path) || output == NULL || output_capacity < 2U ||
        output_capacity > (size_t)INT_MAX) {
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    if (fgets(output, (int)output_capacity, file) == NULL) {
        (void)fclose(file);
        return false;
    }
    length = strlen(output);
    if (length == output_capacity - 1U && output[length - 1U] != '\n' &&
        !feof(file)) {
        (void)fclose(file);
        return false;
    }
    if (fclose(file) != 0) {
        return false;
    }
    while (length > 0U &&
           (output[length - 1U] == '\n' || output[length - 1U] == '\r')) {
        length -= 1U;
        output[length] = '\0';
    }
    return true;
}

static bool parse_u64(const char *text, uint64_t *out_value)
{
    char *end = NULL;
    uintmax_t parsed;

    if (text == NULL || out_value == NULL || text[0] == '\0' ||
        text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
#if UINTMAX_MAX > UINT64_MAX
    if (parsed > UINT64_MAX) {
        return false;
    }
#endif
    *out_value = (uint64_t)parsed;
    return true;
}

static bool add_u64(uint64_t lhs, uint64_t rhs, uint64_t *out_value)
{
    if (out_value == NULL || lhs > UINT64_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
    return true;
}

static bool parse_decimal_token(const char **in_out_cursor, uint64_t *out_value)
{
    const char *cursor;
    uint64_t value = 0U;
    bool digit = false;

    if (in_out_cursor == NULL || *in_out_cursor == NULL || out_value == NULL) {
        return false;
    }
    cursor = *in_out_cursor;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor += 1;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        const uint64_t next = (uint64_t)(unsigned int)(*cursor - '0');
        if (value > (UINT64_MAX - next) / UINT64_C(10)) {
            return false;
        }
        value = value * UINT64_C(10) + next;
        cursor += 1;
        digit = true;
    }
    if (!digit || (*cursor != '\0' && *cursor != ' ' && *cursor != '\t')) {
        return false;
    }
    *in_out_cursor = cursor;
    *out_value = value;
    return true;
}

static bool read_cpu_counters(
    const MeisoDeviceTelemetryLinuxAdapter *adapter,
    uint64_t *out_total,
    uint64_t *out_idle
)
{
    char line[DEVICE_TELEMETRY_LINUX_LINE_CAPACITY];
    uint64_t fields[10];
    size_t count = 0U;
    size_t index;
    const char *cursor;
    uint64_t total = 0U;
    uint64_t idle = 0U;

    if (out_total == NULL || out_idle == NULL ||
        !regular_source(adapter->proc_stat_path) ||
        !read_text_line(adapter->proc_stat_path, line, sizeof(line)) ||
        line[0] != 'c' || line[1] != 'p' || line[2] != 'u' ||
        (line[3] != ' ' && line[3] != '\t')) {
        return false;
    }
    memset(fields, 0, sizeof(fields));
    cursor = line + 3U;
    while (*cursor != '\0') {
        if (count >= sizeof(fields) / sizeof(fields[0]) ||
            !parse_decimal_token(&cursor, &fields[count])) {
            return false;
        }
        count += 1U;
        while (*cursor == ' ' || *cursor == '\t') {
            cursor += 1;
        }
    }
    if (count < 4U) {
        return false;
    }
    for (index = 0U; index < count && index < 8U; ++index) {
        if (!add_u64(total, fields[index], &total)) {
            return false;
        }
    }
    idle = fields[3];
    if (count >= 5U && !add_u64(idle, fields[4], &idle)) {
        return false;
    }
    if (total == 0U || idle > total) {
        return false;
    }
    *out_total = total;
    *out_idle = idle;
    return true;
}

typedef struct {
    bool total_present;
    bool available_present;
    uint64_t total_bytes;
    uint64_t available_bytes;
} MemoryReading;

static bool parse_meminfo_row(
    const char *line,
    const char *name,
    uint64_t *out_bytes
)
{
    const size_t name_length = name == NULL ? 0U : strlen(name);
    const char *cursor;
    uint64_t kib;

    if (line == NULL || name == NULL || out_bytes == NULL ||
        name_length == 0U || strncmp(line, name, name_length) != 0 ||
        line[name_length] != ':') {
        return false;
    }
    cursor = line + name_length + 1U;
    if (!parse_decimal_token(&cursor, &kib)) {
        return false;
    }
    while (*cursor == ' ' || *cursor == '\t') {
        cursor += 1;
    }
    if (cursor[0] != 'k' || cursor[1] != 'B') {
        return false;
    }
    cursor += 2;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor += 1;
    }
    if (*cursor != '\0' ||
        kib > UINT64_MAX / UINT64_C(1024)) {
        return false;
    }
    *out_bytes = kib * UINT64_C(1024);
    return true;
}

static bool read_memory(
    const MeisoDeviceTelemetryLinuxAdapter *adapter,
    MemoryReading *out_reading
)
{
    FILE *file;
    char line[DEVICE_TELEMETRY_LINUX_LINE_CAPACITY];
    MemoryReading reading;
    size_t row_count = 0U;
    size_t total_bytes = 0U;

    if (out_reading == NULL) {
        return false;
    }
    memset(&reading, 0, sizeof(reading));
    if (!regular_source(adapter->proc_meminfo_path)) {
        return false;
    }
    file = fopen(adapter->proc_meminfo_path, "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, (int)sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        uint64_t bytes;

        if (row_count >= DEVICE_TELEMETRY_LINUX_MEMINFO_MAX_ROWS ||
            length > DEVICE_TELEMETRY_LINUX_MEMINFO_MAX_BYTES - total_bytes) {
            (void)fclose(file);
            return false;
        }
        row_count += 1U;
        total_bytes += length;
        if (length == sizeof(line) - 1U && line[length - 1U] != '\n' &&
            !feof(file)) {
            (void)fclose(file);
            return false;
        }
        while (length > 0U &&
               (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
            length -= 1U;
            line[length] = '\0';
        }
        if (strncmp(line, "MemTotal:", 9U) == 0) {
            if (!parse_meminfo_row(line, "MemTotal", &bytes)) {
                (void)fclose(file);
                return false;
            }
            if (reading.total_present) {
                (void)fclose(file);
                return false;
            }
            reading.total_present = true;
            reading.total_bytes = bytes;
        } else if (strncmp(line, "MemAvailable:", 13U) == 0) {
            if (!parse_meminfo_row(line, "MemAvailable", &bytes)) {
                (void)fclose(file);
                return false;
            }
            if (reading.available_present) {
                (void)fclose(file);
                return false;
            }
            reading.available_present = true;
            reading.available_bytes = bytes;
        }
    }
    if (ferror(file)) {
        (void)fclose(file);
        return false;
    }
    if (fclose(file) != 0 ||
        (reading.total_present && reading.available_present &&
         reading.available_bytes > reading.total_bytes)) {
        return false;
    }
    *out_reading = reading;
    return reading.total_present || reading.available_present;
}

static bool thermal_source_observable(
    const MeisoDeviceTelemetryLinuxAdapter *adapter
)
{
    char type[64];

    return regular_source(adapter->thermal_type_path) &&
        regular_source(adapter->thermal_value_path) &&
        access(adapter->thermal_value_path, R_OK) == 0 &&
        read_text_line(adapter->thermal_type_path, type, sizeof(type)) &&
        strcmp(type, K230_THERMAL_TYPE) == 0;
}

static bool temperature_from_raw(
    uint64_t raw,
    MeisoDeviceTelemetryLinuxTemperature *out_temperature
)
{
    uint64_t code;
    double x;
    double converted_c;
    double converted_millic;
    MeisoDeviceTelemetryLinuxTemperature result;

    memset(&result, 0, sizeof(result));
    if ((raw >> 12U) == 0U) {
        *out_temperature = result;
        return true;
    }
    code = raw & UINT64_C(0x0fff);
    if (code < UINT64_C(2822) || code > UINT64_C(3350)) {
        *out_temperature = result;
        return true;
    }
    x = (double)code;
    converted_c = ((((1.01472e-10 * x - 1.10063e-6) * x + 4.36150e-3) *
        x - 7.10128) * x + 3565.87);
    if (converted_c < -40.0 || converted_c > 125.0) {
        *out_temperature = result;
        return true;
    }
    converted_millic = converted_c * 1000.0;
    result.present = true;
    result.temperature_millic = converted_millic >= 0.0 ?
        (int32_t)(converted_millic + 0.5) :
        (int32_t)(converted_millic - 0.5);
    result.uncertainty_millic =
        MEISO_DEVICE_TELEMETRY_LINUX_TEMPERATURE_UNCERTAINTY_MILLIC;
    *out_temperature = result;
    return true;
}

static bool cpu_load_ppm(
    uint64_t total_delta,
    uint64_t idle_delta,
    uint32_t *out_ppm
)
{
    uint64_t busy;
    uint64_t numerator;

    if (out_ppm == NULL || total_delta == 0U || idle_delta > total_delta) {
        return false;
    }
    busy = total_delta - idle_delta;
    if (busy > UINT64_MAX /
            (uint64_t)DEVICE_TELEMETRY_LINUX_CPU_MAX_PPM) {
        return false;
    }
    numerator = busy * (uint64_t)DEVICE_TELEMETRY_LINUX_CPU_MAX_PPM;
    if (numerator > UINT64_MAX - total_delta / 2U) {
        return false;
    }
    *out_ppm = (uint32_t)((numerator + total_delta / 2U) / total_delta);
    return true;
}

void meiso_device_telemetry_linux_adapter_default_config(
    MeisoDeviceTelemetryFieldMask allowed_fields,
    MeisoDeviceTelemetryLinuxAdapterConfig *out_config
)
{
    if (out_config == NULL) {
        return;
    }
    out_config->proc_stat_path = DEFAULT_PROC_STAT_PATH;
    out_config->proc_meminfo_path = DEFAULT_PROC_MEMINFO_PATH;
    out_config->thermal_type_path = DEFAULT_THERMAL_TYPE_PATH;
    out_config->thermal_value_path = DEFAULT_THERMAL_VALUE_PATH;
    out_config->allowed_fields = allowed_fields;
}

MeisoDeviceTelemetryLinuxAdapterStatus
meiso_device_telemetry_linux_adapter_init(
    MeisoDeviceTelemetryLinuxAdapter *adapter,
    const MeisoDeviceTelemetryLinuxAdapterConfig *config
)
{
    MeisoDeviceTelemetryLinuxAdapter candidate;
    size_t stat_length;
    size_t meminfo_length;
    size_t thermal_type_length;
    size_t thermal_value_length;

    if (adapter == NULL || config == NULL) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    if (!mask_valid(config->allowed_fields) ||
        !path_length(config->proc_stat_path, &stat_length) ||
        !path_length(config->proc_meminfo_path, &meminfo_length) ||
        !path_length(config->thermal_type_path, &thermal_type_length) ||
        !path_length(config->thermal_value_path, &thermal_value_length)) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_CONFIG;
    }
    if (ranges_overlap(adapter, sizeof(*adapter), config, sizeof(*config)) ||
        ranges_overlap(
            adapter, sizeof(*adapter), config->proc_stat_path,
            stat_length + 1U
        ) ||
        ranges_overlap(
            adapter, sizeof(*adapter), config->proc_meminfo_path,
            meminfo_length + 1U
        ) ||
        ranges_overlap(
            adapter, sizeof(*adapter), config->thermal_type_path,
            thermal_type_length + 1U
        ) ||
        ranges_overlap(
            adapter, sizeof(*adapter), config->thermal_value_path,
            thermal_value_length + 1U
        )) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    memset(&candidate, 0, sizeof(candidate));
    if (!copy_path(candidate.proc_stat_path, config->proc_stat_path) ||
        !copy_path(candidate.proc_meminfo_path, config->proc_meminfo_path) ||
        !copy_path(candidate.thermal_type_path, config->thermal_type_path) ||
        !copy_path(candidate.thermal_value_path, config->thermal_value_path)) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_CONFIG;
    }
    candidate.allowed_fields = config->allowed_fields;
    candidate.magic = DEVICE_TELEMETRY_LINUX_MAGIC;
    *adapter = candidate;
    return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK;
}

MeisoDeviceTelemetryLinuxAdapterStatus
meiso_device_telemetry_linux_adapter_probe(
    MeisoDeviceTelemetryLinuxAdapter *adapter,
    MeisoDeviceTelemetryCapability *out_capability
)
{
    MeisoDeviceTelemetryLinuxAdapter candidate;
    MeisoDeviceTelemetryCapability capability;
    MeisoDeviceTelemetryFieldMask supported = 0U;
    uint64_t cpu_total;
    uint64_t cpu_idle;
    MemoryReading memory;

    if (!adapter_valid(adapter) || out_capability == NULL ||
        ranges_overlap(
            adapter, sizeof(*adapter), out_capability,
            sizeof(*out_capability)
        )) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    candidate = *adapter;
    memset(&capability, 0, sizeof(capability));
    if ((adapter->allowed_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) != 0U &&
        thermal_source_observable(adapter)) {
        supported |= MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC;
    }
    if ((adapter->allowed_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM) != 0U &&
        read_cpu_counters(adapter, &cpu_total, &cpu_idle)) {
        supported |= MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM;
    }
    memset(&memory, 0, sizeof(memory));
    if ((adapter->allowed_fields &
         (MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES |
          MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES)) != 0U &&
        read_memory(adapter, &memory)) {
        if (memory.total_present &&
            (adapter->allowed_fields &
             MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES) != 0U) {
            supported |= MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES;
        }
        if (memory.available_present &&
            (adapter->allowed_fields &
             MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES) != 0U) {
            supported |=
                MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES;
        }
    }
    if (supported == 0U) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNAVAILABLE;
    }
    capability.supported_fields = supported;
    capability.min_period_ms = MEISO_DEVICE_TELEMETRY_MIN_PERIOD_MS;
    capability.max_period_ms = MEISO_DEVICE_TELEMETRY_MAX_PERIOD_MS;
    capability.period_step_ms = MEISO_DEVICE_TELEMETRY_PERIOD_STEP_MS;
    if ((supported &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) != 0U) {
        capability.min_temperature_millic =
            DEVICE_TELEMETRY_LINUX_MIN_TEMPERATURE_MILLIC;
        capability.max_temperature_millic =
            DEVICE_TELEMETRY_LINUX_MAX_TEMPERATURE_MILLIC;
        capability.max_temperature_uncertainty_millic =
            MEISO_DEVICE_TELEMETRY_LINUX_TEMPERATURE_UNCERTAINTY_MILLIC;
    }
    candidate.supported_fields = supported;
    candidate.previous_cpu_total = 0U;
    candidate.previous_cpu_idle = 0U;
    candidate.cpu_baseline_valid = false;
    candidate.probed = true;
    *adapter = candidate;
    *out_capability = capability;
    return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK;
}

MeisoDeviceTelemetryLinuxAdapterStatus
meiso_device_telemetry_linux_adapter_reset_sampling(
    MeisoDeviceTelemetryLinuxAdapter *adapter
)
{
    MeisoDeviceTelemetryLinuxAdapter candidate;

    if (!adapter_valid(adapter)) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    candidate = *adapter;
    candidate.previous_cpu_total = 0U;
    candidate.previous_cpu_idle = 0U;
    candidate.cpu_baseline_valid = false;
    *adapter = candidate;
    return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK;
}

MeisoDeviceTelemetryLinuxAdapterStatus
meiso_device_telemetry_linux_adapter_read_temperature_direct(
    const MeisoDeviceTelemetryLinuxAdapter *adapter,
    MeisoDeviceTelemetryLinuxTemperature *out_temperature
)
{
    char text[64];
    uint64_t raw;
    MeisoDeviceTelemetryLinuxTemperature temperature;

    if (!adapter_valid(adapter) || out_temperature == NULL ||
        ranges_overlap(
            adapter, sizeof(*adapter), out_temperature,
            sizeof(*out_temperature)
        )) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    if (!adapter->probed) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_NOT_PROBED;
    }
    if ((adapter->supported_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) == 0U) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNSUPPORTED;
    }
    if (!thermal_source_observable(adapter) ||
        !read_text_line(adapter->thermal_value_path, text, sizeof(text)) ||
        !parse_u64(text, &raw) || !temperature_from_raw(raw, &temperature)) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNAVAILABLE;
    }
    *out_temperature = temperature;
    return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK;
}

MeisoDeviceTelemetryLinuxAdapterStatus
meiso_device_telemetry_linux_adapter_sample(
    MeisoDeviceTelemetryLinuxAdapter *adapter,
    MeisoDeviceTelemetryFieldMask selected_fields,
    const MeisoDeviceTelemetryLinuxTemperature *bounded_temperature,
    MeisoDeviceTelemetrySample *out_sample
)
{
    MeisoDeviceTelemetryLinuxAdapter candidate;
    MeisoDeviceTelemetrySample sample;
    uint64_t cpu_total;
    uint64_t cpu_idle;
    MemoryReading memory;

    if (!adapter_valid(adapter) || out_sample == NULL ||
        ranges_overlap(adapter, sizeof(*adapter), out_sample, sizeof(*out_sample)) ||
        (bounded_temperature != NULL &&
         (ranges_overlap(
              adapter, sizeof(*adapter), bounded_temperature,
              sizeof(*bounded_temperature)
          ) ||
          ranges_overlap(
              bounded_temperature, sizeof(*bounded_temperature), out_sample,
              sizeof(*out_sample)
          )))) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    if (!adapter->probed) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_NOT_PROBED;
    }
    if (!mask_valid(selected_fields) ||
        (selected_fields & ~adapter->supported_fields) != 0U) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNSUPPORTED;
    }
    if (bounded_temperature != NULL && bounded_temperature->present &&
        (bounded_temperature->temperature_millic <
             DEVICE_TELEMETRY_LINUX_MIN_TEMPERATURE_MILLIC ||
         bounded_temperature->temperature_millic >
             DEVICE_TELEMETRY_LINUX_MAX_TEMPERATURE_MILLIC ||
         bounded_temperature->uncertainty_millic !=
             MEISO_DEVICE_TELEMETRY_LINUX_TEMPERATURE_UNCERTAINTY_MILLIC)) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    if (bounded_temperature != NULL && !bounded_temperature->present &&
        (bounded_temperature->temperature_millic != 0 ||
         bounded_temperature->uncertainty_millic != 0U)) {
        return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT;
    }

    candidate = *adapter;
    memset(&sample, 0, sizeof(sample));
    sample.selected_fields = selected_fields;

    if ((selected_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) != 0U &&
        bounded_temperature != NULL && bounded_temperature->present) {
        sample.present_fields |=
            MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC;
        sample.temperature_millic = bounded_temperature->temperature_millic;
        sample.temperature_uncertainty_millic =
            bounded_temperature->uncertainty_millic;
    }

    if ((selected_fields & MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM) == 0U) {
        candidate.previous_cpu_total = 0U;
        candidate.previous_cpu_idle = 0U;
        candidate.cpu_baseline_valid = false;
    } else if (!read_cpu_counters(adapter, &cpu_total, &cpu_idle)) {
        candidate.previous_cpu_total = 0U;
        candidate.previous_cpu_idle = 0U;
        candidate.cpu_baseline_valid = false;
    } else if (!candidate.cpu_baseline_valid) {
        candidate.previous_cpu_total = cpu_total;
        candidate.previous_cpu_idle = cpu_idle;
        candidate.cpu_baseline_valid = true;
    } else if (cpu_total < candidate.previous_cpu_total ||
               cpu_idle < candidate.previous_cpu_idle) {
        candidate.previous_cpu_total = 0U;
        candidate.previous_cpu_idle = 0U;
        candidate.cpu_baseline_valid = false;
    } else {
        const uint64_t total_delta = cpu_total - candidate.previous_cpu_total;
        const uint64_t idle_delta = cpu_idle - candidate.previous_cpu_idle;
        uint32_t ppm;

        if (cpu_load_ppm(total_delta, idle_delta, &ppm)) {
            sample.present_fields |=
                MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM;
            sample.cpu_load_ppm = ppm;
            candidate.previous_cpu_total = cpu_total;
            candidate.previous_cpu_idle = cpu_idle;
        } else {
            candidate.previous_cpu_total = 0U;
            candidate.previous_cpu_idle = 0U;
            candidate.cpu_baseline_valid = false;
        }
    }

    memset(&memory, 0, sizeof(memory));
    if ((selected_fields &
         (MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES |
          MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES)) != 0U &&
        read_memory(adapter, &memory)) {
        if ((selected_fields &
             MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES) != 0U &&
            memory.total_present) {
            sample.present_fields |=
                MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES;
            sample.memory_total_bytes = memory.total_bytes;
        }
        if ((selected_fields &
             MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES) != 0U &&
            memory.available_present) {
            sample.present_fields |=
                MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES;
            sample.memory_available_bytes = memory.available_bytes;
        }
    }

    *adapter = candidate;
    *out_sample = sample;
    return MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK;
}
