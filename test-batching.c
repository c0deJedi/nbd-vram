/* Real codecs and NBD sockets, with device copies deferred until synchronize. */
#include "test-metadata.h"

struct CUstream_st {
    struct { void *dst; const void *src; size_t bytes; } copies[BATCH_DEPTH_MAX];
    unsigned pending, syncs, uploads, downloads;
    size_t pending_bytes, max_pending_bytes;
    int fail_enqueue, fail_sync;
    int require_reply_before_upload, reply_fd;
    pthread_barrier_t *read_gate;
    pthread_barrier_t *dma_gate;
    unsigned pause_sync;
};

/* The controller waits once for arrival, does competing I/O, then releases the
 * worker with a second wait. No timing/sleep assumptions in the locking tests. */
static void pause_at_gate(pthread_barrier_t *gate)
{
    pthread_barrier_wait(gate);
    pthread_barrier_wait(gate);
}

static CUresult queue_copy(CUstream stream, void *dst, const void *src, size_t bytes)
{
    assert(stream && stream->pending < BATCH_DEPTH_MAX);
    if (stream->fail_enqueue && --stream->fail_enqueue == 0) return 1;
    unsigned i = stream->pending++;
    stream->copies[i].dst = dst;
    stream->copies[i].src = src;
    stream->copies[i].bytes = bytes;
    stream->pending_bytes += bytes;
    if (stream->pending_bytes > stream->max_pending_bytes)
        stream->max_pending_bytes = stream->pending_bytes;
    return CUDA_SUCCESS;
}

static CUresult upload(CUdeviceptr dst, const void *src, size_t bytes, CUstream stream)
{
    assert(dst >= g_vram_ptr && dst - g_vram_ptr + bytes <= g_vram_size);
    if (stream->require_reply_before_upload) {
        /* The client deliberately leaves its READ reply queued. Later writes
         * must not be submitted until that reply has been sent. */
        struct nbd_resp_hdr r;
        assert(recv(stream->reply_fd, &r, sizeof(r), MSG_PEEK | MSG_DONTWAIT) == (ssize_t)sizeof(r));
        assert(ntohl(r.magic) == NBD_RESPONSE_MAGIC && r.handle == 81);
    }
    stream->uploads++;
    return queue_copy(stream, (void *)(uintptr_t)dst, src, bytes);
}

static CUresult download(void *dst, CUdeviceptr src, size_t bytes, CUstream stream)
{
    assert(src >= g_vram_ptr && src - g_vram_ptr + bytes <= g_vram_size);
    stream->downloads++;
    return queue_copy(stream, dst, (void *)(uintptr_t)src, bytes);
}

static CUresult synchronize(CUstream stream)
{
    assert(stream);
    stream->syncs++;
    if (stream->read_gate) pthread_barrier_wait(stream->read_gate);
    if (stream->dma_gate && stream->syncs == stream->pause_sync)
        pause_at_gate(stream->dma_gate);
    for (unsigned i = 0; i < stream->pending; i++)
        memcpy(stream->copies[i].dst, stream->copies[i].src, stream->copies[i].bytes);
    stream->pending = 0;
    stream->pending_bytes = 0;
    return stream->fail_sync ? 1 : CUDA_SUCCESS;
}

static void init_worker(int idx)
{
    t_cstage = calloc(1, compression_stage_size());
    assert(t_cstage);
    t_zstd_cctx = g_zstd_cctx[idx];
    t_zstd_dctx = g_zstd_dctx[idx];
    metadata_heap_forbidden = 1;
}

static void pattern(char *page, unsigned seed)
{
    for (unsigned i = 0; i < COMP_PAGE; i++) page[i] = (char)(i % 67 + seed);
}

static pfn_LZ4_decompress_safe real_lz4_decode;
static size_t (*real_zstd_decode)(ZSTD_DCtx *, void *, size_t, const void *, size_t);
static __thread pthread_barrier_t *decode_gate;
static __thread int fail_decode;

static void before_decode(void)
{
    if (decode_gate) {
        pthread_barrier_t *gate = decode_gate;
        decode_gate = NULL; /* stop once, even for a multi-page read */
        pause_at_gate(gate);
    }
}

static int gated_lz4_decode(const char *src, char *dst, int len, int capacity)
{
    before_decode();
    return fail_decode ? -1 : real_lz4_decode(src, dst, len, capacity);
}

static size_t gated_zstd_decode(ZSTD_DCtx *ctx, void *dst, size_t capacity,
                                const void *src, size_t len)
{
    before_decode();
    return fail_decode ? SIZE_MAX : real_zstd_decode(ctx, dst, capacity, src, len);
}

static void clear_store(CUstream stream)
{
    assert(!stream->pending);
    if (packed_store_enabled()) {
        assert(comp_trim(0, (uint32_t)g_export_size, stream) == 0);
        assert(g_nfree_chunks == g_nchunks && !g_vram_obj_bytes);
        assert(!g_dedup_refs && !g_dedup_unique);
        assert(!g_pages_compressed && !g_pages_raw && !g_pages_same);
    } else {
        memset((void *)(uintptr_t)g_vram_ptr, 0, g_vram_size);
    }
    *stream = (struct CUstream_st){0};
}

static void check_batching(CUstream stream)
{
    enum { REQUESTS = 40 };
    char input[REQUESTS * COMP_PAGE], output[REQUESTS * COMP_PAGE];
    struct bop ops[REQUESTS];
    for (int i = 0; i < REQUESTS; i++) {
        pattern(input + i * COMP_PAGE, (unsigned)i);
        ops[i] = (struct bop){ .cmd = NBD_CMD_WRITE, .offset = (uint64_t)i * COMP_PAGE,
                              .len = COMP_PAGE, .slot = input + i * COMP_PAGE };
    }
    flush_packed_ops(ops, REQUESTS, stream);
    assert(stream->syncs == 2 && stream->uploads == REQUESTS && !stream->pending);
    for (int i = 0; i < REQUESTS; i++) {
        assert(!ops[i].error);
        ops[i].cmd = NBD_CMD_READ;
        ops[i].slot = output + i * COMP_PAGE;
    }
    flush_packed_ops(ops, REQUESTS, stream);
    assert(stream->syncs == 4 && !memcmp(input, output, sizeof(input)));
    clear_store(stream);

    /* Different pages can alias one lock stripe. They must form separate
     * groups, rather than recursively acquiring an exclusive rwlock. */
    ops[0] = (struct bop){ .cmd = NBD_CMD_WRITE, .offset = 0,
                           .len = COMP_PAGE, .slot = input };
    ops[1] = (struct bop){ .cmd = NBD_CMD_WRITE, .offset = (uint64_t)NPLOCK * COMP_PAGE,
                           .len = COMP_PAGE, .slot = input + COMP_PAGE };
    flush_packed_ops(ops, 2, stream);
    assert(!ops[0].error && !ops[1].error && stream->syncs == 2);
    clear_store(stream);

    /* Request count alone does not bound scatter copies: each 64 KiB request
     * needs 16 pages. Five requests must run as 32 + 32 + 16 page groups. */
    char large[5 * BATCH_SLOT];
    for (int i = 0; i < 5 * BATCH_SLOT / COMP_PAGE; i++)
        pattern(large + i * COMP_PAGE, (unsigned)i);
    for (int i = 0; i < 5; i++)
        ops[i] = (struct bop){ .cmd = NBD_CMD_WRITE, .offset = (uint64_t)i * BATCH_SLOT,
                              .len = BATCH_SLOT, .slot = large + i * BATCH_SLOT };
    flush_packed_ops(ops, 5, stream);
    assert(stream->syncs == 3 && stream->uploads == 80);
    for (int i = 0; i < 5; i++) assert(!ops[i].error);
    clear_store(stream);

    if (g_dedup) {
        for (int i = 0; i < COMP_BATCH; i++)
            ops[i] = (struct bop){ .cmd = NBD_CMD_WRITE, .offset = (uint64_t)i * COMP_PAGE,
                                  .len = COMP_PAGE, .slot = input };
        flush_packed_ops(ops, COMP_BATCH, stream);
        assert(stream->syncs == 1 && stream->uploads == 1 && !stream->downloads);
        assert(g_dedup_unique == 1 && g_dedup_refs == COMP_BATCH);
        clear_store(stream);
    }
}

static void check_failures(CUstream stream)
{
    char a[COMP_PAGE], b[COMP_PAGE], out[COMP_PAGE];
    pattern(a, 1); pattern(b, 2);
    struct bop ops[2] = {
        { .cmd = NBD_CMD_WRITE, .offset = 0, .len = COMP_PAGE, .slot = a },
        { .cmd = NBD_CMD_WRITE, .offset = COMP_PAGE, .len = COMP_PAGE, .slot = b }
    };
    stream->fail_enqueue = 2;
    flush_packed_ops(ops, 2, stream);
    assert(ops[0].error == EIO && ops[1].error == EIO);
    assert(!stream->pending && stream->syncs == 1); /* first upload was drained */
    assert(g_ptes[0].kind == PTE_NONE && g_ptes[1].kind == PTE_NONE);
    assert(!g_vram_obj_bytes && !g_dedup_refs && !g_dedup_unique);
    clear_store(stream);

    assert(comp_write(0, a, COMP_PAGE, stream) == 0);
    uint64_t old = g_ptes[0].vram_off;
    ops[0].error = 0; ops[0].slot = b;
    stream->fail_sync = 1;
    flush_packed_ops(ops, 1, stream);
    assert(ops[0].error == EIO && g_ptes[0].vram_off == old && !stream->pending);
    stream->fail_sync = 0;
    assert(comp_read(0, out, COMP_PAGE, stream) == 0 && !memcmp(a, out, COMP_PAGE));
    stream->fail_sync = 1;
    ops[0].cmd = NBD_CMD_READ; ops[0].slot = out; ops[0].error = 0;
    flush_packed_ops(ops, 1, stream);
    assert(ops[0].error == EIO && !stream->pending);
    stream->fail_sync = 0;
    clear_store(stream);
}

static pthread_barrier_t readers_gate;

static void *shared_reader(void *arg)
{
    init_worker((int)(intptr_t)arg);
    struct CUstream_st stream = { .read_gate = &readers_gate };
    char page[COMP_PAGE], expected[COMP_PAGE];
    pattern(expected, 7);
    /* Both readers must reach DMA while holding the same page's read lock. */
    assert(comp_read(0, page, COMP_PAGE, &stream) == 0);
    assert(!memcmp(page, expected, COMP_PAGE));
    free(t_cstage);
    return NULL;
}

static pthread_barrier_t start_gate;

static void *contended_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;
    init_worker(idx);
    struct CUstream_st stream = {0};
    char page[COMP_PAGE], out[2 * COMP_PAGE], expected[COMP_PAGE];
    pthread_barrier_wait(&start_gate);
    for (int pass = 0; pass < 200; pass++) {
        if (idx < 2) {
            pattern(page, (unsigned)(1 + idx));
            /* Reverse request order on one connection, including lock stripes
             * 1023 and 0. Sorted acquisition must prevent a lock cycle. */
            uint64_t first = idx ? NPLOCK : NPLOCK - 1;
            uint64_t second = idx ? NPLOCK - 1 : NPLOCK;
            struct bop ops[2] = {
                { .cmd = NBD_CMD_WRITE, .offset = first * COMP_PAGE, .len = COMP_PAGE, .slot = page },
                { .cmd = NBD_CMD_WRITE, .offset = second * COMP_PAGE, .len = COMP_PAGE, .slot = page }
            };
            flush_packed_ops(ops, 2, &stream);
            assert(!ops[0].error && !ops[1].error);
        } else if (idx == 2) {
            assert(comp_read((NPLOCK - 1) * COMP_PAGE, out, sizeof(out), &stream) == 0);
            for (int i = 0; i < 2; i++) {
                unsigned seed = (unsigned char)out[i * COMP_PAGE];
                assert(seed <= 2);
                if (seed) pattern(expected, seed);
                else memset(expected, 0, sizeof(expected));
                assert(!memcmp(out + i * COMP_PAGE, expected, COMP_PAGE));
            }
        } else {
            assert(comp_trim((NPLOCK - 1) * COMP_PAGE, 2 * COMP_PAGE, &stream) == 0);
        }
        assert(!stream.pending);
    }
    free(t_cstage);
    return NULL;
}

static void check_concurrency(CUstream stream)
{
    char page[COMP_PAGE];
    pattern(page, 7);
    assert(comp_write(0, page, sizeof(page), stream) == 0);
    pthread_t threads[4];
    assert(pthread_barrier_init(&readers_gate, NULL, 2) == 0);
    for (int i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, shared_reader, (void *)(intptr_t)i));
    for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    pthread_barrier_destroy(&readers_gate);
    clear_store(stream);
    assert(pthread_barrier_init(&start_gate, NULL, 4) == 0);
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, contended_worker, (void *)(intptr_t)i));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    pthread_barrier_destroy(&start_gate);
    clear_store(stream);
}

struct paused_io {
    struct CUstream_st stream;
    pthread_barrier_t *decode_gate;
    int writing, fail_decode;
    uint64_t offset;
    uint32_t len, error;
    char data[2 * COMP_PAGE];
};

static void *run_paused_io(void *arg)
{
    struct paused_io *io = arg;
    init_worker(1);
    decode_gate = io->decode_gate;
    fail_decode = io->fail_decode;
    io->error = io->writing
        ? comp_write(io->offset, io->data, io->len, &io->stream)
        : comp_read(io->offset, io->data, io->len, &io->stream);
    assert(!io->stream.pending);
    free(t_cstage);
    return NULL;
}

static void assert_page_unlocked(uint64_t pg)
{
    assert(pthread_rwlock_trywrlock(&g_plock[pg % NPLOCK]) == 0);
    unlock_page(pg);
}

static void check_upload_progress(CUstream stream)
{
    if (g_dedup) return; /* dedup retains its existing commit lock order */
    char old[2 * COMP_PAGE], alias[COMP_PAGE], current[2 * COMP_PAGE], out[2 * COMP_PAGE];
    pattern(old, 71); pattern(old + COMP_PAGE, 72); pattern(alias, 73);
    for (int failure = 0; failure < 3; failure++) {
        assert(comp_write(0, old, sizeof(old), stream) == 0);
        assert(comp_write((uint64_t)NPLOCK * COMP_PAGE, alias, COMP_PAGE, stream) == 0);
        pthread_barrier_t gate;
        assert(!pthread_barrier_init(&gate, NULL, 2));
        struct paused_io io = {
            .writing = 1, .len = sizeof(old),
            .stream = { .dma_gate = &gate, .pause_sync = 1,
                        .fail_enqueue = failure == 1 ? 2 : 0, .fail_sync = failure == 2 }
        };
        pattern(io.data, 74); pattern(io.data + COMP_PAGE, 75);
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, run_paused_io, &io));
        pthread_barrier_wait(&gate); /* upload queued, but not yet copied */
        assert_page_unlocked(0); assert_page_unlocked(1);
        /* Both the same pages and unrelated pages sharing their lock stripes
         * must remain readable while the new payload is still uploading. */
        assert(comp_read(0, out, sizeof(old), stream) == 0 && !memcmp(out, old, sizeof(old)));
        assert(comp_read((uint64_t)NPLOCK * COMP_PAGE, out, COMP_PAGE, stream) == 0);
        assert(!memcmp(out, alias, COMP_PAGE));

        /* Change both the mapping kind and object lifetime during the upload.
         * A stale pre-upload snapshot would free retired slots twice or corrupt
         * accounting. Rollback must preserve these newer writes/TRIMs too. */
        memset(current, 0x73, COMP_PAGE); memset(current + COMP_PAGE, 0, COMP_PAGE);
        assert(comp_write(0, current, COMP_PAGE, stream) == 0);
        assert(comp_trim(COMP_PAGE, COMP_PAGE, stream) == 0);
        pthread_barrier_wait(&gate);
        assert(!pthread_join(thread, NULL) && io.error == (failure ? EIO : 0));
        assert(comp_read(0, out, sizeof(out), stream) == 0);
        assert(!memcmp(out, failure ? current : io.data, sizeof(out)));
        assert(comp_read((uint64_t)NPLOCK * COMP_PAGE, out, COMP_PAGE, stream) == 0);
        assert(!memcmp(out, alias, COMP_PAGE));
        pthread_barrier_destroy(&gate);
        clear_store(stream);
    }
}

static void check_partial_write_lock(CUstream stream)
{
    char page[COMP_PAGE], out[COMP_PAGE];
    pattern(page, 81);
    assert(comp_write(0, page, sizeof(page), stream) == 0);
    pthread_barrier_t gate;
    assert(!pthread_barrier_init(&gate, NULL, 2));
    struct paused_io io = {
        .writing = 1, .offset = 17, .len = 77,
        .stream = { .dma_gate = &gate, .pause_sync = 2 } /* download, then upload */
    };
    pattern(io.data, 82);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, run_paused_io, &io));
    pthread_barrier_wait(&gate);
    /* The partial writer must exclude other updates until read/modify/write
     * completes, including while the replacement payload is uploading. */
    assert(pthread_rwlock_trywrlock(&g_plock[0]) == EBUSY);
    pthread_barrier_wait(&gate);
    assert(!pthread_join(thread, NULL) && !io.error);
    memcpy(page + io.offset, io.data, io.len);
    assert(comp_read(0, out, sizeof(out), stream) == 0 && !memcmp(out, page, sizeof(page)));
    pthread_barrier_destroy(&gate);
    clear_store(stream);
}

static void check_decode_progress(CUstream stream)
{
    if (!g_compress) return;
    char original[2 * COMP_PAGE], replacement[2 * COMP_PAGE], out[2 * COMP_PAGE];
    pattern(original, 91); pattern(original + COMP_PAGE, 92);
    uint32_t rng = 719;
    for (size_t i = 0; i < sizeof(replacement); i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        replacement[i] = (char)rng;
    }
    for (int partial = 0; partial < 2; partial++) {
        for (int failure = 0; failure < 2; failure++) {
            assert(comp_write(0, original, sizeof(original), stream) == 0);
            assert(g_ptes[0].kind == PTE_COMPRESSED && g_ptes[1].kind == PTE_COMPRESSED);
            pthread_barrier_t dma, decode;
            assert(!pthread_barrier_init(&dma, NULL, 2));
            assert(!pthread_barrier_init(&decode, NULL, 2));
            struct paused_io io = {
                .offset = partial ? 17 : 0, .len = partial ? 77 : sizeof(original),
                .decode_gate = &decode, .fail_decode = failure,
                .stream = { .dma_gate = &dma, .pause_sync = 1 }
            };
            pthread_t thread;
            assert(!pthread_create(&thread, NULL, run_paused_io, &io));
            pthread_barrier_wait(&dma);
            /* Objects must remain protected until DMA finishes reading them. */
            assert(pthread_rwlock_trywrlock(&g_plock[0]) == EBUSY);
            if (!partial) assert(pthread_rwlock_trywrlock(&g_plock[1]) == EBUSY);
            pthread_barrier_wait(&dma);
            pthread_barrier_wait(&decode);
            assert_page_unlocked(0); assert_page_unlocked(1);
            /* Recycle VRAM and dedup references before the reader decompresses.
             * Its private bytes and metadata must still produce the old data. */
            assert(comp_trim(0, sizeof(original), stream) == 0);
            assert(comp_write(0, replacement, sizeof(replacement), stream) == 0);
            assert(g_ptes[0].kind == PTE_RAW && g_ptes[1].kind == PTE_RAW);
            pthread_barrier_wait(&decode);
            assert(!pthread_join(thread, NULL) && io.error == (failure ? EIO : 0));
            if (!failure) assert(!memcmp(io.data, original + io.offset, io.len));
            assert(comp_read(0, out, sizeof(out), stream) == 0);
            assert(!memcmp(out, replacement, sizeof(out)));
            pthread_barrier_destroy(&dma); pthread_barrier_destroy(&decode);
            clear_store(stream);
        }
    }
}

struct connection { int fd[2], idx, result; pthread_t thread; struct CUstream_st stream; };

static void *serve_connection(void *arg)
{
    struct connection *c = arg;
    init_worker(c->idx);
    char *batch = calloc(1, BATCH_BYTES_MAX), *io = malloc(IO_BUF_SIZE);
    assert(batch && io);
    c->result = handle_client(c->fd[1], &c->stream, batch, io);
    assert(!c->stream.pending);
    close(c->fd[1]);
    free(batch); free(io); free(t_cstage);
    return NULL;
}

static void connect_client(struct connection *c, int idx)
{
    c->idx = idx;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, c->fd) == 0);
    c->stream.reply_fd = c->fd[0];
    assert(!pthread_create(&c->thread, NULL, serve_connection, c));
    unsigned char greeting[18];
    assert(recv_all(c->fd[0], greeting, sizeof(greeting)) == 0);
    uint32_t flags = htonl(NBD_FLAG_C_FIXED_NEWSTYLE | NBD_FLAG_C_NO_ZEROES);
    struct { uint64_t magic; uint32_t opt, len; } __attribute__((packed)) request = {
        htobe64(NBD_IHAVEOPT), htonl(NBD_OPT_EXPORT_NAME), 0
    };
    assert(send_all(c->fd[0], &flags, sizeof(flags)) == 0);
    assert(send_all(c->fd[0], &request, sizeof(request)) == 0);
    unsigned char info[10];
    assert(recv_all(c->fd[0], info, sizeof(info)) == 0);
}

static size_t append_request(char *packet, size_t n, uint16_t cmd, uint64_t offset,
                             uint32_t len, uint64_t handle, const char *payload)
{
    struct nbd_req_hdr h = { .magic = htonl(NBD_REQUEST_MAGIC), .type = htons(cmd),
                            .handle = handle, .from = htobe64(offset), .len = htonl(len) };
    memcpy(packet + n, &h, sizeof(h)); n += sizeof(h);
    if (cmd == NBD_CMD_WRITE) { memcpy(packet + n, payload, len); n += len; }
    return n;
}

static void response(int fd, uint64_t handle, uint32_t error, const char *expected, uint32_t len)
{
    struct nbd_resp_hdr r;
    assert(recv_all(fd, &r, sizeof(r)) == 0);
    assert(ntohl(r.magic) == NBD_RESPONSE_MAGIC && r.handle == handle && ntohl(r.error) == error);
    if (expected && !error) {
        char data[COMP_PAGE];
        for (uint32_t off = 0; off < len; ) {
            uint32_t bytes = len - off < sizeof(data) ? len - off : sizeof(data);
            assert(recv_all(fd, data, bytes) == 0);
            assert(!memcmp(data, expected + off, bytes));
            off += bytes;
        }
    }
}

static void disconnect_client(struct connection *c)
{
    char packet[sizeof(struct nbd_req_hdr)];
    size_t n = append_request(packet, 0, NBD_CMD_DISC, 0, 0, 0, NULL);
    assert(send_all(c->fd[0], packet, n) == 0);
    assert(!pthread_join(c->thread, NULL) && c->result == 0);
    assert(c->stream.max_pending_bytes <= BATCH_BYTES_MAX);
    close(c->fd[0]);
}

static void check_protocol(CUstream stream)
{
    char a[COMP_PAGE], b[COMP_PAGE], zero[COMP_PAGE] = {0}, packet[8 * COMP_PAGE];
    char patched[COMP_PAGE], large[BATCH_SLOT + COMP_PAGE] = {0};
    pattern(a, 11); pattern(b, 12);
    for (int mode = 0; mode < 3; mode++) {
        g_batch_enabled = mode != 0;
        g_batch_depth = mode == 1 ? 1 : BATCH_DEPTH_MAX;
        struct connection c = {0}, other = {0};
        connect_client(&c, 0); connect_client(&other, 1);
        unsigned long flushes = __atomic_load_n(&g_flush_count, __ATOMIC_RELAXED);
        size_t n = 0;
        n = append_request(packet, n, NBD_CMD_WRITE, 0, COMP_PAGE, 1, a);
        n = append_request(packet, n, NBD_CMD_READ, 0, COMP_PAGE, 2, NULL);
        n = append_request(packet, n, NBD_CMD_WRITE, 0, COMP_PAGE, 3, b);
        n = append_request(packet, n, NBD_CMD_READ, 0, COMP_PAGE, 4, NULL);
        n = append_request(packet, n, NBD_CMD_WRITE, COMP_PAGE, COMP_PAGE, 5, a);
        n = append_request(packet, n, NBD_CMD_TRIM, COMP_PAGE, COMP_PAGE, 6, NULL);
        n = append_request(packet, n, NBD_CMD_READ, COMP_PAGE, COMP_PAGE, 7, NULL);
        n = append_request(packet, n, NBD_CMD_FLUSH, 0, 0, 8, NULL);
        n = append_request(packet, n, NBD_CMD_WRITE, g_export_size, 512, 9, a);
        n = append_request(packet, n, NBD_CMD_READ, 0, COMP_PAGE, 10, NULL);
        n = append_request(packet, n, NBD_CMD_WRITE, 33, 77, 11, a);
        n = append_request(packet, n, NBD_CMD_READ, 0, COMP_PAGE, 12, NULL);
        n = append_request(packet, n, NBD_CMD_READ, 13, 123, 13, NULL);
        n = append_request(packet, n, NBD_CMD_READ, 0, sizeof(large), 14, NULL);
        assert(send_all(c.fd[0], packet, n) == 0);
        response(c.fd[0], 1, 0, NULL, 0); response(c.fd[0], 2, 0, a, COMP_PAGE);
        response(c.fd[0], 3, 0, NULL, 0); response(c.fd[0], 4, 0, b, COMP_PAGE);
        response(c.fd[0], 5, 0, NULL, 0); response(c.fd[0], 6, 0, NULL, 0);
        response(c.fd[0], 7, 0, packed_store_enabled() ? zero : a, COMP_PAGE);
        response(c.fd[0], 8, 0, NULL, 0);
        response(c.fd[0], 9, EINVAL, NULL, 0); response(c.fd[0], 10, 0, b, COMP_PAGE);
        memcpy(patched, b, sizeof(patched)); memcpy(patched + 33, a, 77);
        memcpy(large, patched, sizeof(patched));
        if (!packed_store_enabled()) memcpy(large + COMP_PAGE, a, COMP_PAGE);
        response(c.fd[0], 11, 0, NULL, 0); response(c.fd[0], 12, 0, patched, COMP_PAGE);
        response(c.fd[0], 13, 0, patched + 13, 123);
        response(c.fd[0], 14, 0, large, sizeof(large));
        /* An acknowledged write must be visible over a different connection. */
        n = append_request(packet, 0, NBD_CMD_READ, 0, COMP_PAGE, 15, NULL);
        assert(send_all(other.fd[0], packet, n) == 0);
        response(other.fd[0], 15, 0, patched, COMP_PAGE);
        disconnect_client(&c); disconnect_client(&other);
        assert((__atomic_load_n(&g_flush_count, __ATOMIC_RELAXED) > flushes) == (mode != 0));
        clear_store(stream);
    }
}

static void check_protocol_errors(CUstream stream)
{
    char raw[COMP_PAGE], changed[COMP_PAGE], packet[2 * COMP_PAGE];
    uint32_t rng = 973;
    for (int i = 0; i < COMP_PAGE; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        raw[i] = (char)rng;
    }
    memcpy(changed, raw, sizeof(raw)); changed[0] ^= 1;
    assert(comp_write(0, raw, COMP_PAGE, stream) == 0 && g_ptes[0].kind == PTE_RAW);
    uint64_t reserved[1024];
    int count = 0;
    for (;;) {
        uint8_t klass = (uint8_t)class_for(COMP_PAGE);
        uint64_t off = pool_alloc(&klass);
        if (off == UINT64_MAX) break;
        assert(count < 1024);
        reserved[count++] = off;
    }
    assert(g_vram_obj_bytes == g_vram_size);
    struct connection c = {0};
    connect_client(&c, 0);
    size_t n = append_request(packet, 0, NBD_CMD_WRITE, 0, COMP_PAGE, 1, changed);
    n = append_request(packet, n, NBD_CMD_READ, 0, COMP_PAGE, 2, NULL);
    assert(send_all(c.fd[0], packet, n) == 0);
    response(c.fd[0], 1, ENOSPC, NULL, 0);
    response(c.fd[0], 2, 0, raw, COMP_PAGE);
    disconnect_client(&c);
    for (int i = 0; i < count; i++) pool_free(reserved[i], (uint8_t)class_for(COMP_PAGE));
    clear_store(stream);

    c = (struct connection){ .stream = { .fail_sync = 1 } };
    connect_client(&c, 0);
    n = append_request(packet, 0, NBD_CMD_WRITE, 0, COMP_PAGE, 3, raw);
    assert(send_all(c.fd[0], packet, n) == 0);
    response(c.fd[0], 3, EIO, NULL, 0);
    assert(!pthread_join(c.thread, NULL) && c.result == -1);
    char byte;
    assert(recv(c.fd[0], &byte, 1, 0) == 0);
    close(c.fd[0]);
    assert(g_ptes[0].kind == PTE_NONE);
    clear_store(stream);
}

static void check_fragmented_requests(CUstream stream)
{
    char a[COMP_PAGE], b[COMP_PAGE], packet[3 * COMP_PAGE];
    pattern(a, 31); pattern(b, 42);
    g_batch_enabled = 1;
    g_batch_depth = BATCH_DEPTH_MAX;
    for (int payload = 0; payload < 2; payload++) {
        struct connection c = {0};
        connect_client(&c, 0);
        size_t first = append_request(packet, 0, NBD_CMD_WRITE, 0, COMP_PAGE, 1, a);
        size_t n = append_request(packet, first, NBD_CMD_WRITE, COMP_PAGE, COMP_PAGE, 2, b);
        size_t cut = first + (payload ? sizeof(struct nbd_req_hdr) + 17 : 7);
        assert(send_all(c.fd[0], packet, cut) == 0);
        /* The peer withholds the next header/payload until the first reply.
         * A blocking batch collector would deadlock here. */
        struct pollfd pfd = { .fd = c.fd[0], .events = POLLIN };
        assert(poll(&pfd, 1, 5000) == 1 && (pfd.revents & POLLIN));
        response(c.fd[0], 1, 0, NULL, 0);
        assert(send_all(c.fd[0], packet + cut, n - cut) == 0);
        response(c.fd[0], 2, 0, NULL, 0);
        n = append_request(packet, 0, NBD_CMD_READ, 0, COMP_PAGE, 3, NULL);
        n = append_request(packet, n, NBD_CMD_READ, COMP_PAGE, COMP_PAGE, 4, NULL);
        assert(send_all(c.fd[0], packet, n) == 0);
        response(c.fd[0], 3, 0, a, COMP_PAGE);
        response(c.fd[0], 4, 0, b, COMP_PAGE);
        disconnect_client(&c);
        clear_store(stream);
    }
}

static void check_reply_progress(CUstream stream)
{
    struct connection c = { .stream = { .require_reply_before_upload = 1 } };
    connect_client(&c, 0);
    char packet[2 * COMP_PAGE], data[COMP_PAGE], zero[COMP_PAGE] = {0};
    pattern(data, 52);
    size_t n = append_request(packet, 0, NBD_CMD_READ, 0, COMP_PAGE, 81, NULL);
    n = append_request(packet, n, NBD_CMD_WRITE, COMP_PAGE, COMP_PAGE, 82, data);
    n = append_request(packet, n, NBD_CMD_DISC, 0, 0, 0, NULL);
    assert(send_all(c.fd[0], packet, n) == 0);
    assert(!pthread_join(c.thread, NULL) && c.result == 0);
    response(c.fd[0], 81, 0, zero, COMP_PAGE);
    response(c.fd[0], 82, 0, NULL, 0);
    close(c.fd[0]);
    clear_store(stream);
}

static void check_transfer_limits(CUstream stream)
{
    /* Direct mode also covers the old >4 MiB pinned-buffer reuse corruption.
     * Packed mode checks chunk boundaries without requiring a larger pool. */
    uint32_t bytes = packed_store_enabled() ? 3 * BATCH_SLOT : 4 * 1024 * 1024 + COMP_PAGE;
    char *data = malloc(bytes), *packet = malloc(bytes + 4 * sizeof(struct nbd_req_hdr));
    assert(data && packet);
    for (uint32_t off = 0; off < bytes; off += COMP_PAGE)
        pattern(data + off, (off / COMP_PAGE) * 17 + off / (1024 * 1024));
    for (int batching = 0; batching < 2; batching++) {
        g_batch_enabled = batching;
        g_batch_depth = BATCH_DEPTH_MAX;
        struct connection c = {0};
        connect_client(&c, 0);
        size_t n = append_request(packet, 0, NBD_CMD_WRITE, 0, bytes, 1, data);
        assert(send_all(c.fd[0], packet, n) == 0);
        response(c.fd[0], 1, 0, NULL, 0);
        n = append_request(packet, 0, NBD_CMD_READ, 0, bytes, 2, NULL);
        assert(send_all(c.fd[0], packet, n) == 0);
        response(c.fd[0], 2, 0, data, bytes);

        /* Queued reads each fit the per-request limit but together exceed the
         * total byte cap. They must be split even at maximum batch depth. */
        n = 0;
        for (int i = 0; i < 3; i++)
            n = append_request(packet, n, NBD_CMD_READ, (uint64_t)i * BATCH_SLOT,
                               BATCH_SLOT, (uint64_t)i + 3, NULL);
        assert(send_all(c.fd[0], packet, n) == 0);
        for (int i = 0; i < 3; i++)
            response(c.fd[0], (uint64_t)i + 3, 0, data + i * BATCH_SLOT, BATCH_SLOT);
        disconnect_client(&c);
        clear_store(stream);
    }
    free(data); free(packet);
}

static void check_direct_errors(void)
{
    for (int batching = 0; batching < 2; batching++) {
        for (int cmd = NBD_CMD_READ; cmd <= NBD_CMD_WRITE; cmd++) {
            g_batch_enabled = batching;
            struct connection c = { .stream = { .fail_sync = 1 } };
            connect_client(&c, 0);
            char data[COMP_PAGE] = {0}, packet[2 * COMP_PAGE];
            size_t n = append_request(packet, 0, cmd, 0, COMP_PAGE, 1, data);
            assert(send_all(c.fd[0], packet, n) == 0);
            /* The per-request READ path sends its header before DMA. A later
             * failure must close the connection without sending stale data. */
            response(c.fd[0], 1, !batching && cmd == NBD_CMD_READ ? 0 : EIO, NULL, 0);
            assert(!pthread_join(c.thread, NULL) && c.result == -1);
            char byte;
            assert(recv(c.fd[0], &byte, 1, 0) == 0);
            close(c.fd[0]);
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3 && !parse_compression(argv[1], &g_compress, &g_compress_level));
    g_dedup = atoi(argv[2]);
    g_batch_debug = 1;
    alarm(45); /* lock regressions must fail instead of hanging the suite */
    char tmp[] = "/tmp/nbd-vram-batching-test-XXXXXX";
    assert(mkdtemp(tmp) && chdir(tmp) == 0);
    g_nbd_threads = 4;
    g_vram_size = (packed_store_enabled() ? 4 : 8) * 1024 * 1024;
    g_export_size = (packed_store_enabled() ? 2 : 1) * g_vram_size;
    g_vram_ptr = (CUdeviceptr)(uintptr_t)calloc(1, g_vram_size);
    assert(g_vram_ptr);
    if (packed_store_enabled()) assert(!compress_init());
    if (g_compress == COMP_LZ4) {
        real_lz4_decode = _LZ4_decompress_safe;
        _LZ4_decompress_safe = gated_lz4_decode;
    } else if (g_compress == COMP_ZSTD) {
        real_zstd_decode = _ZSTD_decompressDCtx;
        _ZSTD_decompressDCtx = gated_zstd_decode;
    }
    _cuMemcpyHtoDAsync = upload; _cuMemcpyDtoHAsync = download; _cuStreamSynchronize = synchronize;
    init_worker(0);
    struct CUstream_st stream = {0};
    if (packed_store_enabled()) {
        check_batching(&stream);
        check_failures(&stream);
        check_concurrency(&stream);
        check_upload_progress(&stream);
        check_partial_write_lock(&stream);
        check_decode_progress(&stream);
    }
    check_protocol(&stream);
    if (packed_store_enabled()) check_protocol_errors(&stream);
    else check_direct_errors();
    check_fragmented_requests(&stream);
    check_reply_progress(&stream);
    check_transfer_limits(&stream);
    free(t_cstage);
    metadata_heap_forbidden = 0;
    if (packed_store_enabled()) compress_shutdown();
    assert(!metadata_live_allocations);
    free((void *)(uintptr_t)g_vram_ptr);
    assert(!chdir("/") && !rmdir(tmp));
    printf("PASS batching %s dedup=%d: DMA lifetime, lock progress, rollback, bounded bursts, NBD ordering\n",
           argv[1], g_dedup);
}
