#include "meiso_four_kind_lab_private.h"

/* CLI owns process storage, mode selection and top-level exit semantics. */

const char meiso_four_kind_lab_private_address_env[] = "MEISO_LAB_EDGE_ADDRESS";
const char meiso_four_kind_lab_private_expected_edge[] =
    "four-kind-lab-edge-v0";
MeisoFourKindLabStorage meiso_four_kind_lab_private_storage;
#ifdef MEISO_LAB_INTERNAL_TEST
MeisoFourKindLabSelfTestObservation meiso_four_kind_lab_private_self_test;
#endif

static bool parse_test(const char *value, MeisoFourKindLabTest *out_test)
{
    if (value == NULL || out_test == NULL) {
        return false;
    }
    if (strcmp(value, "state") == 0) {
        *out_test = MEISO_FOUR_KIND_LAB_TEST_STATE;
    } else if (strcmp(value, "stream") == 0) {
        *out_test = MEISO_FOUR_KIND_LAB_TEST_STREAM;
    } else if (strcmp(value, "action") == 0) {
        *out_test = MEISO_FOUR_KIND_LAB_TEST_ACTION;
    } else if (strcmp(value, "blob") == 0) {
        *out_test = MEISO_FOUR_KIND_LAB_TEST_BLOB;
    } else if (strcmp(value, "all") == 0) {
        *out_test = MEISO_FOUR_KIND_LAB_TEST_ALL;
    } else {
        return false;
    }
    return true;
}

static bool parse_sessions(const char *value, unsigned int *out_sessions)
{
    uint64_t parsed;
    if (out_sessions == NULL ||
        !meiso_four_kind_lab_private_parse_u64_text(value, &parsed) ||
        parsed == 0U || parsed > MEISO_FOUR_KIND_LAB_MAX_SESSIONS) {
        return false;
    }
    *out_sessions = (unsigned int)parsed;
    return true;
}

static int run_host(const char *config_path, MeisoFourKindLabTest test)
{
    MeisoFourKindLabHostLocator locator;
    int fd;
    bool passed;

    if (!meiso_four_kind_lab_private_parse_host_config(config_path, &locator)) {
        meiso_four_kind_lab_private_emit_session_failed();
        meiso_four_kind_lab_private_emit_verdicts(test, false);
        return 1;
    }
    fd = meiso_four_kind_lab_private_connect_host(&locator);
    if (fd < 0) {
        meiso_four_kind_lab_private_emit_session_failed();
        meiso_four_kind_lab_private_emit_verdicts(test, false);
        return 1;
    }
    passed = meiso_four_kind_lab_private_run_host_transcript(fd, test);
    meiso_four_kind_lab_private_close_fd(&fd);
    return passed ? 0 : 1;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_run_edge_accept_loop(
    int listener, unsigned int sessions, MeisoFourKindLabEdgeProcess *process)
{
    unsigned int completed = 0U;
    bool all_passed = true;

    while (completed < sessions) {
        int fd;
        bool passed;
        meiso_four_kind_lab_private_thermal_child_reap_nonblocking(
            &process->thermal_child);
        fd = meiso_four_kind_lab_private_accept_edge(listener);
        if (fd < 0) {
            all_passed = false;
            break;
        }
        passed = meiso_four_kind_lab_private_run_edge_transcript(fd, process);
        meiso_four_kind_lab_private_close_fd(&fd);
        meiso_four_kind_lab_private_thermal_child_reap_nonblocking(
            &process->thermal_child);
        if (!passed) {
            all_passed = false;
            break;
        }
        completed += 1U;
    }
    return all_passed && completed == sessions;
}

static int run_edge(uint16_t port, unsigned int sessions)
{
    MeisoFourKindLabEdgeProcess process;
    int listener;
    bool passed;

    if (!meiso_four_kind_lab_private_initialize_edge_process(&process)) {
        meiso_four_kind_lab_private_emit_session_failed();
        return 1;
    }
    meiso_four_kind_lab_private_thermal_child_reap_nonblocking(
        &process.thermal_child);
    listener = meiso_four_kind_lab_private_make_listener(port);
    if (listener < 0) {
        meiso_four_kind_lab_private_thermal_child_terminate_nonblocking(
            &process.thermal_child);
        meiso_four_kind_lab_private_emit_session_failed();
        return 1;
    }
    passed = meiso_four_kind_lab_private_run_edge_accept_loop(
        listener, sessions, &process);
    meiso_four_kind_lab_private_close_fd(&listener);
    meiso_four_kind_lab_private_thermal_child_terminate_nonblocking(
        &process.thermal_child);
    meiso_four_kind_lab_private_thermal_child_reap_nonblocking(
        &process.thermal_child);
    return passed ? 0 : 1;
}

static void print_usage(void)
{
    (void)fprintf(
        stderr, "usage: four_kind_lab --host --config PATH "
                "--test state|stream|action|blob|all\n"
                "       four_kind_lab --edge --port 27640 --sessions 1..16\n");
}

int main(int argc, char **argv)
{
#ifdef MEISO_LAB_INTERNAL_TEST
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        return meiso_four_kind_lab_private_run_internal_self_test();
    }
#endif
    if (argc == 6 && strcmp(argv[1], "--host") == 0 &&
        strcmp(argv[2], "--config") == 0 && strcmp(argv[4], "--test") == 0) {
        MeisoFourKindLabTest test;
        if (!parse_test(argv[5], &test)) {
            print_usage();
            return 2;
        }
        return run_host(argv[3], test);
    }
    if (argc == 6 && strcmp(argv[1], "--edge") == 0 &&
        strcmp(argv[2], "--port") == 0 && strcmp(argv[4], "--sessions") == 0) {
        uint16_t port;
        unsigned int sessions;
        if (!meiso_four_kind_lab_private_parse_port(argv[3], &port) ||
            !parse_sessions(argv[5], &sessions)) {
            print_usage();
            return 2;
        }
        return run_edge(port, sessions);
    }
    print_usage();
    return 2;
}
