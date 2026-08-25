#ifndef MEISO_TEST_COMPOSITION_VIRTUAL_EDGE_APPLICATION_H
#define MEISO_TEST_COMPOSITION_VIRTUAL_EDGE_APPLICATION_H

#include "meiso_composition_edge.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool meiso_test_virtual_edge_bootstrap(MeisoEdgeHandle *handle);
bool meiso_test_virtual_edge_submit_wifi(
    MeisoEdgeHandle *handle,
    MeisoEdgeCommandReceipt *out_receipt
);
bool meiso_test_virtual_edge_ready(
    MeisoEdgeHandle *handle,
    uint64_t continuity_serial,
    bool expect_sample
);
bool meiso_test_virtual_edge_camera_frame(
    MeisoEdgeHandle *handle,
    uint64_t continuity_serial,
    const uint8_t *expected_bytes,
    size_t expected_size
);
bool meiso_test_virtual_edge_terminal(MeisoEdgeHandle *handle);

#endif
