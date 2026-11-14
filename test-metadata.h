/* Intercept the daemon's heap calls, not the test fixtures or codec libraries.
 * Storage tests forbid metadata allocation/free after startup. A thread-local
 * guard also covers concurrent workers without racing with fixture setup. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

static __thread int metadata_heap_forbidden;
static int metadata_fail_after = -1;
static size_t metadata_live_allocations;

static void *metadata_test_malloc(size_t bytes) __attribute__((unused));
static void *metadata_test_malloc(size_t bytes)
{
    assert(!metadata_heap_forbidden);
    return malloc(bytes);
}

static void *metadata_test_calloc(size_t count, size_t size)
{
    assert(!metadata_heap_forbidden);
    if (metadata_fail_after == 0) { errno = ENOMEM; return NULL; }
    if (metadata_fail_after > 0) metadata_fail_after--;
    void *p = calloc(count, size);
    if (p) metadata_live_allocations++;
    return p;
}

static void *metadata_test_realloc(void *p, size_t size) __attribute__((unused));
static void *metadata_test_realloc(void *p, size_t size)
{
    assert(!metadata_heap_forbidden);
    return realloc(p, size);
}

static void metadata_test_free(void *p)
{
    assert(!metadata_heap_forbidden);
    /* Tests invoke daemon cleanup only for the calloc-backed metadata pools. */
    if (p) {
        assert(metadata_live_allocations > 0);
        metadata_live_allocations--;
    }
    free(p);
}

#define malloc metadata_test_malloc
#define calloc metadata_test_calloc
#define realloc metadata_test_realloc
#define free metadata_test_free
#define main daemon_main
#define STATUS_PATH "nbd-vram.status"
#include "nbd-vram.c"
#undef main
#undef malloc
#undef calloc
#undef realloc
#undef free
