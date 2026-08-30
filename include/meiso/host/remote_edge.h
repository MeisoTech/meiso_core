#ifndef MEISO_HOST_REMOTE_EDGE_H
#define MEISO_HOST_REMOTE_EDGE_H

#include <meiso/host/composition_runtime.h>
#include <meiso/host/composition.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    MeisoCompositionHostRuntime *host_runtime;
    void *host_runtime_arena;
    size_t host_runtime_arena_size;
    uint64_t catalog_generation;
    MeisoCompositionProfileCatalogConfig catalog;
    /* Nonzero and local to this newly created handle lifetime. */
    uint64_t continuity_serial;
} MeisoCompositionRemoteEdgeConfig;

typedef uint8_t MeisoCompositionRemoteEdgeInitStatus;
enum {
    MEISO_COMPOSITION_REMOTE_EDGE_INIT_OK = 0,
    MEISO_COMPOSITION_REMOTE_EDGE_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_REMOTE_EDGE_INIT_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_REMOTE_EDGE_INIT_MISALIGNED_ARENA = 3,
    MEISO_COMPOSITION_REMOTE_EDGE_INIT_ARENA_TOO_SMALL = 4,
    MEISO_COMPOSITION_REMOTE_EDGE_INIT_BACKEND_TERMINAL = 5,
    MEISO_COMPOSITION_REMOTE_EDGE_INIT_INTERNAL_FAILURE = 6
};

size_t meiso_composition_remote_edge_alignment_required(void);
size_t meiso_composition_remote_edge_memory_required(void);

/*
 * The adapter borrows a live Host Runtime and its complete arena; it owns neither.
 * It owns and writes exactly the leading memory_required() bytes of its arena;
 * any extra capacity is neither read nor written. That owned prefix, the
 * complete Host Runtime arena, config and out_handle storage are mutually disjoint.
 * Every failure leaves both arenas and out_handle byte-exact. The integration
 * continues to own Host Runtime pump/readiness and every socket.
 */
MeisoCompositionRemoteEdgeInitStatus
meiso_composition_remote_edge_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionRemoteEdgeConfig *config,
    MeisoEdgeHandle **out_handle
);

#ifdef __cplusplus
}
#endif

#endif
