#include "meiso_four_kind_lab_private.h"

/* Output projection owns stable CLI events, verdicts and blob verification. */

static const char *signal_bucket_name(bool present, int32_t signal_mbm)
{
    if (!present) {
        return "UNKNOWN";
    }
    if (signal_mbm < -8000) {
        return "POOR";
    }
    if (signal_mbm < -7000) {
        return "FAIR";
    }
    if (signal_mbm < -6000) {
        return "GOOD";
    }
    return "EXCELLENT";
}

MEISO_FOUR_KIND_LAB_PRIVATE void meiso_four_kind_lab_private_emit_session_ready(
    const MeisoFourKindLabConnection *connection)
{
    MeisoSessionSnapshot snapshot;

    if (!meiso_session_snapshot(connection->session, &snapshot)) {
        return;
    }
    (void)printf("{\"type\":\"session\",\"layer\":\"session\","
                 "\"phase\":\"handshake\",\"status\":\"READY\","
                 "\"state\":\"READY\",\"failure\":\"NONE\","
                 "\"remote_identity_known\":true,\"identity_match\":true,"
                 "\"session_id_known\":true,\"limits\":{"
                 "\"max_frame_bytes\":%" PRIu32 ","
                 "\"max_runtime_payload_bytes\":%" PRIu32 ","
                 "\"max_registry_entries\":%" PRIu32 "}}\n",
                 snapshot.negotiated_limits.max_frame_bytes,
                 snapshot.negotiated_limits.max_runtime_payload_bytes,
                 snapshot.negotiated_limits.max_registry_entries);
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_session_failed(void)
{
    (void)printf("{\"type\":\"session\",\"layer\":\"session\","
                 "\"phase\":\"failed\",\"status\":\"FAILED\","
                 "\"state\":\"FAILED\",\"failure\":\"TRANSPORT_LOST\"}\n");
    (void)fflush(stdout);
}

static const char *session_failure_name(MeisoSessionFailure failure)
{
    switch (failure) {
    case MEISO_SESSION_FAILURE_MALFORMED_FRAME:
        return "MALFORMED_FRAME";
    case MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE:
        return "UNEXPECTED_MESSAGE";
    case MEISO_SESSION_FAILURE_ROLE_MISMATCH:
        return "ROLE_MISMATCH";
    case MEISO_SESSION_FAILURE_PEER_MISMATCH:
        return "PEER_MISMATCH";
    case MEISO_SESSION_FAILURE_INVALID_INCARNATION:
        return "INVALID_INCARNATION";
    case MEISO_SESSION_FAILURE_INVALID_NONCE:
        return "INVALID_NONCE";
    case MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH:
        return "PROTOCOL_MISMATCH";
    case MEISO_SESSION_FAILURE_PROFILE_MISMATCH:
        return "PROFILE_MISMATCH";
    case MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE:
        return "LIMITS_INCOMPATIBLE";
    case MEISO_SESSION_FAILURE_SESSION_MISMATCH:
        return "SESSION_MISMATCH";
    case MEISO_SESSION_FAILURE_EARLY_RUNTIME:
        return "EARLY_RUNTIME";
    case MEISO_SESSION_FAILURE_LOCAL_CLOSED:
        return "LOCAL_CLOSED";
    case MEISO_SESSION_FAILURE_PEER_CLOSED:
        return "PEER_CLOSED";
    case MEISO_SESSION_FAILURE_TRANSPORT_LOST:
    case MEISO_SESSION_FAILURE_NONE:
    default:
        return "TRANSPORT_LOST";
    }
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_connection_failed(
    const MeisoFourKindLabConnection *connection)
{
    MeisoSessionSnapshot snapshot;
    const char *failure = "TRANSPORT_LOST";

    if (connection != NULL && connection->session != NULL &&
        meiso_session_snapshot(connection->session, &snapshot)) {
        failure = session_failure_name(snapshot.failure);
    }
    (void)printf("{\"type\":\"session\",\"layer\":\"session\","
                 "\"phase\":\"failed\",\"status\":\"FAILED\","
                 "\"state\":\"FAILED\",\"failure\":\"%s\"}\n",
                 failure);
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_transcript_failed(
    const MeisoFourKindLabConnection *connection)
{
    MeisoSessionSnapshot snapshot;
    const char *failure;

    if (connection != NULL && connection->session != NULL &&
        meiso_session_snapshot(connection->session, &snapshot) &&
        snapshot.failure != MEISO_SESSION_FAILURE_NONE) {
        failure = session_failure_name(snapshot.failure);
    } else if (connection != NULL &&
               connection->local_failure ==
                   MEISO_FOUR_KIND_LAB_LOCAL_FAILURE_PROTOCOL) {
        failure = "UNEXPECTED_MESSAGE";
    } else {
        failure = "TRANSPORT_LOST";
    }
    (void)printf("{\"type\":\"session\",\"layer\":\"session\","
                 "\"phase\":\"failed\",\"status\":\"FAILED\","
                 "\"state\":\"FAILED\",\"failure\":\"%s\"}\n",
                 failure);
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_session_closed(
    const MeisoFourKindLabConnection *connection)
{
    MeisoSessionSnapshot snapshot;
    const char *failure = "UNKNOWN";

    if (connection != NULL && connection->session != NULL &&
        meiso_session_snapshot(connection->session, &snapshot)) {
        failure = session_failure_name(snapshot.failure);
    }
    (void)printf("{\"type\":\"session\",\"layer\":\"session\","
                 "\"phase\":\"closed\",\"status\":\"CLOSED\","
                 "\"state\":\"CLOSED\",\"failure\":\"%s\"}\n",
                 failure);
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void meiso_four_kind_lab_private_emit_registry(void)
{
    (void)printf("{\"type\":\"registry\",\"layer\":\"registry\","
                 "\"phase\":\"accepted\",\"status\":\"ACCEPTED\","
                 "\"state\":\"FIXTURE\",\"entries\":4,\"capacity\":4,"
                 "\"source\":\"fixture\"}\n");
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_state_event(uint64_t version,
                                             const MeisoLabEdgeState *state)
{
    (void)printf("{\"type\":\"state\",\"layer\":\"primitive\","
                 "\"direction\":\"edge_to_host\",\"phase\":\"snapshot\","
                 "\"status\":\"ACCEPTED\",\"name\":\"EdgeState\","
                 "\"version\":%" PRIu64 ",\"window_id\":%" PRIu64 ","
                 "\"interface_present\":%s,\"carrier_present\":%s",
                 version, state->window_id,
                 state->interface_present ? "true" : "false",
                 state->carrier_present ? "true" : "false");
    if (state->temperature_present) {
        (void)printf(",\"temperature_millic\":%" PRId32
                     ",\"temperature_uncertainty_millic\":%" PRIu32,
                     state->corrected_millic, state->uncertainty_millic);
    }
    (void)printf(",\"verified\":true}\n");
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void meiso_four_kind_lab_private_emit_stream_event(
    const MeisoLabStreamMetrics *metrics, uint64_t receive_time_ns)
{
    const MeisoLabTelemetrySample *sample = &metrics->value;

    (void)printf(
        "{\"type\":\"stream\",\"layer\":\"primitive\","
        "\"direction\":\"edge_to_host\",\"phase\":\"sample\","
        "\"status\":\"ACCEPTED\",\"name\":\"TelemetrySample\","
        "\"monotonic_ns\":%" PRIu64 ","
        "\"epoch\":%" PRIu64 ",\"sequence\":%" PRIu64 ","
        "\"source_time_ns\":%" PRIu64 ","
        "\"window_id\":%" PRIu64 ",\"elapsed_ns\":%" PRIu64 ","
        "\"rx_bytes\":%" PRIu64 ",\"tx_bytes\":%" PRIu64 ","
        "\"rx_packets\":%" PRIu64 ",\"tx_packets\":%" PRIu64 ","
        "\"rx_dropped\":%" PRIu64 ",\"tx_dropped\":%" PRIu64 ","
        "\"signal_bucket\":\"%s\"",
        receive_time_ns, metrics->epoch, metrics->sequence,
        metrics->source_time.monotonic_ns, sample->window_id,
        sample->elapsed_ns, sample->rx_bytes, sample->tx_bytes,
        sample->rx_packets, sample->tx_packets, sample->rx_dropped,
        sample->tx_dropped,
        signal_bucket_name(sample->signal_present, sample->signal_mbm));
    if (sample->temperature_present) {
        (void)printf(",\"temperature_millic\":%" PRId32
                     ",\"temperature_uncertainty_millic\":%" PRIu32,
                     sample->corrected_millic, sample->uncertainty_millic);
    }
    (void)printf(",\"verified\":true}\n");
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_action_event(uint64_t new_window_id)
{
    (void)printf("{\"type\":\"action\",\"layer\":\"primitive\","
                 "\"direction\":\"host_to_edge\",\"phase\":\"result\","
                 "\"status\":\"SUCCEEDED\","
                 "\"operation\":\"telemetry.reset_window\","
                 "\"delivery_accepted\":true,\"edge_accepted\":true,"
                 "\"executed_once\":true,\"result_code\":\"OK\","
                 "\"result_value\":%" PRIu64 ",\"verified\":true}\n",
                 new_window_id);
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_blob_event(size_t content_size)
{
    (void)printf("{\"type\":\"blob\",\"layer\":\"primitive\","
                 "\"direction\":\"edge_to_host\",\"phase\":\"published\","
                 "\"status\":\"PUBLISHED\","
                 "\"label\":\"edge.capability.manifest\","
                 "\"content_size\":%zu,\"integrity_verified\":true,"
                 "\"published\":true,\"duplicate\":false,"
                 "\"verified\":true}\n",
                 content_size);
    (void)fflush(stdout);
}

static const char *test_name(MeisoFourKindLabTest test)
{
    switch (test) {
    case MEISO_FOUR_KIND_LAB_TEST_STATE:
        return "state";
    case MEISO_FOUR_KIND_LAB_TEST_STREAM:
        return "stream";
    case MEISO_FOUR_KIND_LAB_TEST_ACTION:
        return "action";
    case MEISO_FOUR_KIND_LAB_TEST_BLOB:
        return "blob";
    case MEISO_FOUR_KIND_LAB_TEST_ALL:
    default:
        return "all";
    }
}

static void emit_one_verdict(const char *name, bool passed, unsigned int checks)
{
    (void)printf("{\"type\":\"verification\",\"layer\":\"fixture\","
                 "\"phase\":\"verified\",\"status\":\"%s\","
                 "\"test\":\"%s\",\"result\":\"%s\","
                 "\"checks_passed\":%u,\"checks_total\":%u}\n",
                 passed ? "PASS" : "FAIL", name, passed ? "PASS" : "FAIL",
                 passed ? checks : 0U, checks);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_verdicts(MeisoFourKindLabTest test,
                                          bool passed)
{
    if (test == MEISO_FOUR_KIND_LAB_TEST_ALL) {
        emit_one_verdict("state", passed, 3U);
        emit_one_verdict("stream", passed, 3U);
        emit_one_verdict("action", passed, 4U);
        emit_one_verdict("blob", passed, 3U);
    } else {
        const unsigned int checks =
            test == MEISO_FOUR_KIND_LAB_TEST_ACTION ? 4U : 3U;
        emit_one_verdict(test_name(test), passed, checks);
    }
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_stream_consumed(
    const MeisoLabStreamMetrics *metrics)
{
    (void)printf("{\"type\":\"stream\",\"layer\":\"primitive\","
                 "\"direction\":\"local\",\"phase\":\"applied\","
                 "\"status\":\"ACCEPTED\",\"name\":\"TelemetrySample\","
                 "\"epoch\":%" PRIu64 ",\"sequence\":%" PRIu64 ","
                 "\"verified\":true}\n",
                 metrics->epoch, metrics->sequence);
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_action_delivery_accepted(void)
{
    (void)printf("{\"type\":\"action\",\"layer\":\"primitive\","
                 "\"direction\":\"edge_to_host\",\"phase\":\"accepted\","
                 "\"status\":\"ACCEPTED\","
                 "\"operation\":\"telemetry.reset_window\","
                 "\"delivery_accepted\":true,\"edge_accepted\":false,"
                 "\"executed_once\":false,\"verified\":true}\n");
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_action_executor_accepted(void)
{
    (void)printf("{\"type\":\"action\",\"layer\":\"primitive\","
                 "\"direction\":\"edge_to_host\",\"phase\":\"result\","
                 "\"status\":\"ACCEPTED\","
                 "\"operation\":\"telemetry.reset_window\","
                 "\"delivery_accepted\":true,\"edge_accepted\":true,"
                 "\"executed_once\":false,\"verified\":true}\n");
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_blob_staged(size_t content_size)
{
    (void)printf("{\"type\":\"blob\",\"layer\":\"primitive\","
                 "\"direction\":\"local\",\"phase\":\"received\","
                 "\"status\":\"RUNNING\","
                 "\"label\":\"edge.capability.manifest\","
                 "\"content_size\":%zu,\"integrity_verified\":false,"
                 "\"published\":false,\"duplicate\":false,"
                 "\"verified\":false}\n",
                 content_size);
    (void)fflush(stdout);
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_write_u64_be(uint8_t *destination, uint64_t value)
{
    destination[0] = (uint8_t)(value >> 56U);
    destination[1] = (uint8_t)(value >> 48U);
    destination[2] = (uint8_t)(value >> 40U);
    destination[3] = (uint8_t)(value >> 32U);
    destination[4] = (uint8_t)(value >> 24U);
    destination[5] = (uint8_t)(value >> 16U);
    destination[6] = (uint8_t)(value >> 8U);
    destination[7] = (uint8_t)value;
}

MEISO_FOUR_KIND_LAB_PRIVATE uint64_t
meiso_four_kind_lab_private_read_u64_be(const uint8_t *source)
{
    return ((uint64_t)source[0] << 56U) | ((uint64_t)source[1] << 48U) |
           ((uint64_t)source[2] << 40U) | ((uint64_t)source[3] << 32U) |
           ((uint64_t)source[4] << 24U) | ((uint64_t)source[5] << 16U) |
           ((uint64_t)source[6] << 8U) | (uint64_t)source[7];
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_bytes_equal(MeisoByteView lhs, MeisoByteView rhs)
{
    return lhs.size == rhs.size &&
           (lhs.size == 0U || memcmp(lhs.data, rhs.data, lhs.size) == 0);
}

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_verify_fixed_blob(
    const MeisoContentId *expected_content_id, MeisoByteView staged_content,
    void *context)
{
    MeisoFourKindLabBlobVerifyContext *verify = context;

    if (verify == NULL) {
        return false;
    }
    verify->calls += 1U;
    return meiso_content_id_equal(expected_content_id, &verify->expected_id) &&
           meiso_four_kind_lab_private_bytes_equal(staged_content,
                                                   verify->expected_content);
}
