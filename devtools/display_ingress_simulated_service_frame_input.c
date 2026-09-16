/* REFERENCE: diagnostic/fixture entry, not the installed Edge runtime. */
#define _POSIX_C_SOURCE 200809L

#include "../applications/display_ingress/service_private.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>

typedef uint8_t FrameReadStep;
enum {
    FRAME_READ_PROGRESS = 1,
    FRAME_READ_WOULD_BLOCK = 2,
    FRAME_READ_EOF = 3,
    FRAME_READ_FAILED = 4
};

static bool connected_nonblocking_unix_stream(int fd)
{
    struct sockaddr_storage local;
    struct sockaddr_storage peer;
    socklen_t local_size = (socklen_t)sizeof(local);
    socklen_t peer_size = (socklen_t)sizeof(peer);
    socklen_t type_size;
    int socket_type = 0;
    int flags;

    if (fd < 3) {
        return false;
    }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_NONBLOCK) == 0) {
        return false;
    }
    type_size = (socklen_t)sizeof(socket_type);
    memset(&local, 0, sizeof(local));
    memset(&peer, 0, sizeof(peer));
    return getsockopt(
        fd, SOL_SOCKET, SO_TYPE, &socket_type, &type_size
    ) == 0 && socket_type == SOCK_STREAM &&
        getsockname(fd, (struct sockaddr *)(void *)&local, &local_size) == 0 &&
        getpeername(fd, (struct sockaddr *)(void *)&peer, &peer_size) == 0 &&
        local.ss_family == AF_UNIX && peer.ss_family == AF_UNIX;
}

static FrameReadStep read_once(
    int fd,
    uint8_t *destination,
    size_t capacity,
    size_t *out_count
)
{
    ssize_t count;

    if (destination == NULL || capacity == 0U || out_count == NULL) {
        return FRAME_READ_FAILED;
    }
    count = recv(fd, destination, capacity, 0);
    if (count > 0) {
        *out_count = (size_t)count;
        return FRAME_READ_PROGRESS;
    }
    if (count == 0) {
        *out_count = 0U;
        return FRAME_READ_EOF;
    }
    if (errno == EINTR) {
        return FRAME_READ_WOULD_BLOCK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return FRAME_READ_WOULD_BLOCK;
    }
    return FRAME_READ_FAILED;
}

static bool socket_error_absent(int fd)
{
    int socket_error = 0;
    socklen_t error_size = (socklen_t)sizeof(socket_error);

    return getsockopt(
        fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size
    ) == 0 && socket_error == 0;
}

static bool wait_readable(int fd, uint64_t deadline_ns)
{
    while (!service_deadline_expired(deadline_ns)) {
        struct pollfd descriptor;
        int poll_status;

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        poll_status = poll(
            &descriptor, (nfds_t)1U, SERVICE_POLL_SLICE_MS
        );
        if (poll_status < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (poll_status == 0) {
            continue;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            return false;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            return (descriptor.revents & POLLERR) == 0 ||
                socket_error_absent(fd);
        }
    }
    return false;
}

static bool read_exact(
    int fd,
    uint8_t *destination,
    size_t size,
    uint64_t deadline_ns
)
{
    size_t offset = 0U;

    while (offset < size) {
        size_t count = 0U;
        FrameReadStep step;

        if (!wait_readable(fd, deadline_ns)) {
            return false;
        }
        step = read_once(fd, destination + offset, size - offset, &count);
        if (step == FRAME_READ_PROGRESS) {
            offset += count;
        } else if (step == FRAME_READ_EOF || step == FRAME_READ_FAILED) {
            return false;
        }
    }
    return true;
}

static bool require_eof(int fd, uint64_t deadline_ns)
{
    uint8_t extra;

    while (!service_deadline_expired(deadline_ns)) {
        size_t count = 0U;
        FrameReadStep step;

        if (!wait_readable(fd, deadline_ns)) {
            return false;
        }
        step = read_once(fd, &extra, sizeof(extra), &count);
        if (step == FRAME_READ_EOF) {
            return true;
        }
        if (step == FRAME_READ_PROGRESS || step == FRAME_READ_FAILED) {
            return false;
        }
    }
    return false;
}

bool service_read_host_fixture_input(Service *service)
{
    uint8_t header_wire[
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE
    ];
    MeisoDisplayIngressHostFixtureInputHeader header;
    MeisoDisplayIngressMediaFrame frame;
    const uint64_t deadline_ns =
        service_deadline_after_ms(SERVICE_TIMEOUT_MS);

    if (service == NULL || service->role != MEISO_SESSION_ROLE_HOST ||
        service->frame_input_complete ||
        !connected_nonblocking_unix_stream(service->frame_fd) ||
        deadline_ns == 0U) {
        return service_fail(service, "application", "input");
    }
    if (!read_exact(
            service->frame_fd, header_wire, sizeof(header_wire), deadline_ns
        ) || meiso_display_ingress_host_fixture_input_header_decode(
            (MeisoByteView){header_wire, sizeof(header_wire)}, &header
        ) != MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK ||
        header.input_width != SERVICE_FRAME_WIDTH ||
        header.input_height != SERVICE_FRAME_HEIGHT ||
        header.row_pitch_bytes != SERVICE_FRAME_WIDTH * 4U ||
        header.rgba_bytes != SERVICE_FRAME_BYTES ||
        header.frame_sequence != 1U ||
        header.record_bytes !=
            MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE +
            SERVICE_FRAME_BYTES ||
        header.sender_deadline.present) {
        return service_fail(service, "application", "input");
    }
    if (!read_exact(
            service->frame_fd,
            service->frame_bytes,
            sizeof(service->frame_bytes),
            deadline_ns
        ) || meiso_display_ingress_host_fixture_input_validate_payload(
            &header,
            (MeisoByteView){
                service->frame_bytes, sizeof(service->frame_bytes)
            },
            &frame
        ) != MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK ||
        !require_eof(service->frame_fd, deadline_ns)) {
        return service_fail(service, "application", "input");
    }

    service->frame_input_width = header.input_width;
    service->frame_input_height = header.input_height;
    service->frame_row_pitch_bytes = header.row_pitch_bytes;
    service->frame_rgba_bytes = header.rgba_bytes;
    service->frame_sequence = frame.frame_sequence;
    service->frame_source_time = frame.source_time;
    service->frame_sender_deadline = frame.sender_deadline;
    service->frame_producer_dropped_frames =
        frame.producer_dropped_frames;
    memmove(
        service->input_sha256,
        header.rgba_sha256,
        sizeof(service->input_sha256)
    );
    service_close_fd(&service->frame_fd);
    service->frame_input_complete = true;
    return true;
}
