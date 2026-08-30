#include <meiso/simulation/composition_edge.h>
#include <meiso/simulation/composition_trace.h>

int main(void)
{
    return meiso_composition_edge_simulator_alignment_required() > 0U &&
        meiso_composition_edge_trace_writer_alignment_required() > 0U ? 0 : 1;
}
