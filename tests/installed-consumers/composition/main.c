#include <meiso/composition/coordinator.h>

int main(void)
{
    return meiso_composition_coordinator_alignment_required() > 0U ? 0 : 1;
}
