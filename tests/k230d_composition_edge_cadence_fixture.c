#include "composition_edge_runtime_private.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool meiso_test_k230d_composition_edge_one_hz_cadence(void);

bool meiso_test_k230d_composition_edge_one_hz_cadence(void)
{
    EdgeCadence cadence;
    const uint64_t first = UINT64_C(123456789);

    memset(&cadence, 0, sizeof(cadence));
    if (!meiso_k230d_composition_edge_runtime_private_cadence_ready(
            &cadence, 7U, 1000U, first
        ) ||
        !meiso_k230d_composition_edge_runtime_private_cadence_commit(
            &cadence, first
        ) ||
        meiso_k230d_composition_edge_runtime_private_cadence_ready(
            &cadence, 7U, 1000U, first + UINT64_C(999999999)
        )) {
        return false;
    }
    return meiso_k230d_composition_edge_runtime_private_cadence_ready(
        &cadence, 7U, 1000U, first + UINT64_C(1000000000)
    );
}
