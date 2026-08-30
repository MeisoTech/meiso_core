#include "meiso_four_kind_lab_private.h"

/* Network setup owns host configuration, connect, listen and accept. */

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_parse_port(const char *value, uint16_t *out_port)
{
    uint64_t parsed;

    if (!meiso_four_kind_lab_private_parse_u64_text(value, &parsed) ||
        parsed != MEISO_FOUR_KIND_LAB_TCP_PORT || out_port == NULL) {
        return false;
    }
    *out_port = (uint16_t)parsed;
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_parse_host_config(
    const char *path, MeisoFourKindLabHostLocator *out_locator)
{
    FILE *file;
    char line[MEISO_FOUR_KIND_LAB_CONFIG_LINE_BYTES];
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
            strcmp(equals + 1, meiso_four_kind_lab_private_address_env) == 0) {
            have_address_env = true;
        } else if (strcmp(line, "expected_edge") == 0 && !have_expected_edge &&
                   strcmp(equals + 1,
                          meiso_four_kind_lab_private_expected_edge) == 0) {
            have_expected_edge = true;
        } else if (strcmp(line, "port") == 0 && !have_port &&
                   meiso_four_kind_lab_private_parse_port(equals + 1, &port)) {
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
    address_value = getenv(meiso_four_kind_lab_private_address_env);
    if (address_value == NULL || address_value[0] == '\0' ||
        inet_pton(AF_INET, address_value, &out_locator->address) != 1) {
        return false;
    }
    out_locator->port = port;
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE int meiso_four_kind_lab_private_connect_host(
    const MeisoFourKindLabHostLocator *locator)
{
    struct sockaddr_in address;
    struct timespec deadline;
    struct pollfd descriptor;
    int fd = -1;

    if (locator == NULL ||
        !meiso_four_kind_lab_private_make_deadline(&deadline)) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || !meiso_four_kind_lab_private_set_nonblocking_cloexec(fd)) {
        meiso_four_kind_lab_private_close_fd(&fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(locator->port);
    address.sin_addr = locator->address;
    if (connect(fd, (const struct sockaddr *)(const void *)&address,
                (socklen_t)sizeof(address)) != 0) {
        if (errno != EINPROGRESS) {
            meiso_four_kind_lab_private_close_fd(&fd);
            return -1;
        }
        for (;;) {
            int result;
            int socket_error = 0;
            socklen_t error_size = (socklen_t)sizeof(socket_error);
            const int wait_ms =
                meiso_four_kind_lab_private_deadline_wait_ms(&deadline);

            if (wait_ms == 0) {
                meiso_four_kind_lab_private_close_fd(&fd);
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
                (result > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                          &socket_error, &error_size) != 0)) {
                meiso_four_kind_lab_private_close_fd(&fd);
                return -1;
            }
            if (result > 0) {
                if (socket_error != 0 ||
                    (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) !=
                        0) {
                    meiso_four_kind_lab_private_close_fd(&fd);
                    return -1;
                }
                break;
            }
        }
    }
    return fd;
}

MEISO_FOUR_KIND_LAB_PRIVATE int
meiso_four_kind_lab_private_make_listener(uint16_t port)
{
    struct sockaddr_in address;
    int enabled = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0 || !meiso_four_kind_lab_private_set_nonblocking_cloexec(fd) ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   (socklen_t)sizeof(enabled)) != 0) {
        meiso_four_kind_lab_private_close_fd(&fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (const struct sockaddr *)(const void *)&address,
             (socklen_t)sizeof(address)) != 0 ||
        listen(fd, 1) != 0) {
        meiso_four_kind_lab_private_close_fd(&fd);
        return -1;
    }
    return fd;
}

MEISO_FOUR_KIND_LAB_PRIVATE int
meiso_four_kind_lab_private_accept_edge(int listener)
{
    struct timespec deadline;
    struct pollfd descriptor;

    if (listener < 0 || !meiso_four_kind_lab_private_make_deadline(&deadline)) {
        return -1;
    }
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd >= 0) {
            if (!meiso_four_kind_lab_private_set_nonblocking_cloexec(fd)) {
                meiso_four_kind_lab_private_close_fd(&fd);
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
            const int wait_ms =
                meiso_four_kind_lab_private_deadline_wait_ms(&deadline);
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
