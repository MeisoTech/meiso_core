#ifndef MEISO_TEST_COMPOSITION_EDGE_CLIENT_FIXTURE_H
#define MEISO_TEST_COMPOSITION_EDGE_CLIENT_FIXTURE_H

#include <meiso/host/composition.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool meiso_test_composition_edge_bootstrap(MeisoEdgeHandle *handle);
bool meiso_test_composition_edge_submit_wifi(
    MeisoEdgeHandle *handle,
    MeisoEdgeCommandReceipt *out_receipt
);
bool meiso_test_composition_edge_ready(
    MeisoEdgeHandle *handle,
    uint64_t continuity_serial,
    bool expect_sample
);
bool meiso_test_composition_edge_camera_frame(
    MeisoEdgeHandle *handle,
    uint64_t continuity_serial,
    const uint8_t *expected_bytes,
    size_t expected_size
);
bool meiso_test_composition_edge_terminal(MeisoEdgeHandle *handle);

#endif
