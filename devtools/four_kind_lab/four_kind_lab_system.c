#include "meiso_four_kind_lab_private.h"

/* System support owns fds, clocks, entropy, files and Wi-Fi readings. */

MEISO_FOUR_KIND_LAB_PRIVATE void meiso_four_kind_lab_private_close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_set_nonblocking_cloexec(int fd)
{
    const int status_flags = fcntl(fd, F_GETFL);
    const int descriptor_flags = fcntl(fd, F_GETFD);

    return status_flags >= 0 && descriptor_flags >= 0 &&
           fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_monotonic_now_ns(uint64_t *out_value)
{
    struct timespec now;
    uint64_t seconds;

    if (out_value == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
        return false;
    }
    seconds = (uint64_t)now.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)now.tv_nsec) / UINT64_C(1000000000)) {
        return false;
    }
    *out_value = seconds * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_make_deadline_after_ms(
    struct timespec *out_deadline, unsigned int timeout_ms)
{
    if (out_deadline == NULL ||
        clock_gettime(CLOCK_MONOTONIC, out_deadline) != 0) {
        return false;
    }
    out_deadline->tv_sec += (time_t)(timeout_ms / 1000U);
    out_deadline->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (out_deadline->tv_nsec >= 1000000000L) {
        out_deadline->tv_nsec -= 1000000000L;
        out_deadline->tv_sec += (time_t)1;
    }
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_make_deadline(struct timespec *out_deadline)
{
    return meiso_four_kind_lab_private_make_deadline_after_ms(
        out_deadline, MEISO_FOUR_KIND_LAB_PHASE_TIMEOUT_MS);
}

MEISO_FOUR_KIND_LAB_PRIVATE int
meiso_four_kind_lab_private_deadline_wait_ms(const struct timespec *deadline)
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
        return MEISO_FOUR_KIND_LAB_POLL_SLICE_MS;
    }
    remaining_ns = seconds * INT64_C(1000000000) + nanoseconds;
    milliseconds = (remaining_ns + INT64_C(999999)) / INT64_C(1000000);
    if (milliseconds > MEISO_FOUR_KIND_LAB_POLL_SLICE_MS) {
        milliseconds = MEISO_FOUR_KIND_LAB_POLL_SLICE_MS;
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
            meiso_four_kind_lab_private_close_fd(&fd);
            return false;
        }
    }
    meiso_four_kind_lab_private_close_fd(&fd);
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_random_nonzero_u64(uint64_t *out_value)
{
    uint64_t value;

    if (out_value == NULL || !read_entropy(&value, sizeof(value))) {
        return false;
    }
    value &= UINT64_C(0x7fffffffffffffff);
    *out_value = value == 0U ? 1U : value;
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_random_nonce(MeisoSessionNonce *out_nonce)
{
    if (out_nonce == NULL || !read_entropy(out_nonce, sizeof(*out_nonce))) {
        return false;
    }
    if (!meiso_session_nonce_valid(*out_nonce)) {
        out_nonce->low = 1U;
    }
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_random_action_id(MeisoActionId *out_id)
{
#ifdef MEISO_LAB_INTERNAL_TEST
    if (out_id != NULL &&
        meiso_four_kind_lab_private_self_test.fixed_action_id) {
        *out_id = (MeisoActionId){UINT64_C(0x4d344c315f544553),
                                  UINT64_C(0x545f414354494f4e)};
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

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_read_text_file(const char *path, char *output,
                                           size_t capacity)
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
    if (length == capacity - 1U && output[length - 1U] != '\n' && !feof(file)) {
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

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_parse_u64_text(const char *text_value,
                                           uint64_t *out_value)
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

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_read_u64_file(const char *path, uint64_t *out_value)
{
    char text_value[64];
    return meiso_four_kind_lab_private_read_text_file(path, text_value,
                                                      sizeof(text_value)) &&
           meiso_four_kind_lab_private_parse_u64_text(text_value, out_value);
}

static MeisoFourKindLabWifiOperstate parse_operstate(const char *value)
{
    if (strcmp(value, "up") == 0) {
        return MEISO_FOUR_KIND_WIFI_OPERSTATE_UP;
    }
    if (strcmp(value, "down") == 0) {
        return MEISO_FOUR_KIND_WIFI_OPERSTATE_DOWN;
    }
    if (strcmp(value, "dormant") == 0) {
        return MEISO_FOUR_KIND_WIFI_OPERSTATE_DORMANT;
    }
    if (strcmp(value, "lowerlayerdown") == 0) {
        return MEISO_FOUR_KIND_WIFI_OPERSTATE_LOWERLAYERDOWN;
    }
    if (strcmp(value, "notpresent") == 0) {
        return MEISO_FOUR_KIND_WIFI_OPERSTATE_NOTPRESENT;
    }
    if (strcmp(value, "testing") == 0) {
        return MEISO_FOUR_KIND_WIFI_OPERSTATE_TESTING;
    }
    return MEISO_FOUR_KIND_WIFI_OPERSTATE_UNKNOWN;
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
        if ((size_t)(name_end - name) != 5U || memcmp(name, "wlan0", 5U) != 0) {
            continue;
        }
        if (sscanf(colon + 1, " %x %lf %lf", &status, &link, &level) == 3 &&
            level >= -200.0 && level <= 0.0) {
            const double scaled = level * 100.0;
            (void)status;
            (void)link;
            *out_signal_mbm = scaled >= 0.0 ? (int32_t)(scaled + 0.5)
                                            : (int32_t)(scaled - 0.5);
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

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_read_network(
    MeisoFourKindLabNetworkReading *out_reading)
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
    out_reading->operstate = MEISO_FOUR_KIND_WIFI_OPERSTATE_NOTPRESENT;
    if (!meiso_four_kind_lab_private_read_text_file(OPERSTATE_PATH, operstate,
                                                    sizeof(operstate))) {
        return true;
    }
    out_reading->interface_present = true;
    out_reading->operstate = parse_operstate(operstate);
    if (!meiso_four_kind_lab_private_read_u64_file(CARRIER_PATH, &carrier) ||
        carrier > 1U ||
        !meiso_four_kind_lab_private_read_u64_file(RX_BYTES_PATH,
                                                   &out_reading->rx_bytes) ||
        !meiso_four_kind_lab_private_read_u64_file(TX_BYTES_PATH,
                                                   &out_reading->tx_bytes) ||
        !meiso_four_kind_lab_private_read_u64_file(RX_PACKETS_PATH,
                                                   &out_reading->rx_packets) ||
        !meiso_four_kind_lab_private_read_u64_file(TX_PACKETS_PATH,
                                                   &out_reading->tx_packets) ||
        !meiso_four_kind_lab_private_read_u64_file(RX_DROPPED_PATH,
                                                   &out_reading->rx_dropped) ||
        !meiso_four_kind_lab_private_read_u64_file(TX_DROPPED_PATH,
                                                   &out_reading->tx_dropped) ||
        !read_wireless_signal(&out_reading->signal_mbm,
                              &out_reading->signal_present)) {
        return false;
    }
    out_reading->carrier = carrier == 1U;
    return true;
}
