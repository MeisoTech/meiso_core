#include "meiso_four_kind_lab_private.h"

/* Thermal/telemetry support owns bounded child probing and sample windows. */

static bool
thermal_preflight_direct(MeisoFourKindLabThermalReading *out_reading)
{
    static const char TYPE_PATH[] = "/sys/class/thermal/thermal_zone0/type";
    static const char VALUE_PATH[] = "/sys/class/thermal/thermal_zone0/temp";
    char type[64];
    uint64_t raw;
    uint64_t code;
    double x;
    double converted_c;
    double converted_millic;

    if (out_reading == NULL) {
        return false;
    }
    memset(out_reading, 0, sizeof(*out_reading));
    if (!meiso_four_kind_lab_private_read_text_file(TYPE_PATH, type,
                                                    sizeof(type)) ||
        strcmp(type, "canaan_thermal_zone") != 0 ||
        !meiso_four_kind_lab_private_read_u64_file(VALUE_PATH, &raw)) {
        return true;
    }
    if ((raw >> 12U) == 0U) {
        return true;
    }
    code = raw & UINT64_C(0x0fff);
    if (code < UINT64_C(2822) || code > UINT64_C(3350)) {
        return true;
    }
    x = (double)code;
    converted_c =
        ((((1.01472e-10 * x - 1.10063e-6) * x + 4.36150e-3) * x - 7.10128) * x +
         3565.87);
    if (converted_c < -40.0 || converted_c > 125.0) {
        return true;
    }
    converted_millic = converted_c * 1000.0;
    out_reading->corrected_millic = converted_millic >= 0.0
                                        ? (int32_t)(converted_millic + 0.5)
                                        : (int32_t)(converted_millic - 0.5);
    out_reading->uncertainty_millic = MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC;
    out_reading->present = true;
    return true;
}

enum {
    THERMAL_RESULT_WIRE_SIZE = 16,
    THERMAL_RESULT_MAGIC = 0x54345031,
    THERMAL_RESULT_VERSION = 1
};

static void put_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

static uint32_t get_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) | ((uint32_t)source[1] << 16U) |
           ((uint32_t)source[2] << 8U) | (uint32_t)source[3];
}

static void encode_thermal_result(const MeisoFourKindLabThermalReading *reading,
                                  uint8_t output[THERMAL_RESULT_WIRE_SIZE])
{
    uint32_t temperature_bits;

    memset(output, 0, THERMAL_RESULT_WIRE_SIZE);
    put_u32_be(output, THERMAL_RESULT_MAGIC);
    output[4] = THERMAL_RESULT_VERSION;
    output[5] = reading->present ? 1U : 0U;
    memcpy(&temperature_bits, &reading->corrected_millic,
           sizeof(temperature_bits));
    put_u32_be(output + 8U, temperature_bits);
    put_u32_be(output + 12U, reading->uncertainty_millic);
}

static bool decode_thermal_result(const uint8_t input[THERMAL_RESULT_WIRE_SIZE],
                                  MeisoFourKindLabThermalReading *out_reading)
{
    const uint32_t temperature_bits = get_u32_be(input + 8U);
    int32_t temperature;

    if (get_u32_be(input) != THERMAL_RESULT_MAGIC ||
        input[4] != THERMAL_RESULT_VERSION || input[5] > 1U || input[6] != 0U ||
        input[7] != 0U) {
        return false;
    }
    memcpy(&temperature, &temperature_bits, sizeof(temperature));
    if (input[5] == 0U) {
        if (temperature != 0 || get_u32_be(input + 12U) != 0U) {
            return false;
        }
        memset(out_reading, 0, sizeof(*out_reading));
        return true;
    }
    if (temperature < MEISO_LAB_TEMPERATURE_MIN_MILLIC ||
        temperature > MEISO_LAB_TEMPERATURE_MAX_MILLIC ||
        get_u32_be(input + 12U) != MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC) {
        return false;
    }
    out_reading->present = true;
    out_reading->corrected_millic = temperature;
    out_reading->uncertainty_millic = get_u32_be(input + 12U);
    return true;
}

static bool write_all(int fd, const uint8_t *data, size_t size)
{
    size_t offset = 0U;

    while (offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static void thermal_child_run(int write_fd,
                              MeisoFourKindLabThermalChildMode mode)
{
    MeisoFourKindLabThermalReading reading;
    uint8_t encoded[THERMAL_RESULT_WIRE_SIZE];

    memset(&reading, 0, sizeof(reading));
    if (mode == MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_TIMEOUT) {
        for (;;) {
            (void)pause();
        }
    }
    if (mode == MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_EARLY_EXIT) {
        _exit(0);
    }
    if (mode == MEISO_FOUR_KIND_THERMAL_CHILD_ACTUAL) {
        if (!thermal_preflight_direct(&reading)) {
            _exit(2);
        }
    } else if (mode == MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_PRESENT) {
        reading.present = true;
        reading.corrected_millic = 52000;
        reading.uncertainty_millic = MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC;
    } else if (mode != MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_ABSENT &&
               mode != MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_PARTIAL) {
        _exit(2);
    }
    encode_thermal_result(&reading, encoded);
    if (mode == MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_PARTIAL) {
        if (!write_all(write_fd, encoded, THERMAL_RESULT_WIRE_SIZE / 2U)) {
            _exit(2);
        }
        for (;;) {
            (void)pause();
        }
    }
    _exit(write_all(write_fd, encoded, sizeof(encoded)) ? 0 : 2);
}

static void thermal_child_close_inherited_stdio(int write_fd)
{
    int descriptor;
    for (descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO; ++descriptor) {
        if (descriptor != write_fd) {
            (void)close(descriptor);
        }
    }
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_make_thermal_pipe(int descriptors[2])
{
    int read_flags;
    int read_descriptor_flags;
    int write_descriptor_flags;

    descriptors[0] = -1;
    descriptors[1] = -1;
    if (pipe(descriptors) != 0) {
        return false;
    }
    read_flags = fcntl(descriptors[0], F_GETFL);
    read_descriptor_flags = fcntl(descriptors[0], F_GETFD);
    write_descriptor_flags = fcntl(descriptors[1], F_GETFD);
    if (read_flags < 0 || read_descriptor_flags < 0 ||
        write_descriptor_flags < 0 ||
        fcntl(descriptors[0], F_SETFL, read_flags | O_NONBLOCK) != 0 ||
        fcntl(descriptors[0], F_SETFD, read_descriptor_flags | FD_CLOEXEC) !=
            0 ||
        fcntl(descriptors[1], F_SETFD, write_descriptor_flags | FD_CLOEXEC) !=
            0) {
        meiso_four_kind_lab_private_close_fd(&descriptors[0]);
        meiso_four_kind_lab_private_close_fd(&descriptors[1]);
        return false;
    }
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_thermal_child_reap_nonblocking(
    MeisoFourKindLabThermalChild *child)
{
    int status;
    pid_t result;

    if (child == NULL || !child->pending) {
        return;
    }
    result = waitpid(child->pid, &status, WNOHANG);
    if (result == child->pid || (result < 0 && errno == ECHILD)) {
        child->pid = (pid_t)-1;
        child->pending = false;
    }
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_thermal_child_terminate_nonblocking(
    MeisoFourKindLabThermalChild *child)
{
    if (child == NULL || !child->pending) {
        return;
    }
    if (kill(child->pid, SIGKILL) != 0 && errno != ESRCH) {
        return;
    }
    meiso_four_kind_lab_private_thermal_child_reap_nonblocking(child);
}

MEISO_FOUR_KIND_LAB_PRIVATE MeisoFourKindLabThermalPreflightStatus
meiso_four_kind_lab_private_thermal_preflight_bounded(
    MeisoFourKindLabThermalReading *out_reading,
    MeisoFourKindLabThermalChild *child, MeisoFourKindLabThermalChildMode mode)
{
    uint8_t input[THERMAL_RESULT_WIRE_SIZE + 1U];
    size_t input_size = 0U;
    struct timespec deadline;
    int descriptors[2];
    bool eof = false;
    pid_t pid;
    MeisoFourKindLabThermalPreflightStatus status =
        MEISO_FOUR_KIND_THERMAL_PREFLIGHT_CHILD_ERROR;

    if (out_reading == NULL || child == NULL ||
        !meiso_four_kind_lab_private_make_deadline_after_ms(
            &deadline, MEISO_FOUR_KIND_LAB_THERMAL_PREFLIGHT_TIMEOUT_MS)) {
        return MEISO_FOUR_KIND_THERMAL_PREFLIGHT_LOCAL_ERROR;
    }
    memset(out_reading, 0, sizeof(*out_reading));
    child->pid = (pid_t)-1;
    child->pending = false;
    if (!meiso_four_kind_lab_private_make_thermal_pipe(descriptors)) {
        return MEISO_FOUR_KIND_THERMAL_PREFLIGHT_LOCAL_ERROR;
    }
    pid = fork();
    if (pid < 0) {
        meiso_four_kind_lab_private_close_fd(&descriptors[0]);
        meiso_four_kind_lab_private_close_fd(&descriptors[1]);
        return MEISO_FOUR_KIND_THERMAL_PREFLIGHT_LOCAL_ERROR;
    }
    if (pid == 0) {
        meiso_four_kind_lab_private_close_fd(&descriptors[0]);
        thermal_child_close_inherited_stdio(descriptors[1]);
        thermal_child_run(descriptors[1], mode);
    }
    child->pid = pid;
    child->pending = true;
    meiso_four_kind_lab_private_close_fd(&descriptors[1]);

    while (!eof) {
        struct pollfd poll_descriptor;
        ssize_t received;
        int wait_ms;
        int poll_result;

        received = read(descriptors[0], input + input_size,
                        sizeof(input) - input_size);
        if (received > 0) {
            input_size += (size_t)received;
            continue;
        }
        if (received == 0) {
            eof = true;
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
        wait_ms = meiso_four_kind_lab_private_deadline_wait_ms(&deadline);
        if (wait_ms == 0) {
            status = MEISO_FOUR_KIND_THERMAL_PREFLIGHT_TIMEOUT;
            break;
        }
        poll_descriptor.fd = descriptors[0];
        poll_descriptor.events = (short)(POLLIN | POLLHUP);
        poll_descriptor.revents = 0;
        poll_result = poll(&poll_descriptor, (nfds_t)1U, wait_ms);
        if (poll_result < 0 && errno == EINTR) {
            continue;
        }
        if (poll_result < 0 ||
            (poll_descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            break;
        }
    }
    meiso_four_kind_lab_private_close_fd(&descriptors[0]);
    if (eof && input_size == THERMAL_RESULT_WIRE_SIZE &&
        decode_thermal_result(input, out_reading)) {
        status = out_reading->present
                     ? MEISO_FOUR_KIND_THERMAL_PREFLIGHT_PRESENT
                     : MEISO_FOUR_KIND_THERMAL_PREFLIGHT_ABSENT;
        meiso_four_kind_lab_private_thermal_child_reap_nonblocking(child);
    } else {
        memset(out_reading, 0, sizeof(*out_reading));
        meiso_four_kind_lab_private_thermal_child_terminate_nonblocking(child);
    }
    return status;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_initialize_telemetry(
    MeisoFourKindLabTelemetryEnvironment *environment,
    MeisoFourKindLabThermalChild *thermal_child,
    MeisoFourKindLabThermalChildMode thermal_mode)
{
    MeisoFourKindLabThermalPreflightStatus thermal_status;

    if (environment == NULL) {
        return false;
    }
    memset(environment, 0, sizeof(*environment));
    environment->window_id = 1U;
    thermal_status = meiso_four_kind_lab_private_thermal_preflight_bounded(
        &environment->thermal, thermal_child, thermal_mode);
    (void)thermal_status;
    return meiso_four_kind_lab_private_read_network(&environment->baseline) &&
           meiso_four_kind_lab_private_monotonic_now_ns(
               &environment->started_ns);
}

static bool counters_decreased(const MeisoFourKindLabNetworkReading *current,
                               const MeisoFourKindLabNetworkReading *baseline)
{
    return current->rx_bytes < baseline->rx_bytes ||
           current->tx_bytes < baseline->tx_bytes ||
           current->rx_packets < baseline->rx_packets ||
           current->tx_packets < baseline->tx_packets ||
           current->rx_dropped < baseline->rx_dropped ||
           current->tx_dropped < baseline->tx_dropped;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_reset_telemetry_window(
    MeisoFourKindLabTelemetryEnvironment *environment)
{
    MeisoFourKindLabNetworkReading current;
    uint64_t now;

    if (environment == NULL || environment->window_id == UINT64_MAX ||
        !meiso_four_kind_lab_private_read_network(&current) ||
        !meiso_four_kind_lab_private_monotonic_now_ns(&now)) {
        return false;
    }
    environment->baseline = current;
    environment->started_ns = now;
    environment->window_id += 1U;
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_capture_telemetry(
    MeisoFourKindLabTelemetryEnvironment *environment,
    MeisoLabEdgeState *out_state, MeisoLabTelemetrySample *out_sample)
{
    MeisoFourKindLabNetworkReading current;
    uint64_t now;

    if (environment == NULL || out_state == NULL || out_sample == NULL ||
        !meiso_four_kind_lab_private_read_network(&current) ||
        !meiso_four_kind_lab_private_monotonic_now_ns(&now)) {
        return false;
    }
    if (counters_decreased(&current, &environment->baseline)) {
        if (environment->window_id == UINT64_MAX) {
            return false;
        }
        environment->baseline = current;
        environment->started_ns = now;
        environment->window_id += 1U;
    }
    memset(out_state, 0, sizeof(*out_state));
    out_state->window_id = environment->window_id;
    out_state->interface_present = current.interface_present;
    out_state->carrier_present = current.carrier;
    out_state->temperature_present = environment->thermal.present;
    out_state->corrected_millic = environment->thermal.corrected_millic;
    out_state->uncertainty_millic = environment->thermal.uncertainty_millic;

    memset(out_sample, 0, sizeof(*out_sample));
    out_sample->window_id = environment->window_id;
    out_sample->elapsed_ns = now - environment->started_ns;
    out_sample->rx_bytes = current.rx_bytes - environment->baseline.rx_bytes;
    out_sample->tx_bytes = current.tx_bytes - environment->baseline.tx_bytes;
    out_sample->rx_packets =
        current.rx_packets - environment->baseline.rx_packets;
    out_sample->tx_packets =
        current.tx_packets - environment->baseline.tx_packets;
    out_sample->rx_dropped =
        current.rx_dropped - environment->baseline.rx_dropped;
    out_sample->tx_dropped =
        current.tx_dropped - environment->baseline.tx_dropped;
    out_sample->signal_mbm = current.signal_mbm;
    out_sample->signal_present = current.signal_present;
    out_sample->corrected_millic = environment->thermal.corrected_millic;
    out_sample->uncertainty_millic = environment->thermal.uncertainty_millic;
    out_sample->temperature_present = environment->thermal.present;
    return true;
}
