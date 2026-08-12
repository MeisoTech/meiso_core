#include <stddef.h>
#include <stdio.h>

#ifndef MEISO_CONFORMANCE_SOURCE_COMMIT
#define MEISO_CONFORMANCE_SOURCE_COMMIT "UNLOCKED_HOST_BUILD"
#endif

typedef int (*MeisoConformanceSuiteEntry)(void);

typedef struct {
    const char *name;
    MeisoConformanceSuiteEntry entry;
} MeisoConformanceSuite;

int meiso_conformance_run_resource(void);
int meiso_conformance_run_state(void);
int meiso_conformance_run_stream(void);
int meiso_conformance_run_action(void);
int meiso_conformance_run_blob(void);
int meiso_conformance_run_descriptor_set(void);
int meiso_conformance_run_four_kind(void);

static const MeisoConformanceSuite suites[] = {
    {"resource", meiso_conformance_run_resource},
    {"state", meiso_conformance_run_state},
    {"stream", meiso_conformance_run_stream},
    {"action", meiso_conformance_run_action},
    {"blob", meiso_conformance_run_blob},
    {"descriptor_set", meiso_conformance_run_descriptor_set},
    {"four_kind", meiso_conformance_run_four_kind}
};

int main(void)
{
    const size_t suite_count = sizeof(suites) / sizeof(suites[0]);
    const char *first_failed_suite = "none";
    size_t passed = 0U;
    size_t failed = 0U;
    size_t index;

    (void)printf(
        "MEISO_CONFORMANCE_BEGIN format=1 source_commit=%s suites_total=%zu\n",
        MEISO_CONFORMANCE_SOURCE_COMMIT,
        suite_count
    );
    (void)fflush(stdout);

    for (index = 0U; index < suite_count; ++index) {
        int result;

        (void)printf(
            "MEISO_CONFORMANCE_SUITE_BEGIN name=%s\n", suites[index].name
        );
        (void)fflush(stdout);
        result = suites[index].entry();
        if (result == 0) {
            passed += 1U;
            (void)printf(
                "MEISO_CONFORMANCE_SUITE_PASS name=%s rc=%d\n",
                suites[index].name,
                result
            );
        } else {
            if (failed == 0U) {
                first_failed_suite = suites[index].name;
            }
            failed += 1U;
            (void)printf(
                "MEISO_CONFORMANCE_SUITE_FAIL name=%s rc=%d\n",
                suites[index].name,
                result
            );
        }
        (void)fflush(stdout);
    }

    if (failed == 0U) {
        (void)printf(
            "MEISO_CONFORMANCE_PASS suites_total=%zu passed=%zu failed=0 "
            "first_failed_suite=none\n",
            suite_count,
            passed
        );
        return 0;
    }

    (void)printf(
        "MEISO_CONFORMANCE_FAIL suites_total=%zu passed=%zu failed=%zu "
        "first_failed_suite=%s\n",
        suite_count,
        passed,
        failed,
        first_failed_suite
    );
    return 1;
}
