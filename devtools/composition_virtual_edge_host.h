#ifndef MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_H
#define MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_H

#include "composition_host_app.h"
#include "meiso_composition_edge.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    MeisoCompositionHostApp *host_app;
    void *host_app_arena;
    size_t host_app_arena_size;
    uint64_t catalog_generation;
    MeisoCompositionSessionV3CatalogConfig catalog;
    /* Nonzero and local to this newly created handle lifetime. */
    uint64_t continuity_serial;
} MeisoCompositionVirtualEdgeHostConfig;

typedef uint8_t MeisoCompositionVirtualEdgeHostInitStatus;
enum {
    MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_OK = 0,
    MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_MISALIGNED_ARENA = 3,
    MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_ARENA_TOO_SMALL = 4,
    MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_BACKEND_TERMINAL = 5,
    MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_INTERNAL_FAILURE = 6
};

size_t meiso_composition_virtual_edge_host_alignment_required(void);
size_t meiso_composition_virtual_edge_host_memory_required(void);

/*
 * The adapter borrows a live HostApp and its complete arena; it owns neither.
 * It owns and writes exactly the leading memory_required() bytes of its arena;
 * any extra capacity is neither read nor written. That owned prefix, the
 * complete HostApp arena, config and out_handle storage are mutually disjoint.
 * Every failure leaves both arenas and out_handle byte-exact. The integration
 * continues to own HostApp pump/readiness and every socket.
 */
MeisoCompositionVirtualEdgeHostInitStatus
meiso_composition_virtual_edge_host_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionVirtualEdgeHostConfig *config,
    MeisoEdgeHandle **out_handle
);

#ifdef __cplusplus
}
#endif

#endif
