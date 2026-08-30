#ifndef MEISO_COMPOSITION_EDGE_TRACE_PRIVATE_H
#define MEISO_COMPOSITION_EDGE_TRACE_PRIVATE_H

/* Build-private/non-installed bridge; trace borrows a live simulator handle. */

#include <meiso/simulation/composition_edge.h>

#include <stdbool.h>
#include <stddef.h>

bool meiso_composition_edge_internal_simulator_storage_disjoint(
    const MeisoEdgeHandle *handle,
    const void *pointer,
    size_t size
);

bool meiso_composition_edge_internal_simulator_fresh_matches(
    const MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorConfig *config
);

#endif
