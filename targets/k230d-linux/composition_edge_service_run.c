#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_composition_edge_service_private.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/*
 * Owns CLI dispatch, hosted lifecycle self-test dispatch, process bootstrap,
 * ordered teardown, and the final NDJSON shutdown receipt.
 */

static void usage(const char *program)
{
    (void)fprintf(
        stderr, "usage: %s --config PATH\n",
        program != NULL ? program : "composition_edge_service"
    );
}

#if defined(MEISO_K230D_COMPOSITION_EDGE_SERVICE_LIFECYCLE_TEST)
static bool pending_media_candidate_self_test(void)
{
    int pair[2] = {-1, -1};
    struct pollfd descriptor;
    uint8_t sent = UINT8_C(0x4d);
    uint8_t received = 0U;
    bool passed = false;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
        !meiso_k230d_composition_edge_service_private_set_nonblocking(
            pair[0]
        ) || !meiso_k230d_composition_edge_service_private_set_nonblocking(
            pair[1]
        ) ||
        meiso_k230d_composition_edge_service_private_pending_media_candidate_status(
            pair[0], 0
        ) != PENDING_MEDIA_CANDIDATE_LIVE ||
        send(pair[1], &sent, sizeof(sent), 0) != (ssize_t)sizeof(sent)) {
        goto cleanup;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = pair[0];
    descriptor.events = POLLIN;
    if (poll(&descriptor, 1U, 100) != 1 ||
        meiso_k230d_composition_edge_service_private_pending_media_candidate_status(
            pair[0], descriptor.revents
        ) != PENDING_MEDIA_CANDIDATE_LIVE ||
        recv(pair[0], &received, sizeof(received), 0) !=
            (ssize_t)sizeof(received) ||
        received != sent) {
        goto cleanup;
    }
    meiso_k230d_composition_edge_service_private_close_fd(&pair[1]);
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = pair[0];
    descriptor.events = POLLIN;
    if (poll(&descriptor, 1U, 100) != 1 ||
        meiso_k230d_composition_edge_service_private_pending_media_candidate_status(
            pair[0], descriptor.revents
        ) != PENDING_MEDIA_CANDIDATE_CLOSED) {
        goto cleanup;
    }
    meiso_k230d_composition_edge_service_private_close_fd(&pair[0]);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
        !meiso_k230d_composition_edge_service_private_set_nonblocking(
            pair[0]
        ) || !meiso_k230d_composition_edge_service_private_set_nonblocking(
            pair[1]
        ) || send(pair[1], &sent, sizeof(sent), 0) !=
            (ssize_t)sizeof(sent)) {
        goto cleanup;
    }
    meiso_k230d_composition_edge_service_private_close_fd(&pair[1]);
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = pair[0];
    descriptor.events = POLLIN;
    if (poll(&descriptor, 1U, 100) != 1 ||
        meiso_k230d_composition_edge_service_private_pending_media_candidate_status(
            pair[0], descriptor.revents
        ) != PENDING_MEDIA_CANDIDATE_CLOSED) {
        goto cleanup;
    }
    passed = true;

cleanup:
    meiso_k230d_composition_edge_service_private_close_fd(&pair[1]);
    meiso_k230d_composition_edge_service_private_close_fd(&pair[0]);
    return passed;
}

static int run_hosted_lifecycle_test(void)
{
    K230dCompositionEdgeShutdownLedger ledger;

    if (!pending_media_candidate_self_test()) {
        return 1;
    }
    memset(&ledger, 0, sizeof(ledger));
    ledger.runtime_initialized = true;
    ledger.abort_status = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK;
    ledger.snapshot_present = true;
    ledger.snapshot.phase =
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL;
    ledger.snapshot.terminal_reason =
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_LOCAL_ABORT;
    ledger.snapshot.camera.phase = MEISO_CAMERA_V4L2_PHASE_STOPPED;
    ledger.thermal_joined = true;
    if (!meiso_k230d_composition_edge_service_private_final_shutdown_clean(
            SIGTERM, true, &ledger
        )) {
        return 1;
    }
    ledger.snapshot.terminal_reason =
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CONTROL_TRANSPORT;
    if (meiso_k230d_composition_edge_service_private_final_shutdown_clean(
            SIGTERM, true, &ledger
        )) {
        return 1;
    }
    ledger.snapshot.terminal_reason =
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_LOCAL_ABORT;
    ledger.snapshot.camera.mapped_buffer_count = 1U;
    if (meiso_k230d_composition_edge_service_private_final_shutdown_clean(
            SIGTERM, true, &ledger
        )) {
        return 1;
    }
    ledger.snapshot.camera.mapped_buffer_count = 0U;
    ledger.snapshot.media.bound = true;
    if (meiso_k230d_composition_edge_service_private_final_shutdown_clean(
            SIGTERM, true, &ledger
        )) {
        return 1;
    }
    ledger.snapshot.media.bound = false;
    if (meiso_k230d_composition_edge_service_private_final_shutdown_clean(
            SIGINT, true, &ledger
        ) || meiso_k230d_composition_edge_service_private_final_shutdown_clean(
            SIGTERM, false, &ledger
        )) {
        return 1;
    }
    ledger.runtime_initialized = false;
    ledger.snapshot_present = false;
    if (!meiso_k230d_composition_edge_service_private_final_shutdown_clean(
            SIGTERM, false, &ledger
        )) {
        return 1;
    }
    puts("composition_edge_service_lifecycle PASS");
    return 0;
}
#endif

int meiso_k230d_composition_edge_service_private_run(
    int argc,
    char **argv
)
{
    K230dCompositionEdgeService service;
    K230dCompositionEdgeShutdownLedger ledger;
    bool success = false;
    bool clean_shutdown;
    MeisoK230dCompositionEdgeRuntimeTerminalReason receipt_reason =
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_NONE;

    memset(&service, 0, sizeof(service));
    memset(&ledger, 0, sizeof(ledger));
    service.control_listener = -1;
    service.media_listener = -1;
    service.control_fd = -1;
    service.media_fd = -1;
    atomic_init(&service.thermal.done, false);
#if defined(MEISO_K230D_COMPOSITION_EDGE_SERVICE_LIFECYCLE_TEST)
    if (argc == 2 && strcmp(argv[1], "--lifecycle-self-test") == 0) {
        return run_hosted_lifecycle_test();
    }
#endif
    if (argc != 3 || strcmp(argv[1], "--config") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (!meiso_k230d_composition_edge_service_private_install_signal_handlers() ||
        !meiso_k230d_composition_edge_service_private_parse_private_config(
            argv[2], &service.private_config
        )) {
        fputs("composition Edge config/bootstrap failed\n", stderr);
        return 1;
    }
    service.control_listener =
        meiso_k230d_composition_edge_service_private_make_listener(
            service.private_config.listen_ipv4,
            service.private_config.control_port,
            false
        );
    service.media_listener =
        meiso_k230d_composition_edge_service_private_make_listener(
            service.private_config.listen_ipv4,
            service.private_config.media_port,
            true
        );
    if (service.control_listener < 0 || service.media_listener < 0) {
        fputs("composition Edge listen failed\n", stderr);
        goto cleanup;
    }
    (void)printf(
        "{\"type\":\"edge\",\"phase\":\"listen\","
        "\"status\":\"READY\"}\n"
    );
    (void)fflush(stdout);
    service.control_fd =
        meiso_k230d_composition_edge_service_private_accept_connected(
            service.control_listener, true
        );
    meiso_k230d_composition_edge_service_private_close_fd(
        &service.control_listener
    );
    if (service.control_fd < 0) {
        if (!meiso_k230d_composition_edge_service_private_g_stop_requested) {
            fputs("composition Edge control accept failed\n", stderr);
        }
        goto cleanup;
    }
    if (!meiso_k230d_composition_edge_service_private_initialize_edge_runtime(
            &service
        )) {
        fputs("composition K230D Edge Runtime init failed\n", stderr);
        goto cleanup;
    }
    (void)printf(
        "{\"type\":\"control\",\"phase\":\"accept\","
        "\"status\":\"CONNECTED\"}\n"
    );
    (void)fflush(stdout);
    success = meiso_k230d_composition_edge_service_private_run_loop(&service);

cleanup:
    if (service.edge_runtime != NULL) {
        ledger.runtime_initialized = true;
        ledger.abort_status = meiso_k230d_composition_edge_runtime_abort(
            service.edge_runtime,
            meiso_k230d_composition_edge_service_private_g_stop_requested != 0 ?
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_LOCAL_ABORT :
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE,
            meiso_k230d_composition_edge_service_private_g_stop_requested != 0 ?
                0 : EPROTO
        );
        ledger.snapshot_present =
            meiso_k230d_composition_edge_runtime_snapshot(
                service.edge_runtime, &ledger.snapshot
            );
    }
    ledger.thermal_joined =
        meiso_k230d_composition_edge_service_private_join_thermal_worker(
            &service
        );
    clean_shutdown =
        meiso_k230d_composition_edge_service_private_final_shutdown_clean(
            (int)meiso_k230d_composition_edge_service_private_g_stop_requested,
            success,
            &ledger
        );
    if (ledger.snapshot_present) {
        receipt_reason = ledger.snapshot.terminal_reason;
    } else if (
        meiso_k230d_composition_edge_service_private_g_stop_requested != 0
    ) {
        receipt_reason =
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_LOCAL_ABORT;
    }
    meiso_k230d_composition_edge_service_private_close_fd(&service.media_fd);
    meiso_k230d_composition_edge_service_private_close_fd(&service.control_fd);
    meiso_k230d_composition_edge_service_private_close_fd(
        &service.media_listener
    );
    meiso_k230d_composition_edge_service_private_close_fd(
        &service.control_listener
    );
    free(service.edge_runtime_arena);
    (void)printf(
        "{\"type\":\"edge\",\"phase\":\"close\","
        "\"status\":\"%s\",\"reason\":\"%s\","
        "\"snapshot\":%s,\"camera_phase\":%u,"
        "\"camera_driver_buffers\":%u,\"camera_mapped\":%u,"
        "\"camera_mapped_bytes\":%" PRIu64 ","
        "\"camera_queued\":%u,\"camera_borrowed\":%s,"
        "\"camera_stream_on\":%s,\"media_bound\":%s}\n",
        clean_shutdown ? "SHUTDOWN" : "FAILED",
        meiso_k230d_composition_edge_service_private_terminal_reason_name(
            receipt_reason
        ),
        ledger.snapshot_present ? "true" : "false",
        ledger.snapshot_present ?
            (unsigned)ledger.snapshot.camera.phase : 0U,
        ledger.snapshot_present ?
            (unsigned)ledger.snapshot.camera.driver_buffer_count : 0U,
        ledger.snapshot_present ?
            (unsigned)ledger.snapshot.camera.mapped_buffer_count : 0U,
        ledger.snapshot_present ? ledger.snapshot.camera.mapped_bytes : 0U,
        ledger.snapshot_present ?
            (unsigned)ledger.snapshot.camera.queued_buffer_count : 0U,
        ledger.snapshot_present && ledger.snapshot.camera.frame_borrowed ?
            "true" : "false",
        ledger.snapshot_present && ledger.snapshot.camera.stream_on ?
            "true" : "false",
        ledger.snapshot_present && ledger.snapshot.media.bound ?
            "true" : "false"
    );
    return clean_shutdown ? 0 : 1;
}
