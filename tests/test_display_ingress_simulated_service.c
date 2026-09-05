#define _POSIX_C_SOURCE 200809L

#include <meiso/host/display_ingress_fixture_input.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            return false;                                                      \
        }                                                                      \
    } while (0)

enum {
    OUTPUT_CAPACITY = 4096,
    LOOPBACK_ADDRESS_HOST_ORDER = 0x7f000001,
    CHILD_TIMEOUT_MS = 10000,
    FRAME_WIDTH = 64,
    FRAME_HEIGHT = 32,
    FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 4,
    FRAME_RECORD_BYTES =
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE +
        FRAME_BYTES
};

static const uint64_t FRAME_SOURCE_CLOCK_EPOCH =
    UINT64_C(0x1122334455667788);
static const uint64_t FRAME_SOURCE_TIME_NS =
    UINT64_C(0x0102030405060708);
static const uint64_t FRAME_PRODUCER_DROPS = 3U;
static const char FRAME_SHA256_HEX[] =
    "dea251d45e84a4130d8b423a6626724fd916fb3c199f17737dd655d59c58f0ad";

typedef struct {
    pid_t pid;
    int stdout_fd;
    int stderr_fd;
    int exit_code;
    size_t stdout_size;
    size_t stderr_size;
    bool reaped;
    char stdout_bytes[OUTPUT_CAPACITY];
    char stderr_bytes[OUTPUT_CAPACITY];
} Child;

typedef struct {
    uint16_t local_port;
    uint16_t peer_port;
} UdpTuple;

static void close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool set_close_on_exec(int fd)
{
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool clear_close_on_exec(int fd)
{
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
        (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static bool make_tcp_pair(int pair[2])
{
    struct sockaddr_in address;
    socklen_t address_size = (socklen_t)sizeof(address);
    int listener = -1;
    int client = -1;
    int server = -1;
    int reuse = 1;
    bool ok = false;

    pair[0] = -1;
    pair[1] = -1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || setsockopt(
            listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)
        ) != 0) {
        goto done;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(LOOPBACK_ADDRESS_HOST_ORDER);
    if (bind(
            listener, (const struct sockaddr *)(const void *)&address,
            sizeof(address)
        ) != 0 || listen(listener, 1) != 0 || getsockname(
            listener, (struct sockaddr *)(void *)&address, &address_size
        ) != 0) {
        goto done;
    }
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 || connect(
            client, (const struct sockaddr *)(const void *)&address,
            address_size
        ) != 0) {
        goto done;
    }
    server = accept(listener, NULL, NULL);
    if (server < 0 || !set_nonblocking(client) ||
        !set_nonblocking(server) || !set_close_on_exec(client) ||
        !set_close_on_exec(server)) {
        goto done;
    }
    pair[0] = client;
    pair[1] = server;
    client = -1;
    server = -1;
    ok = true;

done:
    close_fd(&listener);
    close_fd(&client);
    close_fd(&server);
    return ok;
}

static bool bind_udp_loopback(int fd, struct sockaddr_in *out_address)
{
    struct sockaddr_in address;
    socklen_t size = (socklen_t)sizeof(address);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(LOOPBACK_ADDRESS_HOST_ORDER);
    if (bind(
            fd, (const struct sockaddr *)(const void *)&address,
            sizeof(address)
        ) != 0 || getsockname(
            fd, (struct sockaddr *)(void *)&address, &size
        ) != 0) {
        return false;
    }
    *out_address = address;
    return true;
}

static bool make_udp_pair(int pair[2])
{
    struct sockaddr_in first;
    struct sockaddr_in second;

    pair[0] = socket(AF_INET, SOCK_DGRAM, 0);
    pair[1] = socket(AF_INET, SOCK_DGRAM, 0);
    if (pair[0] < 0 || pair[1] < 0 ||
        !bind_udp_loopback(pair[0], &first) ||
        !bind_udp_loopback(pair[1], &second) ||
        connect(
            pair[0], (const struct sockaddr *)(const void *)&second,
            sizeof(second)
        ) != 0 || connect(
            pair[1], (const struct sockaddr *)(const void *)&first,
            sizeof(first)
        ) != 0 || !set_nonblocking(pair[0]) ||
        !set_nonblocking(pair[1]) || !set_close_on_exec(pair[0]) ||
        !set_close_on_exec(pair[1])) {
        close_fd(&pair[0]);
        close_fd(&pair[1]);
        return false;
    }
    return true;
}

static bool make_frame_pair(int pair[2])
{
    pair[0] = -1;
    pair[1] = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
        !set_nonblocking(pair[0]) || !set_nonblocking(pair[1]) ||
        !set_close_on_exec(pair[0]) || !set_close_on_exec(pair[1])) {
        close_fd(&pair[0]);
        close_fd(&pair[1]);
        return false;
    }
    return true;
}

static void fill_external_frame(uint8_t frame[FRAME_BYTES])
{
    uint32_t y;

    for (y = 0U; y < FRAME_HEIGHT; ++y) {
        uint32_t x;

        for (x = 0U; x < FRAME_WIDTH; ++x) {
            const size_t offset =
                ((size_t)y * FRAME_WIDTH + (size_t)x) * 4U;

            frame[offset] = (uint8_t)(x * 5U + y * 3U + 17U);
            frame[offset + 1U] = (uint8_t)(x ^ (y * 11U) ^ 0x40U);
            frame[offset + 2U] = (uint8_t)(x * 13U + y * 7U + 0x23U);
            frame[offset + 3U] = UINT8_MAX;
        }
    }
}

static bool make_frame_record(uint8_t record[FRAME_RECORD_BYTES])
{
    MeisoDisplayIngressHostFixtureInput input;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];
    uint8_t frame[FRAME_BYTES];
    char digest_hex[MEISO_SHA256_DIGEST_SIZE * 2U + 1U];
    size_t header_size = 0U;
    size_t index;

    fill_external_frame(frame);
    memset(&input, 0, sizeof(input));
    input.input_width = FRAME_WIDTH;
    input.input_height = FRAME_HEIGHT;
    input.row_pitch_bytes = FRAME_WIDTH * 4U;
    input.frame.frame_sequence = 1U;
    input.frame.source_time = (MeisoTimePoint){
        FRAME_SOURCE_CLOCK_EPOCH, FRAME_SOURCE_TIME_NS
    };
    input.frame.producer_dropped_frames = FRAME_PRODUCER_DROPS;
    input.frame.rgba_frame = (MeisoByteView){frame, sizeof(frame)};
    if (!meiso_display_ingress_host_fixture_input_sha256(
            input.frame.rgba_frame, digest
        )) {
        return false;
    }
    for (index = 0U; index < sizeof(digest); ++index) {
        static const char HEX[] = "0123456789abcdef";
        digest_hex[index * 2U] = HEX[digest[index] >> 4U];
        digest_hex[index * 2U + 1U] = HEX[digest[index] & 0x0fU];
    }
    digest_hex[sizeof(digest) * 2U] = '\0';
    if (strcmp(digest_hex, FRAME_SHA256_HEX) != 0 ||
        meiso_display_ingress_host_fixture_input_header_encode(
            &input,
            (MeisoMutableBuffer){
                record,
                MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE
            },
            &header_size
        ) != MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK ||
        header_size !=
            MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE) {
        return false;
    }
    memmove(record + header_size, frame, sizeof(frame));
    return true;
}

static bool send_bytes(int fd, const uint8_t *bytes, size_t size)
{
    const uint64_t start = monotonic_ms();
    const uint64_t deadline = start == 0U ? 0U : start + 2000U;
    size_t offset = 0U;

    while (deadline != 0U && monotonic_ms() < deadline && offset < size) {
        const ssize_t count = send(fd, bytes + offset, size - offset, 0);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 &&
                   (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {fd, POLLOUT, 0};
            if (poll(&descriptor, (nfds_t)1U, 20) < 0 && errno != EINTR) {
                return false;
            }
        } else {
            return false;
        }
    }
    return offset == size;
}

static bool send_frame_record_and_eof(
    int fd,
    const uint8_t *record,
    size_t size
)
{
    return send_bytes(fd, record, size) && shutdown(fd, SHUT_WR) == 0;
}

static bool send_frame_record_fragmented_and_eof(
    int fd,
    const uint8_t record[FRAME_RECORD_BYTES]
)
{
    const size_t header_tail =
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE - 3U;
    const size_t payload_offset =
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE;

    return send_bytes(fd, record, 3U) &&
        send_bytes(fd, record + 3U, header_tail) &&
        send_bytes(fd, record + payload_offset, 17U) &&
        send_bytes(
            fd,
            record + payload_offset + 17U,
            FRAME_BYTES - 17U
        ) && shutdown(fd, SHUT_WR) == 0;
}

static bool wait_for_peer_close(int fd)
{
    const uint64_t start = monotonic_ms();
    const uint64_t deadline = start == 0U ? 0U : start + 2000U;

    while (deadline != 0U && monotonic_ms() < deadline) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        uint8_t byte;
        const int poll_status = poll(&descriptor, (nfds_t)1U, 20);

        if (poll_status < 0 && errno == EINTR) {
            continue;
        }
        if (poll_status < 0 || (descriptor.revents & POLLNVAL) != 0) {
            return false;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const ssize_t count = recv(fd, &byte, sizeof(byte), 0);
            if (count == 0) {
                return true;
            }
            if (count > 0 ||
                (count < 0 && errno != EINTR && errno != EAGAIN &&
                 errno != EWOULDBLOCK)) {
                return false;
            }
        }
    }
    return false;
}

static bool udp_tuple(int fd, UdpTuple *out_tuple)
{
    struct sockaddr_in local;
    struct sockaddr_in peer;
    socklen_t local_size = (socklen_t)sizeof(local);
    socklen_t peer_size = (socklen_t)sizeof(peer);

    memset(&local, 0, sizeof(local));
    memset(&peer, 0, sizeof(peer));
    if (out_tuple == NULL || getsockname(
            fd, (struct sockaddr *)(void *)&local, &local_size
        ) != 0 || getpeername(
            fd, (struct sockaddr *)(void *)&peer, &peer_size
        ) != 0 || local.sin_family != AF_INET || peer.sin_family != AF_INET ||
        local.sin_port == 0U || peer.sin_port == 0U) {
        return false;
    }
    out_tuple->local_port = local.sin_port;
    out_tuple->peer_port = peer.sin_port;
    return true;
}

static bool spawn_role(
    const char *program,
    const char *role,
    int control_fd,
    int media_fd,
    int frame_fd,
    int close_control_fd,
    int close_media_fd,
    int close_frame_fd,
    bool duplicate_frame_argument,
    Child *out_child
)
{
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    char control_text[32U];
    char media_text[32U];
    char frame_text[32U];
    pid_t pid;

    memset(out_child, 0, sizeof(*out_child));
    out_child->pid = -1;
    out_child->stdout_fd = -1;
    out_child->stderr_fd = -1;
    out_child->exit_code = -1;
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0 ||
        !set_close_on_exec(stdout_pipe[0]) ||
        !set_close_on_exec(stdout_pipe[1]) ||
        !set_close_on_exec(stderr_pipe[0]) ||
        !set_close_on_exec(stderr_pipe[1])) {
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        return false;
    }
    (void)snprintf(control_text, sizeof(control_text), "%d", control_fd);
    (void)snprintf(media_text, sizeof(media_text), "%d", media_fd);
    (void)snprintf(frame_text, sizeof(frame_text), "%d", frame_fd);
    pid = fork();
    if (pid < 0) {
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        return false;
    }
    if (pid == 0) {
        (void)close(stdout_pipe[0]);
        (void)close(stderr_pipe[0]);
        if (close_control_fd >= 0 && close_control_fd != control_fd &&
            close_control_fd != media_fd && close_control_fd != frame_fd) {
            (void)close(close_control_fd);
        }
        if (close_media_fd >= 0 && close_media_fd != control_fd &&
            close_media_fd != media_fd && close_media_fd != frame_fd) {
            (void)close(close_media_fd);
        }
        if (close_frame_fd >= 0 && close_frame_fd != control_fd &&
            close_frame_fd != media_fd && close_frame_fd != frame_fd) {
            (void)close(close_frame_fd);
        }
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        (void)close(stdout_pipe[1]);
        (void)close(stderr_pipe[1]);
        if ((control_fd >= 3 && !clear_close_on_exec(control_fd)) ||
            (media_fd >= 3 && !clear_close_on_exec(media_fd)) ||
            (frame_fd >= 3 && !clear_close_on_exec(frame_fd))) {
            _exit(126);
        }
        if (frame_fd >= 0 && duplicate_frame_argument) {
            execl(
                program,
                program,
                "--role", role,
                "--control-fd", control_text,
                "--media-fd", media_text,
                "--frame-fd", frame_text,
                "--frame-fd", frame_text,
                (char *)NULL
            );
        } else if (frame_fd >= 0) {
            execl(
                program,
                program,
                "--role", role,
                "--control-fd", control_text,
                "--media-fd", media_text,
                "--frame-fd", frame_text,
                (char *)NULL
            );
        } else {
            execl(
                program,
                program,
                "--role", role,
                "--control-fd", control_text,
                "--media-fd", media_text,
                (char *)NULL
            );
        }
        _exit(127);
    }
    close_fd(&stdout_pipe[1]);
    close_fd(&stderr_pipe[1]);
    if (!set_nonblocking(stdout_pipe[0]) ||
        !set_nonblocking(stderr_pipe[0])) {
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
        close_fd(&stdout_pipe[0]);
        close_fd(&stderr_pipe[0]);
        return false;
    }
    out_child->pid = pid;
    out_child->stdout_fd = stdout_pipe[0];
    out_child->stderr_fd = stderr_pipe[0];
    out_child->exit_code = -1;
    return true;
}

static bool spawn_pair(
    const char *program,
    const char *argument_zero,
    int frame_fd,
    int close_frame_fd,
    bool inherit_frame_fd,
    bool ignore_sigchld,
    Child *out_child
)
{
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    char frame_text[32U];
    pid_t pid;

    if (program == NULL || argument_zero == NULL || frame_fd < 3 ||
        out_child == NULL) {
        return false;
    }
    memset(out_child, 0, sizeof(*out_child));
    out_child->pid = -1;
    out_child->stdout_fd = -1;
    out_child->stderr_fd = -1;
    out_child->exit_code = -1;
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0 ||
        !set_close_on_exec(stdout_pipe[0]) ||
        !set_close_on_exec(stdout_pipe[1]) ||
        !set_close_on_exec(stderr_pipe[0]) ||
        !set_close_on_exec(stderr_pipe[1])) {
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        return false;
    }
    (void)snprintf(frame_text, sizeof(frame_text), "%d", frame_fd);
    pid = fork();
    if (pid < 0) {
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        return false;
    }
    if (pid == 0) {
        struct sigaction child_action;

        (void)close(stdout_pipe[0]);
        (void)close(stderr_pipe[0]);
        if (close_frame_fd >= 0 && close_frame_fd != frame_fd) {
            (void)close(close_frame_fd);
        }
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        (void)close(stdout_pipe[1]);
        (void)close(stderr_pipe[1]);
        if (inherit_frame_fd && !clear_close_on_exec(frame_fd)) {
            _exit(126);
        }
        if (ignore_sigchld) {
            memset(&child_action, 0, sizeof(child_action));
            child_action.sa_handler = SIG_IGN;
            if (sigemptyset(&child_action.sa_mask) != 0 ||
                sigaction(SIGCHLD, &child_action, NULL) != 0) {
                _exit(126);
            }
        }
        execl(
            program,
            argument_zero,
            "--pair",
            "--frame-fd",
            frame_text,
            (char *)NULL
        );
        _exit(127);
    }
    close_fd(&stdout_pipe[1]);
    close_fd(&stderr_pipe[1]);
    if (!set_nonblocking(stdout_pipe[0]) ||
        !set_nonblocking(stderr_pipe[0])) {
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
        close_fd(&stdout_pipe[0]);
        close_fd(&stderr_pipe[0]);
        return false;
    }
    out_child->pid = pid;
    out_child->stdout_fd = stdout_pipe[0];
    out_child->stderr_fd = stderr_pipe[0];
    return true;
}

static bool drain_output(
    int *fd,
    char output[OUTPUT_CAPACITY],
    size_t *used
)
{
    while (*fd >= 0 && *used + 1U < OUTPUT_CAPACITY) {
        const ssize_t count = read(
            *fd, output + *used, OUTPUT_CAPACITY - *used - 1U
        );
        if (count > 0) {
            *used += (size_t)count;
            output[*used] = '\0';
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 &&
                   (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        } else if (count == 0) {
            close_fd(fd);
            break;
        } else {
            return false;
        }
    }
    return *used + 1U < OUTPUT_CAPACITY;
}

static void terminate_children(Child *children, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (children[index].pid > 0 && !children[index].reaped) {
            (void)kill(children[index].pid, SIGKILL);
        }
    }
    for (index = 0U; index < count; ++index) {
        if (children[index].pid > 0 && !children[index].reaped) {
            while (waitpid(children[index].pid, NULL, 0) < 0 &&
                   errno == EINTR) {
            }
            children[index].reaped = true;
        }
        close_fd(&children[index].stdout_fd);
        close_fd(&children[index].stderr_fd);
    }
}

static bool collect_children(Child *children, size_t count)
{
    const uint64_t start = monotonic_ms();
    const uint64_t deadline = start == 0U ? 0U : start + CHILD_TIMEOUT_MS;
    bool success = false;

    if (children == NULL || count == 0U || count > 2U) {
        return false;
    }
    while (deadline != 0U && monotonic_ms() < deadline) {
        struct pollfd descriptors[4U];
        nfds_t descriptor_count = 0U;
        size_t index;
        bool all_reaped = true;
        bool all_closed = true;

        for (index = 0U; index < count; ++index) {
            int status;
            pid_t waited;

            if (!drain_output(
                    &children[index].stdout_fd,
                    children[index].stdout_bytes,
                    &children[index].stdout_size
                ) || !drain_output(
                    &children[index].stderr_fd,
                    children[index].stderr_bytes,
                    &children[index].stderr_size
                )) {
                goto done;
            }
            if (!children[index].reaped) {
                waited = waitpid(children[index].pid, &status, WNOHANG);
                if (waited == children[index].pid) {
                    children[index].reaped = true;
                    children[index].exit_code = WIFEXITED(status) ?
                        WEXITSTATUS(status) : -1;
                } else if (waited < 0 && errno != EINTR) {
                    goto done;
                }
            }
            all_reaped = all_reaped && children[index].reaped;
            all_closed = all_closed && children[index].stdout_fd < 0 &&
                children[index].stderr_fd < 0;
            if (children[index].stdout_fd >= 0) {
                descriptors[descriptor_count++] = (struct pollfd){
                    children[index].stdout_fd, POLLIN, 0
                };
            }
            if (children[index].stderr_fd >= 0) {
                descriptors[descriptor_count++] = (struct pollfd){
                    children[index].stderr_fd, POLLIN, 0
                };
            }
        }
        if (all_reaped && all_closed) {
            success = true;
            break;
        }
        if (poll(descriptors, descriptor_count, 20) < 0 && errno != EINTR) {
            goto done;
        }
    }

done:
    if (!success) {
        terminate_children(children, count);
    } else {
        size_t index;
        for (index = 0U; index < count; ++index) {
            close_fd(&children[index].stdout_fd);
            close_fd(&children[index].stderr_fd);
        }
    }
    return success;
}

static bool collect_child(Child *child)
{
    return collect_children(child, 1U);
}

static bool extract_session_id(const char *receipt, char output[65U])
{
    static const char PREFIX[] = "\"session_id\":\"";
    const char *start = strstr(receipt, PREFIX);

    if (start == NULL) {
        return false;
    }
    start += sizeof(PREFIX) - 1U;
    if (strlen(start) < 65U || start[64] != '"') {
        return false;
    }
    memmove(output, start, 64U);
    output[64U] = '\0';
    return true;
}

static bool lowercase_hex_64(const char *value)
{
    size_t index;

    if (value == NULL || strlen(value) != 64U) {
        return false;
    }
    for (index = 0U; index < 64U; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool format_success_receipt(
    char expected[2048U],
    const char *role,
    const char *session
)
{
    int size;

    if (expected == NULL || role == NULL || session == NULL) {
        return false;
    }
    if (strcmp(role, "host") == 0) {
        size = snprintf(
            expected,
            2048U,
            "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V2\","
            "\"role\":\"host\",\"status\":\"SUCCESS\","
            "\"frame_input\":\"DIFI/v1\","
            "\"session_id\":\"%s\",\"catalog_generation\":1,"
            "\"applied_desired_version\":1,\"actual_version\":1,"
            "\"production_epoch\":1,\"media_stream_epoch\":1,"
            "\"input_width\":64,\"input_height\":32,"
            "\"row_pitch_bytes\":256,\"rgba_bytes\":8192,"
            "\"frame_sequence\":1,"
            "\"source_clock_epoch\":%" PRIu64 ","
            "\"source_time_ns\":%" PRIu64 ","
            "\"sender_deadline_present\":false,"
            "\"sender_deadline_clock_epoch\":0,"
            "\"sender_deadline_ns\":0,"
            "\"producer_dropped_frames\":%" PRIu64 ","
            "\"input_sha256\":\"%s\","
            "\"evidence_input_sha256\":\"%s\","
            "\"evidence_class\":\"SIMULATED\","
            "\"connector\":\"UNSPECIFIED\","
            "\"carrier\":\"CONTROLLED_RAW_UDP\","
            "\"physical_evidence\":false,\"link_security\":false,"
            "\"clean_shutdown\":true}\n",
            session,
            FRAME_SOURCE_CLOCK_EPOCH,
            FRAME_SOURCE_TIME_NS,
            FRAME_PRODUCER_DROPS,
            FRAME_SHA256_HEX,
            FRAME_SHA256_HEX
        );
    } else {
        size = snprintf(
            expected,
            2048U,
            "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V2\","
            "\"role\":\"edge\",\"status\":\"SUCCESS\","
            "\"session_id\":\"%s\",\"catalog_generation\":1,"
            "\"applied_desired_version\":1,\"actual_version\":1,"
            "\"production_epoch\":1,\"media_stream_epoch\":1,"
            "\"input_width\":64,\"input_height\":32,"
            "\"row_pitch_bytes\":256,\"rgba_bytes\":8192,"
            "\"frame_sequence\":1,"
            "\"source_clock_epoch\":%" PRIu64 ","
            "\"source_time_ns\":%" PRIu64 ","
            "\"sender_deadline_present\":false,"
            "\"sender_deadline_clock_epoch\":0,"
            "\"sender_deadline_ns\":0,"
            "\"producer_dropped_frames\":%" PRIu64 ","
            "\"received_sha256\":\"%s\","
            "\"evidence_input_sha256\":\"%s\","
            "\"evidence_class\":\"SIMULATED\","
            "\"connector\":\"UNSPECIFIED\","
            "\"udp_armed_before_actual_write\":true,"
            "\"carrier\":\"CONTROLLED_RAW_UDP\","
            "\"physical_evidence\":false,\"link_security\":false,"
            "\"clean_shutdown\":true}\n",
            session,
            FRAME_SOURCE_CLOCK_EPOCH,
            FRAME_SOURCE_TIME_NS,
            FRAME_PRODUCER_DROPS,
            FRAME_SHA256_HEX,
            FRAME_SHA256_HEX
        );
    }
    return size > 0 && (size_t)size < 2048U;
}

static bool success_receipt_valid(
    const char *receipt,
    const char *role,
    const char *session
)
{
    char expected[2048U];

    return receipt != NULL &&
        format_success_receipt(expected, role, session) &&
        strcmp(receipt, expected) == 0;
}

static bool run_happy_once(
    const char *program,
    char out_session_id[65U],
    int udp[2]
)
{
    int tcp[2] = {-1, -1};
    int frame[2] = {-1, -1};
    uint8_t record[FRAME_RECORD_BYTES];
    Child children[2U];
    bool ok = false;

    memset(children, 0, sizeof(children));
    children[0].pid = -1;
    children[0].stdout_fd = -1;
    children[0].stderr_fd = -1;
    children[1].pid = -1;
    children[1].stdout_fd = -1;
    children[1].stderr_fd = -1;
    if (!make_tcp_pair(tcp) || !make_frame_pair(frame) ||
        !make_frame_record(record) || udp[0] < 0 || udp[1] < 0) {
        goto done;
    }
    if (!spawn_role(
            program,
            "host",
            tcp[0],
            udp[0],
            frame[0],
            tcp[1],
            udp[1],
            frame[1],
            false,
            &children[0]
        )) {
        goto done;
    }
    close_fd(&frame[0]);
    if (!send_frame_record_fragmented_and_eof(frame[1], record) ||
        !wait_for_peer_close(frame[1])) {
        goto done;
    }
    close_fd(&frame[1]);
    if (!spawn_role(
            program,
            "edge",
            tcp[1],
            udp[1],
            -1,
            tcp[0],
            udp[0],
            -1,
            false,
            &children[1]
        )) {
        terminate_children(children, 1U);
        goto done;
    }
    close_fd(&tcp[0]);
    close_fd(&tcp[1]);
    close_fd(&udp[0]);
    close_fd(&udp[1]);
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    if (!collect_children(children, 2U) || children[0].exit_code != 0 ||
        children[1].exit_code != 0 || children[0].stderr_bytes[0] != '\0' ||
        children[1].stderr_bytes[0] != '\0' || !extract_session_id(
            children[0].stdout_bytes, out_session_id
        ) || !lowercase_hex_64(out_session_id) || !success_receipt_valid(
            children[0].stdout_bytes, "host", out_session_id
        )) {
        goto done;
    }
    {
        char edge_session[65U];
        if (!extract_session_id(children[1].stdout_bytes, edge_session) ||
            strcmp(out_session_id, edge_session) != 0 ||
            !success_receipt_valid(
                children[1].stdout_bytes, "edge", edge_session
            )) {
            goto done;
        }
    }
    ok = true;

done:
    close_fd(&tcp[0]);
    close_fd(&tcp[1]);
    close_fd(&udp[0]);
    close_fd(&udp[1]);
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    if (!ok) {
        terminate_children(children, 2U);
    }
    return ok;
}

static bool pair_success_receipt_valid(
    const char *receipt,
    const char *session
)
{
    char host[2048U];
    char edge[2048U];
    char expected[4096U];
    size_t host_size;
    size_t edge_size;
    int size;

    if (receipt == NULL || session == NULL ||
        !format_success_receipt(host, "host", session) ||
        !format_success_receipt(edge, "edge", session)) {
        return false;
    }
    host_size = strlen(host);
    edge_size = strlen(edge);
    if (host_size < 2U || edge_size < 2U ||
        host[host_size - 1U] != '\n' || edge[edge_size - 1U] != '\n') {
        return false;
    }
    host[host_size - 1U] = '\0';
    edge[edge_size - 1U] = '\0';
    size = snprintf(
        expected,
        sizeof(expected),
        "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_PAIR_RECEIPT_V1\","
        "\"status\":\"SUCCESS\",\"host\":%s,\"edge\":%s,"
        "\"fixture_only\":true,\"physical_evidence\":false,"
        "\"link_security\":false,\"clean_shutdown\":true}\n",
        host,
        edge
    );
    return size > 0 && (size_t)size < sizeof(expected) &&
        strcmp(receipt, expected) == 0;
}

static bool run_pair_happy_once(
    const char *program,
    char out_session_id[65U],
    bool inherit_ignored_sigchld
)
{
    int frame[2] = {-1, -1};
    uint8_t record[FRAME_RECORD_BYTES];
    Child child;
    bool spawned = false;
    bool collected = false;
    bool ok = false;

    memset(&child, 0, sizeof(child));
    child.pid = -1;
    child.stdout_fd = -1;
    child.stderr_fd = -1;
    if (!make_frame_pair(frame) || !make_frame_record(record) ||
        !spawn_pair(
            program,
            program,
            frame[0],
            frame[1],
            true,
            inherit_ignored_sigchld,
            &child
        )) {
        goto done;
    }
    spawned = true;
    close_fd(&frame[0]);
    if (!send_frame_record_fragmented_and_eof(frame[1], record) ||
        !wait_for_peer_close(frame[1])) {
        goto done;
    }
    close_fd(&frame[1]);
    collected = collect_child(&child);
    if (!collected || child.exit_code != 0 ||
        child.stderr_bytes[0] != '\0' || !extract_session_id(
            child.stdout_bytes, out_session_id
        ) || !lowercase_hex_64(out_session_id) ||
        !pair_success_receipt_valid(child.stdout_bytes, out_session_id)) {
        goto done;
    }
    ok = true;

done:
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    if (spawned && !collected) {
        terminate_children(&child, 1U);
    }
    return ok;
}

static bool pair_failure_receipt_valid(
    const Child *child,
    const char *stage
)
{
    char expected[512U];
    int size;

    if (child == NULL || stage == NULL || child->exit_code != 1 ||
        child->stderr_bytes[0] != '\0') {
        return false;
    }
    size = snprintf(
        expected,
        sizeof(expected),
        "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_PAIR_RECEIPT_V1\","
        "\"status\":\"FAILURE\",\"stage\":\"%s\","
        "\"fixture_only\":true,\"physical_evidence\":false,"
        "\"link_security\":false,\"clean_shutdown\":false}\n",
        stage
    );
    return size > 0 && (size_t)size < sizeof(expected) &&
        strcmp(child->stdout_bytes, expected) == 0;
}

static bool run_pair_input_failure_case(
    const char *program,
    const uint8_t *bytes,
    size_t size,
    bool send_eof
)
{
    int frame[2] = {-1, -1};
    Child child;
    bool spawned = false;
    bool collected = false;
    bool ok = false;

    memset(&child, 0, sizeof(child));
    child.pid = -1;
    child.stdout_fd = -1;
    child.stderr_fd = -1;
    if (!make_frame_pair(frame) ||
        !spawn_pair(
            program, program, frame[0], frame[1], true, false, &child
        )) {
        goto done;
    }
    spawned = true;
    close_fd(&frame[0]);
    if (!send_bytes(frame[1], bytes, size) ||
        (send_eof && shutdown(frame[1], SHUT_WR) != 0)) {
        goto done;
    }
    collected = collect_child(&child);
    ok = collected && pair_failure_receipt_valid(&child, "input") &&
        wait_for_peer_close(frame[1]);

done:
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    if (spawned && !collected) {
        terminate_children(&child, 1U);
    }
    return ok;
}

static bool run_pair_input_failure_matrix(const char *program)
{
    uint8_t record[FRAME_RECORD_BYTES + 1U];

    CHECK(make_frame_record(record));
    CHECK(run_pair_input_failure_case(program, record, 0U, true));
    CHECK(run_pair_input_failure_case(program, record, 64U, true));
    CHECK(run_pair_input_failure_case(
        program, record, FRAME_RECORD_BYTES, false
    ));

    CHECK(make_frame_record(record));
    record[FRAME_RECORD_BYTES] = 0x6dU;
    CHECK(run_pair_input_failure_case(
        program, record, sizeof(record), true
    ));

    CHECK(make_frame_record(record));
    record[MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE] ^= 1U;
    CHECK(run_pair_input_failure_case(
        program, record, FRAME_RECORD_BYTES, true
    ));
    return true;
}

static bool run_pair_argument_failure(const char *program)
{
    int frame[2] = {-1, -1};
    Child child;
    bool spawned = false;
    bool collected = false;
    bool ok = false;

    memset(&child, 0, sizeof(child));
    child.pid = -1;
    child.stdout_fd = -1;
    child.stderr_fd = -1;
    if (!make_frame_pair(frame) || !spawn_pair(
            program,
            "relative-program",
            frame[0],
            frame[1],
            true,
            false,
            &child
        )) {
        goto done;
    }
    spawned = true;
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    collected = collect_child(&child);
    ok = collected && child.exit_code == 2 &&
        child.stdout_bytes[0] == '\0' &&
        strcmp(child.stderr_bytes, "E_ARGUMENT\n") == 0;

done:
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    if (spawned && !collected) {
        terminate_children(&child, 1U);
    }
    return ok;
}

static bool run_pair_closed_fd_failure(const char *program)
{
    Child child;
    int closed_fd;
    bool spawned = false;
    bool collected = false;
    bool ok = false;

    memset(&child, 0, sizeof(child));
    child.pid = -1;
    child.stdout_fd = -1;
    child.stderr_fd = -1;
    closed_fd = fcntl(STDERR_FILENO, F_DUPFD, 64);
    if (closed_fd < 64 || close(closed_fd) != 0 ||
        fcntl(closed_fd, F_GETFD) >= 0 || errno != EBADF || !spawn_pair(
            program,
            program,
            closed_fd,
            -1,
            false,
            false,
            &child
        )) {
        return false;
    }
    spawned = true;
    collected = collect_child(&child);
    ok = collected && pair_failure_receipt_valid(&child, "input");
    if (spawned && !collected) {
        terminate_children(&child, 1U);
    }
    return ok;
}

static bool argument_failure_child(
    const char *program,
    const char *role,
    int control_fd,
    int media_fd,
    int frame_fd,
    bool duplicate_frame_argument
)
{
    Child child;

    return spawn_role(
            program,
            role,
            control_fd,
            media_fd,
            frame_fd,
            -1,
            -1,
            -1,
            duplicate_frame_argument,
            &child
        ) && collect_child(&child) && child.exit_code == 2 &&
        child.stdout_bytes[0] == '\0' &&
        strcmp(child.stderr_bytes, "E_ARGUMENT\n") == 0;
}

static bool run_argument_failure(const char *program)
{
    int tcp[2] = {-1, -1};
    int udp[2] = {-1, -1};
    int frame[2] = {-1, -1};
    bool ok;

    if (!make_tcp_pair(tcp) || !make_udp_pair(udp) ||
        !make_frame_pair(frame)) {
        close_fd(&tcp[0]);
        close_fd(&tcp[1]);
        close_fd(&udp[0]);
        close_fd(&udp[1]);
        close_fd(&frame[0]);
        close_fd(&frame[1]);
        return false;
    }
    ok = argument_failure_child(
            program, "host", tcp[0], udp[0], -1, false
        ) && argument_failure_child(
            program, "host", tcp[0], udp[0], frame[0], true
        ) && argument_failure_child(
            program, "host", tcp[0], udp[0], tcp[0], false
        ) && argument_failure_child(
            program, "host", tcp[0], udp[0], udp[0], false
        ) && argument_failure_child(
            program, "edge", tcp[1], udp[1], frame[0], false
        ) && argument_failure_child(
            program, "invalid", tcp[0], udp[0], -1, false
        ) && argument_failure_child(
            program, "host", 0, udp[0], frame[0], false
        ) && argument_failure_child(
            program, "host", 1, udp[0], frame[0], false
        ) && argument_failure_child(
            program, "host", 2, udp[0], frame[0], false
        ) && argument_failure_child(
            program, "host", tcp[0], tcp[0], frame[0], false
        );
    close_fd(&tcp[0]);
    close_fd(&tcp[1]);
    close_fd(&udp[0]);
    close_fd(&udp[1]);
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    return ok;
}

static bool failure_receipt_valid(
    const Child *child,
    const char *role,
    const char *plane,
    const char *stage
)
{
    char expected[512U];
    int size;

    CHECK(child->exit_code == 1);
    CHECK(child->stderr_bytes[0] == '\0');
    size = snprintf(
        expected,
        sizeof(expected),
        "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V2\","
        "\"role\":\"%s\",\"status\":\"FAILURE\","
        "\"plane\":\"%s\",\"stage\":\"%s\","
        "\"carrier\":\"CONTROLLED_RAW_UDP\","
        "\"physical_evidence\":false,\"link_security\":false,"
        "\"clean_shutdown\":false}\n",
        role,
        plane,
        stage
    );
    return size > 0 && (size_t)size < sizeof(expected) &&
        strcmp(child->stdout_bytes, expected) == 0;
}

static bool stream_closed_without_bytes(int fd)
{
    uint8_t byte;
    ssize_t count;

    do {
        count = recv(fd, &byte, sizeof(byte), 0);
    } while (count < 0 && errno == EINTR);
    return count == 0;
}

static bool run_input_failure_bytes(
    const char *program,
    const uint8_t *bytes,
    size_t size,
    bool send_eof
)
{
    int control[2] = {-1, -1};
    int media[2] = {-1, -1};
    int frame[2] = {-1, -1};
    Child child;
    bool spawned = false;
    bool collected = false;
    bool ok = false;

    memset(&child, 0, sizeof(child));
    child.pid = -1;
    child.stdout_fd = -1;
    child.stderr_fd = -1;
    if (!make_tcp_pair(control) || !make_udp_pair(media) ||
        !make_frame_pair(frame) || !spawn_role(
            program,
            "host",
            control[0],
            media[0],
            frame[0],
            control[1],
            media[1],
            frame[1],
            false,
            &child
        )) {
        goto done;
    }
    spawned = true;
    close_fd(&control[0]);
    close_fd(&media[0]);
    close_fd(&frame[0]);
    if (!send_bytes(frame[1], bytes, size) ||
        (send_eof && shutdown(frame[1], SHUT_WR) != 0)) {
        goto done;
    }
    collected = collect_child(&child);
    if (!collected || !failure_receipt_valid(
            &child, "host", "application", "input"
        ) || !stream_closed_without_bytes(control[1]) ||
        !wait_for_peer_close(frame[1])) {
        goto done;
    }
    ok = true;

done:
    close_fd(&control[0]);
    close_fd(&control[1]);
    close_fd(&media[0]);
    close_fd(&media[1]);
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    if (spawned && !collected) {
        terminate_children(&child, 1U);
    }
    return ok;
}

static bool run_one_invalid_frame_fd_failure(
    const char *program,
    int *wrong_fd,
    int *peer_fd
)
{
    int control[2] = {-1, -1};
    int media[2] = {-1, -1};
    Child child;
    bool spawned = false;
    bool collected = false;
    bool ok = false;

    memset(&child, 0, sizeof(child));
    child.pid = -1;
    child.stdout_fd = -1;
    child.stderr_fd = -1;
    if (wrong_fd == NULL || *wrong_fd < 3 ||
        !make_tcp_pair(control) || !make_udp_pair(media) || !spawn_role(
            program,
            "host",
            control[0],
            media[0],
            *wrong_fd,
            control[1],
            media[1],
            peer_fd != NULL ? *peer_fd : -1,
            false,
            &child
        )) {
        goto done;
    }
    spawned = true;
    close_fd(&control[0]);
    close_fd(&media[0]);
    close_fd(wrong_fd);
    collected = collect_child(&child);
    if (!collected || !failure_receipt_valid(
            &child, "host", "application", "input"
        ) || !stream_closed_without_bytes(control[1]) ||
        (peer_fd != NULL && !wait_for_peer_close(*peer_fd))) {
        goto done;
    }
    ok = true;

done:
    close_fd(&control[0]);
    close_fd(&control[1]);
    close_fd(&media[0]);
    close_fd(&media[1]);
    close_fd(wrong_fd);
    if (peer_fd != NULL) {
        close_fd(peer_fd);
    }
    if (spawned && !collected) {
        terminate_children(&child, 1U);
    }
    return ok;
}

static bool clear_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL);

    return flags >= 0 && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

static bool make_unix_datagram_pair(int pair[2])
{
    pair[0] = -1;
    pair[1] = -1;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0 ||
        !set_nonblocking(pair[0]) || !set_nonblocking(pair[1]) ||
        !set_close_on_exec(pair[0]) || !set_close_on_exec(pair[1])) {
        close_fd(&pair[0]);
        close_fd(&pair[1]);
        return false;
    }
    return true;
}

static bool run_one_pair_invalid_frame_fd(
    const char *program,
    int *wrong_fd,
    int *peer_fd,
    bool expect_stream_close
)
{
    Child child;
    bool spawned = false;
    bool collected = false;
    bool ok = false;

    memset(&child, 0, sizeof(child));
    child.pid = -1;
    child.stdout_fd = -1;
    child.stderr_fd = -1;
    if (wrong_fd == NULL || *wrong_fd < 3 || !spawn_pair(
            program,
            program,
            *wrong_fd,
            peer_fd != NULL ? *peer_fd : -1,
            true,
            false,
            &child
        )) {
        goto done;
    }
    spawned = true;
    close_fd(wrong_fd);
    collected = collect_child(&child);
    ok = collected && pair_failure_receipt_valid(&child, "input") &&
        (!expect_stream_close ||
         (peer_fd != NULL && wait_for_peer_close(*peer_fd)));

done:
    close_fd(wrong_fd);
    if (peer_fd != NULL) {
        close_fd(peer_fd);
    }
    if (spawned && !collected) {
        terminate_children(&child, 1U);
    }
    return ok;
}

static bool run_pair_invalid_frame_fd_failure(const char *program)
{
    int descriptors[2] = {-1, -1};
    int unconnected = -1;

    CHECK(make_frame_pair(descriptors));
    CHECK(clear_nonblocking(descriptors[0]));
    CHECK(run_one_pair_invalid_frame_fd(
        program, &descriptors[0], &descriptors[1], true
    ));

    CHECK(make_unix_datagram_pair(descriptors));
    CHECK(run_one_pair_invalid_frame_fd(
        program, &descriptors[0], &descriptors[1], false
    ));

    unconnected = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(unconnected >= 3 && set_nonblocking(unconnected) &&
          set_close_on_exec(unconnected));
    CHECK(run_one_pair_invalid_frame_fd(
        program, &unconnected, NULL, false
    ));
    return true;
}

static bool run_invalid_frame_fd_failure(const char *program)
{
    int descriptors[2] = {-1, -1};
    int unconnected = -1;

    CHECK(make_tcp_pair(descriptors));
    CHECK(run_one_invalid_frame_fd_failure(
        program, &descriptors[0], &descriptors[1]
    ));

    CHECK(make_frame_pair(descriptors));
    CHECK(clear_nonblocking(descriptors[0]));
    CHECK(run_one_invalid_frame_fd_failure(
        program, &descriptors[0], &descriptors[1]
    ));

    CHECK(make_unix_datagram_pair(descriptors));
    CHECK(run_one_invalid_frame_fd_failure(
        program, &descriptors[0], NULL
    ));
    close_fd(&descriptors[1]);

    unconnected = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(unconnected >= 3 && set_nonblocking(unconnected) &&
          set_close_on_exec(unconnected));
    CHECK(run_one_invalid_frame_fd_failure(program, &unconnected, NULL));
    return true;
}

static bool run_input_failure_matrix(const char *program)
{
    uint8_t record[FRAME_RECORD_BYTES + 1U];

    CHECK(make_frame_record(record));
    CHECK(run_input_failure_bytes(program, record, 0U, true));
    CHECK(run_input_failure_bytes(program, record, 64U, true));
    CHECK(run_input_failure_bytes(
        program,
        record,
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE + 37U,
        true
    ));
    CHECK(run_input_failure_bytes(
        program, record, FRAME_RECORD_BYTES, false
    ));

    CHECK(make_frame_record(record));
    record[FRAME_RECORD_BYTES] = 0x7fU;
    CHECK(run_input_failure_bytes(
        program, record, sizeof(record), true
    ));

    CHECK(make_frame_record(record));
    record[MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE] ^= 1U;
    CHECK(run_input_failure_bytes(
        program, record, FRAME_RECORD_BYTES, true
    ));

    CHECK(make_frame_record(record));
    record[39U] = 0x02U;
    CHECK(run_input_failure_bytes(program, record, 128U, true));

    CHECK(make_frame_record(record));
    record[16U] = 0x00U;
    record[17U] = 0x00U;
    record[18U] = 0x10U;
    record[19U] = 0x01U;
    CHECK(run_input_failure_bytes(program, record, 128U, true));

    CHECK(make_frame_record(record));
    record[15U] = 1U;
    memmove(record + 56U, record + 40U, 8U);
    record[71U] = 9U;
    CHECK(run_input_failure_bytes(program, record, 128U, true));
    CHECK(run_invalid_frame_fd_failure(program));
    return true;
}

static bool run_control_bootstrap_failure(const char *program)
{
    int wrong_control[2] = {-1, -1};
    int media[2] = {-1, -1};
    int frame[2] = {-1, -1};
    uint8_t record[FRAME_RECORD_BYTES];
    Child child;
    bool spawned = false;
    bool collected = false;
    bool ok = false;

    memset(&child, 0, sizeof(child));
    child.pid = -1;
    child.stdout_fd = -1;
    child.stderr_fd = -1;
    if (!make_udp_pair(wrong_control) || !make_udp_pair(media) ||
        !make_frame_pair(frame) || !make_frame_record(record) ||
        !spawn_role(
            program,
            "host",
            wrong_control[0],
            media[0],
            frame[0],
            wrong_control[1],
            media[1],
            frame[1],
            false,
            &child
        )) {
        goto done;
    }
    spawned = true;
    close_fd(&frame[0]);
    if (!send_frame_record_and_eof(
            frame[1], record, sizeof(record)
        ) || !wait_for_peer_close(frame[1])) {
        goto done;
    }
    close_fd(&frame[1]);
    close_fd(&wrong_control[0]);
    close_fd(&wrong_control[1]);
    close_fd(&media[0]);
    close_fd(&media[1]);
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    collected = collect_child(&child);
    ok = collected && failure_receipt_valid(
        &child, "host", "control", "bootstrap"
    );

done:
    close_fd(&wrong_control[0]);
    close_fd(&wrong_control[1]);
    close_fd(&media[0]);
    close_fd(&media[1]);
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    if (spawned && !collected) {
        terminate_children(&child, 1U);
    }
    return ok;
}

static bool run_media_bootstrap_failure(const char *program)
{
    int control[2] = {-1, -1};
    int wrong_media[2] = {-1, -1};
    Child child;
    bool spawned = false;
    bool collected = false;
    bool ok = false;

    memset(&child, 0, sizeof(child));
    child.pid = -1;
    child.stdout_fd = -1;
    child.stderr_fd = -1;
    if (!make_tcp_pair(control) || !make_tcp_pair(wrong_media) ||
        !spawn_role(
            program,
            "edge",
            control[0],
            wrong_media[0],
            -1,
            control[1],
            wrong_media[1],
            -1,
            false,
            &child
        )) {
        goto done;
    }
    spawned = true;
    close_fd(&control[0]);
    close_fd(&control[1]);
    close_fd(&wrong_media[0]);
    close_fd(&wrong_media[1]);
    collected = collect_child(&child);
    ok = collected && failure_receipt_valid(
        &child, "edge", "media", "bootstrap"
    );

done:
    close_fd(&control[0]);
    close_fd(&control[1]);
    close_fd(&wrong_media[0]);
    close_fd(&wrong_media[1]);
    if (spawned && !collected) {
        terminate_children(&child, 1U);
    }
    return ok;
}

static bool wait_for_media_datagram(int fd)
{
    const uint64_t start = monotonic_ms();
    const uint64_t deadline = start == 0U ? 0U : start + 2000U;
    uint8_t bytes[1200U];

    while (deadline != 0U && monotonic_ms() < deadline) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        const int poll_status = poll(&descriptor, (nfds_t)1U, 20);

        if (poll_status < 0 && errno == EINTR) {
            continue;
        }
        if (poll_status < 0 ||
            (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            return recv(fd, bytes, sizeof(bytes), 0) > 0;
        }
    }
    return false;
}

static bool run_post_arm_control_loss(const char *program)
{
    int tcp[2] = {-1, -1};
    int host_media[2] = {-1, -1};
    int edge_media[2] = {-1, -1};
    int frame[2] = {-1, -1};
    uint8_t record[FRAME_RECORD_BYTES];
    Child children[2U];
    bool collected = false;
    bool ok = false;

    memset(children, 0, sizeof(children));
    children[0].pid = -1;
    children[0].stdout_fd = -1;
    children[0].stderr_fd = -1;
    children[1].pid = -1;
    children[1].stdout_fd = -1;
    children[1].stderr_fd = -1;
    if (!make_tcp_pair(tcp) || !make_udp_pair(host_media) ||
        !make_udp_pair(edge_media) || !make_frame_pair(frame) ||
        !make_frame_record(record) || !spawn_role(
            program,
            "host",
            tcp[0],
            host_media[0],
            frame[0],
            tcp[1],
            host_media[1],
            frame[1],
            false,
            &children[0]
        )) {
        goto done;
    }
    close_fd(&frame[0]);
    if (!send_frame_record_and_eof(
            frame[1], record, sizeof(record)
        ) || !wait_for_peer_close(frame[1])) {
        goto done;
    }
    close_fd(&frame[1]);
    if (!spawn_role(
            program,
            "edge",
            tcp[1],
            edge_media[0],
            -1,
            tcp[0],
            edge_media[1],
            -1,
            false,
            &children[1]
        )) {
        goto done;
    }
    close_fd(&tcp[1]);
    close_fd(&host_media[0]);
    close_fd(&edge_media[0]);
    if (!wait_for_media_datagram(host_media[1]) ||
        shutdown(tcp[0], SHUT_RDWR) != 0) {
        goto done;
    }
    close_fd(&tcp[0]);
    collected = collect_children(children, 2U);
    if (!collected || !failure_receipt_valid(
            &children[0], "host", "control", "transfer"
        ) || !failure_receipt_valid(
            &children[1], "edge", "control", "transfer"
        )) {
        goto done;
    }
    ok = true;

done:
    close_fd(&tcp[0]);
    close_fd(&tcp[1]);
    close_fd(&host_media[0]);
    close_fd(&host_media[1]);
    close_fd(&edge_media[0]);
    close_fd(&edge_media[1]);
    close_fd(&frame[0]);
    close_fd(&frame[1]);
    if (!collected) {
        terminate_children(children, 2U);
    }
    return ok;
}

int main(int argc, char **argv)
{
    char first_session[65U];
    char second_session[65U];
    char first_pair_session[65U];
    char second_pair_session[65U];
    UdpTuple first_tuple;
    UdpTuple second_tuple;
    int first_udp[2] = {-1, -1};
    int second_udp[2] = {-1, -1};
    bool ok;

    if (argc != 2) {
        return 2;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return 1;
    }
    ok = make_udp_pair(first_udp) && make_udp_pair(second_udp) &&
        udp_tuple(first_udp[0], &first_tuple) &&
        udp_tuple(second_udp[0], &second_tuple) &&
        (first_tuple.local_port != second_tuple.local_port ||
         first_tuple.peer_port != second_tuple.peer_port) &&
        !(
            first_tuple.local_port == second_tuple.peer_port &&
            first_tuple.peer_port == second_tuple.local_port
        ) && run_happy_once(argv[1], first_session, first_udp) &&
        run_happy_once(argv[1], second_session, second_udp) &&
        strcmp(first_session, second_session) != 0 &&
        run_argument_failure(argv[1]) &&
        run_input_failure_matrix(argv[1]) &&
        run_control_bootstrap_failure(argv[1]) &&
        run_media_bootstrap_failure(argv[1]) &&
        run_post_arm_control_loss(argv[1]) &&
        run_pair_happy_once(argv[1], first_pair_session, true) &&
        run_pair_happy_once(argv[1], second_pair_session, false) &&
        strcmp(first_pair_session, second_pair_session) != 0 &&
        run_pair_input_failure_matrix(argv[1]) &&
        run_pair_invalid_frame_fd_failure(argv[1]) &&
        run_pair_argument_failure(argv[1]) &&
        run_pair_closed_fd_failure(argv[1]);
    close_fd(&first_udp[0]);
    close_fd(&first_udp[1]);
    close_fd(&second_udp[0]);
    close_fd(&second_udp[1]);
    if (!ok) {
        return 1;
    }
    puts("display ingress simulated service: ok");
    return 0;
}
