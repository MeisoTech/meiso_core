#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_composition_edge_service_private.h"

/*
 * Thin process entry point. CLI, lifecycle, run-loop, and shutdown ownership
 * are implemented by the target-local service modules in this source graph.
 */

int main(int argc, char **argv)
{
    return meiso_k230d_composition_edge_service_private_run(argc, argv);
}
