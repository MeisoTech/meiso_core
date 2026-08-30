#define _POSIX_C_SOURCE 200809L

#include "meiso_wifi_runtime_host_private.h"

#include <stdio.h>

/* Owns normal/protocol close, failure truth, and best-effort cleanup. */

bool meiso_wifi_runtime_host_private_normal_close(MeisoWifiRuntimeHostPrivate *host)
{
    MeisoSessionSnapshot snapshot;

    if (meiso_session_close(
            host->session, MEISO_SESSION_CLOSE_NORMAL
        ) != MEISO_SESSION_CLOSE_QUEUED || !meiso_wifi_runtime_host_private_flush_outbound(host) ||
        !meiso_session_snapshot(host->session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_CLOSED ||
        snapshot.failure != MEISO_SESSION_FAILURE_LOCAL_CLOSED ||
        !snapshot.local_close_reason_known ||
        snapshot.local_close_reason != MEISO_SESSION_CLOSE_NORMAL) {
        return false;
    }
    (void)printf(
        "{\"type\":\"session\",\"status\":\"CLOSED\","
        "\"reason\":\"NORMAL\"}\n"
    );
    (void)fflush(stdout);
    return true;
}
void meiso_wifi_runtime_host_private_best_effort_close(MeisoWifiRuntimeHostPrivate *host)
{
    MeisoSessionSnapshot snapshot;

    if (host == NULL || host->session == NULL ||
        !meiso_session_snapshot(host->session, &snapshot) ||
        snapshot.state == MEISO_SESSION_STATE_FAILED ||
        snapshot.state == MEISO_SESSION_STATE_CLOSED) {
        return;
    }
    if (meiso_session_close(
            host->session, MEISO_SESSION_CLOSE_PROTOCOL_ERROR
        ) == MEISO_SESSION_CLOSE_QUEUED) {
        (void)meiso_wifi_runtime_host_private_flush_outbound(host);
    }
}

void meiso_wifi_runtime_host_private_emit_failure(const char *stage)
{
    (void)printf(
        "{\"type\":\"failure\",\"status\":\"FAILED\","
        "\"stage\":\"%s\"}\n",
        stage
    );
    (void)fflush(stdout);
}
