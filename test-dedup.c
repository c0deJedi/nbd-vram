/* Real codecs with CPU-backed CUDA copies, including injected I/O failures. */
#include "test-metadata.h"

static int fail_upload, fail_download, fail_sync;
static int upload_countdown;
static pthread_barrier_t barrier;

static CUresult copy_to_device(CUdeviceptr dst, const void *src, size_t n, CUstream stream)
{
    (void)stream;
    if (fail_upload || (upload_countdown && --upload_countdown == 0)) return 1;
    assert(dst >= g_vram_ptr && dst - g_vram_ptr + n <= g_vram_size);
    memcpy((void *)(uintptr_t)dst, src, n);
    return CUDA_SUCCESS;
}

static CUresult copy_from_device(void *dst, CUdeviceptr src, size_t n, CUstream stream)
{
    (void)stream;
    if (fail_download) return 1;
    assert(src >= g_vram_ptr && src - g_vram_ptr + n <= g_vram_size);
    memcpy(dst, (void *)(uintptr_t)src, n);
    return CUDA_SUCCESS;
}

static CUresult synchronize(CUstream stream)
{
    (void)stream;
    return fail_sync ? 1 : CUDA_SUCCESS;
}

static void init_worker(int idx)
{
    t_cstage = calloc(1, compression_stage_size());
    assert(t_cstage);
    t_zstd_cctx = g_zstd_cctx[idx];
    t_zstd_dctx = g_zstd_dctx[idx];
}

static void pattern(char *p, int seed, int raw)
{
    uint32_t rng = (uint32_t)seed + 1;
    for (int i = 0; i < COMP_PAGE; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        p[i] = raw ? (char)rng : (char)(i % 67 + seed);
    }
}

static void read_equals(uint64_t page, const char *expected)
{
    char actual[COMP_PAGE];
    assert(comp_read(page * COMP_PAGE, actual, sizeof(actual), NULL) == 0);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
}

/* Called with workers stopped or at a barrier. Check every reference rather
 * than only the counters; a dangling pointer or leaked object fails here. */
static void check_references(void)
{
    uint64_t refs = 0, unique = 0, saved = 0, object_bytes = 0, pages = 0;
    for (size_t bucket = 0; bucket < g_dedup_buckets; bucket++) {
        struct dedup_obj **prev = &g_dedup_index[bucket];
        for (struct dedup_obj *obj = *prev; obj; obj = obj->next) {
            assert(obj->prev == prev && *prev == obj && obj->refs > 0);
            assert((obj->hash & (g_dedup_buckets - 1)) == bucket);
            uint64_t found = 0;
            for (uint64_t pg = 0; pg < g_npages; pg++) {
                if (g_dedup_ptes[pg] != obj) continue;
                found++;
                assert(g_ptes[pg].vram_off == obj->vram_off);
                assert(g_ptes[pg].kind == obj->kind && g_ptes[pg].clen == obj->clen);
            }
            assert(found == obj->refs);
            refs += found;
            unique++;
            saved += (found - 1) * k_class_sz[obj->klass];
            object_bytes += k_class_sz[obj->klass];
            prev = &obj->next;
        }
    }
    for (uint64_t pg = 0; pg < g_npages; pg++) {
        if (g_ptes[pg].kind == PTE_RAW || g_ptes[pg].kind == PTE_COMPRESSED) {
            assert(g_dedup_ptes[pg]);
            pages++;
        } else assert(!g_dedup_ptes[pg]);
    }
    assert(refs == g_dedup_refs && unique == g_dedup_unique && refs == pages);
    assert(saved == g_dedup_saved_bytes && object_bytes == g_vram_obj_bytes);
    assert(pages == g_pages_compressed + g_pages_raw);
    size_t free_objects = 0;
    for (struct dedup_obj *obj = g_dedup_free; obj; obj = obj->next) {
        assert(obj >= g_dedup_objects && obj < g_dedup_objects + g_dedup_capacity);
        assert(obj->refs == 0 && !obj->prev);
        assert(++free_objects <= g_dedup_capacity); /* also catches list cycles */
    }
    assert(free_objects + unique == g_dedup_capacity);
}

static void clear_store(void)
{
    assert(comp_trim(0, (uint32_t)g_export_size, NULL) == 0);
    check_references();
    assert(g_dedup_refs == 0 && g_dedup_unique == 0 && g_dedup_saved_bytes == 0);
    assert(g_nfree_chunks == g_nchunks && g_vram_obj_bytes == 0);
}

static void check_sharing(void)
{
    char input[40 * COMP_PAGE], a[COMP_PAGE], b[COMP_PAGE], same[COMP_PAGE];
    for (int raw = 0; raw <= 1; raw++) {
        pattern(a, 1, raw);
        pattern(b, 2, raw);
        memset(same, 0x5a, sizeof(same));
        for (int i = 0; i < 40; i++) memcpy(input + i * COMP_PAGE, a, COMP_PAGE);
        uint64_t hits = g_dedup_hits;
        assert(comp_write(0, input, sizeof(input), NULL) == 0);
        assert(g_dedup_unique == 1 && g_dedup_refs == 40 && g_dedup_hits == hits + 39);
        assert(g_dedup_ptes[0] == g_dedup_ptes[39]);
        assert(g_dedup_saved_bytes == 39u * k_class_sz[g_ptes[0].klass]);
        check_references();
        for (int i = 0; i < 40; i++) read_equals(i, a);

        compress_status_write();
        FILE *f = fopen(STATUS_PATH, "r");
        char status[2048] = {0};
        assert(f && fread(status, 1, sizeof(status) - 1, f) > 0);
        fclose(f);
        assert(strstr(status, "dedup=1\n") && strstr(status, "dedup_pages=39\n"));
        assert(strstr(status, "dedup_unique_pages=1\n"));
        char line[100];
        snprintf(line, sizeof(line), "dedup_saved_bytes=%llu\n", (unsigned long long)g_dedup_saved_bytes);
        assert(strstr(status, line));
        uint64_t reserved = g_dedup_buckets * sizeof(*g_dedup_index) +
                            g_npages * sizeof(*g_dedup_ptes) +
                            g_dedup_capacity * sizeof(*g_dedup_objects);
        snprintf(line, sizeof(line), "dedup_index_bytes=%llu\n", (unsigned long long)reserved);
        assert(strstr(status, line));

        /* Self-overwrite must neither free the object nor inflate live savings. */
        assert(comp_write(0, a, COMP_PAGE, NULL) == 0);
        assert(g_dedup_unique == 1 && g_dedup_refs == 40);
        assert(comp_write(COMP_PAGE, b, COMP_PAGE, NULL) == 0);
        read_equals(0, a); read_equals(1, b); read_equals(39, a);
        assert(g_dedup_unique == 2 && g_dedup_refs == 40);

        /* Partial overwrite and partial discard both split a shared reference. */
        assert(comp_write(2 * COMP_PAGE + 37, b + 37, 200, NULL) == 0);
        char partial[COMP_PAGE];
        memcpy(partial, a, COMP_PAGE); memcpy(partial + 37, b + 37, 200);
        read_equals(2, partial); read_equals(3, a);
        assert(comp_trim(3 * COMP_PAGE + 17, 81, NULL) == 0);
        memcpy(partial, a, COMP_PAGE); memset(partial + 17, 0, 81);
        read_equals(3, partial); read_equals(4, a);

        assert(comp_write(4 * COMP_PAGE, same, COMP_PAGE, NULL) == 0);
        assert(!g_dedup_ptes[4] && g_dedup_refs == 39);
        read_equals(4, same);
        assert(comp_trim(0, COMP_PAGE, NULL) == 0); /* remove the original owner */
        read_equals(39, a);
        check_references();
        clear_store();
    }
}

static void check_collision(void)
{
    char a[COMP_PAGE], b[COMP_PAGE];
    pattern(a, 10, 0); pattern(b, 20, 0);
    assert(comp_write(0, a, COMP_PAGE, NULL) == 0);
    /* Force a hash collision by moving A into B's hash chain. Matching the
     * hash must still read/compare all bytes before it can share the object. */
    struct dedup_obj *obj = g_dedup_ptes[0];
    *obj->prev = obj->next;
    if (obj->next) obj->next->prev = obj->prev;
    obj->hash = page_hash(b);
    size_t bucket = obj->hash & (g_dedup_buckets - 1);
    obj->next = g_dedup_index[bucket];
    obj->prev = &g_dedup_index[bucket];
    if (obj->next) obj->next->prev = &obj->next;
    g_dedup_index[bucket] = obj;
    assert(comp_write(COMP_PAGE, b, COMP_PAGE, NULL) == 0);
    assert(g_dedup_ptes[0] != g_dedup_ptes[1]);
    assert(comp_write(2 * COMP_PAGE, b, COMP_PAGE, NULL) == 0);
    assert(g_dedup_ptes[1] == g_dedup_ptes[2]);
    read_equals(0, a); read_equals(1, b); read_equals(2, b);
    check_references();
    clear_store();
}

static void check_failures(void)
{
    char a[COMP_PAGE], b[COMP_PAGE], input[3 * COMP_PAGE];
    pattern(a, 50, 0); pattern(b, 60, 0);
    assert(comp_write(0, a, COMP_PAGE, NULL) == 0);
    assert(comp_write(COMP_PAGE, a, COMP_PAGE, NULL) == 0);
    uint64_t hits = g_dedup_hits, saved = g_dedup_saved_bytes;
    struct dedup_obj *original = g_dedup_ptes[0];
    /* Exhaust the metadata pool independently of VRAM. The original mapping
     * survives, and restoring the free list makes later writes possible. */
    struct dedup_obj *free_objects = g_dedup_free;
    g_dedup_free = NULL;
    assert(comp_write(0, b, COMP_PAGE, NULL) == ENOSPC);
    g_dedup_free = free_objects;
    fail_download = 1;
    assert(comp_write(2 * COMP_PAGE, a, COMP_PAGE, NULL) == EIO);
    fail_download = 0;
    fail_sync = 1;
    assert(comp_write(2 * COMP_PAGE, a, COMP_PAGE, NULL) == EIO);
    assert(comp_write(0, b, COMP_PAGE, NULL) == EIO);
    fail_sync = 0;
    fail_upload = 1;
    assert(comp_write(0, b, COMP_PAGE, NULL) == EIO);
    fail_upload = 0;
    assert(g_dedup_ptes[0] == original && g_dedup_ptes[1] == original);
    assert(g_dedup_hits == hits && g_dedup_saved_bytes == saved);
    check_references();

    /* Roll back a reused reference AND a newly created object when a later
     * upload in the batch fails; do not publish any changed PTEs. */
    memcpy(input, a, COMP_PAGE); memcpy(input + COMP_PAGE, b, COMP_PAGE);
    pattern(input + 2 * COMP_PAGE, 70, 0);
    upload_countdown = 2;
    assert(comp_write(0, input, sizeof(input), NULL) == EIO);
    assert(upload_countdown == 0);
    assert(g_dedup_refs == 2 && g_dedup_unique == 1 && g_dedup_hits == hits);
    read_equals(0, a); read_equals(1, a);
    check_references();
    clear_store();
}

static void check_full_pool(void)
{
    char raw[COMP_PAGE], first[COMP_PAGE], input[2 * COMP_PAGE];
    uint64_t count = g_vram_size / COMP_PAGE;
    for (uint64_t pg = 0; pg < count; pg++) {
        pattern(raw, 1000 + (int)pg, 1);
        if (!pg) memcpy(first, raw, COMP_PAGE);
        assert(comp_write(pg * COMP_PAGE, raw, COMP_PAGE, NULL) == 0);
    }
    assert(g_nfree_chunks == 0 && g_dedup_unique == count);
    /* Duplicates still succeed at capacity, without a replacement allocation. */
    assert(comp_write(count * COMP_PAGE, first, COMP_PAGE, NULL) == 0);
    assert(comp_write(0, first, COMP_PAGE, NULL) == 0);
    pattern(raw, 9999, 1);
    assert(comp_write(0, raw, COMP_PAGE, NULL) == ENOSPC);
    read_equals(0, first); read_equals(count, first);
    memcpy(input, first, COMP_PAGE); memcpy(input + COMP_PAGE, raw, COMP_PAGE);
    uint64_t hits = g_dedup_hits;
    assert(comp_write((count + 1) * COMP_PAGE, input, sizeof(input), NULL) == ENOSPC);
    assert(!g_dedup_ptes[count + 1] && g_dedup_hits == hits);
    assert(comp_trim(0, COMP_PAGE, NULL) == 0);
    assert(g_nfree_chunks == 0); /* second reference retains the first payload */
    assert(comp_write(0, raw, COMP_PAGE, NULL) == ENOSPC);
    assert(comp_trim(count * COMP_PAGE, COMP_PAGE, NULL) == 0);
    assert(comp_write(0, raw, COMP_PAGE, NULL) == 0);
    check_references();
    clear_store();
}

static void check_random_operations(void)
{
    enum { PAGES = 32 };
    char expected[PAGES][COMP_PAGE] = {{0}}, page[COMP_PAGE];
    uint32_t rng = 871;
    for (int i = 0; i < 600; i++) {
        rng = rng * 1664525u + 1013904223u;
        unsigned pg = (rng >> 16) % PAGES, op = (rng >> 8) % 4;
        pattern(page, (int)(rng % 8), (int)(rng % 2));
        if (op == 0) {
            assert(comp_write(pg * COMP_PAGE, page, COMP_PAGE, NULL) == 0);
            memcpy(expected[pg], page, COMP_PAGE);
        } else if (op == 1) {
            assert(comp_trim(pg * COMP_PAGE, COMP_PAGE, NULL) == 0);
            memset(expected[pg], 0, COMP_PAGE);
        } else if (op == 2) {
            assert(comp_write(pg * COMP_PAGE + 100, page, 600, NULL) == 0);
            memcpy(expected[pg] + 100, page, 600);
        } else {
            assert(comp_trim(pg * COMP_PAGE + 200, 400, NULL) == 0);
            memset(expected[pg] + 200, 0, 400);
        }
        for (int p = 0; p < PAGES; p++) read_equals(p, expected[p]);
        check_references();
    }
    clear_store();
}

static void check_larger_slots(void)
{
    if (!g_compress) return;
    char raw[COMP_PAGE], small[COMP_PAGE], other[COMP_PAGE], input[2 * COMP_PAGE];
    uint64_t count = g_vram_size / COMP_PAGE;
    for (uint64_t pg = 0; pg < count; pg++) {
        pattern(raw, 1000 + (int)pg, 1);
        assert(comp_write(pg * COMP_PAGE, raw, COMP_PAGE, NULL) == 0);
    }
    assert(comp_trim(COMP_PAGE, 2 * COMP_PAGE, NULL) == 0);
    assert(g_nfree_chunks == 0);
    pattern(small, 7, 0); pattern(other, 8, 0);
    memcpy(input, small, COMP_PAGE); memcpy(input + COMP_PAGE, other, COMP_PAGE);
    uint64_t used = g_vram_obj_bytes, unique = g_dedup_unique;

    /* Both pending objects borrow raw slots. A later upload failure must
     * return those slots and their metadata to their respective free lists. */
    upload_countdown = 2;
    assert(comp_write(count * COMP_PAGE, input, sizeof(input), NULL) == EIO);
    assert(upload_countdown == 0 && g_vram_obj_bytes == used && g_dedup_unique == unique);
    assert(!g_dedup_ptes[count] && !g_dedup_ptes[count + 1]);
    check_references();
    assert(comp_write(count * COMP_PAGE, small, COMP_PAGE, NULL) == 0);
    assert(g_ptes[count].kind == PTE_COMPRESSED);
    assert(g_ptes[count].klass == class_for(COMP_PAGE));
    assert(g_vram_obj_bytes == used + COMP_PAGE);

    /* Sharing counts the borrowed slot's actual cost, and releasing one
     * reference keeps it alive until the final reference is removed. */
    assert(comp_write((count + 1) * COMP_PAGE, small, COMP_PAGE, NULL) == 0);
    assert(g_dedup_saved_bytes == COMP_PAGE);
    assert(comp_trim(count * COMP_PAGE, COMP_PAGE, NULL) == 0);
    read_equals(count + 1, small);
    assert(g_vram_obj_bytes == used + COMP_PAGE && g_dedup_saved_bytes == 0);
    assert(comp_trim((count + 1) * COMP_PAGE, COMP_PAGE, NULL) == 0);
    assert(g_vram_obj_bytes == used);
    check_references();
    clear_store();
}

static void check_metadata_headroom(void)
{
    if (!g_compress) return;
    char page[COMP_PAGE], input[COMP_BATCH * COMP_PAGE];
    /* Every logical page owns a distinct small payload. Replacing a full
     * batch must still have metadata available before old objects are freed. */
    for (uint64_t pg = 0; pg < g_npages; pg++) {
        pattern(page, 1, 0);
        memcpy(page, &pg, sizeof(pg));
        assert(comp_write(pg * COMP_PAGE, page, COMP_PAGE, NULL) == 0);
    }
    assert(g_dedup_unique == g_npages);
    for (uint64_t i = 0; i < COMP_BATCH; i++) {
        pattern(input + i * COMP_PAGE, 1, 0);
        uint64_t id = g_npages + i;
        memcpy(input + i * COMP_PAGE, &id, sizeof(id));
    }
    assert(comp_write(0, input, sizeof(input), NULL) == 0);
    assert(g_dedup_unique == g_npages);
    for (uint64_t i = 0; i < COMP_BATCH; i++) read_equals(i, input + i * COMP_PAGE);
    check_references();
    clear_store();
}

static void *concurrent_sharing(void *arg)
{
    int idx = (int)(intptr_t)arg;
    init_worker(idx);
    metadata_heap_forbidden = 1;
    char page[COMP_PAGE];
    for (int pass = 0; pass < 30; pass++) {
        pattern(page, pass, pass % 2);
        assert(comp_write((uint64_t)idx * COMP_PAGE, page, COMP_PAGE, NULL) == 0);
        read_equals((uint64_t)idx, page);
        pthread_barrier_wait(&barrier);
        if (!idx) {
            assert(g_dedup_unique == 1 && g_dedup_refs == 4);
            check_references();
        }
        pthread_barrier_wait(&barrier);
        if (idx % 2) {
            page[100] ^= 0x7f;
            assert(comp_write((uint64_t)idx * COMP_PAGE + 100, page + 100, 1, NULL) == 0);
        }
        read_equals((uint64_t)idx, page);
        assert(comp_trim((uint64_t)idx * COMP_PAGE, COMP_PAGE, NULL) == 0);
        pthread_barrier_wait(&barrier);
    }
    free(t_cstage);
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(parse_compression(argv[1], &g_compress, &g_compress_level) == 0);
    g_dedup = 1;
    assert(packed_store_enabled());
    char tmp[] = "/tmp/nbd-vram-dedup-test-XXXXXX";
    assert(mkdtemp(tmp) && chdir(tmp) == 0);
    g_nbd_threads = 4;
    g_vram_size = 16 * SLAB_SZ;
    g_export_size = 2 * g_vram_size;
    g_vram_ptr = (CUdeviceptr)(uintptr_t)calloc(1, g_vram_size);
    assert(g_vram_ptr);
    _cuMemcpyHtoDAsync = copy_to_device;
    _cuMemcpyDtoHAsync = copy_from_device;
    _cuStreamSynchronize = synchronize;
    assert(compress_init() == 0);
    if (g_compress == COMP_OFF) assert(!g_liblz4 && !g_libzstd);
    init_worker(0);
    metadata_heap_forbidden = 1;
    check_sharing();
    check_collision();
    check_failures();
    check_full_pool();
    check_larger_slots();
    check_metadata_headroom();
    check_random_operations();
    pthread_t threads[4];
    assert(pthread_barrier_init(&barrier, NULL, 4) == 0);
    for (int i = 0; i < 4; i++)
        assert(pthread_create(&threads[i], NULL, concurrent_sharing, (void *)(intptr_t)i) == 0);
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    pthread_barrier_destroy(&barrier);
    clear_store();
    /* Shutdown must also reclaim metadata for live shared objects. */
    char page[COMP_PAGE];
    pattern(page, 1, 0);
    assert(comp_write(0, page, COMP_PAGE, NULL) == 0);
    assert(comp_write(COMP_PAGE, page, COMP_PAGE, NULL) == 0);
    free(t_cstage);
    metadata_heap_forbidden = 0;
    compress_shutdown();
    assert(metadata_live_allocations == 0);
    free((void *)(uintptr_t)g_vram_ptr);
    assert(chdir("/") == 0 && rmdir(tmp) == 0);
    printf("PASS dedup %s: sharing, COW, TRIM, collisions, rollback, ENOSPC, random I/O, concurrency, stats\n", argv[1]);
    return 0;
}
