#define _POSIX_C_SOURCE 200809L

#include <meiso/edge/linux/wifi.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

typedef struct {
    char directory[128];
    char sys_class_net_root[192];
    char interface_root[224];
    char wireless_directory[240];
    char statistics_directory[240];
    char carrier_path[256];
    char proc_wireless_path[192];
    char counter_paths[8][256];
    MeisoWifiLinuxAdapterConfig config;
    MeisoWifiLinuxAdapter adapter;
} WifiFixture;

static const char *const COUNTER_NAMES[8] = {
    "rx_bytes", "tx_bytes", "rx_packets", "tx_packets",
    "rx_dropped", "tx_dropped", "rx_errors", "tx_errors"
};

static const uint64_t COUNTER_VALUES[8] = {
    UINT64_C(101), UINT64_C(202), UINT64_C(303), UINT64_C(404),
    UINT64_C(5), UINT64_C(6), UINT64_C(7), UINT64_C(8)
};

static bool format_path(
    char *output,
    size_t output_capacity,
    const char *parent,
    const char *leaf
)
{
    const int count = snprintf(output, output_capacity, "%s/%s", parent, leaf);
    return count > 0 && (size_t)count < output_capacity;
}

static bool write_text(const char *path, const char *text)
{
    FILE *file;
    size_t size;
    bool written;

    if (path == NULL || text == NULL) {
        return false;
    }
    file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    size = strlen(text);
    written = fwrite(text, 1U, size, file) == size;
    return fclose(file) == 0 && written;
}

static bool write_wireless_rows(const char *path, size_t rows)
{
    FILE *file;
    size_t index;
    bool ok = true;

    file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    for (index = 0U; ok && index < rows; ++index) {
        ok = fprintf(file, "other%zu: 0000 1. -1.\n", index) > 0;
    }
    return fclose(file) == 0 && ok;
}

static bool fixture_create(WifiFixture *fixture)
{
    char template_path[] = "/tmp/meiso-wifi-linux.XXXXXX";
    char counter_text[64];
    int temporary_fd;
    size_t index;

    memset(fixture, 0, sizeof(*fixture));
    temporary_fd = mkstemp(template_path);
    CHECK(temporary_fd >= 0);
    CHECK(close(temporary_fd) == 0);
    CHECK(unlink(template_path) == 0);
    CHECK(mkdir(template_path, 0700) == 0);
    CHECK(strlen(template_path) < sizeof(fixture->directory));
    memcpy(fixture->directory, template_path, strlen(template_path) + 1U);
    CHECK(format_path(
        fixture->sys_class_net_root, sizeof(fixture->sys_class_net_root),
        fixture->directory, "class-net"
    ));
    CHECK(mkdir(fixture->sys_class_net_root, 0700) == 0);
    CHECK(format_path(
        fixture->interface_root, sizeof(fixture->interface_root),
        fixture->sys_class_net_root, "wlan0"
    ));
    CHECK(mkdir(fixture->interface_root, 0700) == 0);
    CHECK(format_path(
        fixture->wireless_directory, sizeof(fixture->wireless_directory),
        fixture->interface_root, "wireless"
    ));
    CHECK(mkdir(fixture->wireless_directory, 0700) == 0);
    CHECK(format_path(
        fixture->statistics_directory,
        sizeof(fixture->statistics_directory), fixture->interface_root,
        "statistics"
    ));
    CHECK(mkdir(fixture->statistics_directory, 0700) == 0);
    CHECK(format_path(
        fixture->carrier_path, sizeof(fixture->carrier_path),
        fixture->interface_root, "carrier"
    ));
    CHECK(write_text(fixture->carrier_path, "1\n"));
    CHECK(format_path(
        fixture->proc_wireless_path, sizeof(fixture->proc_wireless_path),
        fixture->directory, "wireless"
    ));
    CHECK(write_text(
        fixture->proc_wireless_path,
        "Inter-| sta\n face | quality\n wlan0: 0000 42. -55. 0 0 0 0 0\n"
    ));
    for (index = 0U; index < 8U; ++index) {
        CHECK(format_path(
            fixture->counter_paths[index],
            sizeof(fixture->counter_paths[index]),
            fixture->statistics_directory, COUNTER_NAMES[index]
        ));
        CHECK(snprintf(
            counter_text, sizeof(counter_text), "%llu\n",
            (unsigned long long)COUNTER_VALUES[index]
        ) > 0);
        CHECK(write_text(fixture->counter_paths[index], counter_text));
    }
    memcpy(fixture->config.interface_name, "wlan0", sizeof("wlan0"));
    fixture->config.allowed_fields = MEISO_WIFI_TELEMETRY_FIELD_ALL;
    fixture->config.sys_class_net_root = fixture->sys_class_net_root;
    fixture->config.proc_net_wireless_path = fixture->proc_wireless_path;
    CHECK(meiso_wifi_linux_adapter_init(
        &fixture->adapter, &fixture->config
    ) == MEISO_WIFI_LINUX_ADAPTER_OK);
    return true;
}

static void fixture_destroy(WifiFixture *fixture)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        (void)unlink(fixture->counter_paths[index]);
    }
    (void)unlink(fixture->carrier_path);
    (void)unlink(fixture->proc_wireless_path);
    (void)rmdir(fixture->wireless_directory);
    (void)rmdir(fixture->statistics_directory);
    (void)rmdir(fixture->interface_root);
    (void)rmdir(fixture->sys_class_net_root);
    (void)rmdir(fixture->directory);
}

static bool test_init_defaults_bounds_alias_and_no_partial(void)
{
    MeisoWifiLinuxAdapterConfig config;
    MeisoWifiLinuxAdapter adapter;
    MeisoWifiLinuxAdapter before;
    char exact[MEISO_WIFI_LINUX_LOCATOR_CAPACITY];
    char unterminated[MEISO_WIFI_LINUX_LOCATOR_CAPACITY];
    union {
        MeisoWifiLinuxAdapter adapter;
        MeisoWifiLinuxAdapterConfig config;
    } alias;

    memset(&config, 0, sizeof(config));
    memcpy(config.interface_name, "wlan0", sizeof("wlan0"));
    config.allowed_fields = MEISO_WIFI_TELEMETRY_FIELD_ALL;
    memset(&adapter, 0xa5, sizeof(adapter));
    CHECK(meiso_wifi_linux_adapter_init(&adapter, &config) ==
          MEISO_WIFI_LINUX_ADAPTER_OK);
    CHECK(strcmp(adapter.interface_name, "wlan0") == 0);
    CHECK(strcmp(adapter.sys_class_net_root, "/sys/class/net") == 0);
    CHECK(strcmp(adapter.proc_net_wireless_path,
                 "/proc/net/wireless") == 0);

    memset(&adapter, 0xa5, sizeof(adapter));
    before = adapter;
    config.allowed_fields = 0U;
    CHECK(meiso_wifi_linux_adapter_init(&adapter, &config) ==
          MEISO_WIFI_LINUX_ADAPTER_INVALID_CONFIG);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);
    config.allowed_fields = MEISO_WIFI_TELEMETRY_FIELD_ALL;
    memset(config.interface_name, 'x', sizeof(config.interface_name));
    CHECK(meiso_wifi_linux_adapter_init(&adapter, &config) ==
          MEISO_WIFI_LINUX_ADAPTER_INVALID_CONFIG);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);
    memset(config.interface_name, 0, sizeof(config.interface_name));
    memcpy(config.interface_name, "wlan0", sizeof("wlan0"));

    config.sys_class_net_root = "relative";
    CHECK(meiso_wifi_linux_adapter_init(&adapter, &config) ==
          MEISO_WIFI_LINUX_ADAPTER_INVALID_CONFIG);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);
    memset(exact, 'x', sizeof(exact));
    exact[0] = '/';
    exact[sizeof(exact) - 1U] = '\0';
    config.sys_class_net_root = exact;
    config.proc_net_wireless_path = "/p";
    CHECK(meiso_wifi_linux_adapter_init(&adapter, &config) ==
          MEISO_WIFI_LINUX_ADAPTER_OK);
    CHECK(strcmp(adapter.sys_class_net_root, exact) == 0);
    memset(unterminated, 'x', sizeof(unterminated));
    unterminated[0] = '/';
    config.sys_class_net_root = unterminated;
    adapter = before;
    CHECK(meiso_wifi_linux_adapter_init(&adapter, &config) ==
          MEISO_WIFI_LINUX_ADAPTER_INVALID_CONFIG);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);

    memset(&alias, 0, sizeof(alias));
    memcpy(alias.config.interface_name, "wlan0", sizeof("wlan0"));
    alias.config.allowed_fields = MEISO_WIFI_TELEMETRY_FIELD_ALL;
    CHECK(meiso_wifi_linux_adapter_init(&alias.adapter, &alias.config) ==
          MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT);

    memset(&adapter, 0xa5, sizeof(adapter));
    memcpy(adapter.sys_class_net_root, "/inside", sizeof("/inside"));
    before = adapter;
    memset(&config, 0, sizeof(config));
    memcpy(config.interface_name, "wlan0", sizeof("wlan0"));
    config.allowed_fields = MEISO_WIFI_TELEMETRY_FIELD_ALL;
    config.sys_class_net_root = adapter.sys_class_net_root;
    CHECK(meiso_wifi_linux_adapter_init(&adapter, &config) ==
          MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);
    return true;
}

static bool test_temp_fixture_probe_sample_and_atomicity(void)
{
    WifiFixture fixture;
    MeisoWifiTelemetryFieldMask supported = 0U;
    MeisoWifiTelemetryFieldMask supported_before;
    MeisoWifiTelemetrySample sample;
    MeisoWifiTelemetrySample sample_before;
    MeisoWifiLinuxAdapter adapter_before;

    CHECK(fixture_create(&fixture));
    CHECK(strcmp(fixture.adapter.sys_class_net_root,
                 fixture.sys_class_net_root) == 0);
    CHECK(strcmp(fixture.adapter.proc_net_wireless_path,
                 fixture.proc_wireless_path) == 0);
    CHECK(meiso_wifi_linux_adapter_probe(&fixture.adapter, &supported) ==
          MEISO_WIFI_LINUX_ADAPTER_OK);
    CHECK(supported == MEISO_WIFI_TELEMETRY_FIELD_ALL);
    memset(&sample, 0xa5, sizeof(sample));
    CHECK(meiso_wifi_linux_adapter_sample(
        &fixture.adapter, MEISO_WIFI_TELEMETRY_FIELD_ALL, &sample
    ) == MEISO_WIFI_LINUX_ADAPTER_OK);
    CHECK(sample.selected_fields == MEISO_WIFI_TELEMETRY_FIELD_ALL);
    CHECK(sample.present_fields == MEISO_WIFI_TELEMETRY_FIELD_ALL);
    CHECK(sample.link_state);
    CHECK(sample.signal_mbm == -5500);
    CHECK(sample.rx_bytes == COUNTER_VALUES[0]);
    CHECK(sample.tx_bytes == COUNTER_VALUES[1]);
    CHECK(sample.rx_packets == COUNTER_VALUES[2]);
    CHECK(sample.tx_packets == COUNTER_VALUES[3]);
    CHECK(sample.rx_dropped == COUNTER_VALUES[4]);
    CHECK(sample.tx_dropped == COUNTER_VALUES[5]);
    CHECK(sample.rx_errors == COUNTER_VALUES[6]);
    CHECK(sample.tx_errors == COUNTER_VALUES[7]);

    adapter_before = fixture.adapter;
    CHECK(meiso_wifi_linux_adapter_probe(
        &fixture.adapter,
        (MeisoWifiTelemetryFieldMask *)(void *)&fixture.adapter
    ) == MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT);
    CHECK(memcmp(&fixture.adapter, &adapter_before,
                 sizeof(adapter_before)) == 0);
    sample_before = sample;
    CHECK(meiso_wifi_linux_adapter_sample(
        &fixture.adapter, MEISO_WIFI_TELEMETRY_FIELD_ALL,
        (MeisoWifiTelemetrySample *)(void *)&fixture.adapter
    ) == MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT);
    CHECK(memcmp(&fixture.adapter, &adapter_before,
                 sizeof(adapter_before)) == 0);

    CHECK(rmdir(fixture.wireless_directory) == 0);
    supported_before = UINT32_C(0xa5a5a5a5);
    supported = supported_before;
    CHECK(meiso_wifi_linux_adapter_probe(&fixture.adapter, &supported) ==
          MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE);
    CHECK(supported == supported_before);
    memset(&sample, 0xa5, sizeof(sample));
    sample_before = sample;
    CHECK(meiso_wifi_linux_adapter_sample(
        &fixture.adapter, MEISO_WIFI_TELEMETRY_FIELD_ALL, &sample
    ) == MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE);
    CHECK(memcmp(&sample, &sample_before, sizeof(sample)) == 0);
    CHECK(mkdir(fixture.wireless_directory, 0700) == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool test_allowlist_and_missing_field_truth(void)
{
    WifiFixture fixture;
    MeisoWifiTelemetryFieldMask supported = 0U;
    MeisoWifiTelemetrySample sample;
    MeisoWifiTelemetrySample before;
    const MeisoWifiTelemetryFieldMask allowed =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM |
        MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES;

    CHECK(fixture_create(&fixture));
    fixture.config.allowed_fields = allowed;
    CHECK(meiso_wifi_linux_adapter_init(
        &fixture.adapter, &fixture.config
    ) == MEISO_WIFI_LINUX_ADAPTER_OK);
    CHECK(write_text(
        fixture.proc_wireless_path,
        "Inter-| sta\n face | quality\n other0: 0000 1. -1.\n"
    ));
    CHECK(meiso_wifi_linux_adapter_probe(&fixture.adapter, &supported) ==
          MEISO_WIFI_LINUX_ADAPTER_OK);
    CHECK(supported == (allowed & ~MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM));
    CHECK(meiso_wifi_linux_adapter_sample(
        &fixture.adapter, allowed, &sample
    ) == MEISO_WIFI_LINUX_ADAPTER_OK);
    CHECK(sample.selected_fields == allowed);
    CHECK(sample.present_fields ==
          (allowed & ~MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM));
    CHECK(sample.link_state && sample.rx_bytes == COUNTER_VALUES[0]);
    before = sample;
    CHECK(meiso_wifi_linux_adapter_sample(
        &fixture.adapter, MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES, &sample
    ) == MEISO_WIFI_LINUX_ADAPTER_INVALID_ARGUMENT);
    CHECK(memcmp(&sample, &before, sizeof(sample)) == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool test_fifo_and_wireless_scan_bounds(void)
{
    WifiFixture fixture;
    MeisoWifiTelemetryFieldMask supported;
    MeisoWifiTelemetrySample sample;
    MeisoWifiTelemetrySample before;

    CHECK(fixture_create(&fixture));
    fixture.config.allowed_fields = MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    CHECK(meiso_wifi_linux_adapter_init(
        &fixture.adapter, &fixture.config
    ) == MEISO_WIFI_LINUX_ADAPTER_OK);
    CHECK(unlink(fixture.carrier_path) == 0);
    CHECK(unlink(fixture.proc_wireless_path) == 0);
    CHECK(mkfifo(fixture.proc_wireless_path, 0600) == 0);
    supported = UINT32_C(0xa5a5a5a5);
    CHECK(meiso_wifi_linux_adapter_probe(&fixture.adapter, &supported) ==
          MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE);
    CHECK(supported == UINT32_C(0xa5a5a5a5));
    memset(&sample, 0xa5, sizeof(sample));
    before = sample;
    CHECK(meiso_wifi_linux_adapter_sample(
        &fixture.adapter, MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM, &sample
    ) == MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE);
    CHECK(memcmp(&sample, &before, sizeof(sample)) == 0);

    CHECK(unlink(fixture.proc_wireless_path) == 0);
    CHECK(write_wireless_rows(fixture.proc_wireless_path, 257U));
    supported = UINT32_C(0xa5a5a5a5);
    CHECK(meiso_wifi_linux_adapter_probe(&fixture.adapter, &supported) ==
          MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE);
    CHECK(supported == UINT32_C(0xa5a5a5a5));
    CHECK(write_text(
        fixture.proc_wireless_path,
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    ));
    CHECK(meiso_wifi_linux_adapter_sample(
        &fixture.adapter, MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM, &sample
    ) == MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE);
    CHECK(memcmp(&sample, &before, sizeof(sample)) == 0);
    fixture_destroy(&fixture);
    return true;
}

int main(void)
{
    if (!test_init_defaults_bounds_alias_and_no_partial() ||
        !test_temp_fixture_probe_sample_and_atomicity() ||
        !test_allowlist_and_missing_field_truth() ||
        !test_fifo_and_wireless_scan_bounds()) {
        return 1;
    }
    puts("Wi-Fi Linux adapter tests passed");
    return 0;
}
