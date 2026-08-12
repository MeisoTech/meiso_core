#ifndef MEISO_INTERNAL_H
#define MEISO_INTERNAL_H

#include <stdint.h>

static inline void meiso_increment_saturated(uint64_t *value)
{
    if (*value != UINT64_MAX) {
        *value += 1U;
    }
}

#endif
