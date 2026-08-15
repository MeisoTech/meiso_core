#define _POSIX_C_SOURCE 200809L

#include "lab_protocol.h"
#include "meiso_blob.h"
#include "meiso_descriptor_set.h"
#include "meiso_posix_tcp.h"
#include "meiso_session.h"
#include "meiso_state.h"
#include "meiso_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    LAB_TCP_PORT = MEISO_LAB_TCP_PORT,
    LAB_MAX_SESSIONS = 16,
    LAB_PHASE_TIMEOUT_MS = 5000,
    LAB_THERMAL_PREFLIGHT_TIMEOUT_MS = 1000,
    LAB_POLL_SLICE_MS = 50,
    LAB_IO_CHUNK_BYTES = 1024,
    LAB_ARENA_BYTES = 4096,
    LAB_CONFIG_LINE_BYTES = 256
};

static const char LAB_ADDRESS_ENV[] = "MEISO_LAB_EDGE_ADDRESS";
static const char LAB_EXPECTED_EDGE[] = "four-kind-lab-edge-v0";

typedef union {
    max_align_t alignment;
    uint8_t bytes[LAB_ARENA_BYTES];
} LabArena;

typedef struct {
    LabArena session;
    LabArena adapter;
    LabArena descriptors;
    LabArena state;
    LabArena stream;
    LabArena action;
    LabArena blob;
} LabStorage;

typedef uint8_t LabTest;
enum {
    LAB_TEST_STATE = 1,
    LAB_TEST_STREAM = 2,
    LAB_TEST_ACTION = 3,
    LAB_TEST_BLOB = 4,
    LAB_TEST_ALL = 5
};

typedef uint8_t WifiOperstate;
enum {
    WIFI_OPERSTATE_UNKNOWN = 0,
    WIFI_OPERSTATE_DOWN = 1,
    WIFI_OPERSTATE_DORMANT = 2,
    WIFI_OPERSTATE_LOWERLAYERDOWN = 3,
    WIFI_OPERSTATE_NOTPRESENT = 4,
    WIFI_OPERSTATE_TESTING = 5,
    WIFI_OPERSTATE_UP = 6
};

typedef struct {
    bool interface_present;
    bool carrier;
    WifiOperstate operstate;
    bool signal_present;
    int32_t signal_mbm;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
} NetworkReading;

typedef struct {
    bool present;
    int32_t corrected_millic;
    uint32_t uncertainty_millic;
} ThermalReading;

typedef struct {
    pid_t pid;
    bool pending;
} ThermalChild;

typedef uint8_t ThermalChildMode;
enum {
    THERMAL_CHILD_ACTUAL = 1,
    THERMAL_CHILD_FIXTURE_PRESENT = 2,
    THERMAL_CHILD_FIXTURE_ABSENT = 3,
    THERMAL_CHILD_FIXTURE_TIMEOUT = 4,
    THERMAL_CHILD_FIXTURE_PARTIAL = 5,
    THERMAL_CHILD_FIXTURE_EARLY_EXIT = 6
};

typedef uint8_t ThermalPreflightStatus;
enum {
    THERMAL_PREFLIGHT_PRESENT = 1,
    THERMAL_PREFLIGHT_ABSENT = 2,
    THERMAL_PREFLIGHT_TIMEOUT = 3,
    THERMAL_PREFLIGHT_CHILD_ERROR = 4,
    THERMAL_PREFLIGHT_LOCAL_ERROR = 5
};

typedef struct {
    NetworkReading baseline;
    uint64_t started_ns;
    uint64_t window_id;
    ThermalReading thermal;
} TelemetryEnvironment;

typedef struct {
    struct in_addr address;
    uint16_t port;
} HostLocator;

typedef uint8_t LabLocalFailure;
enum {
    LAB_LOCAL_FAILURE_NONE = 0,
    LAB_LOCAL_FAILURE_TRANSPORT_OR_TIMEOUT = 1,
    LAB_LOCAL_FAILURE_PROTOCOL = 2
};

typedef struct {
    int fd;
    MeisoSessionConfig config;
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoLabOrder order;
    MeisoOwnerScope edge_scope;
    uint64_t stream_epoch;
    uint64_t clock_epoch;
    LabStorage *storage;
    LabLocalFailure local_failure;
} LabConnection;

typedef struct {
    MeisoDescriptorSet *descriptors;
    MeisoStateReplica *state;
    MeisoStream *stream;
    MeisoActionCaller *action;
    MeisoBlobStore *blob;
    MeisoDeliveryGeneration action_delivery;
    MeisoActionId action_id;
    MeisoOwnerScope edge_owner;
    MeisoLabEdgeState edge_state;
    bool edge_state_known;
    bool action_delivery_accepted;
    bool action_terminal;
} HostPrimitives;

typedef struct {
    MeisoStatePublisher *state;
    MeisoDeliveryGeneration state_delivery;
    MeisoOwnerScope state_owner;
    uint64_t state_version;
    bool state_delivery_active;
} EdgePrimitives;

typedef struct {
    MeisoOwnerScope scope;
    uint64_t stream_epoch;
    uint64_t clock_epoch;
    uint64_t state_version;
    uint64_t stream_sequence;
    TelemetryEnvironment telemetry;
    ThermalChild thermal_child;
    LabArena action_arena;
    MeisoActionExecutor *action;
    MeisoActionId active_action_id;
    bool active_action_nonterminal;
} EdgeProcess;

typedef struct {
    MeisoContentId expected_id;
    MeisoByteView expected_content;
    size_t calls;
} BlobVerifyContext;

static LabStorage g_storage;

#ifdef MEISO_LAB_INTERNAL_TEST
enum { LAB_SELF_TEST_SESSIONS = 2 };

typedef struct {
    bool enabled;
    bool fixed_action_id;
    size_t session_count;
    MeisoSessionId session_ids[LAB_SELF_TEST_SESSIONS];
    MeisoOwnerScope owners[LAB_SELF_TEST_SESSIONS];
    uint64_t state_versions[LAB_SELF_TEST_SESSIONS];
    uint64_t stream_epochs[LAB_SELF_TEST_SESSIONS];
    uint64_t stream_sequences[LAB_SELF_TEST_SESSIONS];
    uint64_t clock_epochs[LAB_SELF_TEST_SESSIONS];
    uint64_t action_windows[LAB_SELF_TEST_SESSIONS];
} LabSelfTestObservation;

static LabSelfTestObservation g_self_test;
#endif

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool set_nonblocking_cloexec(int fd)
{
    const int status_flags = fcntl(fd, F_GETFL);
    const int descriptor_flags = fcntl(fd, F_GETFD);

    return status_flags >= 0 && descriptor_flags >= 0 &&
        fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) == 0 &&
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

static bool monotonic_now_ns(uint64_t *out_value)
{
    struct timespec now;
    uint64_t seconds;

    if (out_value == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
        return false;
    }
    seconds = (uint64_t)now.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)now.tv_nsec) /
            UINT64_C(1000000000)) {
        return false;
    }
    *out_value = seconds * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return true;
}

static bool make_deadline_after_ms(
    struct timespec *out_deadline,
    unsigned int timeout_ms
)
{
    if (out_deadline == NULL ||
        clock_gettime(CLOCK_MONOTONIC, out_deadline) != 0) {
        return false;
    }
    out_deadline->tv_sec += (time_t)(timeout_ms / 1000U);
    out_deadline->tv_nsec +=
        (long)(timeout_ms % 1000U) * 1000000L;
    if (out_deadline->tv_nsec >= 1000000000L) {
        out_deadline->tv_nsec -= 1000000000L;
        out_deadline->tv_sec += (time_t)1;
    }
    return true;
}

static bool make_deadline(struct timespec *out_deadline)
{
    return make_deadline_after_ms(out_deadline, LAB_PHASE_TIMEOUT_MS);
}

static int deadline_wait_ms(const struct timespec *deadline)
{
    struct timespec now;
    int64_t remaining_ns;
    int64_t seconds;
    int64_t nanoseconds;
    int64_t milliseconds;

    if (deadline == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
    nanoseconds = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
    if (seconds < 0 || (seconds == 0 && nanoseconds <= 0)) {
        return 0;
    }
    if (nanoseconds < 0) {
        nanoseconds += INT64_C(1000000000);
        seconds -= 1;
    }
    if (seconds > INT64_C(2147482)) {
        return LAB_POLL_SLICE_MS;
    }
    remaining_ns = seconds * INT64_C(1000000000) + nanoseconds;
    milliseconds = (remaining_ns + INT64_C(999999)) / INT64_C(1000000);
    if (milliseconds > LAB_POLL_SLICE_MS) {
        milliseconds = LAB_POLL_SLICE_MS;
    }
    return milliseconds > 0 ? (int)milliseconds : 1;
}

static bool read_entropy(void *output, size_t size)
{
    uint8_t *bytes = output;
    size_t offset = 0U;
    int fd;

    if (output == NULL || size == 0U) {
        return false;
    }
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    while (offset < size) {
        const ssize_t count = read(fd, bytes + offset, size - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            close_fd(&fd);
            return false;
        }
    }
    close_fd(&fd);
    return true;
}

static bool random_nonzero_u64(uint64_t *out_value)
{
    uint64_t value;

    if (out_value == NULL || !read_entropy(&value, sizeof(value))) {
        return false;
    }
    value &= UINT64_C(0x7fffffffffffffff);
    *out_value = value == 0U ? 1U : value;
    return true;
}

static bool random_nonce(MeisoSessionNonce *out_nonce)
{
    if (out_nonce == NULL || !read_entropy(out_nonce, sizeof(*out_nonce))) {
        return false;
    }
    if (!meiso_session_nonce_valid(*out_nonce)) {
        out_nonce->low = 1U;
    }
    return true;
}

static bool random_action_id(MeisoActionId *out_id)
{
#ifdef MEISO_LAB_INTERNAL_TEST
    if (out_id != NULL && g_self_test.fixed_action_id) {
        *out_id = (MeisoActionId){
            UINT64_C(0x4d344c315f544553),
            UINT64_C(0x545f414354494f4e)
        };
        return true;
    }
#endif
    if (out_id == NULL || !read_entropy(out_id, sizeof(*out_id))) {
        return false;
    }
    if (!meiso_action_id_valid(*out_id)) {
        out_id->low = 1U;
    }
    return true;
}

static bool read_text_file(
    const char *path,
    char *output,
    size_t capacity
)
{
    FILE *file;
    size_t length;

    if (path == NULL || output == NULL || capacity < 2U) {
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    if (fgets(output, (int)capacity, file) == NULL) {
        (void)fclose(file);
        return false;
    }
    length = strlen(output);
    if (length == capacity - 1U && output[length - 1U] != '\n' &&
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

static bool parse_u64_text(const char *text_value, uint64_t *out_value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text_value == NULL || out_value == NULL || text_value[0] == '\0' ||
        text_value[0] == '-' || text_value[0] == '+') {
        return false;
    }
    errno = 0;
    parsed = strtoull(text_value, &end, 10);
    if (errno != 0 || end == text_value || *end != '\0') {
        return false;
    }
    *out_value = (uint64_t)parsed;
    return true;
}

static bool read_u64_file(const char *path, uint64_t *out_value)
{
    char text_value[64];
    return read_text_file(path, text_value, sizeof(text_value)) &&
        parse_u64_text(text_value, out_value);
}

static WifiOperstate parse_operstate(const char *value)
{
    if (strcmp(value, "up") == 0) {
        return WIFI_OPERSTATE_UP;
    }
    if (strcmp(value, "down") == 0) {
        return WIFI_OPERSTATE_DOWN;
    }
    if (strcmp(value, "dormant") == 0) {
        return WIFI_OPERSTATE_DORMANT;
    }
    if (strcmp(value, "lowerlayerdown") == 0) {
        return WIFI_OPERSTATE_LOWERLAYERDOWN;
    }
    if (strcmp(value, "notpresent") == 0) {
        return WIFI_OPERSTATE_NOTPRESENT;
    }
    if (strcmp(value, "testing") == 0) {
        return WIFI_OPERSTATE_TESTING;
    }
    return WIFI_OPERSTATE_UNKNOWN;
}

static bool read_wireless_signal(int32_t *out_signal_mbm, bool *out_present)
{
    FILE *file;
    char line[256];

    if (out_signal_mbm == NULL || out_present == NULL) {
        return false;
    }
    *out_present = false;
    *out_signal_mbm = 0;
    file = fopen("/proc/net/wireless", "r");
    if (file == NULL) {
        return true;
    }
    while (fgets(line, (int)sizeof(line), file) != NULL) {
        char *colon = strchr(line, ':');
        char *name;
        char *name_end;
        unsigned int status;
        double link;
        double level;

        if (colon == NULL) {
            continue;
        }
        name = line;
        while (*name == ' ' || *name == '\t') {
            name += 1;
        }
        name_end = colon;
        while (name_end > name &&
               (name_end[-1] == ' ' || name_end[-1] == '\t')) {
            name_end -= 1;
        }
        if ((size_t)(name_end - name) != 5U ||
            memcmp(name, "wlan0", 5U) != 0) {
            continue;
        }
        if (sscanf(colon + 1, " %x %lf %lf", &status, &link, &level) == 3 &&
            level >= -200.0 && level <= 0.0) {
            const double scaled = level * 100.0;
            (void)status;
            (void)link;
            *out_signal_mbm = scaled >= 0.0 ?
                (int32_t)(scaled + 0.5) : (int32_t)(scaled - 0.5);
            *out_present = *out_signal_mbm >= MEISO_LAB_SIGNAL_MIN_MBM &&
                *out_signal_mbm <= MEISO_LAB_SIGNAL_MAX_MBM;
        }
        break;
    }
    if (ferror(file)) {
        (void)fclose(file);
        return false;
    }
    return fclose(file) == 0;
}

static bool read_network(NetworkReading *out_reading)
{
    static const char OPERSTATE_PATH[] = "/sys/class/net/wlan0/operstate";
    static const char CARRIER_PATH[] = "/sys/class/net/wlan0/carrier";
    static const char RX_BYTES_PATH[] =
        "/sys/class/net/wlan0/statistics/rx_bytes";
    static const char TX_BYTES_PATH[] =
        "/sys/class/net/wlan0/statistics/tx_bytes";
    static const char RX_PACKETS_PATH[] =
        "/sys/class/net/wlan0/statistics/rx_packets";
    static const char TX_PACKETS_PATH[] =
        "/sys/class/net/wlan0/statistics/tx_packets";
    static const char RX_DROPPED_PATH[] =
        "/sys/class/net/wlan0/statistics/rx_dropped";
    static const char TX_DROPPED_PATH[] =
        "/sys/class/net/wlan0/statistics/tx_dropped";
    char operstate[32];
    uint64_t carrier;

    if (out_reading == NULL) {
        return false;
    }
    memset(out_reading, 0, sizeof(*out_reading));
    out_reading->operstate = WIFI_OPERSTATE_NOTPRESENT;
    if (!read_text_file(OPERSTATE_PATH, operstate, sizeof(operstate))) {
        return true;
    }
    out_reading->interface_present = true;
    out_reading->operstate = parse_operstate(operstate);
    if (!read_u64_file(CARRIER_PATH, &carrier) || carrier > 1U ||
        !read_u64_file(RX_BYTES_PATH, &out_reading->rx_bytes) ||
        !read_u64_file(TX_BYTES_PATH, &out_reading->tx_bytes) ||
        !read_u64_file(RX_PACKETS_PATH, &out_reading->rx_packets) ||
        !read_u64_file(TX_PACKETS_PATH, &out_reading->tx_packets) ||
        !read_u64_file(RX_DROPPED_PATH, &out_reading->rx_dropped) ||
        !read_u64_file(TX_DROPPED_PATH, &out_reading->tx_dropped) ||
        !read_wireless_signal(
            &out_reading->signal_mbm, &out_reading->signal_present
        )) {
        return false;
    }
    out_reading->carrier = carrier == 1U;
    return true;
}

static bool thermal_preflight_direct(ThermalReading *out_reading)
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
    if (!read_text_file(TYPE_PATH, type, sizeof(type)) ||
        strcmp(type, "canaan_thermal_zone") != 0 ||
        !read_u64_file(VALUE_PATH, &raw)) {
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
    converted_c = ((((1.01472e-10 * x - 1.10063e-6) * x + 4.36150e-3) *
        x - 7.10128) * x + 3565.87);
    if (converted_c < -40.0 || converted_c > 125.0) {
        return true;
    }
    converted_millic = converted_c * 1000.0;
    out_reading->corrected_millic = converted_millic >= 0.0 ?
        (int32_t)(converted_millic + 0.5) :
        (int32_t)(converted_millic - 0.5);
    out_reading->uncertainty_millic =
        MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC;
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
    return ((uint32_t)source[0] << 24U) |
        ((uint32_t)source[1] << 16U) |
        ((uint32_t)source[2] << 8U) |
        (uint32_t)source[3];
}

static void encode_thermal_result(
    const ThermalReading *reading,
    uint8_t output[THERMAL_RESULT_WIRE_SIZE]
)
{
    uint32_t temperature_bits;

    memset(output, 0, THERMAL_RESULT_WIRE_SIZE);
    put_u32_be(output, THERMAL_RESULT_MAGIC);
    output[4] = THERMAL_RESULT_VERSION;
    output[5] = reading->present ? 1U : 0U;
    memcpy(
        &temperature_bits,
        &reading->corrected_millic,
        sizeof(temperature_bits)
    );
    put_u32_be(output + 8U, temperature_bits);
    put_u32_be(output + 12U, reading->uncertainty_millic);
}

static bool decode_thermal_result(
    const uint8_t input[THERMAL_RESULT_WIRE_SIZE],
    ThermalReading *out_reading
)
{
    const uint32_t temperature_bits = get_u32_be(input + 8U);
    int32_t temperature;

    if (get_u32_be(input) != THERMAL_RESULT_MAGIC ||
        input[4] != THERMAL_RESULT_VERSION || input[5] > 1U ||
        input[6] != 0U || input[7] != 0U) {
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
        get_u32_be(input + 12U) !=
            MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC) {
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

static void thermal_child_run(int write_fd, ThermalChildMode mode)
{
    ThermalReading reading;
    uint8_t encoded[THERMAL_RESULT_WIRE_SIZE];

    memset(&reading, 0, sizeof(reading));
    if (mode == THERMAL_CHILD_FIXTURE_TIMEOUT) {
        for (;;) {
            (void)pause();
        }
    }
    if (mode == THERMAL_CHILD_FIXTURE_EARLY_EXIT) {
        _exit(0);
    }
    if (mode == THERMAL_CHILD_ACTUAL) {
        if (!thermal_preflight_direct(&reading)) {
            _exit(2);
        }
    } else if (mode == THERMAL_CHILD_FIXTURE_PRESENT) {
        reading.present = true;
        reading.corrected_millic = 52000;
        reading.uncertainty_millic =
            MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC;
    } else if (mode != THERMAL_CHILD_FIXTURE_ABSENT &&
               mode != THERMAL_CHILD_FIXTURE_PARTIAL) {
        _exit(2);
    }
    encode_thermal_result(&reading, encoded);
    if (mode == THERMAL_CHILD_FIXTURE_PARTIAL) {
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
    for (descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO;
         ++descriptor) {
        if (descriptor != write_fd) {
            (void)close(descriptor);
        }
    }
}

static bool make_thermal_pipe(int descriptors[2])
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
        fcntl(
            descriptors[0], F_SETFD, read_descriptor_flags | FD_CLOEXEC
        ) != 0 ||
        fcntl(
            descriptors[1], F_SETFD, write_descriptor_flags | FD_CLOEXEC
        ) != 0) {
        close_fd(&descriptors[0]);
        close_fd(&descriptors[1]);
        return false;
    }
    return true;
}

static void thermal_child_reap_nonblocking(ThermalChild *child)
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

static void thermal_child_terminate_nonblocking(ThermalChild *child)
{
    if (child == NULL || !child->pending) {
        return;
    }
    if (kill(child->pid, SIGKILL) != 0 && errno != ESRCH) {
        return;
    }
    thermal_child_reap_nonblocking(child);
}

static ThermalPreflightStatus thermal_preflight_bounded(
    ThermalReading *out_reading,
    ThermalChild *child,
    ThermalChildMode mode
)
{
    uint8_t input[THERMAL_RESULT_WIRE_SIZE + 1U];
    size_t input_size = 0U;
    struct timespec deadline;
    int descriptors[2];
    bool eof = false;
    pid_t pid;
    ThermalPreflightStatus status = THERMAL_PREFLIGHT_CHILD_ERROR;

    if (out_reading == NULL || child == NULL ||
        !make_deadline_after_ms(
            &deadline, LAB_THERMAL_PREFLIGHT_TIMEOUT_MS
        )) {
        return THERMAL_PREFLIGHT_LOCAL_ERROR;
    }
    memset(out_reading, 0, sizeof(*out_reading));
    child->pid = (pid_t)-1;
    child->pending = false;
    if (!make_thermal_pipe(descriptors)) {
        return THERMAL_PREFLIGHT_LOCAL_ERROR;
    }
    pid = fork();
    if (pid < 0) {
        close_fd(&descriptors[0]);
        close_fd(&descriptors[1]);
        return THERMAL_PREFLIGHT_LOCAL_ERROR;
    }
    if (pid == 0) {
        close_fd(&descriptors[0]);
        thermal_child_close_inherited_stdio(descriptors[1]);
        thermal_child_run(descriptors[1], mode);
    }
    child->pid = pid;
    child->pending = true;
    close_fd(&descriptors[1]);

    while (!eof) {
        struct pollfd poll_descriptor;
        ssize_t received;
        int wait_ms;
        int poll_result;

        received = read(
            descriptors[0], input + input_size, sizeof(input) - input_size
        );
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
        wait_ms = deadline_wait_ms(&deadline);
        if (wait_ms == 0) {
            status = THERMAL_PREFLIGHT_TIMEOUT;
            break;
        }
        poll_descriptor.fd = descriptors[0];
        poll_descriptor.events = (short)(POLLIN | POLLHUP);
        poll_descriptor.revents = 0;
        poll_result = poll(
            &poll_descriptor, (nfds_t)1U, wait_ms
        );
        if (poll_result < 0 && errno == EINTR) {
            continue;
        }
        if (poll_result < 0 ||
            (poll_descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            break;
        }
    }
    close_fd(&descriptors[0]);
    if (eof && input_size == THERMAL_RESULT_WIRE_SIZE &&
        decode_thermal_result(input, out_reading)) {
        status = out_reading->present ?
            THERMAL_PREFLIGHT_PRESENT : THERMAL_PREFLIGHT_ABSENT;
        thermal_child_reap_nonblocking(child);
    } else {
        memset(out_reading, 0, sizeof(*out_reading));
        thermal_child_terminate_nonblocking(child);
    }
    return status;
}

static bool initialize_telemetry(
    TelemetryEnvironment *environment,
    ThermalChild *thermal_child,
    ThermalChildMode thermal_mode
)
{
    ThermalPreflightStatus thermal_status;

    if (environment == NULL) {
        return false;
    }
    memset(environment, 0, sizeof(*environment));
    environment->window_id = 1U;
    thermal_status = thermal_preflight_bounded(
        &environment->thermal, thermal_child, thermal_mode
    );
    (void)thermal_status;
    return read_network(&environment->baseline) &&
        monotonic_now_ns(&environment->started_ns);
}

static bool counters_decreased(
    const NetworkReading *current,
    const NetworkReading *baseline
)
{
    return current->rx_bytes < baseline->rx_bytes ||
        current->tx_bytes < baseline->tx_bytes ||
        current->rx_packets < baseline->rx_packets ||
        current->tx_packets < baseline->tx_packets ||
        current->rx_dropped < baseline->rx_dropped ||
        current->tx_dropped < baseline->tx_dropped;
}

static bool reset_telemetry_window(TelemetryEnvironment *environment)
{
    NetworkReading current;
    uint64_t now;

    if (environment == NULL || environment->window_id == UINT64_MAX ||
        !read_network(&current) || !monotonic_now_ns(&now)) {
        return false;
    }
    environment->baseline = current;
    environment->started_ns = now;
    environment->window_id += 1U;
    return true;
}

static bool capture_telemetry(
    TelemetryEnvironment *environment,
    MeisoLabEdgeState *out_state,
    MeisoLabTelemetrySample *out_sample
)
{
    NetworkReading current;
    uint64_t now;

    if (environment == NULL || out_state == NULL || out_sample == NULL ||
        !read_network(&current) || !monotonic_now_ns(&now)) {
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

static bool parse_port(const char *value, uint16_t *out_port)
{
    uint64_t parsed;

    if (!parse_u64_text(value, &parsed) || parsed != LAB_TCP_PORT ||
        out_port == NULL) {
        return false;
    }
    *out_port = (uint16_t)parsed;
    return true;
}

static bool parse_host_config(const char *path, HostLocator *out_locator)
{
    FILE *file;
    char line[LAB_CONFIG_LINE_BYTES];
    bool have_address_env = false;
    bool have_expected_edge = false;
    bool have_port = false;
    bool read_error;
    int close_result;
    uint16_t port = 0U;
    const char *address_value;

    if (path == NULL || out_locator == NULL) {
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, (int)sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        char *equals;

        if (length == sizeof(line) - 1U && line[length - 1U] != '\n' &&
            !feof(file)) {
            (void)fclose(file);
            return false;
        }
        while (length > 0U &&
               (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
            line[length - 1U] = '\0';
            length -= 1U;
        }
        if (length == 0U || line[0] == '#') {
            continue;
        }
        equals = strchr(line, '=');
        if (equals == NULL || equals == line || equals[1] == '\0' ||
            strchr(equals + 1, '=') != NULL) {
            (void)fclose(file);
            return false;
        }
        *equals = '\0';
        if (strcmp(line, "address_env") == 0 && !have_address_env &&
            strcmp(equals + 1, LAB_ADDRESS_ENV) == 0) {
            have_address_env = true;
        } else if (strcmp(line, "expected_edge") == 0 &&
                   !have_expected_edge &&
                   strcmp(equals + 1, LAB_EXPECTED_EDGE) == 0) {
            have_expected_edge = true;
        } else if (strcmp(line, "port") == 0 && !have_port &&
                   parse_port(equals + 1, &port)) {
            have_port = true;
        } else {
            (void)fclose(file);
            return false;
        }
    }
    read_error = ferror(file) != 0;
    close_result = fclose(file);
    if (read_error || close_result != 0 || !have_address_env ||
        !have_expected_edge || !have_port) {
        return false;
    }
    address_value = getenv(LAB_ADDRESS_ENV);
    if (address_value == NULL || address_value[0] == '\0' ||
        inet_pton(AF_INET, address_value, &out_locator->address) != 1) {
        return false;
    }
    out_locator->port = port;
    return true;
}

static int connect_host(const HostLocator *locator)
{
    struct sockaddr_in address;
    struct timespec deadline;
    struct pollfd descriptor;
    int fd = -1;

    if (locator == NULL || !make_deadline(&deadline)) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || !set_nonblocking_cloexec(fd)) {
        close_fd(&fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(locator->port);
    address.sin_addr = locator->address;
    if (connect(
            fd,
            (const struct sockaddr *)(const void *)&address,
            (socklen_t)sizeof(address)
        ) != 0) {
        if (errno != EINPROGRESS) {
            close_fd(&fd);
            return -1;
        }
        for (;;) {
            int result;
            int socket_error = 0;
            socklen_t error_size = (socklen_t)sizeof(socket_error);
            const int wait_ms = deadline_wait_ms(&deadline);

            if (wait_ms == 0) {
                close_fd(&fd);
                return -1;
            }
            descriptor.fd = fd;
            descriptor.events = POLLOUT;
            descriptor.revents = 0;
            result = poll(&descriptor, (nfds_t)1U, wait_ms);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result < 0 ||
                (result > 0 && getsockopt(
                    fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size
                ) != 0)) {
                close_fd(&fd);
                return -1;
            }
            if (result > 0) {
                if (socket_error != 0 ||
                    (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) !=
                        0) {
                    close_fd(&fd);
                    return -1;
                }
                break;
            }
        }
    }
    return fd;
}

static int make_listener(uint16_t port)
{
    struct sockaddr_in address;
    int enabled = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0 || !set_nonblocking_cloexec(fd) ||
        setsockopt(
            fd, SOL_SOCKET, SO_REUSEADDR,
            &enabled, (socklen_t)sizeof(enabled)
        ) != 0) {
        close_fd(&fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(
            fd,
            (const struct sockaddr *)(const void *)&address,
            (socklen_t)sizeof(address)
        ) != 0 || listen(fd, 1) != 0) {
        close_fd(&fd);
        return -1;
    }
    return fd;
}

static int accept_edge(int listener)
{
    struct timespec deadline;
    struct pollfd descriptor;

    if (listener < 0 || !make_deadline(&deadline)) {
        return -1;
    }
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd >= 0) {
            if (!set_nonblocking_cloexec(fd)) {
                close_fd(&fd);
                return -1;
            }
            return fd;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
        descriptor.fd = listener;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        {
            const int wait_ms = deadline_wait_ms(&deadline);
            int result;
            if (wait_ms == 0) {
                return -1;
            }
            result = poll(&descriptor, (nfds_t)1U, wait_ms);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result < 0 ||
                (result > 0 &&
                 (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
                return -1;
            }
        }
    }
}

static MeisoSessionConfig session_config(MeisoSessionRole role)
{
    MeisoSessionConfig config;
    const MeisoPeerId host_peer = {
        MEISO_LAB_HOST_PEER_ID_HIGH, MEISO_LAB_HOST_PEER_ID_LOW
    };
    const MeisoPeerId edge_peer = {
        MEISO_LAB_EDGE_PEER_ID_HIGH, MEISO_LAB_EDGE_PEER_ID_LOW
    };

    memset(&config, 0, sizeof(config));
    config.local_role = role;
    config.local_peer_id = role == MEISO_SESSION_ROLE_HOST ?
        host_peer : edge_peer;
    config.expected_peer_id = role == MEISO_SESSION_ROLE_HOST ?
        edge_peer : host_peer;
    config.protocol = (MeisoProtocolVersion){
        MEISO_LAB_PROTOCOL_MAJOR, MEISO_LAB_PROTOCOL_MINOR
    };
    config.profile.id = (MeisoProfileId){
        MEISO_LAB_PROFILE_ID_HIGH, MEISO_LAB_PROFILE_ID_LOW
    };
    config.profile.version = MEISO_LAB_PROFILE_VERSION;
    config.limits = (MeisoSessionLimits){
        MEISO_LAB_SESSION_MAX_FRAME_BYTES,
        MEISO_LAB_SESSION_MAX_RUNTIME_PAYLOAD_BYTES,
        MEISO_LAB_SESSION_MAX_REGISTRY_ENTRIES
    };
    return config;
}

static bool initialize_connection(
    LabConnection *connection,
    int fd,
    MeisoSessionRole role,
    const EdgeProcess *edge_process
)
{
    MeisoPosixTcpConfig adapter_config;
    size_t required;

    if (connection == NULL || fd < 0 ||
        (role == MEISO_SESSION_ROLE_EDGE && edge_process == NULL)) {
        return false;
    }
    memset(&g_storage, 0, sizeof(g_storage));
    memset(connection, 0, sizeof(*connection));
    connection->fd = fd;
    connection->storage = &g_storage;
    connection->config = session_config(role);
    if (!random_nonce(&connection->config.local_nonce)) {
        return false;
    }
    if (role == MEISO_SESSION_ROLE_EDGE) {
        connection->config.local_incarnation = edge_process->scope.incarnation;
        connection->stream_epoch = edge_process->stream_epoch;
        connection->clock_epoch = edge_process->clock_epoch;
        connection->edge_scope = edge_process->scope;
    } else {
        if (!random_nonzero_u64(&connection->config.local_incarnation) ||
            !random_nonzero_u64(&connection->stream_epoch) ||
            !random_nonzero_u64(&connection->clock_epoch)) {
            return false;
        }
        connection->edge_scope = (MeisoOwnerScope){
            MEISO_OWNER_ROLE_EDGE, 1U
        };
    }
    required = meiso_session_memory_required(&connection->config);
    if (required == 0U || required > sizeof(connection->storage->session.bytes) ||
        meiso_session_init(
            connection->storage->session.bytes,
            sizeof(connection->storage->session.bytes),
            &connection->config,
            &connection->session
        ) != MEISO_SESSION_INIT_OK) {
        return false;
    }
    adapter_config.connected_fd = fd;
    adapter_config.session = connection->session;
    adapter_config.max_frame_bytes = MEISO_LAB_SESSION_MAX_FRAME_BYTES;
    adapter_config.max_io_bytes_per_step = LAB_IO_CHUNK_BYTES;
    required = meiso_posix_tcp_memory_required(&adapter_config);
    if (required == 0U || required > sizeof(connection->storage->adapter.bytes) ||
        meiso_posix_tcp_init(
            connection->storage->adapter.bytes,
            sizeof(connection->storage->adapter.bytes),
            &adapter_config,
            &connection->adapter
        ) != MEISO_POSIX_TCP_INIT_OK) {
        return false;
    }
    meiso_lab_order_init(&connection->order);
    return meiso_session_start(connection->session) ==
        MEISO_SESSION_START_STARTED;
}

static bool write_status_ok(MeisoPosixTcpWriteStatus status)
{
    return status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
        status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
        status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
        status == MEISO_POSIX_TCP_WRITE_FRAME_SENT ||
        status == MEISO_POSIX_TCP_WRITE_IDLE;
}

static bool read_status_progress(MeisoPosixTcpReadStatus status)
{
    return status == MEISO_POSIX_TCP_READ_PROGRESS ||
        status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
        status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
        status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS ||
        status == MEISO_POSIX_TCP_READ_PEER_CLOSED;
}

static void record_transport_failure(LabConnection *connection)
{
    if (connection != NULL &&
        connection->local_failure == LAB_LOCAL_FAILURE_NONE) {
        connection->local_failure = LAB_LOCAL_FAILURE_TRANSPORT_OR_TIMEOUT;
    }
}

static void record_protocol_failure(LabConnection *connection)
{
    if (connection != NULL) {
        connection->local_failure = LAB_LOCAL_FAILURE_PROTOCOL;
    }
}

static bool poll_adapter(
    LabConnection *connection,
    const struct timespec *deadline,
    MeisoSessionRuntimeMessage *out_runtime,
    bool *out_received_runtime
)
{
    struct pollfd descriptor;
    int result;
    int wait_ms;

    if (connection == NULL || deadline == NULL || out_received_runtime == NULL) {
        return false;
    }
    *out_received_runtime = false;
    descriptor.fd = connection->fd;
    descriptor.events = 0;
    descriptor.revents = 0;
    if (meiso_posix_tcp_wants_read(connection->adapter)) {
        descriptor.events = (short)(descriptor.events | POLLIN);
    }
    if (meiso_posix_tcp_wants_write(connection->adapter)) {
        descriptor.events = (short)(descriptor.events | POLLOUT);
    }
    wait_ms = deadline_wait_ms(deadline);
    if (wait_ms == 0 || descriptor.events == 0) {
        record_transport_failure(connection);
        return false;
    }
    result = poll(&descriptor, (nfds_t)1U, wait_ms);
    if (result < 0 && errno == EINTR) {
        return true;
    }
    if (result < 0 || (descriptor.revents & POLLNVAL) != 0) {
        record_transport_failure(connection);
        return false;
    }
    if ((descriptor.revents & POLLOUT) != 0) {
        if (!write_status_ok(meiso_posix_tcp_on_writable(connection->adapter))) {
            record_transport_failure(connection);
            return false;
        }
    }
    if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        MeisoSessionRuntimeMessage runtime;
        const MeisoPosixTcpReadStatus status = meiso_posix_tcp_on_readable(
            connection->adapter, &runtime
        );
        if (status == MEISO_POSIX_TCP_READ_RUNTIME) {
            if (out_runtime == NULL) {
                record_protocol_failure(connection);
                return false;
            }
            *out_runtime = runtime;
            *out_received_runtime = true;
        } else if (!read_status_progress(status)) {
            record_transport_failure(connection);
            return false;
        }
    }
    return true;
}

static bool wait_session_ready(LabConnection *connection)
{
    struct timespec deadline;

    if (!make_deadline(&deadline)) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        MeisoSessionRuntimeMessage runtime;
        bool received_runtime;

        if (!meiso_session_snapshot(connection->session, &snapshot)) {
            return false;
        }
        if (snapshot.state == MEISO_SESSION_STATE_READY &&
            snapshot.failure == MEISO_SESSION_FAILURE_NONE &&
            snapshot.remote_identity_known && snapshot.session_id_known &&
            meiso_peer_id_equal(
                snapshot.remote_peer_id, connection->config.expected_peer_id
            )) {
            if (connection->config.local_role == MEISO_SESSION_ROLE_HOST) {
                connection->edge_scope.incarnation =
                    snapshot.remote_incarnation;
            }
            return true;
        }
        if (snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED ||
            !poll_adapter(
                connection, &deadline, &runtime, &received_runtime
            ) || received_runtime) {
            return false;
        }
    }
}

static bool flush_outbound(LabConnection *connection)
{
    struct timespec deadline;

    if (!make_deadline(&deadline)) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        MeisoSessionRuntimeMessage runtime;
        bool received_runtime;

        if (!meiso_session_snapshot(connection->session, &snapshot) ||
            snapshot.state == MEISO_SESSION_STATE_FAILED) {
            return false;
        }
        if (!snapshot.outbound_pending) {
            return true;
        }
        if (!poll_adapter(
                connection, &deadline, &runtime, &received_runtime
            ) || received_runtime) {
            return false;
        }
    }
}

static bool send_lab_message(
    LabConnection *connection,
    MeisoLabSender sender,
    const MeisoLabMessage *message
)
{
    uint8_t encoded[MEISO_LAB_MAX_MESSAGE_SIZE];
    size_t encoded_size = 0U;
    const MeisoLabOrderStatus order_status = meiso_lab_order_advance(
        &connection->order, sender, message
    );

    if (order_status != MEISO_LAB_ORDER_ADVANCED &&
        order_status != MEISO_LAB_ORDER_COMPLETE) {
        record_protocol_failure(connection);
        return false;
    }
    if (meiso_lab_message_encode(
            message,
            (MeisoMutableBuffer){encoded, sizeof(encoded)},
            &encoded_size
        ) != MEISO_LAB_CODEC_OK) {
        record_protocol_failure(connection);
        return false;
    }
    if (meiso_session_submit_runtime(
            connection->session, (MeisoByteView){encoded, encoded_size}
        ) != MEISO_SESSION_SUBMIT_QUEUED) {
        record_transport_failure(connection);
        return false;
    }
    return flush_outbound(connection);
}

static bool receive_lab_message(
    LabConnection *connection,
    MeisoLabSender sender,
    MeisoLabMessage *out_message
)
{
    struct timespec deadline;

    if (out_message == NULL || !make_deadline(&deadline)) {
        return false;
    }
    for (;;) {
        MeisoSessionRuntimeMessage runtime;
        bool received_runtime;

        if (!poll_adapter(
                connection, &deadline, &runtime, &received_runtime
            )) {
            return false;
        }
        if (received_runtime) {
            const MeisoLabCodecStatus codec_status = meiso_lab_message_decode(
                runtime.payload, out_message
            );
            const MeisoLabOrderStatus order_status = codec_status ==
                    MEISO_LAB_CODEC_OK ?
                meiso_lab_order_advance(
                    &connection->order, sender, out_message
                ) : MEISO_LAB_ORDER_UNEXPECTED;
            if (codec_status != MEISO_LAB_CODEC_OK ||
                (order_status != MEISO_LAB_ORDER_ADVANCED &&
                 order_status != MEISO_LAB_ORDER_COMPLETE)) {
                record_protocol_failure(connection);
                return false;
            }
            return true;
        }
    }
}

static void best_effort_protocol_close(LabConnection *connection)
{
    MeisoSessionSnapshot snapshot;

    if (connection == NULL || connection->session == NULL ||
        !meiso_session_snapshot(connection->session, &snapshot) ||
        snapshot.state == MEISO_SESSION_STATE_FAILED ||
        snapshot.state == MEISO_SESSION_STATE_CLOSED) {
        return;
    }
    if (meiso_session_close(
            connection->session,
            connection->local_failure == LAB_LOCAL_FAILURE_PROTOCOL ?
                MEISO_SESSION_CLOSE_PROTOCOL_ERROR :
                MEISO_SESSION_CLOSE_SHUTDOWN
        ) == MEISO_SESSION_CLOSE_QUEUED) {
        (void)flush_outbound(connection);
    }
}

static bool normal_host_close(LabConnection *connection)
{
    return meiso_session_close(
            connection->session, MEISO_SESSION_CLOSE_NORMAL
        ) == MEISO_SESSION_CLOSE_QUEUED && flush_outbound(connection);
}

static bool edge_wait_peer_close(LabConnection *connection)
{
    struct timespec deadline;

    if (!make_deadline(&deadline)) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        MeisoSessionRuntimeMessage runtime;
        bool received_runtime;

        if (!meiso_session_snapshot(connection->session, &snapshot)) {
            return false;
        }
        if (snapshot.state == MEISO_SESSION_STATE_CLOSED &&
            snapshot.failure == MEISO_SESSION_FAILURE_PEER_CLOSED &&
            snapshot.peer_close_reason_known &&
            snapshot.peer_close_reason == MEISO_SESSION_CLOSE_NORMAL) {
            return true;
        }
        if (snapshot.state == MEISO_SESSION_STATE_FAILED ||
            !poll_adapter(
                connection, &deadline, &runtime, &received_runtime
            ) || received_runtime) {
            return false;
        }
    }
}

static const char *signal_bucket_name(
    bool present,
    int32_t signal_mbm
)
{
    if (!present) {
        return "UNKNOWN";
    }
    if (signal_mbm < -8000) {
        return "POOR";
    }
    if (signal_mbm < -7000) {
        return "FAIR";
    }
    if (signal_mbm < -6000) {
        return "GOOD";
    }
    return "EXCELLENT";
}

static void emit_session_ready(const LabConnection *connection)
{
    MeisoSessionSnapshot snapshot;

    if (!meiso_session_snapshot(connection->session, &snapshot)) {
        return;
    }
    (void)printf(
        "{\"type\":\"session\",\"layer\":\"session\","
        "\"phase\":\"handshake\",\"status\":\"READY\","
        "\"state\":\"READY\",\"failure\":\"NONE\","
        "\"remote_identity_known\":true,\"identity_match\":true,"
        "\"session_id_known\":true,\"limits\":{"
        "\"max_frame_bytes\":%" PRIu32 ","
        "\"max_runtime_payload_bytes\":%" PRIu32 ","
        "\"max_registry_entries\":%" PRIu32 "}}\n",
        snapshot.negotiated_limits.max_frame_bytes,
        snapshot.negotiated_limits.max_runtime_payload_bytes,
        snapshot.negotiated_limits.max_registry_entries
    );
    (void)fflush(stdout);
}

static void emit_session_failed(void)
{
    (void)printf(
        "{\"type\":\"session\",\"layer\":\"session\","
        "\"phase\":\"failed\",\"status\":\"FAILED\","
        "\"state\":\"FAILED\",\"failure\":\"TRANSPORT_LOST\"}\n"
    );
    (void)fflush(stdout);
}

static const char *session_failure_name(MeisoSessionFailure failure)
{
    switch (failure) {
    case MEISO_SESSION_FAILURE_MALFORMED_FRAME:
        return "MALFORMED_FRAME";
    case MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE:
        return "UNEXPECTED_MESSAGE";
    case MEISO_SESSION_FAILURE_ROLE_MISMATCH:
        return "ROLE_MISMATCH";
    case MEISO_SESSION_FAILURE_PEER_MISMATCH:
        return "PEER_MISMATCH";
    case MEISO_SESSION_FAILURE_INVALID_INCARNATION:
        return "INVALID_INCARNATION";
    case MEISO_SESSION_FAILURE_INVALID_NONCE:
        return "INVALID_NONCE";
    case MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH:
        return "PROTOCOL_MISMATCH";
    case MEISO_SESSION_FAILURE_PROFILE_MISMATCH:
        return "PROFILE_MISMATCH";
    case MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE:
        return "LIMITS_INCOMPATIBLE";
    case MEISO_SESSION_FAILURE_SESSION_MISMATCH:
        return "SESSION_MISMATCH";
    case MEISO_SESSION_FAILURE_EARLY_RUNTIME:
        return "EARLY_RUNTIME";
    case MEISO_SESSION_FAILURE_LOCAL_CLOSED:
        return "LOCAL_CLOSED";
    case MEISO_SESSION_FAILURE_PEER_CLOSED:
        return "PEER_CLOSED";
    case MEISO_SESSION_FAILURE_TRANSPORT_LOST:
    case MEISO_SESSION_FAILURE_NONE:
    default:
        return "TRANSPORT_LOST";
    }
}

static void emit_connection_failed(const LabConnection *connection)
{
    MeisoSessionSnapshot snapshot;
    const char *failure = "TRANSPORT_LOST";

    if (connection != NULL && connection->session != NULL &&
        meiso_session_snapshot(connection->session, &snapshot)) {
        failure = session_failure_name(snapshot.failure);
    }
    (void)printf(
        "{\"type\":\"session\",\"layer\":\"session\","
        "\"phase\":\"failed\",\"status\":\"FAILED\","
        "\"state\":\"FAILED\",\"failure\":\"%s\"}\n",
        failure
    );
    (void)fflush(stdout);
}

static void emit_transcript_failed(const LabConnection *connection)
{
    MeisoSessionSnapshot snapshot;
    const char *failure;

    if (connection != NULL && connection->session != NULL &&
        meiso_session_snapshot(connection->session, &snapshot) &&
        snapshot.failure != MEISO_SESSION_FAILURE_NONE) {
        failure = session_failure_name(snapshot.failure);
    } else if (connection != NULL &&
               connection->local_failure == LAB_LOCAL_FAILURE_PROTOCOL) {
        failure = "UNEXPECTED_MESSAGE";
    } else {
        failure = "TRANSPORT_LOST";
    }
    (void)printf(
        "{\"type\":\"session\",\"layer\":\"session\","
        "\"phase\":\"failed\",\"status\":\"FAILED\","
        "\"state\":\"FAILED\",\"failure\":\"%s\"}\n",
        failure
    );
    (void)fflush(stdout);
}

static void emit_session_closed(const LabConnection *connection)
{
    MeisoSessionSnapshot snapshot;
    const char *failure = "UNKNOWN";

    if (connection != NULL && connection->session != NULL &&
        meiso_session_snapshot(connection->session, &snapshot)) {
        failure = session_failure_name(snapshot.failure);
    }
    (void)printf(
        "{\"type\":\"session\",\"layer\":\"session\","
        "\"phase\":\"closed\",\"status\":\"CLOSED\","
        "\"state\":\"CLOSED\",\"failure\":\"%s\"}\n",
        failure
    );
    (void)fflush(stdout);
}

static void emit_registry(void)
{
    (void)printf(
        "{\"type\":\"registry\",\"layer\":\"registry\","
        "\"phase\":\"accepted\",\"status\":\"ACCEPTED\","
        "\"state\":\"FIXTURE\",\"entries\":4,\"capacity\":4,"
        "\"source\":\"fixture\"}\n"
    );
    (void)fflush(stdout);
}

static void emit_state_event(
    uint64_t version,
    const MeisoLabEdgeState *state
)
{
    (void)printf(
        "{\"type\":\"state\",\"layer\":\"primitive\","
        "\"direction\":\"edge_to_host\",\"phase\":\"snapshot\","
        "\"status\":\"ACCEPTED\",\"name\":\"EdgeState\","
        "\"version\":%" PRIu64 ",\"window_id\":%" PRIu64 ","
        "\"interface_present\":%s,\"carrier_present\":%s",
        version,
        state->window_id,
        state->interface_present ? "true" : "false",
        state->carrier_present ? "true" : "false"
    );
    if (state->temperature_present) {
        (void)printf(
            ",\"temperature_millic\":%" PRId32
            ",\"temperature_uncertainty_millic\":%" PRIu32,
            state->corrected_millic,
            state->uncertainty_millic
        );
    }
    (void)printf(",\"verified\":true}\n");
    (void)fflush(stdout);
}

static void emit_stream_event(
    const MeisoLabStreamMetrics *metrics,
    uint64_t receive_time_ns
)
{
    const MeisoLabTelemetrySample *sample = &metrics->value;

    (void)printf(
        "{\"type\":\"stream\",\"layer\":\"primitive\","
        "\"direction\":\"edge_to_host\",\"phase\":\"sample\","
        "\"status\":\"ACCEPTED\",\"name\":\"TelemetrySample\","
        "\"monotonic_ns\":%" PRIu64 ","
        "\"epoch\":%" PRIu64 ",\"sequence\":%" PRIu64 ","
        "\"source_time_ns\":%" PRIu64 ","
        "\"window_id\":%" PRIu64 ",\"elapsed_ns\":%" PRIu64 ","
        "\"rx_bytes\":%" PRIu64 ",\"tx_bytes\":%" PRIu64 ","
        "\"rx_packets\":%" PRIu64 ",\"tx_packets\":%" PRIu64 ","
        "\"rx_dropped\":%" PRIu64 ",\"tx_dropped\":%" PRIu64 ","
        "\"signal_bucket\":\"%s\"",
        receive_time_ns,
        metrics->epoch,
        metrics->sequence,
        metrics->source_time.monotonic_ns,
        sample->window_id,
        sample->elapsed_ns,
        sample->rx_bytes,
        sample->tx_bytes,
        sample->rx_packets,
        sample->tx_packets,
        sample->rx_dropped,
        sample->tx_dropped,
        signal_bucket_name(sample->signal_present, sample->signal_mbm)
    );
    if (sample->temperature_present) {
        (void)printf(
            ",\"temperature_millic\":%" PRId32
            ",\"temperature_uncertainty_millic\":%" PRIu32,
            sample->corrected_millic,
            sample->uncertainty_millic
        );
    }
    (void)printf(",\"verified\":true}\n");
    (void)fflush(stdout);
}

static void emit_action_event(uint64_t new_window_id)
{
    (void)printf(
        "{\"type\":\"action\",\"layer\":\"primitive\","
        "\"direction\":\"host_to_edge\",\"phase\":\"result\","
        "\"status\":\"SUCCEEDED\","
        "\"operation\":\"telemetry.reset_window\","
        "\"delivery_accepted\":true,\"edge_accepted\":true,"
        "\"executed_once\":true,\"result_code\":\"OK\","
        "\"result_value\":%" PRIu64 ",\"verified\":true}\n",
        new_window_id
    );
    (void)fflush(stdout);
}

static void emit_blob_event(size_t content_size)
{
    (void)printf(
        "{\"type\":\"blob\",\"layer\":\"primitive\","
        "\"direction\":\"edge_to_host\",\"phase\":\"published\","
        "\"status\":\"PUBLISHED\","
        "\"label\":\"edge.capability.manifest\","
        "\"content_size\":%zu,\"integrity_verified\":true,"
        "\"published\":true,\"duplicate\":false,"
        "\"verified\":true}\n",
        content_size
    );
    (void)fflush(stdout);
}

static const char *test_name(LabTest test)
{
    switch (test) {
    case LAB_TEST_STATE:
        return "state";
    case LAB_TEST_STREAM:
        return "stream";
    case LAB_TEST_ACTION:
        return "action";
    case LAB_TEST_BLOB:
        return "blob";
    case LAB_TEST_ALL:
    default:
        return "all";
    }
}

static void emit_one_verdict(const char *name, bool passed, unsigned int checks)
{
    (void)printf(
        "{\"type\":\"verification\",\"layer\":\"fixture\","
        "\"phase\":\"verified\",\"status\":\"%s\","
        "\"test\":\"%s\",\"result\":\"%s\","
        "\"checks_passed\":%u,\"checks_total\":%u}\n",
        passed ? "PASS" : "FAIL",
        name,
        passed ? "PASS" : "FAIL",
        passed ? checks : 0U,
        checks
    );
}

static void emit_verdicts(LabTest test, bool passed)
{
    if (test == LAB_TEST_ALL) {
        emit_one_verdict("state", passed, 3U);
        emit_one_verdict("stream", passed, 3U);
        emit_one_verdict("action", passed, 4U);
        emit_one_verdict("blob", passed, 3U);
    } else {
        const unsigned int checks = test == LAB_TEST_ACTION ? 4U : 3U;
        emit_one_verdict(test_name(test), passed, checks);
    }
    (void)fflush(stdout);
}

static void emit_stream_consumed(const MeisoLabStreamMetrics *metrics)
{
    (void)printf(
        "{\"type\":\"stream\",\"layer\":\"primitive\","
        "\"direction\":\"local\",\"phase\":\"applied\","
        "\"status\":\"ACCEPTED\",\"name\":\"TelemetrySample\","
        "\"epoch\":%" PRIu64 ",\"sequence\":%" PRIu64 ","
        "\"verified\":true}\n",
        metrics->epoch,
        metrics->sequence
    );
    (void)fflush(stdout);
}

static void emit_action_delivery_accepted(void)
{
    (void)printf(
        "{\"type\":\"action\",\"layer\":\"primitive\","
        "\"direction\":\"edge_to_host\",\"phase\":\"accepted\","
        "\"status\":\"ACCEPTED\","
        "\"operation\":\"telemetry.reset_window\","
        "\"delivery_accepted\":true,\"edge_accepted\":false,"
        "\"executed_once\":false,\"verified\":true}\n"
    );
    (void)fflush(stdout);
}

static void emit_action_executor_accepted(void)
{
    (void)printf(
        "{\"type\":\"action\",\"layer\":\"primitive\","
        "\"direction\":\"edge_to_host\",\"phase\":\"result\","
        "\"status\":\"ACCEPTED\","
        "\"operation\":\"telemetry.reset_window\","
        "\"delivery_accepted\":true,\"edge_accepted\":true,"
        "\"executed_once\":false,\"verified\":true}\n"
    );
    (void)fflush(stdout);
}

static void emit_blob_staged(size_t content_size)
{
    (void)printf(
        "{\"type\":\"blob\",\"layer\":\"primitive\","
        "\"direction\":\"local\",\"phase\":\"received\","
        "\"status\":\"RUNNING\","
        "\"label\":\"edge.capability.manifest\","
        "\"content_size\":%zu,\"integrity_verified\":false,"
        "\"published\":false,\"duplicate\":false,"
        "\"verified\":false}\n",
        content_size
    );
    (void)fflush(stdout);
}

static void write_u64_be(uint8_t *destination, uint64_t value)
{
    destination[0] = (uint8_t)(value >> 56U);
    destination[1] = (uint8_t)(value >> 48U);
    destination[2] = (uint8_t)(value >> 40U);
    destination[3] = (uint8_t)(value >> 32U);
    destination[4] = (uint8_t)(value >> 24U);
    destination[5] = (uint8_t)(value >> 16U);
    destination[6] = (uint8_t)(value >> 8U);
    destination[7] = (uint8_t)value;
}

static uint64_t read_u64_be(const uint8_t *source)
{
    return ((uint64_t)source[0] << 56U) |
        ((uint64_t)source[1] << 48U) |
        ((uint64_t)source[2] << 40U) |
        ((uint64_t)source[3] << 32U) |
        ((uint64_t)source[4] << 24U) |
        ((uint64_t)source[5] << 16U) |
        ((uint64_t)source[6] << 8U) |
        (uint64_t)source[7];
}

static bool bytes_equal(MeisoByteView lhs, MeisoByteView rhs)
{
    return lhs.size == rhs.size &&
        (lhs.size == 0U || memcmp(lhs.data, rhs.data, lhs.size) == 0);
}

static bool verify_fixed_blob(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    BlobVerifyContext *verify = context;

    if (verify == NULL) {
        return false;
    }
    verify->calls += 1U;
    return meiso_content_id_equal(
            expected_content_id, &verify->expected_id
        ) && bytes_equal(staged_content, verify->expected_content);
}

static bool initialize_host_primitives(
    LabConnection *connection,
    HostPrimitives *host
)
{
    const MeisoResourceDescriptor *descriptors;
    const MeisoResourceDescriptor *state_descriptor;
    const MeisoResourceDescriptor *stream_descriptor;
    const MeisoResourceDescriptor *action_descriptor;
    const MeisoResourceDescriptor *blob_descriptor;
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_REJECT_NEWEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        1U,
        0U,
        0U
    };
    const MeisoActionConfig action_config = {1U, 0U, 8U};
    const MeisoBlobConfig blob_config = {1U};
    size_t descriptor_count = 0U;
    size_t required;
    size_t index;

    if (connection == NULL || host == NULL) {
        return false;
    }
    memset(host, 0, sizeof(*host));
    descriptors = meiso_lab_descriptors(&descriptor_count);
    state_descriptor = meiso_lab_descriptor(MEISO_RESOURCE_KIND_STATE);
    stream_descriptor = meiso_lab_descriptor(MEISO_RESOURCE_KIND_STREAM);
    action_descriptor = meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    blob_descriptor = meiso_lab_descriptor(MEISO_RESOURCE_KIND_BLOB);
    if (descriptors == NULL || descriptor_count != MEISO_LAB_DESCRIPTOR_COUNT ||
        state_descriptor == NULL || stream_descriptor == NULL ||
        action_descriptor == NULL || blob_descriptor == NULL) {
        return false;
    }

    required = meiso_descriptor_set_memory_required(descriptor_count);
    if (required == 0U ||
        required > sizeof(connection->storage->descriptors.bytes) ||
        meiso_descriptor_set_init(
            connection->storage->descriptors.bytes,
            sizeof(connection->storage->descriptors.bytes),
            descriptor_count,
            &host->descriptors
        ) != MEISO_DESCRIPTOR_SET_INIT_OK) {
        return false;
    }
    for (index = 0U; index < descriptor_count; ++index) {
        if (meiso_descriptor_set_register(
                host->descriptors, &descriptors[index]
            ) != MEISO_DESCRIPTOR_SET_REGISTERED) {
            return false;
        }
    }
    if (meiso_descriptor_set_seal(host->descriptors) !=
        MEISO_DESCRIPTOR_SET_SEALED) {
        return false;
    }

    required = meiso_state_replica_memory_required(state_descriptor);
    if (required == 0U || required > sizeof(connection->storage->state.bytes) ||
        meiso_state_replica_init(
            connection->storage->state.bytes,
            sizeof(connection->storage->state.bytes),
            state_descriptor,
            &host->state
        ) != MEISO_STATE_INIT_OK) {
        return false;
    }
    required = meiso_stream_memory_required(stream_descriptor, &stream_config);
    if (required == 0U || required > sizeof(connection->storage->stream.bytes) ||
        meiso_stream_init(
            connection->storage->stream.bytes,
            sizeof(connection->storage->stream.bytes),
            stream_descriptor,
            &stream_config,
            &host->stream
        ) != MEISO_STREAM_INIT_OK) {
        return false;
    }
    required = meiso_action_caller_memory_required(
        action_descriptor, &action_config
    );
    if (required == 0U || required > sizeof(connection->storage->action.bytes) ||
        meiso_action_caller_init(
            connection->storage->action.bytes,
            sizeof(connection->storage->action.bytes),
            action_descriptor,
            &action_config,
            &host->action
        ) != MEISO_ACTION_INIT_OK) {
        return false;
    }
    required = meiso_blob_store_memory_required(blob_descriptor, &blob_config);
    if (required == 0U || required > sizeof(connection->storage->blob.bytes) ||
        meiso_blob_store_init(
            connection->storage->blob.bytes,
            sizeof(connection->storage->blob.bytes),
            blob_descriptor,
            &blob_config,
            &host->blob
        ) != MEISO_BLOB_INIT_OK) {
        return false;
    }
    host->action_delivery.value = 1U;
    return random_action_id(&host->action_id) &&
        meiso_action_caller_begin_delivery(
            host->action, host->action_delivery
        ) == MEISO_ACTION_DELIVERY_STARTED;
}

static bool initialize_edge_primitives(
    LabConnection *connection,
    EdgePrimitives *edge
)
{
    const MeisoResourceDescriptor *state_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STATE);
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    size_t required;

    if (connection == NULL || edge == NULL || state_descriptor == NULL) {
        return false;
    }
    memset(edge, 0, sizeof(*edge));
    edge->state_delivery.value = 1U;
    required = meiso_state_publisher_memory_required(
        state_descriptor, &state_config
    );
    if (required == 0U || required > sizeof(connection->storage->state.bytes) ||
        meiso_state_publisher_init(
            connection->storage->state.bytes,
            sizeof(connection->storage->state.bytes),
            state_descriptor,
            &state_config,
            &edge->state
        ) != MEISO_STATE_INIT_OK) {
        return false;
    }
    return true;
}

static bool initialize_edge_process_with_thermal_mode(
    EdgeProcess *process,
    ThermalChildMode thermal_mode
)
{
    const MeisoResourceDescriptor *action_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    const MeisoActionConfig action_config = {
        LAB_MAX_SESSIONS, 0U, 8U
    };
    size_t required;

    if (process == NULL || action_descriptor == NULL) {
        return false;
    }
    memset(process, 0, sizeof(*process));
    process->thermal_child.pid = (pid_t)-1;
    process->scope.role = MEISO_OWNER_ROLE_EDGE;
    if (!random_nonzero_u64(&process->scope.incarnation) ||
        !random_nonzero_u64(&process->stream_epoch) ||
        !random_nonzero_u64(&process->clock_epoch) ||
        !initialize_telemetry(
            &process->telemetry, &process->thermal_child, thermal_mode
        )) {
        thermal_child_terminate_nonblocking(&process->thermal_child);
        return false;
    }
    required = meiso_action_executor_memory_required(
        action_descriptor, &action_config
    );
    if (required == 0U || required > sizeof(process->action_arena.bytes) ||
        meiso_action_executor_init(
            process->action_arena.bytes,
            sizeof(process->action_arena.bytes),
            action_descriptor,
            &action_config,
            process->scope,
            &process->action
        ) != MEISO_ACTION_INIT_OK) {
        thermal_child_terminate_nonblocking(&process->thermal_child);
        return false;
    }
    return true;
}

static bool initialize_edge_process(EdgeProcess *process)
{
    return initialize_edge_process_with_thermal_mode(
        process, THERMAL_CHILD_ACTUAL
    );
}

#ifdef MEISO_LAB_INTERNAL_TEST
static bool self_test_observe_ready(const LabConnection *connection)
{
    MeisoSessionSnapshot snapshot;
    size_t index;

    if (!g_self_test.enabled) {
        return true;
    }
    if (connection == NULL || connection->session == NULL ||
        g_self_test.session_count >= LAB_SELF_TEST_SESSIONS ||
        !meiso_session_snapshot(connection->session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_READY ||
        !snapshot.session_id_known) {
        return false;
    }
    index = g_self_test.session_count;
    g_self_test.session_ids[index] = snapshot.session_id;
    g_self_test.session_count += 1U;
    return true;
}

static bool self_test_observation_index(size_t *out_index)
{
    if (!g_self_test.enabled) {
        return true;
    }
    if (out_index == NULL || g_self_test.session_count == 0U ||
        g_self_test.session_count > LAB_SELF_TEST_SESSIONS) {
        return false;
    }
    *out_index = g_self_test.session_count - 1U;
    return true;
}
#endif

static bool host_receive_registry(
    LabConnection *connection,
    HostPrimitives *host
)
{
    MeisoLabMessage received;
    MeisoLabMessage response;

    if (!receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received
        ) || received.type != MEISO_LAB_MESSAGE_REGISTRY ||
        received.body.registry.descriptor_count !=
            MEISO_LAB_DESCRIPTOR_COUNT ||
        !initialize_host_primitives(connection, host)) {
        return false;
    }
    emit_registry();
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_REGISTRY_ACCEPT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.registry_accept.descriptor_count =
        MEISO_LAB_DESCRIPTOR_COUNT;
    return send_lab_message(connection, MEISO_LAB_SENDER_HOST, &response);
}

static bool host_apply_state_actual(
    LabConnection *connection,
    HostPrimitives *host,
    const MeisoLabMessage *received,
    MeisoStateView *out_current
)
{
    uint8_t payload[MEISO_LAB_EDGE_STATE_WIRE_SIZE];
    size_t payload_size = 0U;

    if (connection == NULL || host == NULL || received == NULL ||
        out_current == NULL ||
        !meiso_owner_scope_equal(
            received->body.state_actual.owner, connection->edge_scope
        ) ||
        meiso_lab_edge_state_encode(
            &received->body.state_actual.value,
            (MeisoMutableBuffer){payload, sizeof(payload)},
            &payload_size
        ) != MEISO_LAB_CODEC_OK ||
        meiso_state_replica_begin_owner(
            host->state, received->body.state_actual.owner
        ) != MEISO_STATE_OWNER_STARTED ||
        meiso_state_replica_receive(
            host->state,
            received->body.state_actual.owner,
            received->body.state_actual.version,
            (MeisoByteView){payload, payload_size}
        ) != MEISO_STATE_RECEIVE_NEW ||
        !meiso_state_replica_current(host->state, out_current) ||
        out_current->version != received->body.state_actual.version ||
        !bytes_equal(
            out_current->payload, (MeisoByteView){payload, payload_size}
        )) {
        return false;
    }
    host->edge_state = received->body.state_actual.value;
    host->edge_owner = received->body.state_actual.owner;
    host->edge_state_known = true;
#ifdef MEISO_LAB_INTERNAL_TEST
    if (g_self_test.enabled) {
        size_t index;
        if (!self_test_observation_index(&index)) {
            return false;
        }
        g_self_test.owners[index] = host->edge_owner;
        g_self_test.state_versions[index] = received->body.state_actual.version;
    }
#endif
    return true;
}

static bool host_receive_state(
    LabConnection *connection,
    HostPrimitives *host
)
{
    MeisoStateView current;
    MeisoLabMessage received;
    MeisoLabMessage response;

    if (!receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received
        ) || received.type != MEISO_LAB_MESSAGE_STATE_ACTUAL ||
        !host_apply_state_actual(connection, host, &received, &current)) {
        return false;
    }
    emit_state_event(current.version, &host->edge_state);
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_STATE_ACCEPT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.state_accept.owner = received.body.state_actual.owner;
    response.body.state_accept.version = received.body.state_actual.version;
    return send_lab_message(connection, MEISO_LAB_SENDER_HOST, &response);
}

static bool host_receive_stream(
    LabConnection *connection,
    HostPrimitives *host,
    MeisoLabStreamMetrics *out_metrics
)
{
    uint8_t payload[MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE];
    size_t payload_size = 0U;
    MeisoStreamSample sample;
    MeisoStreamSample queued;
    MeisoStreamReceiveResult result;
    MeisoLabMessage received;
    uint64_t receive_time_ns;

    if (out_metrics == NULL || !host->edge_state_known ||
        !receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received
        ) || received.type != MEISO_LAB_MESSAGE_STREAM_METRICS ||
        !monotonic_now_ns(&receive_time_ns)) {
        return false;
    }
    if (meiso_lab_telemetry_sample_encode(
            &received.body.stream_metrics.value,
            (MeisoMutableBuffer){payload, sizeof(payload)},
            &payload_size
        ) != MEISO_LAB_CODEC_OK ||
        !meiso_owner_scope_equal(
            received.body.stream_metrics.producer, host->edge_owner
        ) ||
        meiso_stream_begin_producer(
            host->stream,
            received.body.stream_metrics.producer,
            received.body.stream_metrics.epoch
        ) != MEISO_STREAM_PRODUCER_STARTED) {
        return false;
    }
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){
        received.body.stream_metrics.producer,
        received.body.stream_metrics.epoch,
        received.body.stream_metrics.sequence
    };
    sample.source_time = received.body.stream_metrics.source_time;
    sample.payload = (MeisoByteView){payload, payload_size};
    result = meiso_stream_receive(host->stream, &sample, NULL);
    if (result.code != MEISO_STREAM_RECEIVE_ACCEPTED ||
        received.body.stream_metrics.sequence == 0U ||
        result.missing_before != received.body.stream_metrics.sequence - 1U ||
        result.missing_before >= LAB_MAX_SESSIONS ||
        meiso_stream_count(host->stream) != 1U ||
        !meiso_stream_peek(host->stream, &queued) ||
        !bytes_equal(queued.payload, sample.payload)) {
        return false;
    }
    *out_metrics = received.body.stream_metrics;
#ifdef MEISO_LAB_INTERNAL_TEST
    if (g_self_test.enabled) {
        size_t index;
        if (!self_test_observation_index(&index)) {
            return false;
        }
        g_self_test.stream_epochs[index] = out_metrics->epoch;
        g_self_test.stream_sequences[index] = out_metrics->sequence;
        g_self_test.clock_epochs[index] = out_metrics->source_time.clock_epoch;
    }
#endif
    emit_stream_event(out_metrics, receive_time_ns);
    return true;
}

static bool host_send_action(
    LabConnection *connection,
    HostPrimitives *host
)
{
    const MeisoResourceDescriptor *stream_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STREAM);
    const MeisoResourceDescriptor *action_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    MeisoActionRequest request;
    MeisoActionOffer offer;
    MeisoLabMessage message;
    size_t cursor = 0U;

    if (stream_descriptor == NULL || action_descriptor == NULL) {
        return false;
    }
    memset(&request, 0, sizeof(request));
    request.id = host->action_id;
    request.expected_executor = connection->edge_scope;
    request.target = stream_descriptor->id;
    request.operation = action_descriptor->schema;
    request.retry_policy = MEISO_ACTION_RETRY_NONE;
    if (meiso_action_caller_submit(host->action, &request) !=
            MEISO_ACTION_SUBMIT_NEW ||
        !meiso_action_caller_next_offer(
            host->action, host->action_delivery, &cursor, &offer
        ) ||
        !meiso_action_id_equal(offer.request.id, request.id)) {
        return false;
    }
    memset(&message, 0, sizeof(message));
    message.type = MEISO_LAB_MESSAGE_ACTION_RESET;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    message.body.action_reset.id = offer.request.id;
    message.body.action_reset.executor = offer.request.expected_executor;
    return send_lab_message(connection, MEISO_LAB_SENDER_HOST, &message);
}

static bool host_receive_action(
    LabConnection *connection,
    HostPrimitives *host,
    uint64_t *out_window_id
)
{
    MeisoLabMessage received;
    MeisoActionResult result;
    MeisoActionEntryView view;
    uint8_t result_bytes[8];

    if (out_window_id == NULL || !receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received
        ) || received.type != MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT ||
        meiso_action_caller_mark_delivery_accepted(
            host->action,
            host->action_delivery,
            received.body.action_delivery_accept.id,
            received.body.action_delivery_accept.executor
        ) != MEISO_ACTION_DELIVERY_ACCEPT_CONFIRMED) {
        return false;
    }
    host->action_delivery_accepted = true;
    emit_action_delivery_accepted();
    if (!receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received
        ) || received.type != MEISO_LAB_MESSAGE_ACTION_RESULT ||
        received.body.action_result.state != MEISO_ACTION_STATE_ACCEPTED) {
        return false;
    }
    result.id = received.body.action_result.id;
    result.executor = received.body.action_result.executor;
    result.state = received.body.action_result.state;
    result.payload = (MeisoByteView){NULL, 0U};
    if (meiso_action_caller_observe_result(host->action, &result) !=
            MEISO_ACTION_RESULT_RECORDED) {
        return false;
    }
    emit_action_executor_accepted();
    if (!receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received
        ) || received.type != MEISO_LAB_MESSAGE_ACTION_RESULT ||
        received.body.action_result.state != MEISO_ACTION_STATE_SUCCEEDED ||
        received.body.action_result.new_window_id == 0U) {
        return false;
    }
    write_u64_be(result_bytes, received.body.action_result.new_window_id);
    result.id = received.body.action_result.id;
    result.executor = received.body.action_result.executor;
    result.state = received.body.action_result.state;
    result.payload = (MeisoByteView){result_bytes, sizeof(result_bytes)};
    if (meiso_action_caller_observe_result(host->action, &result) !=
            MEISO_ACTION_RESULT_RECORDED ||
        !meiso_action_caller_query(host->action, host->action_id, &view) ||
        view.state != MEISO_ACTION_STATE_SUCCEEDED ||
        !bytes_equal(view.result, result.payload) ||
        meiso_action_caller_end_delivery(
            host->action, host->action_delivery
        ) != MEISO_ACTION_DELIVERY_ENDED) {
        return false;
    }
    host->action_terminal = true;
    *out_window_id = received.body.action_result.new_window_id;
#ifdef MEISO_LAB_INTERNAL_TEST
    if (g_self_test.enabled) {
        size_t index;
        if (!self_test_observation_index(&index)) {
            return false;
        }
        g_self_test.action_windows[index] = *out_window_id;
    }
#endif
    emit_action_event(*out_window_id);
    return true;
}

static bool host_mark_accepted_action_indeterminate(HostPrimitives *host)
{
    MeisoActionEntryView view;

    if (host == NULL || !host->action_delivery_accepted ||
        host->action_terminal) {
        return true;
    }
    if (meiso_action_caller_mark_indeterminate(
            host->action, host->action_id, host->edge_owner
        ) != MEISO_ACTION_INDETERMINATE_MARKED) {
        return false;
    }
    if (!meiso_action_caller_query(host->action, host->action_id, &view) ||
        view.state != MEISO_ACTION_STATE_INDETERMINATE ||
        meiso_action_caller_end_delivery(
            host->action, host->action_delivery
        ) != MEISO_ACTION_DELIVERY_ENDED) {
        return false;
    }
    host->action_terminal = true;
    return true;
}

static bool host_receive_blob(
    LabConnection *connection,
    HostPrimitives *host
)
{
    MeisoLabMessage received;
    MeisoLabMessage response;
    MeisoMutableBuffer staging;
    MeisoBlobView published;
    BlobVerifyContext verify;
    const MeisoContentId *fixed_content_id;

    if (!receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received
        ) || received.type != MEISO_LAB_MESSAGE_BLOB_MANIFEST ||
        meiso_blob_store_stage_begin(
            host->blob,
            &received.body.blob_manifest.content_id,
            received.body.blob_manifest.content.size
        ) != MEISO_BLOB_STAGE_STARTED ||
        !meiso_blob_store_staging_buffer(host->blob, &staging) ||
        received.body.blob_manifest.content.size > staging.capacity) {
        return false;
    }
    emit_blob_staged(received.body.blob_manifest.content.size);
    memmove(
        staging.data,
        received.body.blob_manifest.content.data,
        received.body.blob_manifest.content.size
    );
    fixed_content_id = meiso_lab_capability_manifest_content_id();
    if (fixed_content_id == NULL) {
        return false;
    }
    verify.expected_id = *fixed_content_id;
    {
        size_t manifest_size = 0U;
        const uint8_t *manifest = meiso_lab_capability_manifest(&manifest_size);
        if (manifest == NULL) {
            return false;
        }
        verify.expected_content = (MeisoByteView){manifest, manifest_size};
    }
    verify.calls = 0U;
    if (meiso_blob_store_commit_verified(
            host->blob,
            received.body.blob_manifest.content.size,
            verify_fixed_blob,
            &verify
        ) != MEISO_BLOB_COMMIT_PUBLISHED || verify.calls != 1U ||
        meiso_blob_store_count(host->blob) != 1U ||
        !meiso_blob_store_lookup(
            host->blob,
            &received.body.blob_manifest.content_id,
            &published
        ) || !bytes_equal(published.content, verify.expected_content)) {
        return false;
    }
    emit_blob_event(published.content.size);
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_BLOB_ACCEPT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.blob_accept.content_id = published.content_id;
    return send_lab_message(connection, MEISO_LAB_SENDER_HOST, &response);
}

static bool run_host_transcript(int fd, LabTest test)
{
    LabConnection connection;
    HostPrimitives host;
    MeisoLabStreamMetrics metrics;
    MeisoLabMessage close_message;
    uint64_t new_window_id;
    bool passed = false;

    memset(&connection, 0, sizeof(connection));
    connection.fd = -1;
    memset(&host, 0, sizeof(host));
    memset(&metrics, 0, sizeof(metrics));
    if (!initialize_connection(
            &connection, fd, MEISO_SESSION_ROLE_HOST, NULL
        )) {
        emit_connection_failed(&connection);
        emit_verdicts(test, false);
        return false;
    }
    if (!wait_session_ready(&connection)) {
        emit_connection_failed(&connection);
        emit_verdicts(test, false);
        return false;
    }
    emit_session_ready(&connection);
#ifdef MEISO_LAB_INTERNAL_TEST
    if (!self_test_observe_ready(&connection)) {
        goto finish;
    }
#endif
    if (!host_receive_registry(&connection, &host) ||
        !host_receive_state(&connection, &host) ||
        !host_receive_stream(&connection, &host, &metrics) ||
        !host_send_action(&connection, &host) ||
        !host_receive_action(
            &connection, &host, &new_window_id
        ) || meiso_stream_count(host.stream) != 1U ||
        !meiso_stream_pop(host.stream) ||
        meiso_stream_count(host.stream) != 0U) {
        goto finish;
    }
    emit_stream_consumed(&metrics);
    if (!host_receive_blob(&connection, &host)) {
        goto finish;
    }
    memset(&close_message, 0, sizeof(close_message));
    close_message.type = MEISO_LAB_MESSAGE_CLOSE;
    close_message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    close_message.body.close.status = MEISO_LAB_CLOSE_PASS;
    if (!send_lab_message(
            &connection, MEISO_LAB_SENDER_HOST, &close_message
        ) || !normal_host_close(&connection)) {
        goto finish;
    }
    passed = true;

finish:
    if (!passed) {
        if (!host_mark_accepted_action_indeterminate(&host) ||
            connection.local_failure == LAB_LOCAL_FAILURE_NONE) {
            record_protocol_failure(&connection);
        }
        emit_transcript_failed(&connection);
        best_effort_protocol_close(&connection);
    } else {
        emit_session_closed(&connection);
    }
    emit_verdicts(test, passed);
    return passed;
}

static bool edge_send_registry(LabConnection *connection)
{
    MeisoLabMessage message;

    memset(&message, 0, sizeof(message));
    message.type = MEISO_LAB_MESSAGE_REGISTRY;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    message.body.registry.descriptor_count = MEISO_LAB_DESCRIPTOR_COUNT;
    return send_lab_message(connection, MEISO_LAB_SENDER_EDGE, &message);
}

static bool edge_receive_registry_accept(LabConnection *connection)
{
    MeisoLabMessage received;
    return receive_lab_message(
            connection, MEISO_LAB_SENDER_HOST, &received
        ) && received.type == MEISO_LAB_MESSAGE_REGISTRY_ACCEPT &&
        received.body.registry_accept.descriptor_count ==
            MEISO_LAB_DESCRIPTOR_COUNT;
}

static bool edge_send_state(
    LabConnection *connection,
    EdgePrimitives *edge,
    const MeisoLabEdgeState *state,
    uint64_t version
)
{
    uint8_t payload[MEISO_LAB_EDGE_STATE_WIRE_SIZE];
    size_t payload_size = 0U;
    MeisoStateOffer offer;
    MeisoLabMessage message;

    if (meiso_lab_edge_state_encode(
            state,
            (MeisoMutableBuffer){payload, sizeof(payload)},
            &payload_size
        ) != MEISO_LAB_CODEC_OK ||
        meiso_state_publisher_publish(
            edge->state,
            connection->edge_scope,
            version,
            (MeisoByteView){payload, payload_size}
        ) != MEISO_STATE_PUBLISH_NEW ||
        meiso_state_publisher_begin_delivery(
            edge->state, edge->state_delivery
        ) != MEISO_STATE_DELIVERY_STARTED ||
        !meiso_state_publisher_next_offer(edge->state, &offer)) {
        return false;
    }
    edge->state_owner = offer.state.owner;
    edge->state_version = offer.state.version;
    edge->state_delivery_active = true;
    memset(&message, 0, sizeof(message));
    message.type = MEISO_LAB_MESSAGE_STATE_ACTUAL;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    message.body.state_actual.owner = offer.state.owner;
    message.body.state_actual.version = offer.state.version;
    if (meiso_lab_edge_state_decode(
            offer.state.payload, &message.body.state_actual.value
        ) != MEISO_LAB_CODEC_OK) {
        return false;
    }
    return send_lab_message(connection, MEISO_LAB_SENDER_EDGE, &message);
}

static bool edge_receive_state_accept(
    LabConnection *connection,
    EdgePrimitives *edge
)
{
    MeisoLabMessage received;

    if (!receive_lab_message(
            connection, MEISO_LAB_SENDER_HOST, &received
        ) || received.type != MEISO_LAB_MESSAGE_STATE_ACCEPT ||
        meiso_state_publisher_mark_delivery_accepted(
            edge->state,
            edge->state_delivery,
            received.body.state_accept.owner,
            received.body.state_accept.version
        ) != MEISO_STATE_ACCEPT_CONFIRMED ||
        meiso_state_publisher_end_delivery(
            edge->state, edge->state_delivery
        ) != MEISO_STATE_DELIVERY_ENDED) {
        return false;
    }
    edge->state_delivery_active = false;
    return true;
}

static bool edge_fail_active_state_delivery(EdgePrimitives *edge)
{
    if (edge == NULL || !edge->state_delivery_active) {
        return true;
    }
    if (meiso_state_publisher_mark_delivery_failed(
            edge->state,
            edge->state_delivery,
            edge->state_owner,
            edge->state_version
        ) != MEISO_STATE_FAILURE_RETAINED ||
        meiso_state_publisher_end_delivery(
            edge->state, edge->state_delivery
        ) != MEISO_STATE_DELIVERY_CONVERGENCE_FAILED) {
        return false;
    }
    edge->state_delivery_active = false;
    return true;
}

static bool edge_send_stream(
    LabConnection *connection,
    const MeisoLabTelemetrySample *sample,
    uint64_t sequence
)
{
    MeisoLabMessage message;
    uint64_t source_time;

    if (!monotonic_now_ns(&source_time)) {
        return false;
    }
    memset(&message, 0, sizeof(message));
    message.type = MEISO_LAB_MESSAGE_STREAM_METRICS;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    message.body.stream_metrics.producer = connection->edge_scope;
    message.body.stream_metrics.epoch = connection->stream_epoch;
    message.body.stream_metrics.sequence = sequence;
    message.body.stream_metrics.source_time = (MeisoTimePoint){
        connection->clock_epoch, source_time
    };
    message.body.stream_metrics.value = *sample;
    return send_lab_message(connection, MEISO_LAB_SENDER_EDGE, &message);
}

static bool edge_execute_action(
    LabConnection *connection,
    EdgeProcess *process
)
{
    const MeisoResourceDescriptor *stream_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STREAM);
    const MeisoResourceDescriptor *action_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    MeisoLabMessage received;
    MeisoLabMessage response;
    MeisoActionRequest request;
    MeisoActionRequest application_request;
    MeisoActionResult result;
    MeisoActionEntryView cached;
    MeisoActionIngestStatus ingest_status;
    uint8_t result_bytes[8];
    size_t cursor = 0U;
    uint64_t result_window_id;
    bool execute_side_effect = false;

    if (stream_descriptor == NULL || action_descriptor == NULL ||
        !receive_lab_message(
            connection, MEISO_LAB_SENDER_HOST, &received
        ) || received.type != MEISO_LAB_MESSAGE_ACTION_RESET) {
        return false;
    }
    memset(&request, 0, sizeof(request));
    request.id = received.body.action_reset.id;
    request.expected_executor = received.body.action_reset.executor;
    request.target = stream_descriptor->id;
    request.operation = action_descriptor->schema;
    request.retry_policy = MEISO_ACTION_RETRY_NONE;
    ingest_status = meiso_action_executor_ingest(
        process->action, &request, NULL
    );
    if (ingest_status == MEISO_ACTION_INGEST_NEW) {
        if (!meiso_action_executor_next_application_request(
                process->action, &cursor, &application_request
            ) || !meiso_action_id_equal(application_request.id, request.id) ||
            meiso_action_executor_accept(
                process->action,
                connection->edge_scope,
                request.id,
                (MeisoTimePoint){0U, 0U}
            ) != MEISO_ACTION_EXECUTOR_ACCEPTED) {
            return false;
        }
        process->active_action_id = request.id;
        process->active_action_nonterminal = true;
        execute_side_effect = true;
    } else if (ingest_status == MEISO_ACTION_INGEST_REPLAY) {
        if (!meiso_action_executor_query(
                process->action, request.id, &cached
            ) || cached.state != MEISO_ACTION_STATE_SUCCEEDED ||
            cached.result.size != sizeof(result_bytes)) {
            return false;
        }
        result_window_id = read_u64_be(cached.result.data);
        if (result_window_id == 0U) {
            return false;
        }
    } else {
        return false;
    }
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.action_delivery_accept.id = request.id;
    response.body.action_delivery_accept.executor = connection->edge_scope;
    if (!send_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &response
        )) {
        return false;
    }
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_ACTION_RESULT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.action_result.id = request.id;
    response.body.action_result.executor = connection->edge_scope;
    response.body.action_result.state = MEISO_ACTION_STATE_ACCEPTED;
    if (!send_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &response
        )) {
        return false;
    }
    if (execute_side_effect) {
        if (!reset_telemetry_window(&process->telemetry)) {
            return false;
        }
        result_window_id = process->telemetry.window_id;
        write_u64_be(result_bytes, result_window_id);
        result.id = request.id;
        result.executor = connection->edge_scope;
        result.state = MEISO_ACTION_STATE_SUCCEEDED;
        result.payload = (MeisoByteView){result_bytes, sizeof(result_bytes)};
        if (meiso_action_executor_record_result(process->action, &result) !=
            MEISO_ACTION_RECORD_RECORDED) {
            return false;
        }
        process->active_action_nonterminal = false;
    }
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_ACTION_RESULT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.action_result.id = request.id;
    response.body.action_result.executor = connection->edge_scope;
    response.body.action_result.state = MEISO_ACTION_STATE_SUCCEEDED;
    response.body.action_result.new_window_id = result_window_id;
    return send_lab_message(connection, MEISO_LAB_SENDER_EDGE, &response);
}

static bool edge_mark_active_action_indeterminate(EdgeProcess *process)
{
    MeisoActionEntryView view;

    if (process == NULL || !process->active_action_nonterminal) {
        return true;
    }
    if (meiso_action_executor_mark_indeterminate(
            process->action,
            process->scope,
            process->active_action_id
        ) != MEISO_ACTION_INDETERMINATE_MARKED ||
        !meiso_action_executor_query(
            process->action, process->active_action_id, &view
        ) || view.state != MEISO_ACTION_STATE_INDETERMINATE) {
        return false;
    }
    process->active_action_nonterminal = false;
    return true;
}

static bool edge_send_blob(LabConnection *connection)
{
    MeisoLabMessage message;
    MeisoLabMessage received;
    size_t manifest_size = 0U;
    const uint8_t *manifest = meiso_lab_capability_manifest(&manifest_size);
    const MeisoContentId *content_id =
        meiso_lab_capability_manifest_content_id();

    if (manifest == NULL || content_id == NULL) {
        return false;
    }
    memset(&message, 0, sizeof(message));
    message.type = MEISO_LAB_MESSAGE_BLOB_MANIFEST;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    message.body.blob_manifest.content_id = *content_id;
    message.body.blob_manifest.content =
        (MeisoByteView){manifest, manifest_size};
    return send_lab_message(connection, MEISO_LAB_SENDER_EDGE, &message) &&
        receive_lab_message(
            connection, MEISO_LAB_SENDER_HOST, &received
        ) && received.type == MEISO_LAB_MESSAGE_BLOB_ACCEPT &&
        meiso_content_id_equal(
            &received.body.blob_accept.content_id, content_id
        );
}

static bool edge_receive_lab_close(LabConnection *connection)
{
    MeisoLabMessage received;
    return receive_lab_message(
            connection, MEISO_LAB_SENDER_HOST, &received
        ) && received.type == MEISO_LAB_MESSAGE_CLOSE &&
        received.body.close.status == MEISO_LAB_CLOSE_PASS &&
        edge_wait_peer_close(connection);
}

static bool run_edge_transcript(
    int fd,
    EdgeProcess *process
)
{
    LabConnection connection;
    EdgePrimitives edge;
    MeisoLabEdgeState state;
    MeisoLabTelemetrySample sample;
    bool passed = false;

    memset(&connection, 0, sizeof(connection));
    connection.fd = -1;
    memset(&edge, 0, sizeof(edge));
    if (!initialize_connection(
            &connection, fd, MEISO_SESSION_ROLE_EDGE, process
        )) {
        emit_connection_failed(&connection);
        return false;
    }
    if (!initialize_edge_primitives(&connection, &edge)) {
        emit_connection_failed(&connection);
        return false;
    }
    if (!wait_session_ready(&connection)) {
        emit_connection_failed(&connection);
        return false;
    }
    emit_session_ready(&connection);
    if (!edge_send_registry(&connection) ||
        !edge_receive_registry_accept(&connection) ||
        process->state_version == UINT64_MAX) {
        goto finish;
    }
    process->state_version += 1U;
    if (!capture_telemetry(&process->telemetry, &state, &sample) ||
        !edge_send_state(
            &connection, &edge, &state, process->state_version
        ) ||
        !edge_receive_state_accept(&connection, &edge) ||
        process->stream_sequence == UINT64_MAX) {
        goto finish;
    }
    process->stream_sequence += 1U;
    if (!edge_send_stream(
            &connection, &sample, process->stream_sequence
        ) ||
        !edge_execute_action(&connection, process) ||
        !edge_send_blob(&connection) ||
        !edge_receive_lab_close(&connection)) {
        goto finish;
    }
    passed = true;

finish:
    if (!passed) {
        if (!edge_fail_active_state_delivery(&edge) ||
            !edge_mark_active_action_indeterminate(process) ||
            connection.local_failure == LAB_LOCAL_FAILURE_NONE) {
            record_protocol_failure(&connection);
        }
        emit_transcript_failed(&connection);
        best_effort_protocol_close(&connection);
    } else {
        emit_session_closed(&connection);
    }
    return passed;
}

static bool parse_test(const char *value, LabTest *out_test)
{
    if (value == NULL || out_test == NULL) {
        return false;
    }
    if (strcmp(value, "state") == 0) {
        *out_test = LAB_TEST_STATE;
    } else if (strcmp(value, "stream") == 0) {
        *out_test = LAB_TEST_STREAM;
    } else if (strcmp(value, "action") == 0) {
        *out_test = LAB_TEST_ACTION;
    } else if (strcmp(value, "blob") == 0) {
        *out_test = LAB_TEST_BLOB;
    } else if (strcmp(value, "all") == 0) {
        *out_test = LAB_TEST_ALL;
    } else {
        return false;
    }
    return true;
}

static bool parse_sessions(const char *value, unsigned int *out_sessions)
{
    uint64_t parsed;
    if (out_sessions == NULL || !parse_u64_text(value, &parsed) ||
        parsed == 0U || parsed > LAB_MAX_SESSIONS) {
        return false;
    }
    *out_sessions = (unsigned int)parsed;
    return true;
}

static int run_host(const char *config_path, LabTest test)
{
    HostLocator locator;
    int fd;
    bool passed;

    if (!parse_host_config(config_path, &locator)) {
        emit_session_failed();
        emit_verdicts(test, false);
        return 1;
    }
    fd = connect_host(&locator);
    if (fd < 0) {
        emit_session_failed();
        emit_verdicts(test, false);
        return 1;
    }
    passed = run_host_transcript(fd, test);
    close_fd(&fd);
    return passed ? 0 : 1;
}

static bool run_edge_accept_loop(
    int listener,
    unsigned int sessions,
    EdgeProcess *process
)
{
    unsigned int completed = 0U;
    bool all_passed = true;

    while (completed < sessions) {
        int fd;
        bool passed;
        thermal_child_reap_nonblocking(&process->thermal_child);
        fd = accept_edge(listener);
        if (fd < 0) {
            all_passed = false;
            break;
        }
        passed = run_edge_transcript(fd, process);
        close_fd(&fd);
        thermal_child_reap_nonblocking(&process->thermal_child);
        if (!passed) {
            all_passed = false;
            break;
        }
        completed += 1U;
    }
    return all_passed && completed == sessions;
}

static int run_edge(uint16_t port, unsigned int sessions)
{
    EdgeProcess process;
    int listener;
    bool passed;

    if (!initialize_edge_process(&process)) {
        emit_session_failed();
        return 1;
    }
    thermal_child_reap_nonblocking(&process.thermal_child);
    listener = make_listener(port);
    if (listener < 0) {
        thermal_child_terminate_nonblocking(&process.thermal_child);
        emit_session_failed();
        return 1;
    }
    passed = run_edge_accept_loop(listener, sessions, &process);
    close_fd(&listener);
    thermal_child_terminate_nonblocking(&process.thermal_child);
    thermal_child_reap_nonblocking(&process.thermal_child);
    return passed ? 0 : 1;
}

#ifdef MEISO_LAB_INTERNAL_TEST
static void finish_thermal_fixture(ThermalChild *child)
{
    thermal_child_terminate_nonblocking(child);
    thermal_child_reap_nonblocking(child);
}

static bool self_test_thermal_modes(void)
{
    ThermalReading reading;
    ThermalChild child;
    ThermalPreflightStatus status;
    uint64_t started;
    uint64_t finished;

    memset(&child, 0, sizeof(child));
    child.pid = (pid_t)-1;
    status = thermal_preflight_bounded(
        &reading, &child, THERMAL_CHILD_FIXTURE_PRESENT
    );
    finish_thermal_fixture(&child);
    if (status != THERMAL_PREFLIGHT_PRESENT || !reading.present ||
        reading.corrected_millic != 52000 ||
        reading.uncertainty_millic !=
            MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC) {
        return false;
    }

    status = thermal_preflight_bounded(
        &reading, &child, THERMAL_CHILD_FIXTURE_ABSENT
    );
    finish_thermal_fixture(&child);
    if (status != THERMAL_PREFLIGHT_ABSENT || reading.present ||
        reading.corrected_millic != 0 || reading.uncertainty_millic != 0U) {
        return false;
    }

    if (!monotonic_now_ns(&started)) {
        return false;
    }
    status = thermal_preflight_bounded(
        &reading, &child, THERMAL_CHILD_FIXTURE_TIMEOUT
    );
    if (!monotonic_now_ns(&finished)) {
        finish_thermal_fixture(&child);
        return false;
    }
    finish_thermal_fixture(&child);
    if (status != THERMAL_PREFLIGHT_TIMEOUT || reading.present ||
        finished < started ||
        finished - started > UINT64_C(1500000000)) {
        return false;
    }

    status = thermal_preflight_bounded(
        &reading, &child, THERMAL_CHILD_FIXTURE_PARTIAL
    );
    finish_thermal_fixture(&child);
    if (status != THERMAL_PREFLIGHT_TIMEOUT || reading.present) {
        return false;
    }

    status = thermal_preflight_bounded(
        &reading, &child, THERMAL_CHILD_FIXTURE_EARLY_EXIT
    );
    finish_thermal_fixture(&child);
    return status == THERMAL_PREFLIGHT_CHILD_ERROR && !reading.present;
}

static bool self_test_thermal_stdout_eof(void)
{
    int descriptors[2];
    pid_t runner;
    struct timespec deadline;
    bool eof = false;
    bool runner_ok = false;
    int runner_status = 0;

    if (!make_thermal_pipe(descriptors) ||
        !make_deadline_after_ms(&deadline, 2000U)) {
        return false;
    }
    runner = fork();
    if (runner < 0) {
        close_fd(&descriptors[0]);
        close_fd(&descriptors[1]);
        return false;
    }
    if (runner == 0) {
        ThermalReading reading;
        ThermalChild child;
        ThermalPreflightStatus status;

        close_fd(&descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) {
            _exit(2);
        }
        close_fd(&descriptors[1]);
        memset(&child, 0, sizeof(child));
        child.pid = (pid_t)-1;
        status = thermal_preflight_bounded(
            &reading, &child, THERMAL_CHILD_FIXTURE_TIMEOUT
        );
        finish_thermal_fixture(&child);
        if (status != THERMAL_PREFLIGHT_TIMEOUT ||
            write(STDOUT_FILENO, "ok", 2U) != 2) {
            _exit(2);
        }
        (void)close(STDOUT_FILENO);
        _exit(0);
    }
    close_fd(&descriptors[1]);
    while (!eof && deadline_wait_ms(&deadline) != 0) {
        uint8_t buffer[16];
        const ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
        pid_t waited;

        if (count == 0) {
            eof = true;
        } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
            break;
        }
        waited = waitpid(runner, &runner_status, WNOHANG);
        if (waited == runner) {
            runner_ok = WIFEXITED(runner_status) &&
                WEXITSTATUS(runner_status) == 0;
        }
        if (!eof) {
            (void)poll(NULL, (nfds_t)0U, 10);
        }
    }
    close_fd(&descriptors[0]);
    if (!runner_ok) {
        const pid_t waited = waitpid(runner, &runner_status, WNOHANG);
        if (waited == runner) {
            runner_ok = WIFEXITED(runner_status) &&
                WEXITSTATUS(runner_status) == 0;
        }
    }
    if (!runner_ok) {
        (void)kill(runner, SIGKILL);
        (void)waitpid(runner, &runner_status, WNOHANG);
    }
    return eof && runner_ok;
}

static bool self_test_state_delivery_failure(void)
{
    LabArena arena;
    EdgePrimitives edge;
    MeisoStatePublisher *publisher;
    MeisoStateOffer offer;
    MeisoStatePublisherStats stats;
    const MeisoResourceDescriptor *descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STATE);
    const MeisoStateConfig config = {MEISO_STATE_DELIVERY_CONVERGENT};
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 41U};
    const uint8_t payload[] = {1U};

    memset(&arena, 0, sizeof(arena));
    memset(&edge, 0, sizeof(edge));
    if (descriptor == NULL || meiso_state_publisher_init(
            arena.bytes, sizeof(arena.bytes), descriptor, &config, &publisher
        ) != MEISO_STATE_INIT_OK ||
        meiso_state_publisher_publish(
            publisher, owner, 1U,
            (MeisoByteView){payload, sizeof(payload)}
        ) != MEISO_STATE_PUBLISH_NEW) {
        return false;
    }
    edge.state = publisher;
    edge.state_delivery.value = 1U;
    if (meiso_state_publisher_begin_delivery(
            publisher, edge.state_delivery
        ) != MEISO_STATE_DELIVERY_STARTED ||
        !meiso_state_publisher_next_offer(publisher, &offer)) {
        return false;
    }
    edge.state_owner = offer.state.owner;
    edge.state_version = offer.state.version;
    edge.state_delivery_active = true;
    return edge_fail_active_state_delivery(&edge) &&
        !edge.state_delivery_active &&
        meiso_state_publisher_stats(publisher, &stats) &&
        stats.delivery_failures == 1U &&
        stats.convergence_failures == 1U;
}

static bool self_test_action_caller_indeterminate(void)
{
    LabArena arena;
    HostPrimitives host;
    MeisoActionCaller *caller;
    MeisoActionEntryView view;
    MeisoActionCallerStats stats;
    MeisoActionRequest request;
    const MeisoResourceDescriptor *action_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    const MeisoResourceDescriptor *stream_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STREAM);
    const MeisoActionConfig config = {1U, 0U, 8U};
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 51U};
    const MeisoActionId id = {61U, 62U};

    memset(&arena, 0, sizeof(arena));
    memset(&host, 0, sizeof(host));
    memset(&request, 0, sizeof(request));
    if (action_descriptor == NULL || stream_descriptor == NULL ||
        meiso_action_caller_init(
            arena.bytes, sizeof(arena.bytes), action_descriptor, &config,
            &caller
        ) != MEISO_ACTION_INIT_OK) {
        return false;
    }
    host.action = caller;
    host.action_delivery.value = 1U;
    host.action_id = id;
    host.edge_owner = owner;
    request.id = id;
    request.expected_executor = owner;
    request.target = stream_descriptor->id;
    request.operation = action_descriptor->schema;
    request.retry_policy = MEISO_ACTION_RETRY_NONE;
    if (meiso_action_caller_begin_delivery(
            caller, host.action_delivery
        ) != MEISO_ACTION_DELIVERY_STARTED ||
        meiso_action_caller_submit(caller, &request) !=
            MEISO_ACTION_SUBMIT_NEW ||
        meiso_action_caller_mark_delivery_accepted(
            caller, host.action_delivery, id, owner
        ) != MEISO_ACTION_DELIVERY_ACCEPT_CONFIRMED) {
        return false;
    }
    host.action_delivery_accepted = true;
    return host_mark_accepted_action_indeterminate(&host) &&
        host.action_terminal && meiso_action_caller_query(caller, id, &view) &&
        view.state == MEISO_ACTION_STATE_INDETERMINATE &&
        meiso_action_caller_stats(caller, &stats) && stats.indeterminate == 1U;
}

static bool self_test_action_executor_indeterminate(void)
{
    EdgeProcess process;
    MeisoActionRequest request;
    MeisoActionRequest application;
    MeisoActionEntryView view;
    MeisoActionExecutorStats stats;
    size_t cursor = 0U;
    const MeisoResourceDescriptor *action_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    const MeisoResourceDescriptor *stream_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STREAM);
    const MeisoActionConfig config = {1U, 0U, 8U};
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 71U};
    const MeisoActionId id = {72U, 73U};

    memset(&process, 0, sizeof(process));
    memset(&request, 0, sizeof(request));
    process.scope = owner;
    if (action_descriptor == NULL || stream_descriptor == NULL ||
        meiso_action_executor_init(
            process.action_arena.bytes, sizeof(process.action_arena.bytes),
            action_descriptor, &config, owner, &process.action
        ) != MEISO_ACTION_INIT_OK) {
        return false;
    }
    request.id = id;
    request.expected_executor = owner;
    request.target = stream_descriptor->id;
    request.operation = action_descriptor->schema;
    request.retry_policy = MEISO_ACTION_RETRY_NONE;
    if (meiso_action_executor_ingest(process.action, &request, NULL) !=
            MEISO_ACTION_INGEST_NEW ||
        !meiso_action_executor_next_application_request(
            process.action, &cursor, &application
        ) || !meiso_action_id_equal(application.id, id) ||
        meiso_action_executor_accept(
            process.action, owner, id, (MeisoTimePoint){0U, 0U}
        ) != MEISO_ACTION_EXECUTOR_ACCEPTED) {
        return false;
    }
    process.active_action_id = id;
    process.active_action_nonterminal = true;
    return edge_mark_active_action_indeterminate(&process) &&
        !process.active_action_nonterminal &&
        meiso_action_executor_query(process.action, id, &view) &&
        view.state == MEISO_ACTION_STATE_INDETERMINATE &&
        meiso_action_executor_stats(process.action, &stats) &&
        stats.indeterminate == 1U;
}

static bool self_test_wrong_state_owner(void)
{
    LabArena arena;
    LabConnection connection;
    HostPrimitives host;
    MeisoLabMessage message;
    MeisoStateView current;
    MeisoStateReplicaStats stats;
    const MeisoResourceDescriptor *descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STATE);

    memset(&arena, 0, sizeof(arena));
    memset(&connection, 0, sizeof(connection));
    memset(&host, 0, sizeof(host));
    memset(&message, 0, sizeof(message));
    connection.edge_scope = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 81U};
    if (descriptor == NULL || meiso_state_replica_init(
            arena.bytes, sizeof(arena.bytes), descriptor, &host.state
        ) != MEISO_STATE_INIT_OK) {
        return false;
    }
    message.type = MEISO_LAB_MESSAGE_STATE_ACTUAL;
    message.body.state_actual.owner =
        (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 82U};
    message.body.state_actual.version = 1U;
    message.body.state_actual.value.window_id = 1U;
    message.body.state_actual.value.interface_present = true;
    message.body.state_actual.value.carrier_present = true;
    return !host_apply_state_actual(
            &connection, &host, &message, &current
        ) && !host.edge_state_known &&
        meiso_state_replica_stats(host.state, &stats) && stats.receives == 0U;
}

static bool wait_child_exit_bounded(pid_t child, unsigned int timeout_ms)
{
    struct timespec deadline;
    int status;

    if (!make_deadline_after_ms(&deadline, timeout_ms)) {
        return false;
    }
    while (deadline_wait_ms(&deadline) != 0) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0) {
            return false;
        }
        (void)poll(NULL, (nfds_t)0U, 10);
    }
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &status, WNOHANG);
    return false;
}

static bool listener_port(int listener, uint16_t *out_port)
{
    struct sockaddr_in address;
    socklen_t size = (socklen_t)sizeof(address);

    memset(&address, 0, sizeof(address));
    if (out_port == NULL || getsockname(
            listener,
            (struct sockaddr *)(void *)&address,
            &size
        ) != 0 || size != (socklen_t)sizeof(address) ||
        address.sin_family != AF_INET || ntohs(address.sin_port) == 0U) {
        return false;
    }
    *out_port = ntohs(address.sin_port);
    return true;
}

static bool self_test_two_sessions(void)
{
    HostLocator locator;
    uint16_t port;
    int listener = make_listener(0U);
    pid_t edge_child;
    unsigned int index;
    bool passed = true;

    if (listener < 0 || !listener_port(listener, &port)) {
        close_fd(&listener);
        return false;
    }
    edge_child = fork();
    if (edge_child < 0) {
        close_fd(&listener);
        return false;
    }
    if (edge_child == 0) {
        EdgeProcess process;
        MeisoActionExecutorStats stats;
        bool child_passed = initialize_edge_process_with_thermal_mode(
            &process, THERMAL_CHILD_FIXTURE_ABSENT
        );

        if (child_passed) {
            child_passed = run_edge_accept_loop(
                listener, LAB_SELF_TEST_SESSIONS, &process
            );
        }
        close_fd(&listener);
        if (child_passed) {
            child_passed = process.state_version == 2U &&
                process.stream_sequence == 2U &&
                process.telemetry.window_id == 3U &&
                meiso_action_executor_stats(process.action, &stats) &&
                stats.ingests == 2U && stats.replays == 0U &&
                stats.succeeded == 2U;
        }
        thermal_child_terminate_nonblocking(&process.thermal_child);
        thermal_child_reap_nonblocking(&process.thermal_child);
        _exit(child_passed ? 0 : 1);
    }
    close_fd(&listener);
    memset(&g_self_test, 0, sizeof(g_self_test));
    g_self_test.enabled = true;
    g_self_test.fixed_action_id = false;
    memset(&locator, 0, sizeof(locator));
    locator.address.s_addr = htonl(UINT32_C(0x7f000001));
    locator.port = port;
    for (index = 0U; index < LAB_SELF_TEST_SESSIONS; ++index) {
        int fd = connect_host(&locator);
        if (fd < 0 || !run_host_transcript(fd, LAB_TEST_ALL)) {
            close_fd(&fd);
            passed = false;
            break;
        }
        close_fd(&fd);
    }
    if (!wait_child_exit_bounded(edge_child, 10000U)) {
        passed = false;
    }
    passed = passed &&
        g_self_test.session_count == LAB_SELF_TEST_SESSIONS &&
        !meiso_session_id_equal(
            g_self_test.session_ids[0], g_self_test.session_ids[1]
        ) &&
        meiso_owner_scope_equal(
            g_self_test.owners[0], g_self_test.owners[1]
        ) &&
        g_self_test.owners[0].incarnation != 0U &&
        g_self_test.state_versions[0] == 1U &&
        g_self_test.state_versions[1] == 2U &&
        g_self_test.stream_epochs[0] != 0U &&
        g_self_test.stream_epochs[0] == g_self_test.stream_epochs[1] &&
        g_self_test.stream_sequences[0] == 1U &&
        g_self_test.stream_sequences[1] == 2U &&
        g_self_test.clock_epochs[0] != 0U &&
        g_self_test.clock_epochs[0] == g_self_test.clock_epochs[1] &&
        g_self_test.action_windows[0] == 2U &&
        g_self_test.action_windows[1] == 3U;
    memset(&g_self_test, 0, sizeof(g_self_test));
    return passed;
}

static int run_internal_self_test(void)
{
    const bool passed = self_test_thermal_modes() &&
        self_test_thermal_stdout_eof() &&
        self_test_state_delivery_failure() &&
        self_test_action_caller_indeterminate() &&
        self_test_action_executor_indeterminate() &&
        self_test_wrong_state_owner() &&
        self_test_two_sessions();

    if (!passed) {
        (void)fprintf(stderr, "four-kind lab internal self-test failed\n");
    }
    return passed ? 0 : 1;
}
#endif

static void print_usage(void)
{
    (void)fprintf(
        stderr,
        "usage: four_kind_lab --host --config PATH "
        "--test state|stream|action|blob|all\n"
        "       four_kind_lab --edge --port 27640 --sessions 1..16\n"
    );
}

int main(int argc, char **argv)
{
#ifdef MEISO_LAB_INTERNAL_TEST
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        return run_internal_self_test();
    }
#endif
    if (argc == 6 && strcmp(argv[1], "--host") == 0 &&
        strcmp(argv[2], "--config") == 0 &&
        strcmp(argv[4], "--test") == 0) {
        LabTest test;
        if (!parse_test(argv[5], &test)) {
            print_usage();
            return 2;
        }
        return run_host(argv[3], test);
    }
    if (argc == 6 && strcmp(argv[1], "--edge") == 0 &&
        strcmp(argv[2], "--port") == 0 &&
        strcmp(argv[4], "--sessions") == 0) {
        uint16_t port;
        unsigned int sessions;
        if (!parse_port(argv[3], &port) ||
            !parse_sessions(argv[5], &sessions)) {
            print_usage();
            return 2;
        }
        return run_edge(port, sessions);
    }
    print_usage();
    return 2;
}
