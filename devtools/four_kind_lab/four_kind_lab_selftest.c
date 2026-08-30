#include "meiso_four_kind_lab_private.h"

/* Internal self-test owns fault fixtures and two-session qualification. */

#ifdef MEISO_LAB_INTERNAL_TEST
static bool wait_child_exit_bounded(pid_t child, unsigned int timeout_ms);

static void finish_thermal_fixture(MeisoFourKindLabThermalChild *child)
{
    meiso_four_kind_lab_private_thermal_child_terminate_nonblocking(child);
    meiso_four_kind_lab_private_thermal_child_reap_nonblocking(child);
}

static bool self_test_thermal_modes(void)
{
    MeisoFourKindLabThermalReading reading;
    MeisoFourKindLabThermalChild child;
    MeisoFourKindLabThermalPreflightStatus status;
    uint64_t started;
    uint64_t finished;

    memset(&child, 0, sizeof(child));
    child.pid = (pid_t)-1;
    status = meiso_four_kind_lab_private_thermal_preflight_bounded(
        &reading, &child, MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_PRESENT);
    finish_thermal_fixture(&child);
    if (status != MEISO_FOUR_KIND_THERMAL_PREFLIGHT_PRESENT ||
        !reading.present || reading.corrected_millic != 52000 ||
        reading.uncertainty_millic !=
            MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC) {
        return false;
    }

    status = meiso_four_kind_lab_private_thermal_preflight_bounded(
        &reading, &child, MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_ABSENT);
    finish_thermal_fixture(&child);
    if (status != MEISO_FOUR_KIND_THERMAL_PREFLIGHT_ABSENT || reading.present ||
        reading.corrected_millic != 0 || reading.uncertainty_millic != 0U) {
        return false;
    }

    if (!meiso_four_kind_lab_private_monotonic_now_ns(&started)) {
        return false;
    }
    status = meiso_four_kind_lab_private_thermal_preflight_bounded(
        &reading, &child, MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_TIMEOUT);
    if (!meiso_four_kind_lab_private_monotonic_now_ns(&finished)) {
        finish_thermal_fixture(&child);
        return false;
    }
    finish_thermal_fixture(&child);
    if (status != MEISO_FOUR_KIND_THERMAL_PREFLIGHT_TIMEOUT ||
        reading.present || finished < started ||
        finished - started > UINT64_C(1500000000)) {
        return false;
    }

    status = meiso_four_kind_lab_private_thermal_preflight_bounded(
        &reading, &child, MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_PARTIAL);
    finish_thermal_fixture(&child);
    if (status != MEISO_FOUR_KIND_THERMAL_PREFLIGHT_TIMEOUT ||
        reading.present) {
        return false;
    }

    status = meiso_four_kind_lab_private_thermal_preflight_bounded(
        &reading, &child, MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_EARLY_EXIT);
    finish_thermal_fixture(&child);
    return status == MEISO_FOUR_KIND_THERMAL_PREFLIGHT_CHILD_ERROR &&
           !reading.present;
}

static bool self_test_thermal_stdout_eof(void)
{
    int descriptors[2];
    pid_t runner;
    struct timespec deadline;
    bool eof = false;
    bool runner_ok;

    if (!meiso_four_kind_lab_private_make_thermal_pipe(descriptors) ||
        !meiso_four_kind_lab_private_make_deadline_after_ms(&deadline, 2000U)) {
        return false;
    }
    runner = fork();
    if (runner < 0) {
        meiso_four_kind_lab_private_close_fd(&descriptors[0]);
        meiso_four_kind_lab_private_close_fd(&descriptors[1]);
        return false;
    }
    if (runner == 0) {
        MeisoFourKindLabThermalReading reading;
        MeisoFourKindLabThermalChild child;
        MeisoFourKindLabThermalPreflightStatus status;

        meiso_four_kind_lab_private_close_fd(&descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) {
            _exit(2);
        }
        meiso_four_kind_lab_private_close_fd(&descriptors[1]);
        memset(&child, 0, sizeof(child));
        child.pid = (pid_t)-1;
        status = meiso_four_kind_lab_private_thermal_preflight_bounded(
            &reading, &child, MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_TIMEOUT);
        finish_thermal_fixture(&child);
        if (status != MEISO_FOUR_KIND_THERMAL_PREFLIGHT_TIMEOUT ||
            write(STDOUT_FILENO, "ok", 2U) != 2) {
            _exit(2);
        }
        (void)close(STDOUT_FILENO);
        _exit(0);
    }
    meiso_four_kind_lab_private_close_fd(&descriptors[1]);
    while (!eof &&
           meiso_four_kind_lab_private_deadline_wait_ms(&deadline) != 0) {
        uint8_t buffer[16];
        const ssize_t count = read(descriptors[0], buffer, sizeof(buffer));

        if (count == 0) {
            eof = true;
        } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
            break;
        }
        if (!eof) {
            (void)poll(NULL, (nfds_t)0U, 10);
        }
    }
    meiso_four_kind_lab_private_close_fd(&descriptors[0]);
    runner_ok = wait_child_exit_bounded(
        runner, MEISO_FOUR_KIND_LAB_THERMAL_PREFLIGHT_TIMEOUT_MS);
    return eof && runner_ok;
}

static bool self_test_state_delivery_failure(void)
{
    MeisoFourKindLabArena arena;
    MeisoFourKindLabEdgePrimitives edge;
    MeisoStatePublisher *publisher;
    MeisoStateOffer offer;
    MeisoStatePublisherStats stats;
    const MeisoResourceDescriptor *descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STATE);
    const MeisoStateConfig config = {MEISO_STATE_DELIVERY_CONVERGENT};
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 41U};
    const uint8_t payload[] = {1U};

    memset(&arena, 0, sizeof(arena));
    memset(&edge, 0, sizeof(edge));
    if (descriptor == NULL ||
        meiso_state_publisher_init(arena.bytes, sizeof(arena.bytes), descriptor,
                                   &config,
                                   &publisher) != MEISO_STATE_INIT_OK ||
        meiso_state_publisher_publish(
            publisher, owner, 1U, (MeisoByteView){payload, sizeof(payload)}) !=
            MEISO_STATE_PUBLISH_NEW) {
        return false;
    }
    edge.state = publisher;
    edge.state_delivery.value = 1U;
    if (meiso_state_publisher_begin_delivery(publisher, edge.state_delivery) !=
            MEISO_STATE_DELIVERY_STARTED ||
        !meiso_state_publisher_next_offer(publisher, &offer)) {
        return false;
    }
    edge.state_owner = offer.state.owner;
    edge.state_version = offer.state.version;
    edge.state_delivery_active = true;
    return meiso_four_kind_lab_private_edge_fail_active_state_delivery(&edge) &&
           !edge.state_delivery_active &&
           meiso_state_publisher_stats(publisher, &stats) &&
           stats.delivery_failures == 1U && stats.convergence_failures == 1U;
}

static bool self_test_action_caller_indeterminate(void)
{
    MeisoFourKindLabArena arena;
    MeisoFourKindLabHostPrimitives host;
    MeisoActionCaller *caller;
    MeisoActionEntryView view;
    MeisoActionCallerStats stats;
    MeisoActionRequest request;
    const MeisoResourceDescriptor *action_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    const MeisoResourceDescriptor *stream_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STREAM);
    const MeisoActionConfig config = {1U, 0U, 8U};
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 51U};
    const MeisoActionId id = {61U, 62U};

    memset(&arena, 0, sizeof(arena));
    memset(&host, 0, sizeof(host));
    memset(&request, 0, sizeof(request));
    if (action_descriptor == NULL || stream_descriptor == NULL ||
        meiso_action_caller_init(arena.bytes, sizeof(arena.bytes),
                                 action_descriptor, &config,
                                 &caller) != MEISO_ACTION_INIT_OK) {
        return false;
    }
    host.action = caller;
    host.action_delivery.value = 1U;
    host.action_id = id;
    host.edge_owner = owner;
    request.id = id;
    request.expected_executor = owner;
    request.target = stream_descriptor->id;
    request.operation = action_descriptor->schema;
    request.retry_policy = MEISO_ACTION_RETRY_NONE;
    if (meiso_action_caller_begin_delivery(caller, host.action_delivery) !=
            MEISO_ACTION_DELIVERY_STARTED ||
        meiso_action_caller_submit(caller, &request) !=
            MEISO_ACTION_SUBMIT_NEW ||
        meiso_action_caller_mark_delivery_accepted(caller, host.action_delivery,
                                                   id, owner) !=
            MEISO_ACTION_DELIVERY_ACCEPT_CONFIRMED) {
        return false;
    }
    host.action_delivery_accepted = true;
    return meiso_four_kind_lab_private_host_mark_accepted_action_indeterminate(
               &host) &&
           host.action_terminal &&
           meiso_action_caller_query(caller, id, &view) &&
           view.state == MEISO_ACTION_STATE_INDETERMINATE &&
           meiso_action_caller_stats(caller, &stats) &&
           stats.indeterminate == 1U;
}

static bool self_test_action_executor_indeterminate(void)
{
    MeisoFourKindLabEdgeProcess process;
    MeisoActionRequest request;
    MeisoActionRequest application;
    MeisoActionEntryView view;
    MeisoActionExecutorStats stats;
    size_t cursor = 0U;
    const MeisoResourceDescriptor *action_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    const MeisoResourceDescriptor *stream_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STREAM);
    const MeisoActionConfig config = {1U, 0U, 8U};
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 71U};
    const MeisoActionId id = {72U, 73U};

    memset(&process, 0, sizeof(process));
    memset(&request, 0, sizeof(request));
    process.scope = owner;
    if (action_descriptor == NULL || stream_descriptor == NULL ||
        meiso_action_executor_init(process.action_arena.bytes,
                                   sizeof(process.action_arena.bytes),
                                   action_descriptor, &config, owner,
                                   &process.action) != MEISO_ACTION_INIT_OK) {
        return false;
    }
    request.id = id;
    request.expected_executor = owner;
    request.target = stream_descriptor->id;
    request.operation = action_descriptor->schema;
    request.retry_policy = MEISO_ACTION_RETRY_NONE;
    if (meiso_action_executor_ingest(process.action, &request, NULL) !=
            MEISO_ACTION_INGEST_NEW ||
        !meiso_action_executor_next_application_request(process.action, &cursor,
                                                        &application) ||
        !meiso_action_id_equal(application.id, id) ||
        meiso_action_executor_accept(process.action, owner, id,
                                     (MeisoTimePoint){0U, 0U}) !=
            MEISO_ACTION_EXECUTOR_ACCEPTED) {
        return false;
    }
    process.active_action_id = id;
    process.active_action_nonterminal = true;
    return meiso_four_kind_lab_private_edge_mark_active_action_indeterminate(
               &process) &&
           !process.active_action_nonterminal &&
           meiso_action_executor_query(process.action, id, &view) &&
           view.state == MEISO_ACTION_STATE_INDETERMINATE &&
           meiso_action_executor_stats(process.action, &stats) &&
           stats.indeterminate == 1U;
}

static bool self_test_wrong_state_owner(void)
{
    MeisoFourKindLabArena arena;
    MeisoFourKindLabConnection connection;
    MeisoFourKindLabHostPrimitives host;
    MeisoLabMessage message;
    MeisoStateView current;
    MeisoStateReplicaStats stats;
    const MeisoResourceDescriptor *descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STATE);

    memset(&arena, 0, sizeof(arena));
    memset(&connection, 0, sizeof(connection));
    memset(&host, 0, sizeof(host));
    memset(&message, 0, sizeof(message));
    connection.edge_scope = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 81U};
    if (descriptor == NULL ||
        meiso_state_replica_init(arena.bytes, sizeof(arena.bytes), descriptor,
                                 &host.state) != MEISO_STATE_INIT_OK) {
        return false;
    }
    message.type = MEISO_LAB_MESSAGE_STATE_ACTUAL;
    message.body.state_actual.owner =
        (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 82U};
    message.body.state_actual.version = 1U;
    message.body.state_actual.value.window_id = 1U;
    message.body.state_actual.value.interface_present = true;
    message.body.state_actual.value.carrier_present = true;
    return !meiso_four_kind_lab_private_host_runtimely_state_actual(
               &connection, &host, &message, &current) &&
           !host.edge_state_known &&
           meiso_state_replica_stats(host.state, &stats) &&
           stats.receives == 0U;
}

static bool wait_child_exit_bounded(pid_t child, unsigned int timeout_ms)
{
    struct timespec deadline;
    int status;

    if (!meiso_four_kind_lab_private_make_deadline_after_ms(&deadline,
                                                            timeout_ms)) {
        return false;
    }
    while (meiso_four_kind_lab_private_deadline_wait_ms(&deadline) != 0) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0) {
            return false;
        }
        (void)poll(NULL, (nfds_t)0U, 10);
    }
    if (kill(child, SIGKILL) == 0 || errno == ESRCH) {
        pid_t result;
        do {
            result = waitpid(child, &status, 0);
        } while (result < 0 && errno == EINTR);
    }
    return false;
}

static bool listener_port(int listener, uint16_t *out_port)
{
    struct sockaddr_in address;
    socklen_t size = (socklen_t)sizeof(address);

    memset(&address, 0, sizeof(address));
    if (out_port == NULL ||
        getsockname(listener, (struct sockaddr *)(void *)&address, &size) !=
            0 ||
        size != (socklen_t)sizeof(address) || address.sin_family != AF_INET ||
        ntohs(address.sin_port) == 0U) {
        return false;
    }
    *out_port = ntohs(address.sin_port);
    return true;
}

static bool self_test_two_sessions(void)
{
    MeisoFourKindLabHostLocator locator;
    uint16_t port;
    int listener = meiso_four_kind_lab_private_make_listener(0U);
    pid_t edge_child;
    unsigned int index;
    bool passed = true;

    if (listener < 0 || !listener_port(listener, &port)) {
        meiso_four_kind_lab_private_close_fd(&listener);
        return false;
    }
    edge_child = fork();
    if (edge_child < 0) {
        meiso_four_kind_lab_private_close_fd(&listener);
        return false;
    }
    if (edge_child == 0) {
        MeisoFourKindLabEdgeProcess process;
        MeisoActionExecutorStats stats;
        bool child_passed =
            meiso_four_kind_lab_private_initialize_edge_process_with_thermal_mode(
                &process, MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_ABSENT);

        if (child_passed) {
            child_passed = meiso_four_kind_lab_private_run_edge_accept_loop(
                listener, MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS, &process);
        }
        meiso_four_kind_lab_private_close_fd(&listener);
        if (child_passed) {
            child_passed =
                process.state_version == 2U && process.stream_sequence == 2U &&
                process.telemetry.window_id == 3U &&
                meiso_action_executor_stats(process.action, &stats) &&
                stats.ingests == 2U && stats.replays == 0U &&
                stats.succeeded == 2U;
        }
        meiso_four_kind_lab_private_thermal_child_terminate_nonblocking(
            &process.thermal_child);
        meiso_four_kind_lab_private_thermal_child_reap_nonblocking(
            &process.thermal_child);
        _exit(child_passed ? 0 : 1);
    }
    meiso_four_kind_lab_private_close_fd(&listener);
    memset(&meiso_four_kind_lab_private_self_test, 0,
           sizeof(meiso_four_kind_lab_private_self_test));
    meiso_four_kind_lab_private_self_test.enabled = true;
    meiso_four_kind_lab_private_self_test.fixed_action_id = false;
    memset(&locator, 0, sizeof(locator));
    locator.address.s_addr = htonl(UINT32_C(0x7f000001));
    locator.port = port;
    for (index = 0U; index < MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS; ++index) {
        int fd = meiso_four_kind_lab_private_connect_host(&locator);
        if (fd < 0 || !meiso_four_kind_lab_private_run_host_transcript(
                          fd, MEISO_FOUR_KIND_LAB_TEST_ALL)) {
            meiso_four_kind_lab_private_close_fd(&fd);
            passed = false;
            break;
        }
        meiso_four_kind_lab_private_close_fd(&fd);
    }
    if (!wait_child_exit_bounded(edge_child, 10000U)) {
        passed = false;
    }
    passed =
        passed &&
        meiso_four_kind_lab_private_self_test.session_count ==
            MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS &&
        !meiso_session_id_equal(
            meiso_four_kind_lab_private_self_test.session_ids[0],
            meiso_four_kind_lab_private_self_test.session_ids[1]) &&
        meiso_owner_scope_equal(
            meiso_four_kind_lab_private_self_test.owners[0],
            meiso_four_kind_lab_private_self_test.owners[1]) &&
        meiso_four_kind_lab_private_self_test.owners[0].incarnation != 0U &&
        meiso_four_kind_lab_private_self_test.state_versions[0] == 1U &&
        meiso_four_kind_lab_private_self_test.state_versions[1] == 2U &&
        meiso_four_kind_lab_private_self_test.stream_epochs[0] != 0U &&
        meiso_four_kind_lab_private_self_test.stream_epochs[0] ==
            meiso_four_kind_lab_private_self_test.stream_epochs[1] &&
        meiso_four_kind_lab_private_self_test.stream_sequences[0] == 1U &&
        meiso_four_kind_lab_private_self_test.stream_sequences[1] == 2U &&
        meiso_four_kind_lab_private_self_test.clock_epochs[0] != 0U &&
        meiso_four_kind_lab_private_self_test.clock_epochs[0] ==
            meiso_four_kind_lab_private_self_test.clock_epochs[1] &&
        meiso_four_kind_lab_private_self_test.action_windows[0] == 2U &&
        meiso_four_kind_lab_private_self_test.action_windows[1] == 3U;
    memset(&meiso_four_kind_lab_private_self_test, 0,
           sizeof(meiso_four_kind_lab_private_self_test));
    return passed;
}

MEISO_FOUR_KIND_LAB_PRIVATE int
meiso_four_kind_lab_private_run_internal_self_test(void)
{
    const bool passed =
        self_test_thermal_modes() && self_test_thermal_stdout_eof() &&
        self_test_state_delivery_failure() &&
        self_test_action_caller_indeterminate() &&
        self_test_action_executor_indeterminate() &&
        self_test_wrong_state_owner() && self_test_two_sessions();

    if (!passed) {
        (void)fprintf(stderr, "four-kind lab internal self-test failed\n");
    }
    return passed ? 0 : 1;
}
#endif
