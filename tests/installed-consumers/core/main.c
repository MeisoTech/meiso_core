#include <meiso/core.h>

int main(void)
{
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_HOST, 1U};

    if (MEISO_VERSION_MAJOR != 0 || MEISO_VERSION_MINOR != 1 ||
        MEISO_VERSION_PATCH != 0) {
        return 1;
    }
    return meiso_owner_scope_valid(owner) ? 0 : 2;
}
