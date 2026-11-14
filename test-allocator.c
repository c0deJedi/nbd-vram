/* Allocator capacity/churn and cleanup after failed startup, without CUDA. */
#include "test-metadata.h"
#include <sys/wait.h>

static void check_init_failures(void)
{
    /* PTEs, dedup buckets, dedup PTEs, dedup objects, slab records. Each child
     * starts with fresh globals and fails one more allocation into startup. */
    for (int fail = 0; fail < 5; fail++) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) {
            g_dedup = 1;
            metadata_fail_after = fail;
            assert(compress_init() != 0);
            compress_shutdown();
            assert(!g_ptes && !g_slabs && !g_dedup_index && !g_dedup_ptes);
            assert(!g_dedup_objects && !g_dedup_free && !g_dedup_capacity);
            assert(metadata_live_allocations == 0);
            _exit(0);
        }
        int status;
        assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

static void check_fallback(void)
{
    uint8_t raw = (uint8_t)class_for(COMP_PAGE), small = (uint8_t)class_for(32);
    uint64_t raw_off = pool_alloc(&raw);
    assert(raw_off != UINT64_MAX);
    enum { SMALL_SLOTS = 3 * SLAB_SZ / 32 };
    uint64_t offsets[SMALL_SLOTS];
    for (unsigned i = 0; i < SMALL_SLOTS; i++) {
        uint8_t klass = small;
        offsets[i] = pool_alloc(&klass);
        assert(offsets[i] != UINT64_MAX && klass == small);
    }
    assert(g_nfree_chunks == 0);
    uint8_t borrowed = (uint8_t)class_for(128);
    uint64_t off = pool_alloc(&borrowed);
    assert(off != UINT64_MAX && borrowed == raw);
    assert(g_vram_obj_bytes == (uint64_t)SMALL_SLOTS * 32 + 2 * COMP_PAGE);
    pool_free(off, borrowed);

    /* Fill the raw slab. A hole in a smaller class cannot fit a raw page. */
    uint64_t raw_slots[SLAB_SZ / COMP_PAGE];
    raw_slots[0] = raw_off;
    for (unsigned i = 1; i < SLAB_SZ / COMP_PAGE; i++) {
        uint8_t klass = raw;
        raw_slots[i] = pool_alloc(&klass);
        assert(raw_slots[i] != UINT64_MAX && klass == raw);
    }
    pool_free(offsets[0], small);
    uint8_t klass = raw;
    assert(pool_alloc(&klass) == UINT64_MAX);
    for (unsigned i = 1; i < SMALL_SLOTS; i++) pool_free(offsets[i], small);
    for (unsigned i = 0; i < SLAB_SZ / COMP_PAGE; i++) pool_free(raw_slots[i], raw);
    assert(g_nfree_chunks == g_nchunks && g_vram_obj_bytes == 0);
}

static void check_churn(void)
{
    struct { uint64_t off; uint8_t klass; } live[512];
    unsigned char occupied[4 * SLAB_SZ] = {0};
    uint32_t rng = 1729;
    uint64_t bytes = 0;
    for (unsigned i = 0; i < 512; i++) live[i].off = UINT64_MAX;
    for (int pass = 0; pass < 10000; pass++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        unsigned idx = rng % 512;
        if (live[idx].off != UINT64_MAX) {
            uint64_t off = live[idx].off;
            unsigned size = k_class_sz[live[idx].klass];
            for (unsigned j = 0; j < size; j++) assert(occupied[off + j] == 1);
            memset(occupied + off, 0, size);
            bytes -= size;
            pool_free(off, live[idx].klass);
            live[idx].off = UINT64_MAX;
        } else {
            uint8_t wanted = (uint8_t)((rng >> 16) % NCLASS), actual = wanted;
            uint64_t off = pool_alloc(&actual);
            if (off == UINT64_MAX) {
                assert(g_nfree_chunks == 0);
                for (uint32_t i = 0; i < g_nchunks; i++)
                    assert(!g_slabs[i].nfree || g_slabs[i].klass < wanted);
                continue;
            }
            unsigned size = k_class_sz[actual];
            assert(actual >= wanted && off + size <= g_vram_size);
            for (unsigned j = 0; j < size; j++) assert(!occupied[off + j]);
            memset(occupied + off, 1, size);
            live[idx].off = off; live[idx].klass = actual;
            bytes += size;
        }
        assert(g_vram_obj_bytes == bytes);
    }
    for (unsigned i = 0; i < 512; i++)
        if (live[i].off != UINT64_MAX) pool_free(live[i].off, live[i].klass);
    assert(g_nfree_chunks == g_nchunks && g_vram_obj_bytes == 0);

    /* Completely empty slabs must be reusable by every size class. */
    for (int k = 0; k < NCLASS; k++) {
        uint8_t klass = (uint8_t)k;
        uint64_t off = pool_alloc(&klass);
        assert(off != UINT64_MAX && klass == k);
        pool_free(off, klass);
        assert(g_nfree_chunks == g_nchunks && g_vram_obj_bytes == 0);
    }
}

int main(void)
{
    char tmp[] = "/tmp/nbd-vram-allocator-test-XXXXXX";
    assert(mkdtemp(tmp) && chdir(tmp) == 0);
    g_vram_size = 4 * SLAB_SZ;
    g_export_size = 2 * g_vram_size;
    check_init_failures();
    assert(compress_init() == 0);
    assert(!g_dedup_objects && !g_dedup_capacity);
    metadata_heap_forbidden = 1;
    check_fallback();
    check_churn();
    metadata_heap_forbidden = 0;
    compress_shutdown();
    assert(metadata_live_allocations == 0);
    assert(chdir("/") == 0 && rmdir(tmp) == 0);
    puts("PASS allocator: larger slots, class reuse, churn, no metadata heap calls, startup failure cleanup");
    return 0;
}
