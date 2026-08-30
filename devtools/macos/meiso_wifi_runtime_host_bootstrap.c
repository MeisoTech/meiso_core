#define _POSIX_C_SOURCE 200809L

#include "meiso_wifi_runtime_host_private.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Owns entropy, numeric connect, fd flags, and Session transport bootstrap. */

void meiso_wifi_runtime_host_private_close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
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
        const ssize_t amount = read(fd, bytes + offset, size - offset);
        if (amount > 0) {
            offset += (size_t)amount;
        } else if (amount < 0 && errno == EINTR) {
            continue;
        } else {
            meiso_wifi_runtime_host_private_close_fd(&fd);
            return false;
        }
    }
    meiso_wifi_runtime_host_private_close_fd(&fd);
    return true;
}

static bool random_incarnation(uint64_t *out_value)
{
    if (out_value == NULL || !read_entropy(out_value, sizeof(*out_value))) {
        return false;
    }
    if (*out_value == 0U) {
        *out_value = 1U;
    }
    return true;
}

static bool random_nonce(MeisoSessionNonce *out_value)
{
    if (out_value == NULL || !read_entropy(out_value, sizeof(*out_value))) {
        return false;
    }
    if (!meiso_session_nonce_valid(*out_value)) {
        out_value->low = 1U;
    }
    return true;
}

static bool set_nonblocking_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    int descriptor_flags = fcntl(fd, F_GETFD);

    return flags >= 0 && descriptor_flags >= 0 &&
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

bool meiso_wifi_runtime_host_private_deadline_after_ms(struct timespec *out_deadline, uint64_t ms)
{
    uint64_t nanoseconds;

    if (out_deadline == NULL ||
        clock_gettime(CLOCK_MONOTONIC, out_deadline) != 0) {
        return false;
    }
    out_deadline->tv_sec += (time_t)(ms / UINT64_C(1000));
    nanoseconds = (uint64_t)out_deadline->tv_nsec +
        (ms % UINT64_C(1000)) * UINT64_C(1000000);
    out_deadline->tv_sec += (time_t)(nanoseconds / UINT64_C(1000000000));
    out_deadline->tv_nsec = (long)(nanoseconds % UINT64_C(1000000000));
    return true;
}

int meiso_wifi_runtime_host_private_deadline_wait_ms(const struct timespec *deadline)
{
    struct timespec now;
    int64_t seconds;
    int64_t nanoseconds;
    int64_t milliseconds;

    if (deadline == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
    nanoseconds = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
    if (nanoseconds < 0) {
        nanoseconds += INT64_C(1000000000);
        --seconds;
    }
    if (seconds < 0 || (seconds == 0 && nanoseconds <= 0)) {
        return 0;
    }
    milliseconds = seconds * INT64_C(1000) +
        (nanoseconds + INT64_C(999999)) / INT64_C(1000000);
    return milliseconds > INT32_MAX ? INT32_MAX : (int)milliseconds;
}

int meiso_wifi_runtime_host_private_connect_numeric_ipv4(const MeisoWifiRuntimeHostPrivateBootstrapConfig *config)
{
    struct sockaddr_in address;
    struct timespec deadline;
    struct pollfd descriptor;
    int fd = -1;
    int result;

    if (config == NULL || !meiso_wifi_runtime_host_private_deadline_after_ms(
            &deadline, MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONNECT_TIMEOUT_MS
        )) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0 || !set_nonblocking_cloexec(fd)) {
        meiso_wifi_runtime_host_private_close_fd(&fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(config->connect_port);
    address.sin_addr = config->connect_address;
    result = connect(
        fd, (const struct sockaddr *)(const void *)&address,
        (socklen_t)sizeof(address)
    );
    if (result == 0) {
        return fd;
    }
    if (errno != EINPROGRESS) {
        meiso_wifi_runtime_host_private_close_fd(&fd);
        return -1;
    }
    for (;;) {
        int wait_ms = meiso_wifi_runtime_host_private_deadline_wait_ms(&deadline);
        int socket_error = 0;
        socklen_t socket_error_size = (socklen_t)sizeof(socket_error);

        if (wait_ms == 0) {
            break;
        }
        descriptor.fd = fd;
        descriptor.events = POLLOUT;
        descriptor.revents = 0;
        result = poll(&descriptor, (nfds_t)1U, wait_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (descriptor.revents & POLLNVAL) != 0 ||
            getsockopt(
                fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size
            ) != 0 || socket_error != 0) {
            break;
        }
        if ((descriptor.revents & POLLOUT) != 0) {
            return fd;
        }
    }
    meiso_wifi_runtime_host_private_close_fd(&fd);
    return -1;
}

bool meiso_wifi_runtime_host_private_initialize_session_transport(MeisoWifiRuntimeHostPrivate *host)
{
    MeisoPosixTcpConfig adapter_config;
    size_t required;

    memset(&host->session_config, 0, sizeof(host->session_config));
    host->session_config.local_role = MEISO_SESSION_ROLE_HOST;
    host->session_config.local_peer_id = host->bootstrap.local_peer_id;
    host->session_config.expected_peer_id =
        host->bootstrap.expected_edge_peer_id;
    host->session_config.protocol = (MeisoProtocolVersion){1U, 0U};
    host->session_config.profile = *meiso_wifi_telemetry_profile();
    host->session_config.limits = (MeisoSessionLimits){
        MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_FRAME_BYTES,
        MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_RUNTIME_BYTES,
        MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_REGISTRY_ENTRIES
    };
    if (!random_incarnation(&host->session_config.local_incarnation) ||
        !random_nonce(&host->session_config.local_nonce)) {
        return false;
    }
    required = meiso_session_memory_required(&host->session_config);
    if (required == 0U || required > sizeof(meiso_wifi_runtime_host_private_storage.session.bytes) ||
        meiso_session_init(
            meiso_wifi_runtime_host_private_storage.session.bytes, sizeof(meiso_wifi_runtime_host_private_storage.session.bytes),
            &host->session_config, &host->session
        ) != MEISO_SESSION_INIT_OK) {
        return false;
    }
    adapter_config = (MeisoPosixTcpConfig){
        host->fd, host->session, MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_FRAME_BYTES, MEISO_WIFI_RUNTIME_HOST_PRIVATE_IO_STEP_BYTES
    };
    required = meiso_posix_tcp_memory_required(&adapter_config);
    if (required == 0U || required > sizeof(meiso_wifi_runtime_host_private_storage.adapter.bytes) ||
        meiso_posix_tcp_init(
            meiso_wifi_runtime_host_private_storage.adapter.bytes, sizeof(meiso_wifi_runtime_host_private_storage.adapter.bytes),
            &adapter_config, &host->adapter
        ) != MEISO_POSIX_TCP_INIT_OK) {
        return false;
    }
    return meiso_session_start(host->session) ==
        MEISO_SESSION_START_STARTED;
}
