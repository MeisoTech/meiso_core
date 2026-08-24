#define _POSIX_C_SOURCE 200809L

#include "wifi_linux_adapter.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
    WIFI_LINUX_ADAPTER_MAGIC = 0x574c4131U,
    WIFI_LINUX_PATH_CAPACITY = 256,
    WIFI_LINUX_LINE_CAPACITY = 256,
    WIFI_LINUX_WIRELESS_MAX_ROWS = 256,
    WIFI_LINUX_WIRELESS_MAX_BYTES = 32768
};

static const char DEFAULT_SYS_CLASS_NET_ROOT[] = "/sys/class/net";
static const char DEFAULT_PROC_NET_WIRELESS_PATH[] = "/proc/net/wireless";

typedef struct {
    MeisoWifiTelemetryFieldMask field;
    const char *file_name;
    size_t value_offset;
} CounterField;

static const CounterField COUNTER_FIELDS[] = {
    {
        MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES,
        "rx_bytes",
        offsetof(MeisoWifiTelemetrySample, rx_bytes)
    },
    {
        MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES,
        "tx_bytes",
        offsetof(MeisoWifiTelemetrySample, tx_bytes)
    },
    {
        MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS,
        "rx_packets",
        offsetof(MeisoWifiTelemetrySample, rx_packets)
    },
    {
        MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS,
        "tx_packets",
        offsetof(MeisoWifiTelemetrySample, tx_packets)
    },
    {
        MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED,
        "rx_dropped",
        offsetof(MeisoWifiTelemetrySample, rx_dropped)
    },
    {
        MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED,
        "tx_dropped",
        offsetof(MeisoWifiTelemetrySample, tx_dropped)
    },
    {
        MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS,
        "rx_errors",
        offsetof(MeisoWifiTelemetrySample, rx_errors)
    },
    {
        MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS,
        "tx_errors",
        offsetof(MeisoWifiTelemetrySample, tx_errors)
    }
};

static bool interface_name_length(const char *value, size_t *out_length)
{
    const char *end;
    size_t length;
    size_t index;

    if (value == NULL || out_length == NULL) {
        return false;
    }
    end = memchr(value, '\0', MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY);
    if (end == NULL) {
        return false;
    }
    length = (size_t)(end - value);
    if (length == 0U ||
        length >= MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY ||
        strcmp(value, ".") == 0 || strcmp(value, "..") == 0) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        const unsigned char character = (unsigned char)value[index];
        if (!isalnum(character) && character != '_' && character != '-' &&
            character != '.') {
            return false;
        }
    }
    *out_length = length;
    return true;
}

static bool interface_name_valid(const char *value)
{
    size_t length;
    return interface_name_length(value, &length);
}

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

static bool locator_length(const char *value, size_t *out_length)
{
    const char *end;

    if (value == NULL || out_length == NULL) {
        return false;
    }
    end = memchr(value, '\0', MEISO_WIFI_LINUX_LOCATOR_CAPACITY);
    if (end == NULL || end <= value + 1 || value[0] != '/') {
        return false;
    }
    *out_length = (size_t)(end - value);
    return true;
}

static bool locator_valid(const char *value)
{
    size_t length;
    return locator_length(value, &length);
}

static bool copy_locator(
    char output[MEISO_WIFI_LINUX_LOCATOR_CAPACITY],
    const char *input
)
{
    size_t length;

    if (!locator_length(input, &length)) {
        return false;
    }
    memcpy(output, input, length + 1U);
    return true;
}

static bool field_mask_valid(MeisoWifiTelemetryFieldMask value)
{
    return value != 0U &&
        (value & (MeisoWifiTelemetryFieldMask)
            ~MEISO_WIFI_TELEMETRY_FIELD_ALL) == 0U;
}

static bool adapter_valid(const MeisoWifiLinuxAdapter *adapter)
{
    return adapter != NULL && adapter->magic == WIFI_LINUX_ADAPTER_MAGIC &&
        interface_name_valid(adapter->interface_name) &&
        field_mask_valid(adapter->allowed_fields) &&
        locator_valid(adapter->sys_class_net_root) &&
        locator_valid(adapter->proc_net_wireless_path);
}

static bool regular_source(const char *path)
{
    struct stat status;

    return path != NULL && path[0] == '/' &&
        memchr(path, '\0', WIFI_LINUX_PATH_CAPACITY) != NULL &&
        stat(path, &status) == 0 &&
        S_ISREG(status.st_mode);
}

static bool read_text_line(
    const char *path,
    char *output,
    size_t output_capacity
)
{
    FILE *file;
    size_t length;

    if (path == NULL || output == NULL || output_capacity < 2U ||
        output_capacity > (size_t)INT_MAX) {
        return false;
    }
    if (!regular_source(path)) {
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
        output[length - 1U] = '\0';
        length -= 1U;
    }
    return true;
}

static bool parse_u64(const char *text, uint64_t *out_value)
{
    char *end = NULL;
    uintmax_t value;

    if (text == NULL || out_value == NULL || text[0] == '\0' ||
        text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
#if UINTMAX_MAX > UINT64_MAX
    if (value > UINT64_MAX) {
        return false;
    }
#endif
    *out_value = (uint64_t)value;
    return true;
}

static bool make_link_path(
    const MeisoWifiLinuxAdapter *adapter,
    const char *leaf,
    char output[WIFI_LINUX_PATH_CAPACITY]
)
{
    const int count = snprintf(
        output,
        WIFI_LINUX_PATH_CAPACITY,
        "%s/%s/%s",
        adapter->sys_class_net_root,
        adapter->interface_name,
        leaf
    );
    return count > 0 && count < WIFI_LINUX_PATH_CAPACITY;
}

static bool make_counter_path(
    const MeisoWifiLinuxAdapter *adapter,
    const char *file_name,
    char output[WIFI_LINUX_PATH_CAPACITY]
)
{
    const int count = snprintf(
        output,
        WIFI_LINUX_PATH_CAPACITY,
        "%s/%s/statistics/%s",
        adapter->sys_class_net_root,
        adapter->interface_name,
        file_name
    );
    return count > 0 && count < WIFI_LINUX_PATH_CAPACITY;
}

static bool read_link_leaf(
    const MeisoWifiLinuxAdapter *adapter,
    const char *leaf,
    char *output,
    size_t output_capacity
)
{
    char path[WIFI_LINUX_PATH_CAPACITY];
    return make_link_path(adapter, leaf, path) &&
        read_text_line(path, output, output_capacity);
}

static bool configured_interface_is_wireless(
    const MeisoWifiLinuxAdapter *adapter
)
{
    char path[WIFI_LINUX_PATH_CAPACITY];
    struct stat status;

    return make_link_path(adapter, "wireless", path) &&
        stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool read_carrier(
    const MeisoWifiLinuxAdapter *adapter,
    bool *out_link_state
)
{
    char text[8];
    uint64_t value;

    if (out_link_state == NULL ||
        !read_link_leaf(adapter, "carrier", text, sizeof(text)) ||
        !parse_u64(text, &value) || value > 1U) {
        return false;
    }
    *out_link_state = value == 1U;
    return true;
}

static bool read_operstate(
    const MeisoWifiLinuxAdapter *adapter,
    bool *out_link_state
)
{
    char text[32];

    if (out_link_state == NULL ||
        !read_link_leaf(adapter, "operstate", text, sizeof(text))) {
        return false;
    }
    if (strcmp(text, "up") == 0) {
        *out_link_state = true;
        return true;
    }
    if (strcmp(text, "down") == 0 || strcmp(text, "dormant") == 0 ||
        strcmp(text, "lowerlayerdown") == 0 ||
        strcmp(text, "notpresent") == 0 || strcmp(text, "testing") == 0 ||
        strcmp(text, "unknown") == 0) {
        *out_link_state = false;
        return true;
    }
    return false;
}

static bool read_link_state(
    const MeisoWifiLinuxAdapter *adapter,
    bool *out_link_state
)
{
    return read_carrier(adapter, out_link_state) ||
        read_operstate(adapter, out_link_state);
}

static bool read_counter(
    const MeisoWifiLinuxAdapter *adapter,
    const char *file_name,
    uint64_t *out_value
)
{
    char path[WIFI_LINUX_PATH_CAPACITY];
    char text[64];

    return out_value != NULL &&
        make_counter_path(adapter, file_name, path) &&
        read_text_line(path, text, sizeof(text)) &&
        parse_u64(text, out_value);
}

static bool wireless_row_matches(
    const char *line,
    const char *interface_name,
    const char **out_values
)
{
    const char *start = line;
    const char *colon;
    const char *end;
    size_t interface_length;

    if (line == NULL || interface_name == NULL || out_values == NULL) {
        return false;
    }
    while (*start == ' ' || *start == '\t') {
        start += 1;
    }
    colon = strchr(start, ':');
    if (colon == NULL) {
        return false;
    }
    end = colon;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end -= 1;
    }
    interface_length = strlen(interface_name);
    if ((size_t)(end - start) != interface_length ||
        memcmp(start, interface_name, interface_length) != 0) {
        return false;
    }
    *out_values = colon + 1;
    return true;
}

static bool read_signal_mbm(
    const MeisoWifiLinuxAdapter *adapter,
    int32_t *out_signal_mbm
)
{
    FILE *file;
    char line[WIFI_LINUX_LINE_CAPACITY];
    bool found = false;
    size_t rows = 0U;
    size_t total_bytes = 0U;

    if (out_signal_mbm == NULL ||
        !regular_source(adapter->proc_net_wireless_path)) {
        return false;
    }
    file = fopen(adapter->proc_net_wireless_path, "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, (int)sizeof(line), file) != NULL) {
        const char *values;
        const size_t line_bytes = strlen(line);
        unsigned int status;
        double link;
        double level;

        if (line_bytes == sizeof(line) - 1U &&
            line[line_bytes - 1U] != '\n' && !feof(file)) {
            (void)fclose(file);
            return false;
        }
        if (rows == WIFI_LINUX_WIRELESS_MAX_ROWS ||
            line_bytes > WIFI_LINUX_WIRELESS_MAX_BYTES - total_bytes) {
            (void)fclose(file);
            return false;
        }
        ++rows;
        total_bytes += line_bytes;

        if (!wireless_row_matches(
                line, adapter->interface_name, &values
            )) {
            continue;
        }
        if (sscanf(values, " %x %lf %lf", &status, &link, &level) == 3 &&
            level >= -200.0 && level <= 0.0) {
            const double scaled = level * 100.0;
            const int32_t converted = scaled >= 0.0 ?
                (int32_t)(scaled + 0.5) : (int32_t)(scaled - 0.5);
            (void)status;
            (void)link;
            if (converted >= MEISO_WIFI_TELEMETRY_SIGNAL_MIN_MBM &&
                converted <= MEISO_WIFI_TELEMETRY_SIGNAL_MAX_MBM) {
                *out_signal_mbm = converted;
                found = true;
            }
        }
        break;
    }
    if (ferror(file)) {
        (void)fclose(file);
        return false;
    }
    if (fclose(file) != 0) {
        return false;
    }
    return found;
}

MeisoWifiLinuxAdapterStatus meiso_wifi_linux_adapter_init(
    MeisoWifiLinuxAdapter *adapter,
    const MeisoWifiLinuxAdapterConfig *config
)
{
    MeisoWifiLinuxAdapter candidate;
    const char *sys_class_net_root;
    const char *proc_net_wireless_path;
    size_t sys_class_net_root_length;
    size_t proc_net_wireless_path_length;
    size_t interface_name_length_value;

    if (adapter == NULL || config == NULL) {
        return MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    if (ranges_overlap(adapter, sizeof(*adapter), config, sizeof(*config))) {
        return MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    sys_class_net_root = config->sys_class_net_root != NULL ?
        config->sys_class_net_root : DEFAULT_SYS_CLASS_NET_ROOT;
    proc_net_wireless_path = config->proc_net_wireless_path != NULL ?
        config->proc_net_wireless_path : DEFAULT_PROC_NET_WIRELESS_PATH;
    if (!interface_name_length(
            config->interface_name, &interface_name_length_value
        ) ||
        !field_mask_valid(config->allowed_fields) ||
        !locator_length(
            sys_class_net_root, &sys_class_net_root_length
        ) || !locator_length(
            proc_net_wireless_path, &proc_net_wireless_path_length
        )) {
        return MEISO_WIFI_LINUX_ADAPTER_INVALID_CONFIG;
    }
    if (ranges_overlap(
            adapter, sizeof(*adapter), sys_class_net_root,
            sys_class_net_root_length + 1U
        ) || ranges_overlap(
            adapter, sizeof(*adapter), proc_net_wireless_path,
            proc_net_wireless_path_length + 1U
        )) {
        return MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    memset(&candidate, 0, sizeof(candidate));
    memmove(
        candidate.interface_name,
        config->interface_name,
        interface_name_length_value + 1U
    );
    if (!copy_locator(candidate.sys_class_net_root, sys_class_net_root) ||
        !copy_locator(
            candidate.proc_net_wireless_path, proc_net_wireless_path
        )) {
        return MEISO_WIFI_LINUX_ADAPTER_INVALID_CONFIG;
    }
    candidate.allowed_fields = config->allowed_fields;
    candidate.magic = WIFI_LINUX_ADAPTER_MAGIC;
    *adapter = candidate;
    return MEISO_WIFI_LINUX_ADAPTER_OK;
}

MeisoWifiLinuxAdapterStatus meiso_wifi_linux_adapter_probe(
    const MeisoWifiLinuxAdapter *adapter,
    MeisoWifiTelemetryFieldMask *out_supported_fields
)
{
    MeisoWifiTelemetryFieldMask supported = 0U;
    bool link_state;
    int32_t signal;
    size_t index;

    if (!adapter_valid(adapter) || out_supported_fields == NULL ||
        ranges_overlap(
            adapter, sizeof(*adapter), out_supported_fields,
            sizeof(*out_supported_fields)
        )) {
        return MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    if (!configured_interface_is_wireless(adapter)) {
        return MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE;
    }
    if ((adapter->allowed_fields & MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE) !=
            0U && read_link_state(adapter, &link_state)) {
        supported |= MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE;
    }
    if ((adapter->allowed_fields & MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) !=
            0U && read_signal_mbm(adapter, &signal)) {
        supported |= MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    }
    for (index = 0U;
         index < sizeof(COUNTER_FIELDS) / sizeof(COUNTER_FIELDS[0]);
         ++index) {
        uint64_t value;
        if ((adapter->allowed_fields & COUNTER_FIELDS[index].field) != 0U &&
            read_counter(adapter, COUNTER_FIELDS[index].file_name, &value)) {
            supported |= COUNTER_FIELDS[index].field;
        }
    }
    if (supported == 0U) {
        return MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE;
    }
    *out_supported_fields = supported;
    return MEISO_WIFI_LINUX_ADAPTER_OK;
}

MeisoWifiLinuxAdapterStatus meiso_wifi_linux_adapter_sample(
    const MeisoWifiLinuxAdapter *adapter,
    MeisoWifiTelemetryFieldMask selected_fields,
    MeisoWifiTelemetrySample *out_sample
)
{
    MeisoWifiTelemetrySample candidate;
    size_t index;
    bool source_observed = false;

    if (!adapter_valid(adapter) || out_sample == NULL ||
        !field_mask_valid(selected_fields) ||
        (selected_fields & (MeisoWifiTelemetryFieldMask)
            ~adapter->allowed_fields) != 0U || ranges_overlap(
                adapter, sizeof(*adapter), out_sample, sizeof(*out_sample)
            )) {
        return MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.selected_fields = selected_fields;
    if (!configured_interface_is_wireless(adapter)) {
        return MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE;
    }

    if ((selected_fields & MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE) != 0U &&
        read_link_state(adapter, &candidate.link_state)) {
        candidate.present_fields |=
            MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE;
        source_observed = true;
    }
    if ((selected_fields & MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) != 0U &&
        read_signal_mbm(adapter, &candidate.signal_mbm)) {
        candidate.present_fields |= MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
        source_observed = true;
    }
    for (index = 0U;
         index < sizeof(COUNTER_FIELDS) / sizeof(COUNTER_FIELDS[0]);
         ++index) {
        uint64_t value;
        uint64_t *destination;

        if ((selected_fields & COUNTER_FIELDS[index].field) == 0U ||
            !read_counter(adapter, COUNTER_FIELDS[index].file_name, &value)) {
            continue;
        }
        destination = (uint64_t *)(void *)(
            (uint8_t *)(void *)&candidate +
                COUNTER_FIELDS[index].value_offset
        );
        *destination = value;
        candidate.present_fields |= COUNTER_FIELDS[index].field;
        source_observed = true;
    }
    if (!source_observed) {
        bool ignored_link_state;
        source_observed = read_link_state(adapter, &ignored_link_state);
    }
    if (!source_observed) {
        return MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE;
    }
    *out_sample = candidate;
    return MEISO_WIFI_LINUX_ADAPTER_OK;
}
