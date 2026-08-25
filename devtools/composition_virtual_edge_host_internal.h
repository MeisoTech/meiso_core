#ifndef MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INTERNAL_H
#define MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INTERNAL_H

#include "meiso_composition_edge_simulator.h"

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
