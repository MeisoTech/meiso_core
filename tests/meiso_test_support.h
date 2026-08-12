#ifndef MEISO_TEST_SUPPORT_H
#define MEISO_TEST_SUPPORT_H

/*
 * Host tests intentionally keep automatic storage so sanitizers exercise the
 * normal stack-based fixtures.  Constrained targets may define
 * MEISO_TEST_TARGET_STATIC_STORAGE to move the large, single-run fixture
 * arenas into static storage without maintaining a second set of vectors.
 */
#if defined(MEISO_TEST_TARGET_STATIC_STORAGE)
#define MEISO_TEST_ARENA_STORAGE static
#else
#define MEISO_TEST_ARENA_STORAGE
#endif

#endif
