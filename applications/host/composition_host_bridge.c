#include "meiso_composition_host_bridge_private.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Process argument parsing and the public run/main seam. */

/*
 * Native Composition Host bridge. This is an application adapter, not a
 * new protocol surface: CompositionRemoteEdge owns application commands,
 * typed truth and media borrows; CompositionHostRuntime remains the private
 * fd/pump integration owner. stdin/stdout NDJSON and CMFB/v1 are bounded
 * machine-local bridges.
 */

static bool parse_frame_fd(const char *text, int *out_fd)
{
    uint64_t value;

    if (!meiso_composition_host_bridge_private_parse_u64_text(text, INT_MAX, &value)) {
        return false;
    }
    *out_fd = (int)value;
    return true;
}

/* Testable process seam. It restores the caller's SIGPIPE disposition. */
int meiso_composition_host_bridge_run(int argc, char **argv)
{
    const char *config_path = NULL;
    int frame_fd = -1;
    int index;
    bool interactive = false;
    bool simulator = false;
    MeisoCompositionHostBridgePrivateConfig config;
    MeisoCompositionHostBridgePrivateProcess *service = NULL;
    struct sigaction action;
    struct sigaction old_action;
    bool signal_changed = false;
    bool ok;
    int exit_code = 1;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--config") == 0 && index + 1 < argc &&
            config_path == NULL) {
            config_path = argv[++index];
        } else if (strcmp(argv[index], "--interactive") == 0) {
            if (interactive) {
                fputs("E_ARGUMENT\n", stderr);
                return 2;
            }
            interactive = true;
        } else if (strcmp(argv[index], "--simulator") == 0) {
            if (simulator) {
                fputs("E_ARGUMENT\n", stderr);
                return 2;
            }
            simulator = true;
        } else if (strcmp(argv[index], "--frame-fd") == 0 &&
                   index + 1 < argc && frame_fd < 0) {
            if (!parse_frame_fd(argv[++index], &frame_fd)) {
                fputs("E_ARGUMENT\n", stderr);
                return 2;
            }
        } else {
            fputs("E_ARGUMENT\n", stderr);
            return 2;
        }
    }
    if (!interactive || (simulator && config_path != NULL)) {
        fputs("E_ARGUMENT\n", stderr);
        return 2;
    }
    if (!simulator && (config_path == NULL ||
                       !meiso_composition_host_bridge_private_load_config(config_path, &config))) {
        fputs("E_CONFIG\n", stderr);
        return 2;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, &old_action) != 0) {
        fputs("E_SIGNAL\n", stderr);
        return 1;
    }
    signal_changed = true;
    service = calloc(1U, sizeof(*service));
    if (service == NULL) {
        fputs("E_MEMORY\n", stderr);
        goto done;
    }
    if (!(simulator ? meiso_composition_host_bridge_private_initialize_simulator_process(service, frame_fd) :
                      meiso_composition_host_bridge_private_initialize_remote_process(service, &config, frame_fd))) {
        fputs("E_INIT\n", stderr);
        goto done;
    }
    ok = meiso_composition_host_bridge_private_run_loop(service);
    meiso_composition_host_bridge_private_destroy_process(service);
    service->host_runtime = NULL;
    if (!ok) {
        fputs("E_RUNTIME\n", stderr);
        goto done;
    }
    exit_code = 0;

done:
    if (service != NULL) {
        if (service->host_runtime != NULL) {
            meiso_composition_host_bridge_private_destroy_process(service);
        }
        free(service);
    }
    if (signal_changed && sigaction(SIGPIPE, &old_action, NULL) != 0 &&
        exit_code == 0) {
        fputs("E_SIGNAL\n", stderr);
        exit_code = 1;
    }
    return exit_code;
}

#ifndef MEISO_COMPOSITION_HOST_BRIDGE_NO_MAIN
int main(int argc, char **argv)
{
    return meiso_composition_host_bridge_run(argc, argv);
}
#endif
