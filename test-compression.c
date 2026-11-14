/* CPU-only storage regression tests: real codecs, host memory in place of CUDA. */
#include "test-metadata.h"

static CUresult copy_to_device(CUdeviceptr dst, const void *src, size_t n, CUstream stream)
{
    (void)stream;
    assert(dst >= g_vram_ptr && dst - g_vram_ptr + n <= g_vram_size);
    memcpy((void *)(uintptr_t)dst, src, n);
    return CUDA_SUCCESS;
}

static CUresult copy_from_device(void *dst, CUdeviceptr src, size_t n, CUstream stream)
{
    (void)stream;
    assert(src >= g_vram_ptr && src - g_vram_ptr + n <= g_vram_size);
    memcpy(dst, (void *)(uintptr_t)src, n);
    return CUDA_SUCCESS;
}

static CUresult synchronize(CUstream stream)
{
    (void)stream;
    return CUDA_SUCCESS;
}

static void init_worker(int idx)
{
    t_cstage = calloc(COMP_BATCH, COMP_PAGE);
    assert(t_cstage);
    t_zstd_cctx = g_zstd_cctx[idx];
    t_zstd_dctx = g_zstd_dctx[idx];
}

static void check_parser(void)
{
    int codec, level;
    assert(parse_compression(NULL, &codec, &level) == 0 && codec == COMP_OFF);
    assert(parse_compression("off", &codec, &level) == 0 && codec == COMP_OFF);
    assert(parse_compression("0", &codec, &level) == 0 && codec == COMP_OFF);
    assert(parse_compression("1", &codec, &level) == 0 && codec == COMP_LZ4);
    assert(parse_compression("lz4", &codec, &level) == 0 && codec == COMP_LZ4);
    assert(parse_compression("zstd", &codec, &level) == 0 && codec == COMP_ZSTD && level == 3);
    for (int i = 1; i <= 22; i++) {
        char value[16];
        snprintf(value, sizeof(value), "zstd:%d", i);
        assert(parse_compression(value, &codec, &level) == 0 && codec == COMP_ZSTD && level == i);
    }
    const char *bad[] = {"", "2", "lz4:3", "zram:3", "zstd:", "zstd:0", "zstd:23",
                         "zstd:-1", "zstd:+3", "zstd:03", "zstd:3x", "zstd:3.0",
                         " zstd:3", "zstd:3 ", "zstd:999999999999999999999999999"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(parse_compression(bad[i], &codec, &level) != 0);
}

static void *concurrent_roundtrip(void *arg)
{
    int idx = (int)(intptr_t)arg;
    init_worker(idx);
    metadata_heap_forbidden = 1;
    char in[COMP_PAGE], out[COMP_PAGE];
    uint64_t offset = (uint64_t)idx * COMP_PAGE;
    for (int pass = 0; pass < 32; pass++) {
        for (int i = 0; i < COMP_PAGE; i++) in[i] = (char)(i % 67 + pass + idx);
        assert(comp_write(offset, in, sizeof(in), NULL) == 0);
        assert(comp_read(offset, out, sizeof(out), NULL) == 0);
        assert(memcmp(in, out, sizeof(in)) == 0);
    }
    assert(comp_trim(offset, sizeof(in), NULL) == 0);
    free(t_cstage);
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    check_parser();
    assert(parse_compression(argv[1], &g_compress, &g_compress_level) == 0 && g_compress);
    char tmp[] = "/tmp/nbd-vram-test-XXXXXX";
    assert(mkdtemp(tmp) && chdir(tmp) == 0);
    g_nbd_threads = 4;
    g_vram_size = 4 * SLAB_SZ;
    g_export_size = g_vram_size * 21 / 10; /* fractional ratio, partial final page */
    g_vram_ptr = (CUdeviceptr)(uintptr_t)calloc(1, g_vram_size);
    assert(g_vram_ptr);
    _cuMemcpyHtoDAsync = copy_to_device;
    _cuMemcpyDtoHAsync = copy_from_device;
    _cuStreamSynchronize = synchronize;
    assert(compress_init() == 0);
    assert(!g_dedup_index && !g_dedup_ptes);
    assert(g_export_size == g_npages * COMP_PAGE);
    assert(oob(g_export_size, 1));
    assert(g_compress == COMP_ZSTD ? !g_liblz4 : !g_libzstd);
    init_worker(0);
    metadata_heap_forbidden = 1;
    char last[COMP_PAGE];
    memset(last, 0xab, sizeof(last));
    assert(comp_write(g_export_size - COMP_PAGE, last, sizeof(last), NULL) == 0);
    assert(comp_read(g_export_size - COMP_PAGE, last, sizeof(last), NULL) == 0);
    for (size_t i = 0; i < sizeof(last); i++) assert((unsigned char)last[i] == 0xab);
    assert(comp_trim(g_export_size - COMP_PAGE, COMP_PAGE, NULL) == 0);

    /* Span multiple internal batches with compressed, raw, and same-filled pages. */
    enum { N = 40, LEN = N * COMP_PAGE };
    char in[LEN], out[LEN];
    uint32_t rng = 0x12345678;
    for (int pg = 0; pg < N; pg++) {
        for (int i = 0; i < COMP_PAGE; i++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            in[pg * COMP_PAGE + i] = pg % 3 == 0 ? (char)(i % 73) :
                                   pg % 3 == 1 ? (char)rng : (char)0xa5;
        }
    }
    assert(comp_write(0, in, LEN, NULL) == 0);
    assert(g_pages_compressed == 14 && g_pages_raw == 13 && g_pages_same == 13);
    assert(comp_read(0, out, LEN, NULL) == 0 && memcmp(in, out, LEN) == 0);
    assert(comp_read(17, out, LEN - 34, NULL) == 0 && memcmp(in + 17, out, LEN - 34) == 0);

    compress_status_write();
    FILE *status = fopen(STATUS_PATH, "r");
    assert(status);
    char status_buf[1024] = {0};
    assert(fread(status_buf, 1, sizeof(status_buf) - 1, status) > 0);
    fclose(status);
    assert(strstr(status_buf, g_compress == COMP_ZSTD ? "algorithm=zstd\n" : "algorithm=lz4\n"));
    assert(strstr(status_buf, g_compress == COMP_ZSTD ? "pages_zstd=14\n" : "pages_lz4=14\n"));
    char level_line[64];
    snprintf(level_line, sizeof(level_line), "compression_level=%d\n", g_compress == COMP_ZSTD ? g_compress_level : 0);
    assert(strstr(status_buf, level_line));

    /* Partial writes and trims must preserve bytes around the changed range. */
    char patch[COMP_PAGE + 79];
    memset(patch, 0x5a, sizeof(patch));
    assert(comp_write(COMP_PAGE - 31, patch, sizeof(patch), NULL) == 0);
    memcpy(in + COMP_PAGE - 31, patch, sizeof(patch));
    assert(comp_trim(23, 81, NULL) == 0);
    memset(in + 23, 0, 81);
    assert(comp_read(0, out, LEN, NULL) == 0 && memcmp(in, out, LEN) == 0);

    /* Reject corrupt compressed data on both aligned and partial reads. */
    uint16_t saved_len = g_ptes[0].clen;
    assert(g_ptes[0].kind == PTE_COMPRESSED);
    g_ptes[0].clen = 1;
    assert(comp_read(0, out, COMP_PAGE, NULL) == EIO);
    assert(comp_read(1, out, 100, NULL) == EIO);
    g_ptes[0].clen = saved_len;

    assert(comp_trim(0, LEN, NULL) == 0);
    assert(g_nfree_chunks == g_nchunks && g_vram_obj_bytes == 0);
    assert(g_pages_compressed == 0 && g_pages_raw == 0 && g_pages_same == 0);
    assert(comp_read(0, out, LEN, NULL) == 0);
    for (int i = 0; i < LEN; i++) assert(out[i] == 0);

    /* A full pool must preserve the old page on failed overwrite, then recover
     * once TRIM releases space. Use an incompressible page from the fixture. */
    char *raw = in + 4 * COMP_PAGE;
    for (uint64_t off = 0; off < g_vram_size; off += COMP_PAGE)
        assert(comp_write(off, raw, COMP_PAGE, NULL) == 0);
    assert(g_nfree_chunks == 0);
    assert(comp_write(0, raw, COMP_PAGE, NULL) == ENOSPC);
    assert(comp_read(0, out, COMP_PAGE, NULL) == 0 && memcmp(raw, out, COMP_PAGE) == 0);
    assert(comp_trim(COMP_PAGE, COMP_PAGE, NULL) == 0);

    /* No unused slabs remain, but a raw slot can hold a compressed page. A
     * two-page batch must roll back its borrowed slot if the second won't fit. */
    char packed[2 * COMP_PAGE];
    for (int i = 0; i < (int)sizeof(packed); i++) packed[i] = (char)(i % 71);
    uint64_t used = g_vram_obj_bytes;
    assert(comp_write(g_vram_size, packed, sizeof(packed), NULL) == ENOSPC);
    assert(g_vram_obj_bytes == used && g_ptes[g_vram_size / COMP_PAGE].kind == PTE_NONE);
    assert(comp_write(g_vram_size, packed, COMP_PAGE, NULL) == 0);
    struct pte borrowed = g_ptes[g_vram_size / COMP_PAGE];
    assert(borrowed.kind == PTE_COMPRESSED && borrowed.clen < COMP_PAGE);
    assert(borrowed.klass == class_for(COMP_PAGE));
    assert(g_vram_obj_bytes == used + COMP_PAGE);
    assert(comp_read(g_vram_size, out, COMP_PAGE, NULL) == 0 && memcmp(packed, out, COMP_PAGE) == 0);
    assert(comp_trim(g_vram_size, COMP_PAGE, NULL) == 0);
    assert(g_vram_obj_bytes == used);
    assert(comp_write(g_vram_size, raw, COMP_PAGE, NULL) == 0);
    assert(comp_trim(0, g_export_size, NULL) == 0);

    pthread_t threads[4];
    for (int i = 0; i < 4; i++)
        assert(pthread_create(&threads[i], NULL, concurrent_roundtrip, (void *)(intptr_t)i) == 0);
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(g_nfree_chunks == g_nchunks && g_pages_compressed == 0);

    free(t_cstage);
    metadata_heap_forbidden = 0;
    compress_shutdown();
    assert(metadata_live_allocations == 0);
    free((void *)(uintptr_t)g_vram_ptr);
    assert(chdir("/") == 0 && rmdir(tmp) == 0);
    printf("PASS %s: parser, roundtrip, partial I/O, TRIM, corruption, ENOSPC, concurrency, status\n", argv[1]);
    return 0;
}
