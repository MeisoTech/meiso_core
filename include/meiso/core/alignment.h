#ifndef MEISO_CORE_ALIGNMENT_H
#define MEISO_CORE_ALIGNMENT_H

#include <stddef.h>

/* MSVC's C11 headers omit max_align_t; its C++ library defines it as double.
 * Keep the fallback in Meiso's namespace, and use the native type elsewhere.
 * This is fundamental alignment, not an over-aligned/SIMD allocation promise. */
#if defined(_MSC_VER) && !defined(__clang__)
typedef double MeisoMaxAlign;
#else
typedef max_align_t MeisoMaxAlign;
#endif

#endif
