#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_wifi_runtime_edge_private.h"

/*
 * Thin process entry point. Configuration, bootstrap, Runtime truth,
 * telemetry production, transport pumping, and cleanup live in target-local
 * modules in the same executable source graph.
 */

int main(int argc, char **argv)
{
    return meiso_k230d_wifi_runtime_edge_private_run(argc, argv);
}
