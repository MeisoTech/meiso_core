#ifndef MEISO_INTERNAL_H
#define MEISO_INTERNAL_H

#include <stdint.h>

static inline void meiso_increment_saturated(uint64_t *value)
{
    if (*value != UINT64_MAX) {
        *value += 1U;
    }
}

static inline void meiso_add_saturated(uint64_t *value, uint64_t amount)
{
    if (amount > UINT64_MAX - *value) {
        *value = UINT64_MAX;
    } else {
        *value += amount;
    }
}

#endif
