#include <meiso/runtime.h>

int main(void)
{
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 1U};

    if (!meiso_owner_scope_valid(owner)) {
        return 1;
    }
    return meiso_runtime_alignment_required() > 0U ? 0 : 2;
}
