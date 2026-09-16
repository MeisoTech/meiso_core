#include "meiso_composition_cpu_raster_renderer_private.h"

#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Raster codec owns endian and floating-point conversion. */

uint32_t
meiso_composition_cpu_raster_renderer_private_read_be_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

void meiso_composition_cpu_raster_renderer_private_write_be_u32(uint8_t *output,
                                                                uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}
