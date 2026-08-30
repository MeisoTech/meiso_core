#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_wifi_runtime_edge_private.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * Owns CLI dispatch, signal state, listener/connection descriptors, ordered
 * bootstrap, and final shutdown receipts. All descriptors are closed here or
 * delegated to the pump's bounded close flush before process return.
 */

volatile sig_atomic_t
    meiso_k230d_wifi_runtime_edge_private_g_stop_requested = 0;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    meiso_k230d_wifi_runtime_edge_private_g_stop_requested = 1;
}

static bool install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    if (sigemptyset(&action.sa_mask) != 0) {
        return false;
    }
    return sigaction(SIGINT, &action, NULL) == 0 &&
        sigaction(SIGTERM, &action, NULL) == 0;
}

void meiso_k230d_wifi_runtime_edge_private_close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

bool meiso_k230d_wifi_runtime_edge_private_monotonic_now_ns(uint64_t *out_value)
{
    struct timespec value;
    uint64_t seconds;

    if (out_value == NULL || clock_gettime(CLOCK_MONOTONIC, &value) != 0 ||
        value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= 1000000000L) {
        return false;
    }
    seconds = (uint64_t)value.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)value.tv_nsec) /
            UINT64_C(1000000000)) {
        return false;
    }
    *out_value = seconds * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
    return true;
}

uint64_t meiso_k230d_wifi_runtime_edge_private_deadline_after_ms(
    uint64_t now_ns,
    uint32_t milliseconds
)
{
    const uint64_t delta = (uint64_t)milliseconds * UINT64_C(1000000);
    return delta > UINT64_MAX - now_ns ? UINT64_MAX : now_ns + delta;
}

static bool set_cloexec(int fd)
{
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int make_listener(const K230dWifiRuntimeEdgeConfig *config)
{
    struct sockaddr_in address;
    int fd = -1;
    int enabled = 1;

    if (config == NULL) {
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(config->listen_port);
    if (inet_pton(AF_INET, config->listen_ipv4, &address.sin_addr) != 1) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || !set_cloexec(fd) || setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enabled,
            (socklen_t)sizeof(enabled)
        ) != 0 || bind(
            fd,
            (const struct sockaddr *)(const void *)&address,
            (socklen_t)sizeof(address)
        ) != 0 || listen(fd, 1) != 0) {
        meiso_k230d_wifi_runtime_edge_private_close_fd(&fd);
        return -1;
    }
    return fd;
}

static int accept_one(int listener_fd)
{
    int fd;

    for (;;) {
        fd = accept(listener_fd, NULL, NULL);
        if (fd >= 0) {
            if (set_cloexec(fd) && set_nonblocking(fd)) {
                return fd;
            }
            meiso_k230d_wifi_runtime_edge_private_close_fd(&fd);
            return -1;
        }
        if (errno == EINTR && !meiso_k230d_wifi_runtime_edge_private_g_stop_requested) {
            continue;
        }
        return -1;
    }
}

static void usage(const char *program)
{
    (void)fprintf(
        stderr,
        "usage: %s --config PATH\n",
        program != NULL ? program : "wifi_runtime_edge"
    );
}

int meiso_k230d_wifi_runtime_edge_private_run(int argc, char **argv)
{
    K230dWifiRuntimeEdge edge;
    bool passed;

    memset(&edge, 0, sizeof(edge));
    edge.listener_fd = -1;
    edge.connection_fd = -1;
    if (!meiso_k230d_wifi_runtime_edge_private_counter_continuity_self_test(
            )) {
        fputs("wifi Runtime Edge continuity self-test failed\n", stderr);
        return 1;
    }
    if (argc != 3 || strcmp(argv[1], "--config") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (!install_signal_handlers() ||
        !meiso_k230d_wifi_runtime_edge_private_parse_config(
            argv[2], &edge.private_config
        ) ||
        !meiso_k230d_wifi_runtime_edge_private_initialize_profile(&edge) ||
        !meiso_k230d_wifi_runtime_edge_private_initialize_resources(&edge)) {
        fputs("wifi Runtime Edge bootstrap failed\n", stderr);
        return 1;
    }
    edge.listener_fd = make_listener(&edge.private_config);
    if (edge.listener_fd < 0) {
        fputs("wifi Runtime Edge listen failed\n", stderr);
        return 1;
    }
    (void)printf(
        "{\"type\":\"edge\",\"phase\":\"listen\","
        "\"status\":\"READY\"}\n"
    );
    (void)fflush(stdout);
    edge.connection_fd = accept_one(edge.listener_fd);
    meiso_k230d_wifi_runtime_edge_private_close_fd(&edge.listener_fd);
    if (edge.connection_fd < 0) {
        if (!meiso_k230d_wifi_runtime_edge_private_g_stop_requested) {
            fputs("wifi Runtime Edge accept failed\n", stderr);
        }
        return meiso_k230d_wifi_runtime_edge_private_g_stop_requested ?
            0 : 1;
    }
    if (!meiso_k230d_wifi_runtime_edge_private_initialize_session_and_transport(
            &edge
        )) {
        fputs("wifi Runtime Edge Session bootstrap failed\n", stderr);
        meiso_k230d_wifi_runtime_edge_private_close_fd(&edge.connection_fd);
        return 1;
    }
    passed = meiso_k230d_wifi_runtime_edge_private_run_connection(&edge);
    if (!passed && !edge.peer_closed_normally) {
        meiso_k230d_wifi_runtime_edge_private_flush_local_close(
            &edge,
            (meiso_k230d_wifi_runtime_edge_private_g_stop_requested ||
             edge.source_continuity_lost) ?
                MEISO_SESSION_CLOSE_SHUTDOWN :
                MEISO_SESSION_CLOSE_PROTOCOL_ERROR
        );
    }
    meiso_k230d_wifi_runtime_edge_private_close_fd(&edge.connection_fd);
    if (edge.peer_closed_normally) {
        (void)printf(
            "{\"type\":\"session\",\"phase\":\"close\","
            "\"status\":\"NORMAL\",\"samples_queued\":%" PRIu64 "}\n",
            edge.samples_queued
        );
        return 0;
    }
    if (meiso_k230d_wifi_runtime_edge_private_g_stop_requested) {
        (void)printf(
            "{\"type\":\"session\",\"phase\":\"close\","
            "\"status\":\"SHUTDOWN\"}\n"
        );
        return 0;
    }
    if (edge.source_continuity_lost) {
        (void)printf(
            "{\"type\":\"session\",\"phase\":\"close\","
            "\"status\":\"SHUTDOWN\","
            "\"cause\":\"CONTINUITY_LOST\"}\n"
        );
        return 1;
    }
    fputs("wifi Runtime Edge connection failed\n", stderr);
    return 1;
}
