/* Isolated packed-store latency under concurrent GPU uploads. No NBD device or
 * swap changes: owns 16 MiB of VRAM, two streams, and private codec/staging state.
 * Override NBD_VRAM_SOURCE at compile time to compare revisions with this driver.
 */
#define _GNU_SOURCE
#ifndef NBD_VRAM_SOURCE
#define NBD_VRAM_SOURCE "../nbd-vram.c"
#endif
#define STATUS_PATH "bench.status"
#define main daemon_main
#include NBD_VRAM_SOURCE
#undef main

static pthread_barrier_t bench_start;
static int bench_stop;
static uint64_t bench_write_offset, bench_writes;

static void require(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "bench-contention: %s failed\n", what);
        exit(1);
    }
}

static void require_cuda(CUresult result, const char *what)
{
    if (result != CUDA_SUCCESS) {
        fprintf(stderr, "bench-contention: %s: %s (%d)\n", what, cuda_err(result), result);
        exit(1);
    }
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    require(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "clock_gettime");
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

static void fill_page(char *page, unsigned seed)
{
    for (unsigned i = 0; i < COMP_PAGE; i++) page[i] = (char)(i % 67 + seed);
}

static CUstream setup_worker(int idx)
{
    CUstream stream;
    require_cuda(_cuCtxSetCurrent(g_cu_ctx), "set context");
    require_cuda(_cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "create stream");
    require_cuda(_cuMemAllocHost((void **)&t_cstage, compression_stage_size()), "pin staging");
    t_zstd_cctx = g_zstd_cctx[idx];
    t_zstd_dctx = g_zstd_dctx[idx];
    return stream;
}

static void finish_worker(CUstream stream)
{
    require_cuda(_cuStreamDestroy(stream), "destroy stream");
    require_cuda(_cuMemFreeHost(t_cstage), "free staging");
    t_cstage = NULL;
}

static void *background_writes(void *unused)
{
    (void)unused;
    CUstream stream = setup_worker(1);
    char data[64 * 1024];
    for (unsigned off = 0; off < sizeof(data); off += COMP_PAGE)
        fill_page(data + off, 21 + off / COMP_PAGE);
    require(comp_write(bench_write_offset, data, sizeof(data), stream) == 0, "warmup write");
    pthread_barrier_wait(&bench_start);
    while (!__atomic_load_n(&bench_stop, __ATOMIC_RELAXED)) {
        require(comp_write(bench_write_offset, data, sizeof(data), stream) == 0, "write");
        bench_writes++;
    }
    finish_worker(stream);
    return NULL;
}

static int compare_ns(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    const char *codec = argc > 1 ? argv[1] : "lz4";
    const char *mode = argc > 3 ? argv[3] : "alias";
    char *end = NULL;
    long requested = argc > 2 ? strtol(argv[2], &end, 10) : 10000;
    if (argc > 4 || requested < 1000 || requested > 1000000 || (end && *end) ||
        (strcmp(mode, "alias") && strcmp(mode, "disjoint")) ||
        parse_compression(codec, &g_compress, &g_compress_level) || !g_compress) {
        fprintf(stderr, "Usage: %s [lz4|zstd:LEVEL] [1000..1000000 reads] [alias|disjoint]\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(30); /* bound a hung benchmark; all storage belongs to this process */
    char tmp[] = "/tmp/nbd-vram-contention-XXXXXX";
    require(mkdtemp(tmp) && chdir(tmp) == 0, "temporary directory");
    uint64_t *samples = calloc((size_t)requested, sizeof(*samples));
    require(samples != NULL, "sample buffer");
    require(load_libcuda() == 0, "load CUDA");
    require_cuda(_cuInit(0), "initialize CUDA");
    CUdevice device;
    require_cuda(_cuDeviceGet(&device, 0), "get GPU 0");
    require_cuda(_cuCtxCreate(&g_cu_ctx, CU_CTX_SCHED_BLOCKING_SYNC, device), "create context");
    g_vram_size = 16 * 1024 * 1024;
    g_export_size = (uint64_t)(2 * NPLOCK + COMP_BATCH) * COMP_PAGE;
    g_nbd_threads = 2;
    require_cuda(_cuMemAlloc(&g_vram_ptr, g_vram_size), "allocate 16 MiB VRAM");
    require(compress_init() == 0, "initialize packed store");
    CUstream stream = setup_worker(0);
    char expected[COMP_PAGE], data[COMP_PAGE];
    fill_page(expected, 17);
    require(comp_write(0, expected, COMP_PAGE, stream) == 0, "initialize reader page");
    for (int i = 0; i < 200; i++)
        require(comp_read(0, data, COMP_PAGE, stream) == 0, "warmup read");
    /* The alias case intentionally stresses false sharing of page locks.
     * Disjoint stripes provide a control with the same device/codec workload. */
    bench_write_offset = (uint64_t)(NPLOCK + (!strcmp(mode, "disjoint") ? NPLOCK / 2 : 0)) * COMP_PAGE;
    require(pthread_barrier_init(&bench_start, NULL, 2) == 0, "start barrier");
    pthread_t writer;
    require(pthread_create(&writer, NULL, background_writes, NULL) == 0, "writer thread");
    pthread_barrier_wait(&bench_start);
    uint64_t begin = now_ns(), deadline = begin + UINT64_C(10000000000);
    size_t count = 0, over1 = 0, over5 = 0, over10 = 0;
    do {
        uint64_t start = now_ns();
        require(comp_read(0, data, COMP_PAGE, stream) == 0, "read");
        uint64_t elapsed = now_ns() - start;
        require(!memcmp(data, expected, COMP_PAGE), "read verification");
        samples[count++] = elapsed;
        over1 += elapsed > 1000000; over5 += elapsed > 5000000; over10 += elapsed > 10000000;
    } while (count < (size_t)requested && now_ns() < deadline);
    __atomic_store_n(&bench_stop, 1, __ATOMIC_RELAXED);
    require(pthread_join(writer, NULL) == 0, "writer join");
    double seconds = (double)(now_ns() - begin) / 1e9;
    qsort(samples, count, sizeof(*samples), compare_ns);
    printf("codec=%s mode=%s dedup=off reads=%zu seconds=%.3f write_batches=%llu\n",
           codec, mode, count, seconds, (unsigned long long)bench_writes);
    printf("read latency us: p50=%.2f p99=%.2f p99.9=%.2f max=%.2f\n",
           samples[(count - 1) / 2] / 1e3, samples[(count * 99 + 99) / 100 - 1] / 1e3,
           samples[(count * 999 + 999) / 1000 - 1] / 1e3, samples[count - 1] / 1e3);
    printf("reads over 1/5/10 ms: %zu/%zu/%zu; read=%.1f MiB/s write=%.1f MiB/s\n",
           over1, over5, over10, count * (COMP_PAGE / 1048576.0) / seconds,
           bench_writes * (64 * 1024 / 1048576.0) / seconds);
    printf("Storage-path benchmark with synthetic pages; excludes NBD, reclaim, and desktop frame timing.\n");
    finish_worker(stream);
    compress_shutdown();
    require_cuda(_cuMemFree(g_vram_ptr), "free VRAM");
    require_cuda(_cuCtxDestroy(g_cu_ctx), "destroy context");
    dlclose(g_libcuda);
    pthread_barrier_destroy(&bench_start);
    free(samples);
    require(chdir("/") == 0 && rmdir(tmp) == 0, "remove temporary directory");
    return 0;
}
