#define _POSIX_C_SOURCE 200809L

#include "device_telemetry_linux_adapter.h"

#include <errno.h>
#include <stddef.h>
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
    char stat_path[192];
    char meminfo_path[192];
    char thermal_type_path[192];
    char thermal_value_path[192];
    MeisoDeviceTelemetryLinuxAdapterConfig config;
    MeisoDeviceTelemetryLinuxAdapter adapter;
} LinuxFixture;

static bool bytes_are(const uint8_t *bytes, uint8_t value, size_t size)
{
    size_t i;
    for (i = 0U; i < size; ++i) {
        if (bytes[i] != value) {
            return false;
        }
    }
    return true;
}

static bool write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    const size_t size = text == NULL ? 0U : strlen(text);
    bool ok;
    if (file == NULL || text == NULL) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return false;
    }
    ok = fwrite(text, 1U, size, file) == size;
    return fclose(file) == 0 && ok;
}

static bool append_text(FILE *file, const char *text)
{
    const size_t size = text == NULL ? 0U : strlen(text);
    return file != NULL && text != NULL && fwrite(text, 1U, size, file) == size;
}

static bool fixture_create(LinuxFixture *fixture)
{
    char template_path[] = "/tmp/meiso-device-linux.XXXXXX";
    char *directory;
    int temporary_fd;
    memset(fixture, 0, sizeof(*fixture));
    temporary_fd = mkstemp(template_path);
    CHECK(temporary_fd >= 0);
    CHECK(close(temporary_fd) == 0);
    CHECK(unlink(template_path) == 0);
    CHECK(mkdir(template_path, 0700) == 0);
    directory = template_path;
    CHECK(strlen(directory) < sizeof(fixture->directory));
    memcpy(fixture->directory, directory, strlen(directory) + 1U);
    CHECK(snprintf(fixture->stat_path, sizeof(fixture->stat_path),
                   "%s/stat", directory) > 0);
    CHECK(snprintf(fixture->meminfo_path, sizeof(fixture->meminfo_path),
                   "%s/meminfo", directory) > 0);
    CHECK(snprintf(fixture->thermal_type_path,
                   sizeof(fixture->thermal_type_path),
                   "%s/thermal_type", directory) > 0);
    CHECK(snprintf(fixture->thermal_value_path,
                   sizeof(fixture->thermal_value_path),
                   "%s/thermal_value", directory) > 0);
    CHECK(write_text(fixture->stat_path,
                     "cpu 100 20 30 50 10 5 3 2 999 999\n"
                     "cpu0 1 1 1 1\n"));
    CHECK(write_text(fixture->meminfo_path,
                     "MemTotal: 1000 kB\nMemAvailable: 400 kB\n"));
    CHECK(write_text(fixture->thermal_type_path,
                     "canaan_thermal_zone\n"));
    /* Probe must not parse this value file. */
    CHECK(write_text(fixture->thermal_value_path,
                     "not-a-temperature-and-never-read-by-probe\n"));
    fixture->config = (MeisoDeviceTelemetryLinuxAdapterConfig){
        fixture->stat_path,
        fixture->meminfo_path,
        fixture->thermal_type_path,
        fixture->thermal_value_path,
        MEISO_DEVICE_TELEMETRY_FIELD_ALL
    };
    CHECK(meiso_device_telemetry_linux_adapter_init(
              &fixture->adapter, &fixture->config
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    return true;
}

static void fixture_destroy(LinuxFixture *fixture)
{
    (void)unlink(fixture->stat_path);
    (void)unlink(fixture->meminfo_path);
    (void)unlink(fixture->thermal_type_path);
    (void)unlink(fixture->thermal_value_path);
    (void)rmdir(fixture->directory);
}

static bool probe_all(
    LinuxFixture *fixture,
    MeisoDeviceTelemetryCapability *out_capability
)
{
    CHECK(meiso_device_telemetry_linux_adapter_probe(
              &fixture->adapter, out_capability
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(out_capability->supported_fields ==
          MEISO_DEVICE_TELEMETRY_FIELD_ALL);
    CHECK(out_capability->min_period_ms ==
          MEISO_DEVICE_TELEMETRY_MIN_PERIOD_MS);
    CHECK(out_capability->max_period_ms ==
          MEISO_DEVICE_TELEMETRY_MAX_PERIOD_MS);
    CHECK(out_capability->period_step_ms ==
          MEISO_DEVICE_TELEMETRY_PERIOD_STEP_MS);
    CHECK(out_capability->min_temperature_millic == -40000);
    CHECK(out_capability->max_temperature_millic == 125000);
    CHECK(out_capability->max_temperature_uncertainty_millic ==
          MEISO_DEVICE_TELEMETRY_LINUX_TEMPERATURE_UNCERTAINTY_MILLIC);
    return true;
}

static bool sample_codec_roundtrip(const MeisoDeviceTelemetrySample *sample)
{
    uint8_t wire[MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE];
    MeisoDeviceTelemetrySample decoded;
    size_t size = 0U;
    memset(&decoded, 0, sizeof(decoded));
    CHECK(meiso_device_telemetry_sample_encode(
              sample, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
          ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(size == sizeof(wire));
    CHECK(meiso_device_telemetry_sample_decode(
              (MeisoByteView){wire, sizeof(wire)}, &decoded
          ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(decoded.selected_fields == sample->selected_fields);
    CHECK(decoded.present_fields == sample->present_fields);
    CHECK(decoded.temperature_millic == sample->temperature_millic);
    CHECK(decoded.temperature_uncertainty_millic ==
          sample->temperature_uncertainty_millic);
    CHECK(decoded.cpu_load_ppm == sample->cpu_load_ppm);
    CHECK(decoded.memory_total_bytes == sample->memory_total_bytes);
    CHECK(decoded.memory_available_bytes == sample->memory_available_bytes);
    return true;
}

static bool test_default_config_and_init_boundaries(void)
{
    MeisoDeviceTelemetryLinuxAdapterConfig defaults;
    MeisoDeviceTelemetryLinuxAdapterConfig config;
    MeisoDeviceTelemetryLinuxAdapter adapter;
    MeisoDeviceTelemetryLinuxAdapter before;
    char exact_path[MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY];
    char unterminated_path[MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY];
    LinuxFixture fixture;
    union {
        MeisoDeviceTelemetryLinuxAdapter adapter;
        MeisoDeviceTelemetryLinuxAdapterConfig config;
    } alias;
    size_t i;

    memset(&defaults, 0, sizeof(defaults));
    meiso_device_telemetry_linux_adapter_default_config(
        MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM, &defaults
    );
    CHECK(strcmp(defaults.proc_stat_path, "/proc/stat") == 0);
    CHECK(strcmp(defaults.proc_meminfo_path, "/proc/meminfo") == 0);
    CHECK(strcmp(defaults.thermal_type_path,
                 "/sys/class/thermal/thermal_zone0/type") == 0);
    CHECK(strcmp(defaults.thermal_value_path,
                 "/sys/class/thermal/thermal_zone0/temp") == 0);
    CHECK(defaults.allowed_fields ==
          MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM);

    CHECK(fixture_create(&fixture));
    memset(&adapter, 0xa5, sizeof(adapter));
    before = adapter;
    config = fixture.config;
    config.allowed_fields = 0U;
    CHECK(meiso_device_telemetry_linux_adapter_init(&adapter, &config) ==
          MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_CONFIG);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);
    config = fixture.config;
    config.allowed_fields = MEISO_DEVICE_TELEMETRY_FIELD_ALL | UINT32_C(0x10);
    CHECK(meiso_device_telemetry_linux_adapter_init(&adapter, &config) ==
          MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_CONFIG);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);
    config = fixture.config;
    config.proc_stat_path = "relative/path";
    CHECK(meiso_device_telemetry_linux_adapter_init(&adapter, &config) ==
          MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_CONFIG);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);
    config.proc_stat_path = "/";
    CHECK(meiso_device_telemetry_linux_adapter_init(&adapter, &config) ==
          MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_CONFIG);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);

    memset(exact_path, 'x', sizeof(exact_path));
    exact_path[0] = '/';
    exact_path[sizeof(exact_path) - 1U] = '\0';
    config = fixture.config;
    config.proc_stat_path = exact_path;
    CHECK(meiso_device_telemetry_linux_adapter_init(&adapter, &config) ==
          MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(strcmp(adapter.proc_stat_path, exact_path) == 0);
    memset(unterminated_path, 'x', sizeof(unterminated_path));
    unterminated_path[0] = '/';
    config.proc_stat_path = unterminated_path;
    adapter = before;
    CHECK(meiso_device_telemetry_linux_adapter_init(&adapter, &config) ==
          MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_CONFIG);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);

    memset(&alias, 0, sizeof(alias));
    alias.config = fixture.config;
    CHECK(meiso_device_telemetry_linux_adapter_init(
              &alias.adapter, &alias.config
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT);
    adapter = before;
    config = fixture.config;
    config.proc_stat_path = (const char *)(const void *)&adapter;
    ((char *)(void *)&adapter)[0] = '/';
    for (i = 1U; i < 4U; ++i) {
        ((char *)(void *)&adapter)[i] = 'a';
    }
    ((char *)(void *)&adapter)[4] = '\0';
    before = adapter;
    CHECK(meiso_device_telemetry_linux_adapter_init(&adapter, &config) ==
          MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool test_probe_policy_atomicity_and_codec(void)
{
    LinuxFixture fixture;
    MeisoDeviceTelemetryCapability capability;
    MeisoDeviceTelemetryCapability decoded;
    MeisoDeviceTelemetryCapability output_before;
    MeisoDeviceTelemetryLinuxAdapter adapter_before;
    uint8_t wire[MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE];
    size_t wire_size = 0U;
    CHECK(fixture_create(&fixture));
    CHECK(probe_all(&fixture, &capability));
    CHECK(meiso_device_telemetry_capability_encode(
              &capability, (MeisoMutableBuffer){wire, sizeof(wire)},
              &wire_size
          ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(wire_size == sizeof(wire));
    CHECK(meiso_device_telemetry_capability_decode(
              (MeisoByteView){wire, sizeof(wire)}, &decoded
          ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(memcmp(&decoded, &capability, sizeof(decoded)) == 0);

    memcpy(&adapter_before, &fixture.adapter, sizeof(adapter_before));
    memset(&output_before, 0xa5, sizeof(output_before));
    CHECK(meiso_device_telemetry_linux_adapter_probe(
              &fixture.adapter,
              (MeisoDeviceTelemetryCapability *)(void *)&fixture.adapter
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT);
    CHECK(memcmp(&fixture.adapter, &adapter_before,
                 sizeof(adapter_before)) == 0);
    CHECK(unlink(fixture.stat_path) == 0);
    CHECK(unlink(fixture.meminfo_path) == 0);
    CHECK(unlink(fixture.thermal_type_path) == 0);
    CHECK(meiso_device_telemetry_linux_adapter_probe(
              &fixture.adapter, &output_before
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNAVAILABLE);
    CHECK(memcmp(&fixture.adapter, &adapter_before,
                 sizeof(adapter_before)) == 0);
    CHECK(bytes_are((const uint8_t *)&output_before, 0xa5,
                    sizeof(output_before)));
    fixture_destroy(&fixture);
    return true;
}

static bool test_cpu_aggregate_baseline_reset_failure_and_regression(void)
{
    LinuxFixture fixture;
    MeisoDeviceTelemetryCapability capability;
    MeisoDeviceTelemetrySample sample;
    MeisoDeviceTelemetrySample before;
    CHECK(fixture_create(&fixture));
    CHECK(probe_all(&fixture, &capability));

    memset(&sample, 0xa5, sizeof(sample));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.selected_fields ==
          MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM);
    CHECK(sample.present_fields == 0U && sample.cpu_load_ppm == 0U);
    CHECK(write_text(fixture.stat_path,
                     "cpu 120 30 40 70 10 5 3 2 999 999\n"
                     "cpu0 999999 0 0 0\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM);
    CHECK(sample.cpu_load_ppm == 666667U);
    CHECK(sample_codec_roundtrip(&sample));

    CHECK(meiso_device_telemetry_linux_adapter_reset_sampling(
              &fixture.adapter
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U);
    CHECK(write_text(fixture.stat_path, "cpu 1 2 bad 4\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U);
    CHECK(write_text(fixture.stat_path, "cpu 200 10 10 100\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U);
    CHECK(write_text(fixture.stat_path, "cpu 210 10 10 110\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM);
    CHECK(sample.cpu_load_ppm == 500000U);

    CHECK(write_text(fixture.stat_path, "cpu 100 10 10 50\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U);
    CHECK(write_text(fixture.stat_path, "cpu 120 10 10 60\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U);
    CHECK(write_text(fixture.stat_path, "cpu 140 10 10 70\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM);
    CHECK(sample.cpu_load_ppm == 666667U);

    before = sample;
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, 0U, NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNSUPPORTED);
    CHECK(memcmp(&sample, &before, sizeof(sample)) == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool write_meminfo_rows(
    const char *path,
    size_t rows,
    size_t padding
)
{
    FILE *file = fopen(path, "w");
    size_t row;
    size_t column;
    bool ok = file != NULL;
    for (row = 0U; ok && row < rows; ++row) {
        ok = append_text(file, "Unused: 1 kB ");
        for (column = 0U; ok && column < padding; ++column) {
            ok = fputc('x', file) != EOF;
        }
        if (ok) {
            ok = fputc('\n', file) != EOF;
        }
    }
    if (file == NULL) {
        return false;
    }
    return fclose(file) == 0 && ok;
}

static bool test_memory_bounds_and_fifo_rejection(void)
{
    LinuxFixture fixture;
    MeisoDeviceTelemetryCapability capability;
    MeisoDeviceTelemetrySample sample;
    const MeisoDeviceTelemetryFieldMask memory_fields =
        MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES |
        MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES;
    CHECK(fixture_create(&fixture));
    CHECK(probe_all(&fixture, &capability));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, memory_fields, NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == memory_fields);
    CHECK(sample.memory_total_bytes == UINT64_C(1024000));
    CHECK(sample.memory_available_bytes == UINT64_C(409600));

    CHECK(write_text(fixture.meminfo_path,
                     "MemTotal: 1000 kB\nMemTotal: 1000 kB\n"
                     "MemAvailable: 400 kB\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, memory_fields, NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U && sample.memory_total_bytes == 0U &&
          sample.memory_available_bytes == 0U);
    CHECK(write_text(fixture.meminfo_path,
                     "MemTotal: 100 kB\nMemAvailable: 101 kB\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, memory_fields, NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U);
    CHECK(write_meminfo_rows(fixture.meminfo_path, 257U, 0U));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, memory_fields, NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U);
    CHECK(write_meminfo_rows(fixture.meminfo_path, 150U, 240U));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, memory_fields, NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U);

    CHECK(unlink(fixture.meminfo_path) == 0);
    CHECK(mkfifo(fixture.meminfo_path, 0600) == 0);
    CHECK(meiso_device_telemetry_linux_adapter_probe(
              &fixture.adapter, &capability
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK((capability.supported_fields & memory_fields) == 0U);
    CHECK((capability.supported_fields &
           MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM) != 0U);
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, memory_fields, NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNSUPPORTED);
    fixture_destroy(&fixture);
    return true;
}

static bool read_raw_temperature(
    LinuxFixture *fixture,
    uint64_t raw,
    MeisoDeviceTelemetryLinuxTemperature *out_temperature
)
{
    char text[64];
    CHECK(snprintf(text, sizeof(text), "%llu\n",
                   (unsigned long long)raw) > 0);
    CHECK(write_text(fixture->thermal_value_path, text));
    CHECK(meiso_device_telemetry_linux_adapter_read_temperature_direct(
              &fixture->adapter, out_temperature
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    return true;
}

static bool test_thermal_direct_and_fresh_merge(void)
{
    LinuxFixture fixture;
    MeisoDeviceTelemetryCapability capability;
    MeisoDeviceTelemetryLinuxTemperature temperature;
    MeisoDeviceTelemetryLinuxTemperature temperature_before;
    MeisoDeviceTelemetrySample sample;
    MeisoDeviceTelemetrySample sample_before;
    CHECK(fixture_create(&fixture));
    memset(&temperature, 0xa5, sizeof(temperature));
    temperature_before = temperature;
    CHECK(meiso_device_telemetry_linux_adapter_read_temperature_direct(
              &fixture.adapter, &temperature
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_NOT_PROBED);
    CHECK(memcmp(&temperature, &temperature_before,
                 sizeof(temperature)) == 0);
    CHECK(probe_all(&fixture, &capability));

    CHECK(read_raw_temperature(&fixture, UINT64_C(3000), &temperature));
    CHECK(!temperature.present && temperature.temperature_millic == 0 &&
          temperature.uncertainty_millic == 0U);
    CHECK(read_raw_temperature(
        &fixture, (UINT64_C(1) << 12U) | UINT64_C(2821), &temperature
    ));
    CHECK(!temperature.present);
    CHECK(read_raw_temperature(
        &fixture, (UINT64_C(1) << 12U) | UINT64_C(2822), &temperature
    ));
    CHECK(temperature.present && temperature.temperature_millic == -39980);
    CHECK(read_raw_temperature(
        &fixture, (UINT64_C(1) << 12U) | UINT64_C(3000), &temperature
    ));
    CHECK(temperature.present && temperature.temperature_millic == 17752);
    CHECK(temperature.uncertainty_millic ==
          MEISO_DEVICE_TELEMETRY_LINUX_TEMPERATURE_UNCERTAINTY_MILLIC);
    CHECK(read_raw_temperature(
        &fixture, (UINT64_C(1) << 12U) | UINT64_C(3350), &temperature
    ));
    CHECK(temperature.present && temperature.temperature_millic == 124759);
    CHECK(read_raw_temperature(
        &fixture, (UINT64_C(1) << 12U) | UINT64_C(3351), &temperature
    ));
    CHECK(!temperature.present);

    temperature = (MeisoDeviceTelemetryLinuxTemperature){true, 17752, 3000U};
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter,
              MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC,
              &temperature, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields ==
          MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC);
    CHECK(sample.temperature_millic == 17752 &&
          sample.temperature_uncertainty_millic == 3000U);
    CHECK(sample_codec_roundtrip(&sample));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_ALL,
              &temperature, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields ==
          (MEISO_DEVICE_TELEMETRY_FIELD_ALL &
           ~MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM));
    CHECK(sample.memory_total_bytes == UINT64_C(1024000));
    CHECK(sample.memory_available_bytes == UINT64_C(409600));
    CHECK(write_text(fixture.stat_path,
                     "cpu 120 30 40 70 10 5 3 2\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_ALL,
              &temperature, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == MEISO_DEVICE_TELEMETRY_FIELD_ALL);
    CHECK(sample.cpu_load_ppm == 666667U);
    CHECK(sample.temperature_millic == 17752);
    CHECK(sample.memory_total_bytes == UINT64_C(1024000));
    CHECK(sample.memory_available_bytes == UINT64_C(409600));
    CHECK(sample_codec_roundtrip(&sample));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter,
              MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U && sample.temperature_millic == 0 &&
          sample.temperature_uncertainty_millic == 0U);
    temperature = (MeisoDeviceTelemetryLinuxTemperature){false, 0, 0U};
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter,
              MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC,
              &temperature, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U);

    sample_before = sample;
    temperature = (MeisoDeviceTelemetryLinuxTemperature){true, 17752, 1U};
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter,
              MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC,
              &temperature, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT);
    CHECK(memcmp(&sample, &sample_before, sizeof(sample)) == 0);
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter,
              MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC,
              (const MeisoDeviceTelemetryLinuxTemperature *)(const void *)
                  &sample,
              &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT);
    CHECK(memcmp(&sample, &sample_before, sizeof(sample)) == 0);
    CHECK(meiso_device_telemetry_linux_adapter_read_temperature_direct(
              &fixture.adapter,
              (MeisoDeviceTelemetryLinuxTemperature *)(void *)&fixture.adapter
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT);

    CHECK(unlink(fixture.thermal_value_path) == 0);
    temperature = temperature_before;
    CHECK(meiso_device_telemetry_linux_adapter_read_temperature_direct(
              &fixture.adapter, &temperature
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNAVAILABLE);
    CHECK(memcmp(&temperature, &temperature_before,
                 sizeof(temperature)) == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool test_sample_alias_and_probe_resets_cpu(void)
{
    LinuxFixture fixture;
    MeisoDeviceTelemetryCapability capability;
    MeisoDeviceTelemetrySample sample;
    MeisoDeviceTelemetryLinuxAdapter adapter_before;
    CHECK(fixture_create(&fixture));
    CHECK(probe_all(&fixture, &capability));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(write_text(fixture.stat_path, "cpu 120 30 40 70 10 5 3 2\n"));
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM);
    CHECK(meiso_device_telemetry_linux_adapter_probe(
              &fixture.adapter, &capability
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL, &sample
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    CHECK(sample.present_fields == 0U);

    adapter_before = fixture.adapter;
    CHECK(meiso_device_telemetry_linux_adapter_sample(
              &fixture.adapter, MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
              NULL,
              (MeisoDeviceTelemetrySample *)(void *)&fixture.adapter
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_INVALID_ARGUMENT);
    CHECK(memcmp(&fixture.adapter, &adapter_before,
                 sizeof(adapter_before)) == 0);
    fixture_destroy(&fixture);
    return true;
}

int main(void)
{
    if (!test_default_config_and_init_boundaries() ||
        !test_probe_policy_atomicity_and_codec() ||
        !test_cpu_aggregate_baseline_reset_failure_and_regression() ||
        !test_memory_bounds_and_fifo_rejection() ||
        !test_thermal_direct_and_fresh_merge() ||
        !test_sample_alias_and_probe_resets_cpu()) {
        return 1;
    }
    puts("device telemetry Linux adapter tests passed");
    return 0;
}
