#include <meiso/host/remote_edge.h>

int main(void)
{
    return meiso_composition_remote_edge_alignment_required() > 0U ? 0 : 1;
}
