#define _POSIX_C_SOURCE 200809L

#include "meiso_wifi_runtime_host_private.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Thin CLI entry point; process storage is owned here for the whole run. */

MeisoWifiRuntimeHostPrivateStorage meiso_wifi_runtime_host_private_storage;

static void usage(const char *program)
{
    (void)fprintf(
        stderr,
        "usage: %s --config PATH [--interactive]\n"
        "always required: connect_ipv4, connect_port, "
        "local_host_peer_id_high/low, expected_edge_peer_id_high/low, "
        "one-shot also requires: selected_fields, sample_period_ms, "
        "sample_count\n",
        program
    );
}

int main(int argc, char **argv)
{
    MeisoWifiRuntimeHostPrivate host;
    MeisoSessionSnapshot session_snapshot;
    const char *failure_stage;
    bool passed = false;

    memset(&host, 0, sizeof(host));
    host.fd = -1;
    if ((argc != 3 && argc != 4) ||
        strcmp(argv[1], "--config") != 0 ||
        (argc == 4 && strcmp(argv[3], "--interactive") != 0)) {
        usage(argv[0]);
        return 2;
    }
    host.interactive = argc == 4;
    if (!meiso_wifi_runtime_host_private_load_config(argv[2], host.interactive, &host.bootstrap)) {
        fputs("wifi host: invalid config\n", stderr);
        return 2;
    }
    host.fd = meiso_wifi_runtime_host_private_connect_numeric_ipv4(&host.bootstrap);
    if (host.fd < 0 || !meiso_wifi_runtime_host_private_initialize_session_transport(&host)) {
        fputs("wifi host: bootstrap failed\n", stderr);
        meiso_wifi_runtime_host_private_close_fd(&host.fd);
        return 1;
    }
    (void)printf(
        "{\"type\":\"bootstrap\",\"status\":\"CONNECTED\","
        "\"locator_redacted\":true}\n"
    );
    (void)fflush(stdout);

    failure_stage = "session_ready";
    if (!meiso_wifi_runtime_host_private_wait_session_ready(&host, &session_snapshot)) {
        goto done;
    }
    failure_stage = "runtime_init";
    if (!meiso_wifi_runtime_host_private_initialize_runtime_resources(&host, &session_snapshot)) {
        goto done;
    }
    meiso_wifi_runtime_host_private_emit_session_ready(&session_snapshot);

    failure_stage = "catalog";
    if (!meiso_wifi_runtime_host_private_receive_and_accept_catalog(&host)) {
        goto done;
    }
    if (host.interactive) {
        failure_stage = "interactive";
        if (!meiso_wifi_runtime_host_private_run_interactive(&host)) {
            goto done;
        }
        passed = true;
        goto done;
    }
    failure_stage = "enable_desired";
    if (!meiso_wifi_runtime_host_private_publish_desired(&host, 1U, true) ||
        !meiso_wifi_runtime_host_private_wait_desired_and_actual(
            &host, 1U, MEISO_RUNTIME_PRODUCTION_RUNNING
        )) {
        goto done;
    }
    failure_stage = "samples";
    if (!meiso_wifi_runtime_host_private_receive_requested_samples(&host)) {
        goto done;
    }
    failure_stage = "disable_desired";
    if (!meiso_wifi_runtime_host_private_publish_desired(&host, 2U, false) ||
        !meiso_wifi_runtime_host_private_wait_desired_and_actual(
            &host, 2U, MEISO_RUNTIME_PRODUCTION_STOPPED
        )) {
        goto done;
    }
    failure_stage = "meiso_wifi_runtime_host_private_normal_close";
    if (!meiso_wifi_runtime_host_private_normal_close(&host)) {
        goto done;
    }
    (void)printf(
        "{\"type\":\"run\",\"status\":\"COMPLETE\","
        "\"requested_samples\":%" PRIu32 ","
        "\"received_samples\":%" PRIu64 "}\n",
        host.bootstrap.sample_count,
        host.samples_received
    );
    (void)fflush(stdout);
    passed = true;

done:
    if (!passed) {
        meiso_wifi_runtime_host_private_emit_failure(failure_stage);
        meiso_wifi_runtime_host_private_best_effort_close(&host);
    }
    meiso_wifi_runtime_host_private_close_fd(&host.fd);
    return passed ? 0 : 1;
}
