/* nbd-vram.c - NBD server backed by GPU VRAM via CUDA
 *
 * Implements NBD fixed-newstyle protocol over a Unix socket.
 * No NVIDIA P2P or kernel symbols needed - uses cuMemcpyHtoDAsync/DtoHAsync.
 *
 * Optional lz4 compression (VRAM_COMPRESS=1) stores swap pages packed in VRAM
 * so the NBD export can be larger than the CUDA allocation. Needs liblz4.so.1.
 *
 * Compile: gcc -O2 -o nbd-vram nbd-vram.c -ldl -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <endian.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <time.h>

/* PR_SET_IO_FLUSHER (Linux 5.6+) may be absent from older headers */
#ifndef PR_SET_IO_FLUSHER
#define PR_SET_IO_FLUSHER 57
#endif

/* -------------------------------------------------------------------------
 * CUDA driver API (dynamic load)
 * ---------------------------------------------------------------------- */

typedef int                CUresult;
typedef int                CUdevice;
typedef unsigned long long CUdeviceptr;
typedef struct CUctx_st    *CUcontext;
typedef struct CUstream_st *CUstream;

#define CUDA_SUCCESS           0
#define CU_CTX_SCHED_AUTO      0
#define CU_STREAM_NON_BLOCKING 1

typedef CUresult (*pfn_cuInit)(unsigned int);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*pfn_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
typedef CUresult (*pfn_cuCtxDestroy)(CUcontext);
typedef CUresult (*pfn_cuCtxSetCurrent)(CUcontext);
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*pfn_cuMemFree)(CUdeviceptr);
typedef CUresult (*pfn_cuMemcpyHtoDAsync)(CUdeviceptr, const void *, size_t, CUstream);
typedef CUresult (*pfn_cuMemcpyDtoHAsync)(void *, CUdeviceptr, size_t, CUstream);
typedef CUresult (*pfn_cuStreamCreate)(CUstream *, unsigned int);
typedef CUresult (*pfn_cuStreamDestroy)(CUstream);
typedef CUresult (*pfn_cuStreamSynchronize)(CUstream);
typedef CUresult (*pfn_cuMemAllocHost)(void **, size_t);
typedef CUresult (*pfn_cuMemFreeHost)(void *);
typedef CUresult (*pfn_cuGetErrorString)(CUresult, const char **);

static void                  *g_libcuda;
static pfn_cuInit              _cuInit;
static pfn_cuDeviceGet         _cuDeviceGet;
static pfn_cuCtxCreate         _cuCtxCreate;
static pfn_cuCtxDestroy        _cuCtxDestroy;
static pfn_cuCtxSetCurrent     _cuCtxSetCurrent;
static pfn_cuMemAlloc          _cuMemAlloc;
static pfn_cuMemFree           _cuMemFree;
static pfn_cuMemcpyHtoDAsync   _cuMemcpyHtoDAsync;
static pfn_cuMemcpyDtoHAsync   _cuMemcpyDtoHAsync;
static pfn_cuStreamCreate      _cuStreamCreate;
static pfn_cuStreamDestroy     _cuStreamDestroy;
static pfn_cuStreamSynchronize _cuStreamSynchronize;
static pfn_cuMemAllocHost      _cuMemAllocHost;
static pfn_cuMemFreeHost       _cuMemFreeHost;
static pfn_cuGetErrorString    _cuGetErrorString;

#define LOAD_SYM(h, name, pfn) do { \
    pfn = dlsym(h, name); \
    if (!pfn) { fprintf(stderr, "dlsym(%s) failed\n", name); return -1; } \
} while (0)

static int load_libcuda(void) {
    const char *paths[] = { "libcuda.so.1",
                             "/usr/lib/x86_64-linux-gnu/libcuda.so.1",
                             "/usr/lib64/libcuda.so.1", NULL };
    for (int i = 0; paths[i]; i++) {
        g_libcuda = dlopen(paths[i], RTLD_NOW);
        if (g_libcuda) { printf("[nbd-vram] loaded %s\n", paths[i]); break; }
    }
    if (!g_libcuda) { fprintf(stderr, "[nbd-vram] cannot load libcuda.so.1\n"); return -1; }
    LOAD_SYM(g_libcuda, "cuInit",                _cuInit);
    LOAD_SYM(g_libcuda, "cuDeviceGet",           _cuDeviceGet);
    LOAD_SYM(g_libcuda, "cuCtxCreate_v2",        _cuCtxCreate);
    LOAD_SYM(g_libcuda, "cuCtxDestroy_v2",       _cuCtxDestroy);
    LOAD_SYM(g_libcuda, "cuCtxSetCurrent",       _cuCtxSetCurrent);
    LOAD_SYM(g_libcuda, "cuMemAlloc_v2",         _cuMemAlloc);
    LOAD_SYM(g_libcuda, "cuMemFree_v2",          _cuMemFree);
    LOAD_SYM(g_libcuda, "cuMemcpyHtoDAsync_v2",  _cuMemcpyHtoDAsync);
    LOAD_SYM(g_libcuda, "cuMemcpyDtoHAsync_v2",  _cuMemcpyDtoHAsync);
    LOAD_SYM(g_libcuda, "cuStreamCreate",         _cuStreamCreate);
    LOAD_SYM(g_libcuda, "cuStreamDestroy_v2",     _cuStreamDestroy);
    LOAD_SYM(g_libcuda, "cuStreamSynchronize",    _cuStreamSynchronize);
    LOAD_SYM(g_libcuda, "cuMemAllocHost_v2",      _cuMemAllocHost);
    LOAD_SYM(g_libcuda, "cuMemFreeHost",          _cuMemFreeHost);
    LOAD_SYM(g_libcuda, "cuGetErrorString",       _cuGetErrorString);
    return 0;
}

static const char *cuda_err(CUresult r) {
    const char *s = NULL;
    if (_cuGetErrorString) _cuGetErrorString(r, &s);
    return s ? s : "unknown";
}

#define CUDA_CHECK(call) do { \
    CUresult _r = (call); \
    if (_r != CUDA_SUCCESS) { \
        fprintf(stderr, "[nbd-vram] " #call " failed: %s (%d)\n", cuda_err(_r), _r); \
        return -1; \
    } \
} while (0)

/* -------------------------------------------------------------------------
 * NBD fixed-newstyle protocol constants
 * ---------------------------------------------------------------------- */

/* Handshake magic */
#define NBD_MAGIC_INIT     UINT64_C(0x4e42444d41474943)  /* "NBDMAGIC" */
#define NBD_IHAVEOPT       UINT64_C(0x49484156454f5054)  /* "IHAVEOPT" */
#define NBD_OPT_REP_MAGIC  UINT64_C(0x3e889045565a9)

/* Server handshake flags */
#define NBD_FLAG_FIXED_NEWSTYLE  0x0001
#define NBD_FLAG_NO_ZEROES       0x0002

/* Client handshake flags */
#define NBD_FLAG_C_FIXED_NEWSTYLE 0x00000001
#define NBD_FLAG_C_NO_ZEROES      0x00000002

/* Options (client→server) */
#define NBD_OPT_EXPORT_NAME  1
#define NBD_OPT_ABORT        2
#define NBD_OPT_LIST         3
#define NBD_OPT_INFO         6
#define NBD_OPT_GO           7

/* Option replies (server→client) */
#define NBD_REP_ACK          1
#define NBD_REP_SERVER       2
#define NBD_REP_INFO         3
#define NBD_REP_FLAG_ERROR   UINT32_C(0x80000000)
#define NBD_REP_ERR_UNSUP    (NBD_REP_FLAG_ERROR | 1)

/* Info types */
#define NBD_INFO_EXPORT      0

/* Transmission flags (per-export) */
#define NBD_FLAG_HAS_FLAGS      0x0001
#define NBD_FLAG_SEND_FLUSH     0x0004
#define NBD_FLAG_SEND_TRIM      0x0020
#define NBD_FLAG_CAN_MULTI_CONN 0x0100

/* Transmission request magic */
#define NBD_REQUEST_MAGIC    0x25609513
#define NBD_RESPONSE_MAGIC   0x67446698

/* Commands */
#define NBD_CMD_READ         0
#define NBD_CMD_WRITE        1
#define NBD_CMD_DISC         2
#define NBD_CMD_FLUSH        3
#define NBD_CMD_TRIM         4

/* 28-byte NBD request header, shared by the per-op and batched paths */
struct nbd_req_hdr {
    uint32_t magic;
    uint16_t flags;
    uint16_t type;
    uint64_t handle;   /* opaque, echoed back verbatim (stays network order) */
    uint64_t from;
    uint32_t len;
} __attribute__((packed));

/* -------------------------------------------------------------------------
 * I/O helpers
 * ---------------------------------------------------------------------- */

static int recv_all(int fd, void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = recv(fd, (char *)buf + done, len - done, 0);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, (const char *)buf + done, len - done, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int drain(int fd, uint32_t len) {
    char buf[4096];
    while (len > 0) {
        uint32_t chunk = (len > sizeof(buf)) ? sizeof(buf) : len;
        if (recv_all(fd, buf, chunk) != 0) return -1;
        len -= chunk;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * NBD option reply helpers
 * ---------------------------------------------------------------------- */

static int send_opt_reply(int fd, uint32_t opt, uint32_t reply_type,
                           const void *data, uint32_t data_len)
{
    struct {
        uint64_t magic;
        uint32_t opt;
        uint32_t reply_type;
        uint32_t len;
    } __attribute__((packed)) hdr;

    hdr.magic      = htobe64(NBD_OPT_REP_MAGIC);
    hdr.opt        = htonl(opt);
    hdr.reply_type = htonl(reply_type);
    hdr.len        = htonl(data_len);

    if (send_all(fd, &hdr, sizeof(hdr)) != 0) return -1;
    if (data_len > 0 && send_all(fd, data, data_len) != 0) return -1;
    return 0;
}

static int send_export_info(int fd, uint32_t opt, uint64_t size, uint16_t tx_flags)
{
    struct {
        uint16_t info_type;   /* NBD_INFO_EXPORT = 0 */
        uint64_t export_size;
        uint16_t tx_flags;
    } __attribute__((packed)) info;

    info.info_type   = htons(NBD_INFO_EXPORT);
    info.export_size = htobe64(size);
    info.tx_flags    = htons(tx_flags);

    return send_opt_reply(fd, opt, NBD_REP_INFO, &info, sizeof(info));
}

/* -------------------------------------------------------------------------
 * NBD fixed-newstyle handshake
 * ---------------------------------------------------------------------- */

static int nbd_handshake(int fd, uint64_t export_size, int send_trim)
{
    /* Phase 1: server greeting */
    struct {
        uint64_t magic1;
        uint64_t magic2;
        uint16_t srv_flags;
    } __attribute__((packed)) greeting;

    greeting.magic1    = htobe64(NBD_MAGIC_INIT);
    greeting.magic2    = htobe64(NBD_IHAVEOPT);
    greeting.srv_flags = htons(NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);

    if (send_all(fd, &greeting, sizeof(greeting)) != 0) return -1;

    /* Phase 2: client flags */
    uint32_t client_flags_net;
    if (recv_all(fd, &client_flags_net, 4) != 0) return -1;
    uint32_t client_flags = ntohl(client_flags_net);
    int no_zeroes = !!(client_flags & NBD_FLAG_C_NO_ZEROES);

    /* Phase 3: option haggling */
    for (;;) {
        struct {
            uint64_t ihaveopt;
            uint32_t opt;
            uint32_t opt_len;
        } __attribute__((packed)) opt_hdr;

        if (recv_all(fd, &opt_hdr, sizeof(opt_hdr)) != 0) return -1;
        if (be64toh(opt_hdr.ihaveopt) != NBD_IHAVEOPT) return -1;

        uint32_t opt     = ntohl(opt_hdr.opt);
        uint32_t opt_len = ntohl(opt_hdr.opt_len);

        /* Limit option payload to something sane */
        if (opt_len > 65536) return -1;

        uint16_t tx_flags = NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH | NBD_FLAG_CAN_MULTI_CONN;
        if (send_trim)
            tx_flags |= NBD_FLAG_SEND_TRIM;

        switch (opt) {
        case NBD_OPT_EXPORT_NAME:
            /* Drain the export name (we only have one export) */
            if (drain(fd, opt_len) != 0) return -1;
            /* Reply: export size + tx_flags [+ 124 zeros if needed] */
            {
                struct {
                    uint64_t size;
                    uint16_t tx_flags;
                } __attribute__((packed)) info;
                info.size     = htobe64(export_size);
                info.tx_flags = htons(tx_flags);
                if (send_all(fd, &info, sizeof(info)) != 0) return -1;
                if (!no_zeroes) {
                    char zeros[124] = {0};
                    if (send_all(fd, zeros, sizeof(zeros)) != 0) return -1;
                }
            }
            return 0;  /* transmission begins */

        case NBD_OPT_GO:
        case NBD_OPT_INFO:
            if (drain(fd, opt_len) != 0) return -1;
            if (send_export_info(fd, opt, export_size, tx_flags) != 0) return -1;
            if (send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0) != 0) return -1;
            if (opt == NBD_OPT_GO)
                return 0;  /* transmission begins */
            break;

        case NBD_OPT_LIST:
            /* One anonymous export */
            if (drain(fd, opt_len) != 0) return -1;
            {
                uint32_t name_len = htonl(0);
                if (send_opt_reply(fd, opt, NBD_REP_SERVER, &name_len, 4) != 0)
                    return -1;
            }
            if (send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0) != 0) return -1;
            break;

        case NBD_OPT_ABORT:
            drain(fd, opt_len);
            send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0);
            return -1;

        default:
            if (drain(fd, opt_len) != 0) return -1;
            if (send_opt_reply(fd, opt, NBD_REP_ERR_UNSUP, NULL, 0) != 0)
                return -1;
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Transmission loop
 * ---------------------------------------------------------------------- */

#define DEFAULT_SIZE_MB 7168
#define SIZE_ALIGN      (64 * 1024)
#define SOCK_PATH       "/run/nbd-vram.sock"
#define IO_BUF_SIZE     (4 * 1024 * 1024)
#define NBD_THREADS_MAX     64
#define NBD_THREADS_DEFAULT  4

/* Request-level batching: drain up to N already-queued requests, issue all their
 * VRAM copies, then ONE cuStreamSynchronize for the whole batch. Amortises both
 * the per-op socket round-trip and the per-op CUDA launch+sync (the two halves of
 * the small-IO floor). Requests larger than a slot fall back to the per-op path. */
#define BATCH_SLOT          (64 * 1024)   /* max per-request size that batches */
#define BATCH_DEPTH_DEFAULT 32
#define BATCH_DEPTH_MAX     256

static CUdeviceptr  g_vram_ptr;
static uint64_t     g_vram_size;
static uint64_t     g_export_size;           /* NBD device size; > g_vram_size when compressed */
static int          g_compress;              /* VRAM_COMPRESS=1 */
static int          g_compress_ratio_tenths = 20; /* VRAM_COMPRESS_RATIO in 0.1x units */
static CUcontext    g_cu_ctx;
static int          g_listen_fd  = -1;
static volatile int g_running    = 1;
static volatile sig_atomic_t g_term_requested = 0;
static int          g_nbd_threads = NBD_THREADS_DEFAULT;
static volatile int g_client_fds[NBD_THREADS_MAX];
static int          g_batch_enabled = 1;                  /* VRAM_BATCH=0 disables */
static int          g_batch_depth   = BATCH_DEPTH_DEFAULT; /* VRAM_BATCH_DEPTH */
static int          g_batch_debug   = 0;                  /* VRAM_BATCH_DEBUG=1 */
static unsigned long g_batch_count   = 0;                  /* flushes that coalesced (n>1) */
static unsigned long g_batch_ops     = 0;                  /* ops in those n>1 flushes */
static unsigned long g_flush_count   = 0;                  /* every batched-path flush, incl n==1 */
static unsigned long g_flush_ops     = 0;                  /* ops across all flushes (true depth) */
static unsigned long g_legacy_ops    = 0;                  /* READ/WRITE via the per-op path */

/* Parse X or X.Y where X is 1..8 and Y is one decimal digit. */
static int parse_ratio_tenths(const char *s, int *out)
{
    if (!s || !*s) return -1;
    const char *p = s;
    int whole = 0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        p++;
    }
    int frac = 0;
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return -1;
        frac = *p - '0';
        p++;
    }
    if (*p != '\0') return -1;
    int tenths = whole * 10 + frac;
    if (tenths < 10 || tenths > 80) return -1;
    *out = tenths;
    return 0;
}

static int clients_connected(void) {
    for (int i = 0; i < g_nbd_threads; i++)
        if (g_client_fds[i] >= 0) return 1;
    return 0;
}

static void sig_handler(int sig) {
    (void)sig;
    g_term_requested = 1;
    /* A connected client means the kernel may still have live swap pages on
     * this device. Dying now frees the VRAM behind them - the kernel reads
     * the failed page-in as hardware memory corruption and MCE-kills every
     * process that had pages swapped, PID 1 included. Do NOT exit; keep
     * serving until swapoff completes and nbd-client -d drops the
     * connection, then the workers finish the exit. */
    if (clients_connected()) {
        static const char msg[] =
            "[nbd-vram] SIGTERM with client attached - draining, exit deferred until swap detaches\n";
        ssize_t w = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)w;
        return;
    }
    g_running = 0;
    /* shutdown active client sockets so threads blocked in recv_all() unblock.
     * Threads waiting for a new connection wake on their own via the poll()
     * timeout in thread_worker - closing the listen fd here would NOT reliably
     * interrupt a thread sitting in accept(), which is what hung shutdown. */
    for (int i = 0; i < g_nbd_threads; i++) {
        int fd = g_client_fds[i];
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
    }
}

/* One NBD response header */
struct nbd_resp_hdr {
    uint32_t magic;
    uint32_t error;
    uint64_t handle;
} __attribute__((packed));

/* One batched op: a READ or WRITE that fits in a slot of the pinned batch buffer */
struct bop {
    uint64_t handle;   /* network order, echoed verbatim */
    uint64_t offset;
    uint32_t len;
    uint16_t cmd;
    uint32_t error;
    char    *slot;     /* host staging memory for this op */
};

/* Overflow-safe bounds check against the NBD export. Written as
 * offset > size || length > size - offset so that a near-2^64 offset cannot wrap
 * the sum and slip past, which the naive offset + length > size would allow. */
static inline int oob(uint64_t offset, uint32_t length) {
    return offset > g_export_size || (uint64_t)length > g_export_size - offset;
}

/* -------------------------------------------------------------------------
 * Optional lz4 compressed VRAM store (VRAM_COMPRESS=1)
 *
 * Logical 4K pages are packed into the CUDA allocation. The NBD export is
 * VRAM_COMPRESS_RATIO times the physical size. Same-filled pages (zeros) take
 * no VRAM. TRIM frees slots so the pool can refill; swapon --discard=pages.
 * ---------------------------------------------------------------------- */

#define COMP_PAGE    4096
#define COMP_BATCH   32
#define SLAB_SZ      (64 * 1024)
#define NPLOCK       1024
#define STATUS_PATH  "/run/nbd-vram.status"
#define STATUS_TMP   "/run/nbd-vram.status.tmp"

#define PTE_NONE  0
#define PTE_LZ4   1
#define PTE_RAW   2
#define PTE_SAME  3

static const uint16_t k_class_sz[] = {
    32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448,
    512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096
};
#define NCLASS ((int)(sizeof(k_class_sz) / sizeof(k_class_sz[0])))

struct pte {
    uint64_t vram_off;
    uint16_t clen;
    uint8_t  kind;
    uint8_t  fill;
    uint8_t  klass;
};

struct slab {
    uint32_t chunk;
    uint32_t idx;     /* index in g_cls[klass].slabs */
    uint16_t nobj;
    uint16_t nfree;
    uint16_t hint;
    uint8_t  klass;
    uint8_t *bm;      /* 1 = in use */
};

struct szclass {
    struct slab **slabs;
    uint32_t n, cap;
};

struct cpend {
    uint64_t pg;
    uint64_t new_off;
    uint64_t old_off;
    uint16_t clen;
    uint8_t  kind, fill, klass, slot;
    uint8_t  old_kind, old_klass;
};

struct rpend {
    uint64_t pg;
    uint64_t vram_off;
    uint32_t dst_off;
    uint16_t clen;
    uint8_t  kind, slot;
};

typedef int (*pfn_LZ4_compress_default)(const char *, char *, int, int);
typedef int (*pfn_LZ4_decompress_safe)(const char *, char *, int, int);

static void                      *g_liblz4;
static pfn_LZ4_compress_default   _LZ4_compress_default;
static pfn_LZ4_decompress_safe    _LZ4_decompress_safe;

static struct pte     *g_ptes;
static uint64_t        g_npages;
static struct szclass  g_cls[NCLASS];
static struct slab   **g_chunk_owner;
static uint8_t        *g_chunk_busy;
static uint32_t        g_nchunks, g_nfree_chunks, g_chunk_hint;
static pthread_mutex_t g_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_plock[NPLOCK];
static unsigned long   g_comp_enospc;
static unsigned long   g_pages_lz4, g_pages_raw, g_pages_same;
static uint64_t        g_vram_obj_bytes;   /* allocated object bytes, under g_alloc_lock */

static __thread char *t_cstage;
static __thread int   t_cstage_cuda;
static __thread char  t_page[COMP_PAGE] __attribute__((aligned(16)));

static int load_liblz4(void)
{
    const char *paths[] = {
        "liblz4.so.1",
        "/usr/lib/x86_64-linux-gnu/liblz4.so.1",
        "/usr/lib64/liblz4.so.1",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        g_liblz4 = dlopen(paths[i], RTLD_NOW);
        if (g_liblz4) {
            printf("[nbd-vram] loaded %s\n", paths[i]);
            break;
        }
    }
    if (!g_liblz4) {
        fprintf(stderr, "[nbd-vram] VRAM_COMPRESS=1 but cannot load liblz4.so.1 - install liblz4-1 (Debian) or lz4-libs (Fedora)\n");
        return -1;
    }
    _LZ4_compress_default = (pfn_LZ4_compress_default)dlsym(g_liblz4, "LZ4_compress_default");
    _LZ4_decompress_safe  = (pfn_LZ4_decompress_safe)dlsym(g_liblz4, "LZ4_decompress_safe");
    if (!_LZ4_compress_default || !_LZ4_decompress_safe) {
        fprintf(stderr, "[nbd-vram] liblz4 missing LZ4_compress_default/LZ4_decompress_safe\n");
        return -1;
    }
    return 0;
}

static inline void lock_page(uint64_t pg)   { pthread_mutex_lock(&g_plock[pg % NPLOCK]); }
static inline void unlock_page(uint64_t pg) { pthread_mutex_unlock(&g_plock[pg % NPLOCK]); }

static inline int bm_test(const uint8_t *bm, unsigned i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static inline void bm_set(uint8_t *bm, unsigned i) { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bm_clr(uint8_t *bm, unsigned i) { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static int class_for(uint32_t n)
{
    for (int i = 0; i < NCLASS; i++)
        if (k_class_sz[i] >= n) return i;
    return NCLASS - 1;
}

static int is_same_filled(const char *p, uint8_t *fill)
{
    unsigned char v = (unsigned char)p[0];
    if ((uintptr_t)p & 7) {
        for (int i = 1; i < COMP_PAGE; i++)
            if ((unsigned char)p[i] != v) return 0;
        *fill = (uint8_t)v;
        return 1;
    }
    uint64_t splat = 0x0101010101010101ULL * v;
    const uint64_t *q = (const uint64_t *)(const void *)p;
    for (int i = 0; i < COMP_PAGE / 8; i++)
        if (q[i] != splat) return 0;
    *fill = (uint8_t)v;
    return 1;
}

static int chunk_alloc(void)
{
    if (!g_nfree_chunks) return -1;
    uint32_t i = g_chunk_hint;
    for (uint32_t n = 0; n < g_nchunks; n++) {
        if (!bm_test(g_chunk_busy, i)) {
            bm_set(g_chunk_busy, i);
            g_nfree_chunks--;
            g_chunk_hint = (i + 1 < g_nchunks) ? i + 1 : 0;
            return (int)i;
        }
        if (++i == g_nchunks) i = 0;
    }
    return -1;
}

static void chunk_free(uint32_t chunk)
{
    bm_clr(g_chunk_busy, chunk);
    g_nfree_chunks++;
    g_chunk_hint = chunk;
}

static int class_append(struct szclass *c, struct slab *s)
{
    if (c->n == c->cap) {
        uint32_t cap = c->cap ? c->cap * 2 : 16;
        struct slab **ns = realloc(c->slabs, cap * sizeof(*ns));
        if (!ns) return -1;
        c->slabs = ns;
        c->cap = cap;
    }
    s->idx = c->n;
    c->slabs[c->n++] = s;
    return 0;
}

static struct slab *slab_new(uint8_t klass)
{
    int chunk = chunk_alloc();
    if (chunk < 0) return NULL;
    struct slab *s = calloc(1, sizeof(*s));
    if (!s) { chunk_free((uint32_t)chunk); return NULL; }
    s->chunk = (uint32_t)chunk;
    s->klass = klass;
    s->nobj  = (uint16_t)(SLAB_SZ / k_class_sz[klass]);
    s->nfree = s->nobj;
    s->bm = calloc(((size_t)s->nobj + 7) / 8, 1);
    if (!s->bm) { free(s); chunk_free((uint32_t)chunk); return NULL; }
    if (class_append(&g_cls[klass], s) != 0) {
        free(s->bm); free(s); chunk_free((uint32_t)chunk); return NULL;
    }
    g_chunk_owner[chunk] = s;
    return s;
}

static void slab_destroy(struct slab *s)
{
    struct szclass *c = &g_cls[s->klass];
    uint32_t i = s->idx;
    c->slabs[i] = c->slabs[--c->n];
    if (i < c->n) c->slabs[i]->idx = i;
    g_chunk_owner[s->chunk] = NULL;
    chunk_free(s->chunk);
    free(s->bm);
    free(s);
}

static int bm_alloc_obj(struct slab *s)
{
    unsigned n = s->nobj, i = s->hint;
    for (unsigned k = 0; k < n; k++) {
        if (!bm_test(s->bm, i)) {
            bm_set(s->bm, i);
            s->hint = (i + 1 < n) ? (uint16_t)(i + 1) : 0;
            return (int)i;
        }
        if (++i == n) i = 0;
    }
    return -1;
}

/* Caller holds g_alloc_lock. */
static uint64_t pool_alloc(uint8_t klass)
{
    struct szclass *c = &g_cls[klass];
    for (uint32_t i = 0; i < c->n; i++) {
        struct slab *s = c->slabs[i];
        if (!s->nfree) continue;
        int obj = bm_alloc_obj(s);
        if (obj < 0) continue;
        s->nfree--;
        g_vram_obj_bytes += k_class_sz[klass];
        return (uint64_t)s->chunk * SLAB_SZ + (uint64_t)obj * k_class_sz[klass];
    }
    struct slab *s = slab_new(klass);
    if (!s) return UINT64_MAX;
    int obj = bm_alloc_obj(s);
    if (obj < 0) return UINT64_MAX;
    s->nfree--;
    g_vram_obj_bytes += k_class_sz[klass];
    return (uint64_t)s->chunk * SLAB_SZ + (uint64_t)obj * k_class_sz[klass];
}

static void pool_free(uint64_t off, uint8_t klass)
{
    uint32_t chunk = (uint32_t)(off / SLAB_SZ);
    struct slab *s = g_chunk_owner[chunk];
    unsigned obj = (unsigned)((off % SLAB_SZ) / k_class_sz[klass]);
    if (g_vram_obj_bytes >= k_class_sz[klass])
        g_vram_obj_bytes -= k_class_sz[klass];
    bm_clr(s->bm, obj);
    s->nfree++;
    if (s->nfree == s->nobj)
        slab_destroy(s);
}

static uint32_t load_plain(uint64_t pg, char *dst, CUstream stream)
{
    struct pte e = g_ptes[pg];
    switch (e.kind) {
    case PTE_NONE:
        memset(dst, 0, COMP_PAGE);
        return 0;
    case PTE_SAME:
        memset(dst, e.fill, COMP_PAGE);
        return 0;
    case PTE_RAW:
        if (_cuMemcpyDtoHAsync(dst, g_vram_ptr + e.vram_off, COMP_PAGE, stream) != CUDA_SUCCESS)
            return EIO;
        _cuStreamSynchronize(stream);
        return 0;
    case PTE_LZ4:
        if (_cuMemcpyDtoHAsync(t_cstage, g_vram_ptr + e.vram_off, e.clen, stream) != CUDA_SUCCESS)
            return EIO;
        _cuStreamSynchronize(stream);
        if (_LZ4_decompress_safe(t_cstage, dst, e.clen, COMP_PAGE) != COMP_PAGE)
            return EIO;
        return 0;
    default:
        return EIO;
    }
}

static void prepare_plain(uint64_t pg, const char *plain, int slot, struct cpend *op)
{
    uint8_t fill;
    op->pg       = pg;
    op->slot     = (uint8_t)slot;
    op->old_kind = g_ptes[pg].kind;
    op->old_off  = g_ptes[pg].vram_off;
    op->old_klass= g_ptes[pg].klass;
    op->new_off  = 0;
    op->fill     = 0;
    op->clen     = 0;
    op->klass    = 0;
    if (is_same_filled(plain, &fill)) {
        op->kind = PTE_SAME;
        op->fill = fill;
        return;
    }
    char *slotp = t_cstage + (size_t)slot * COMP_PAGE;
    int csz = _LZ4_compress_default(plain, slotp, COMP_PAGE, COMP_PAGE - 1);
    if (csz <= 0) {
        memcpy(slotp, plain, COMP_PAGE);
        op->kind  = PTE_RAW;
        op->clen  = COMP_PAGE;
        op->klass = (uint8_t)class_for(COMP_PAGE);
    } else {
        op->kind  = PTE_LZ4;
        op->clen  = (uint16_t)csz;
        op->klass = (uint8_t)class_for((uint32_t)csz);
    }
}

static void pte_kind_add(uint8_t kind, int delta)
{
    unsigned long *p = NULL;
    switch (kind) {
    case PTE_LZ4:  p = &g_pages_lz4;  break;
    case PTE_RAW:  p = &g_pages_raw;  break;
    case PTE_SAME: p = &g_pages_same; break;
    default: return;
    }
    if (delta > 0)
        __sync_fetch_and_add(p, (unsigned long)delta);
    else
        __sync_fetch_and_sub(p, (unsigned long)(-delta));
}

/* Unlocks every page in ops on all paths. Caller must not unlock again. */
static uint32_t cpend_alloc_and_commit(struct cpend *ops, int n, CUstream stream)
{
    if (n <= 0) return 0;

    pthread_mutex_lock(&g_alloc_lock);
    int failed = -1;
    for (int i = 0; i < n; i++) {
        if (ops[i].kind != PTE_LZ4 && ops[i].kind != PTE_RAW)
            continue;
        uint64_t off = pool_alloc(ops[i].klass);
        if (off == UINT64_MAX) { failed = i; break; }
        ops[i].new_off = off;
    }
    if (failed >= 0) {
        for (int i = 0; i < failed; i++) {
            if (ops[i].kind == PTE_LZ4 || ops[i].kind == PTE_RAW)
                pool_free(ops[i].new_off, ops[i].klass);
        }
        pthread_mutex_unlock(&g_alloc_lock);
        for (int i = 0; i < n; i++)
            unlock_page(ops[i].pg);
        if (__sync_fetch_and_add(&g_comp_enospc, 1) == 0)
            fprintf(stderr, "[nbd-vram] compressed VRAM pool full (ENOSPC) - kernel will use other swap if available\n");
        return ENOSPC;
    }
    pthread_mutex_unlock(&g_alloc_lock);

    for (int i = 0; i < n; i++) {
        if (ops[i].kind != PTE_LZ4 && ops[i].kind != PTE_RAW)
            continue;
        size_t nbytes = (ops[i].kind == PTE_RAW) ? COMP_PAGE : ops[i].clen;
        CUresult r = _cuMemcpyHtoDAsync(g_vram_ptr + ops[i].new_off,
                                        t_cstage + (size_t)ops[i].slot * COMP_PAGE,
                                        nbytes, stream);
        if (r != CUDA_SUCCESS) {
            pthread_mutex_lock(&g_alloc_lock);
            for (int j = 0; j < n; j++) {
                if (ops[j].kind == PTE_LZ4 || ops[j].kind == PTE_RAW)
                    pool_free(ops[j].new_off, ops[j].klass);
            }
            pthread_mutex_unlock(&g_alloc_lock);
            for (int j = 0; j < n; j++)
                unlock_page(ops[j].pg);
            return EIO;
        }
    }
    _cuStreamSynchronize(stream);

    pthread_mutex_lock(&g_alloc_lock);
    for (int i = 0; i < n; i++) {
        if (ops[i].old_kind == PTE_LZ4 || ops[i].old_kind == PTE_RAW)
            pool_free(ops[i].old_off, ops[i].old_klass);
    }
    pthread_mutex_unlock(&g_alloc_lock);

    for (int i = 0; i < n; i++) {
        struct pte *e = &g_ptes[ops[i].pg];
        e->kind     = ops[i].kind;
        e->fill     = ops[i].fill;
        e->clen     = ops[i].clen;
        e->klass    = ops[i].klass;
        e->vram_off = ops[i].new_off;
        if (ops[i].kind == PTE_SAME) {
            e->vram_off = 0;
            e->clen = 0;
            e->klass = 0;
        }
        pte_kind_add(ops[i].old_kind, -1);
        pte_kind_add(ops[i].kind, 1);
        unlock_page(ops[i].pg);
    }
    return 0;
}

static uint32_t store_partial(uint64_t pg, const char *frag, uint32_t lo, uint32_t hi, CUstream stream)
{
    lock_page(pg);
    uint32_t err = load_plain(pg, t_page, stream);
    if (err) { unlock_page(pg); return err; }
    memcpy(t_page + lo, frag, hi - lo);
    struct cpend op;
    prepare_plain(pg, t_page, 0, &op);
    return cpend_alloc_and_commit(&op, 1, stream);
}

static uint32_t comp_write(uint64_t offset, const char *buf, uint32_t len, CUstream stream)
{
    if (!len) return 0;
    if (!t_cstage) return EIO;
    uint64_t pg0 = offset / COMP_PAGE;
    uint64_t pg1 = (offset + len - 1) / COMP_PAGE;
    struct cpend ops[COMP_BATCH];
    int n = 0;

    for (uint64_t pg = pg0; pg <= pg1; pg++) {
        uint64_t pg_off = pg * COMP_PAGE;
        uint32_t lo = (offset > pg_off) ? (uint32_t)(offset - pg_off) : 0;
        uint32_t hi = (offset + len < pg_off + COMP_PAGE)
                        ? (uint32_t)(offset + len - pg_off) : COMP_PAGE;
        if (lo != 0 || hi != COMP_PAGE) {
            if (n) {
                uint32_t err = cpend_alloc_and_commit(ops, n, stream);
                n = 0;
                if (err) return err;
            }
            uint32_t err = store_partial(pg, buf + (pg_off + lo - offset), lo, hi, stream);
            if (err) return err;
            continue;
        }
        lock_page(pg);
        prepare_plain(pg, buf + (pg_off - offset), n, &ops[n]);
        n++;
        if (n == COMP_BATCH) {
            uint32_t err = cpend_alloc_and_commit(ops, n, stream);
            n = 0;
            if (err) return err;
        }
    }
    if (n) return cpend_alloc_and_commit(ops, n, stream);
    return 0;
}

static uint32_t rpend_commit(struct rpend *ops, int n, char *buf, CUstream stream)
{
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) {
        size_t nbytes = (ops[i].kind == PTE_RAW) ? COMP_PAGE : ops[i].clen;
        CUresult r = _cuMemcpyDtoHAsync(t_cstage + (size_t)ops[i].slot * COMP_PAGE,
                                        g_vram_ptr + ops[i].vram_off, nbytes, stream);
        if (r != CUDA_SUCCESS) {
            for (int j = 0; j < n; j++)
                unlock_page(ops[j].pg);
            return EIO;
        }
    }
    _cuStreamSynchronize(stream);
    for (int i = 0; i < n; i++) {
        char *dst = buf + ops[i].dst_off;
        char *src = t_cstage + (size_t)ops[i].slot * COMP_PAGE;
        if (ops[i].kind == PTE_RAW) {
            memcpy(dst, src, COMP_PAGE);
        } else if (_LZ4_decompress_safe(src, dst, ops[i].clen, COMP_PAGE) != COMP_PAGE) {
            for (int j = i; j < n; j++)
                unlock_page(ops[j].pg);
            return EIO;
        }
        unlock_page(ops[i].pg);
    }
    return 0;
}

static uint32_t comp_read(uint64_t offset, char *buf, uint32_t len, CUstream stream)
{
    if (!len) return 0;
    if (!t_cstage) return EIO;
    uint64_t pg0 = offset / COMP_PAGE;
    uint64_t pg1 = (offset + len - 1) / COMP_PAGE;
    struct rpend ops[COMP_BATCH];
    int n = 0;

    for (uint64_t pg = pg0; pg <= pg1; pg++) {
        uint64_t pg_off = pg * COMP_PAGE;
        uint32_t lo = (offset > pg_off) ? (uint32_t)(offset - pg_off) : 0;
        uint32_t hi = (offset + len < pg_off + COMP_PAGE)
                        ? (uint32_t)(offset + len - pg_off) : COMP_PAGE;
        if (lo != 0 || hi != COMP_PAGE) {
            if (n) {
                uint32_t err = rpend_commit(ops, n, buf, stream);
                n = 0;
                if (err) return err;
            }
            lock_page(pg);
            uint32_t err = load_plain(pg, t_page, stream);
            if (err) { unlock_page(pg); return err; }
            memcpy(buf + (pg_off + lo - offset), t_page + lo, hi - lo);
            unlock_page(pg);
            continue;
        }
        lock_page(pg);
        uint8_t kind = g_ptes[pg].kind;
        uint32_t dst_off = (uint32_t)(pg_off - offset);
        if (kind == PTE_NONE) {
            memset(buf + dst_off, 0, COMP_PAGE);
            unlock_page(pg);
            continue;
        }
        if (kind == PTE_SAME) {
            memset(buf + dst_off, g_ptes[pg].fill, COMP_PAGE);
            unlock_page(pg);
            continue;
        }
        ops[n].pg       = pg;
        ops[n].vram_off = g_ptes[pg].vram_off;
        ops[n].dst_off  = dst_off;
        ops[n].clen     = g_ptes[pg].clen;
        ops[n].kind     = kind;
        ops[n].slot     = (uint8_t)n;
        n++;
        if (n == COMP_BATCH) {
            uint32_t err = rpend_commit(ops, n, buf, stream);
            n = 0;
            if (err) return err;
        }
    }
    if (n) return rpend_commit(ops, n, buf, stream);
    return 0;
}

static uint32_t comp_trim(uint64_t offset, uint32_t len, CUstream stream)
{
    if (!len) return 0;
    uint64_t pg0 = offset / COMP_PAGE;
    uint64_t pg1 = (offset + len - 1) / COMP_PAGE;
    for (uint64_t pg = pg0; pg <= pg1; pg++) {
        uint64_t pg_off = pg * COMP_PAGE;
        uint32_t lo = (offset > pg_off) ? (uint32_t)(offset - pg_off) : 0;
        uint32_t hi = (offset + len < pg_off + COMP_PAGE)
                        ? (uint32_t)(offset + len - pg_off) : COMP_PAGE;
        if (lo != 0 || hi != COMP_PAGE) {
            char z[COMP_PAGE];
            memset(z, 0, hi - lo);
            uint32_t err = store_partial(pg, z, lo, hi, stream);
            if (err) return err;
            continue;
        }
        lock_page(pg);
        struct pte old = g_ptes[pg];
        memset(&g_ptes[pg], 0, sizeof(g_ptes[pg]));
        unlock_page(pg);
        pte_kind_add(old.kind, -1);
        if (old.kind == PTE_LZ4 || old.kind == PTE_RAW) {
            pthread_mutex_lock(&g_alloc_lock);
            pool_free(old.vram_off, old.klass);
            pthread_mutex_unlock(&g_alloc_lock);
        }
    }
    return 0;
}

static void compress_status_write(void)
{
    uint32_t slabs_used;
    uint64_t obj_bytes;
    pthread_mutex_lock(&g_alloc_lock);
    slabs_used = g_nchunks - g_nfree_chunks;
    obj_bytes  = g_vram_obj_bytes;
    pthread_mutex_unlock(&g_alloc_lock);

    unsigned long lz4  = g_pages_lz4;
    unsigned long raw  = g_pages_raw;
    unsigned long same = g_pages_same;
    char buf[768];
    int cfg_whole = g_compress_ratio_tenths / 10;
    int cfg_frac  = g_compress_ratio_tenths % 10;
    int n = snprintf(buf, sizeof(buf),
        "compress=1\n"
        "configured_ratio=%d.%d\n"
        "configured_ratio_tenths=%d\n"
        "vram_bytes=%llu\n"
        "export_bytes=%llu\n"
        "vram_slab_bytes=%llu\n"
        "vram_obj_bytes=%llu\n"
        "pages_lz4=%lu\n"
        "pages_raw=%lu\n"
        "pages_same=%lu\n"
        "enospc=%lu\n",
        cfg_whole, cfg_frac, g_compress_ratio_tenths,
        (unsigned long long)g_vram_size,
        (unsigned long long)g_export_size,
        (unsigned long long)slabs_used * SLAB_SZ,
        (unsigned long long)obj_bytes,
        lz4, raw, same, g_comp_enospc);
    if (n <= 0 || n >= (int)sizeof(buf)) return;

    int fd = open(STATUS_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    if (w == (ssize_t)n)
        rename(STATUS_TMP, STATUS_PATH);
    else
        unlink(STATUS_TMP);
}

static void *status_worker(void *arg)
{
    (void)arg;
    prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);
    while (g_running) {
        compress_status_write();
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
    }
    unlink(STATUS_PATH);
    unlink(STATUS_TMP);
    return NULL;
}

static int compress_init(void)
{
    if (load_liblz4() != 0) return -1;
    if (g_vram_size < SLAB_SZ || (g_vram_size % SLAB_SZ) != 0) {
        fprintf(stderr, "[nbd-vram] VRAM size not aligned to %u KiB slabs\n", SLAB_SZ / 1024);
        return -1;
    }
    g_npages = g_export_size / COMP_PAGE;
    g_ptes = calloc(g_npages, sizeof(*g_ptes));
    if (!g_ptes) { perror("calloc ptes"); return -1; }
    g_nchunks = (uint32_t)(g_vram_size / SLAB_SZ);
    g_chunk_busy  = calloc(((size_t)g_nchunks + 7) / 8, 1);
    g_chunk_owner = calloc(g_nchunks, sizeof(*g_chunk_owner));
    if (!g_chunk_busy || !g_chunk_owner) { perror("calloc chunks"); return -1; }
    g_nfree_chunks = g_nchunks;
    for (int i = 0; i < NPLOCK; i++)
        pthread_mutex_init(&g_plock[i], NULL);
    return 0;
}

static void compress_shutdown(void)
{
    unlink(STATUS_PATH);
    unlink(STATUS_TMP);
    if (g_ptes)
        printf("[nbd-vram] compress: %u/%u slabs used, %lu ENOSPC writes\n",
               g_nchunks - g_nfree_chunks, g_nchunks, g_comp_enospc);
    free(g_ptes); g_ptes = NULL;
    for (int k = 0; k < NCLASS; k++) {
        for (uint32_t i = 0; i < g_cls[k].n; i++) {
            free(g_cls[k].slabs[i]->bm);
            free(g_cls[k].slabs[i]);
        }
        free(g_cls[k].slabs);
        g_cls[k].slabs = NULL;
        g_cls[k].n = g_cls[k].cap = 0;
    }
    free(g_chunk_owner); g_chunk_owner = NULL;
    free(g_chunk_busy);  g_chunk_busy  = NULL;
    if (g_liblz4) { dlclose(g_liblz4); g_liblz4 = NULL; }
}

/* Read a full 28-byte request header WITHOUT blocking if none is queued. The
 * first byte(s) decide: nothing queued -> 0 (adaptive batch cutoff); a header
 * began arriving -> finish it blocking to stay frame-aligned. */
static int recv_hdr_nb(int fd, struct nbd_req_hdr *h)
{
    ssize_t r = recv(fd, h, sizeof(*h), MSG_DONTWAIT);
    if (r == 0) return -1;                                   /* peer closed */
    if (r < 0)  return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    if ((size_t)r == sizeof(*h)) return 1;
    return (recv_all(fd, (char *)h + r, sizeof(*h) - r) == 0) ? 1 : -1;
}

/* Legacy per-request path: exact prior semantics, handles a single header that
 * was already read. Used for oversized requests, FLUSH/TRIM, and VRAM_BATCH=0.
 * Returns 0 to keep serving, -1 to drop the connection. */
static int handle_one(int fd, const struct nbd_req_hdr *h, CUstream stream, char *iobuf)
{
    uint16_t cmd    = ntohs(h->type);
    uint64_t handle = h->handle;
    uint64_t offset = be64toh(h->from);
    uint32_t length = ntohl(h->len);
    uint32_t error  = 0;

    if (g_batch_debug && (cmd == NBD_CMD_READ || cmd == NBD_CMD_WRITE))
        __sync_fetch_and_add(&g_legacy_ops, 1);

    if ((cmd == NBD_CMD_READ || cmd == NBD_CMD_WRITE) && oob(offset, length)) {
        fprintf(stderr, "[nbd-vram] oob off=%llu len=%u\n",
                (unsigned long long)offset, length);
        error = EINVAL;
    }

    if (cmd == NBD_CMD_WRITE) {
        uint32_t remaining = length;
        uint64_t voff      = offset;
        while (remaining > 0) {
            uint32_t chunk = (remaining > IO_BUF_SIZE) ? IO_BUF_SIZE : remaining;
            if (recv_all(fd, iobuf, chunk) != 0) return -1;
            if (!error) {
                if (g_compress) {
                    uint32_t e = comp_write(voff, iobuf, chunk, stream);
                    if (e) error = e;
                } else {
                    CUresult r = _cuMemcpyHtoDAsync(g_vram_ptr + voff, iobuf, chunk, stream);
                    if (r != CUDA_SUCCESS) {
                        fprintf(stderr, "[nbd-vram] HtoD failed: %s\n", cuda_err(r));
                        error = EIO;
                    }
                }
                voff += chunk;
            }
            remaining -= chunk;
        }
        if (!error && !g_compress) _cuStreamSynchronize(stream);
    } else if (cmd == NBD_CMD_FLUSH) {
        _cuStreamSynchronize(stream);
    } else if (cmd == NBD_CMD_TRIM && g_compress) {
        if (oob(offset, length)) error = EINVAL;
        else error = comp_trim(offset, length, stream);
    }
    /* uncompressed TRIM: ack success, VRAM doesn't need trimming */

    struct nbd_resp_hdr resp;
    resp.magic  = htonl(NBD_RESPONSE_MAGIC);
    resp.error  = htonl(error);
    resp.handle = handle;
    if (send_all(fd, &resp, sizeof(resp)) != 0) return -1;
    if (error == EIO) return -1;   /* real copy failure: hard-reset the connection */
    if (error)        return 0;    /* EINVAL / ENOSPC: reported, keep serving */

    if (cmd == NBD_CMD_READ) {
        uint32_t remaining = length;
        uint64_t voff      = offset;
        while (remaining > 0) {
            uint32_t chunk = (remaining > IO_BUF_SIZE) ? IO_BUF_SIZE : remaining;
            if (g_compress) {
                uint32_t e = comp_read(voff, iobuf, chunk, stream);
                if (e) return -1;
            } else {
                CUresult r = _cuMemcpyDtoHAsync(iobuf, g_vram_ptr + voff, chunk, stream);
                if (r != CUDA_SUCCESS) {
                    fprintf(stderr, "[nbd-vram] DtoH failed: %s\n", cuda_err(r));
                    return -1;
                }
                _cuStreamSynchronize(stream);
            }
            if (send_all(fd, iobuf, chunk) != 0) return -1;
            remaining -= chunk;
            voff      += chunk;
        }
    }
    return 0;
}

/* Validate a header and, if it is a batchable READ/WRITE that fits a slot,
 * populate *op (reading the WRITE payload into the slot now, since it must be
 * drained in frame order). Returns 1 = batched, 0 = not batchable (caller falls
 * back to handle_one), -1 = protocol/socket error. */
static int batch_admit(int fd, const struct nbd_req_hdr *h, struct bop *op, char *slot)
{
    if (ntohl(h->magic) != NBD_REQUEST_MAGIC) {
        fprintf(stderr, "[nbd-vram] bad request magic 0x%x\n", ntohl(h->magic));
        return -1;
    }
    uint16_t cmd = ntohs(h->type);
    uint32_t len = ntohl(h->len);
    if ((cmd != NBD_CMD_READ && cmd != NBD_CMD_WRITE) || len == 0 || len > BATCH_SLOT)
        return 0;   /* DISC/FLUSH/TRIM or oversized: not for the batch path */

    op->handle = h->handle;
    op->offset = be64toh(h->from);
    op->len    = len;
    op->cmd    = cmd;
    op->slot   = slot;
    op->error  = oob(op->offset, len) ? EINVAL : 0;

    if (cmd == NBD_CMD_WRITE) {
        /* Drain the payload even when OOB, to stay frame-aligned; the copy is
         * skipped in flush_batch for errored ops. */
        if (recv_all(fd, slot, len) != 0) return -1;
    }
    return 1;
}

/* Issue every batched copy on the stream, then a SINGLE synchronize for the whole
 * batch, then reply to each op. Stream FIFO order preserves intra-batch RAW/WAR.
 * NBD matches replies by handle, so a per-op error never strands the others: a bad
 * request (EINVAL) is reported and we keep serving; a real copy failure (EIO) is
 * reported too, then the connection is dropped after every reply is sent. */
static int flush_batch(int fd, struct bop *ops, int n, CUstream stream)
{
    int hard_err = 0;   /* an EIO occurred: reset the connection once all replies are out */
    for (int i = 0; i < n; i++) {
        if (ops[i].error) continue;
        CUdeviceptr d = g_vram_ptr + ops[i].offset;
        CUresult r = (ops[i].cmd == NBD_CMD_READ)
            ? _cuMemcpyDtoHAsync(ops[i].slot, d, ops[i].len, stream)
            : _cuMemcpyHtoDAsync(d, ops[i].slot, ops[i].len, stream);
        if (r != CUDA_SUCCESS) {
            fprintf(stderr, "[nbd-vram] batch copy failed: %s\n", cuda_err(r));
            ops[i].error = EIO;
            hard_err = 1;
        }
    }
    _cuStreamSynchronize(stream);   /* one sync amortised across the whole batch */

    if (g_batch_debug) {            /* opt-in: measure real-world batch depth */
        __sync_fetch_and_add(&g_flush_count, 1);
        __sync_fetch_and_add(&g_flush_ops, n);
        if (n > 1) { __sync_fetch_and_add(&g_batch_count, 1);
                     __sync_fetch_and_add(&g_batch_ops, n); }
    }

    for (int i = 0; i < n; i++) {
        struct nbd_resp_hdr resp;
        resp.magic  = htonl(NBD_RESPONSE_MAGIC);
        resp.error  = htonl(ops[i].error);
        resp.handle = ops[i].handle;
        if (send_all(fd, &resp, sizeof(resp)) != 0) return -1;
        if (ops[i].cmd == NBD_CMD_READ && !ops[i].error) {
            if (send_all(fd, ops[i].slot, ops[i].len) != 0) return -1;
        }
    }
    return hard_err ? -1 : 0;   /* EIO: every reply sent, now reset the connection */
}

static int handle_client(int fd, CUstream stream, char *batchbuf, char *iobuf)
{
    if (nbd_handshake(fd, g_export_size, g_compress) != 0) {
        fprintf(stderr, "[nbd-vram] handshake failed\n");
        return -1;
    }
    printf("[nbd-vram] handshake OK, entering transmission mode\n");

    struct bop ops[BATCH_DEPTH_MAX];
    int depth = g_batch_depth;

    while (g_running) {
        /* Block here for the first request: this is the worker's idle wait. */
        struct nbd_req_hdr h;
        if (recv_all(fd, &h, sizeof(h)) != 0) return -1;
        if (ntohl(h.magic) != NBD_REQUEST_MAGIC) {
            fprintf(stderr, "[nbd-vram] bad request magic 0x%x\n", ntohl(h.magic));
            return -1;
        }
        if (ntohs(h.type) == NBD_CMD_DISC) break;

        if (!g_batch_enabled || g_compress) {
            if (handle_one(fd, &h, stream, iobuf) != 0) return -1;
            continue;
        }

        int adm = batch_admit(fd, &h, &ops[0], batchbuf);
        if (adm < 0) return -1;
        if (adm == 0) {                       /* not batchable: legacy path */
            if (handle_one(fd, &h, stream, iobuf) != 0) return -1;
            continue;
        }

        /* Drain whatever else is already queued, without blocking. Idle clients
         * yield a batch of 1 (= legacy behaviour); a busy client supplies depth.
         * The adaptivity is free: we never wait to assemble a batch. */
        int n = 1;
        struct nbd_req_hdr extra;
        int trailer = 0;   /* 0 none, 1 non-batchable header in `extra`, 2 DISC */
        while (n < depth) {
            int g = recv_hdr_nb(fd, &extra);
            if (g < 0) return -1;
            if (g == 0) break;                /* nothing more queued right now */
            if (ntohl(extra.magic) != NBD_REQUEST_MAGIC) {
                fprintf(stderr, "[nbd-vram] bad request magic 0x%x\n", ntohl(extra.magic));
                return -1;
            }
            if (ntohs(extra.type) == NBD_CMD_DISC) { trailer = 2; break; }
            int a = batch_admit(fd, &extra, &ops[n], batchbuf + (size_t)n * BATCH_SLOT);
            if (a < 0) return -1;
            if (a == 0) { trailer = 1; break; }   /* flush, then handle it per-op */
            n++;
        }

        if (flush_batch(fd, ops, n, stream) != 0) return -1;

        if (trailer == 1) { if (handle_one(fd, &extra, stream, iobuf) != 0) return -1; }
        else if (trailer == 2) break;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Thread worker - one per NBD connection slot
 * ---------------------------------------------------------------------- */

static void *thread_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;
    CUstream stream;
    void *iobuf = NULL;
    void *batchbuf = NULL;
    /* PF_MEMALLOC_NOIO/PF_LOCAL_THROTTLE are per-task; pthread inheritance of
     * PR_SET_IO_FLUSHER is undocumented, so set it per worker too (cheap, the
     * failure case is already logged once from main). */
    prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);
    _cuCtxSetCurrent(g_cu_ctx);
    _cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING);
    /* Pinned host memory: eliminates CUDA driver staging copy for each DMA.
     * iobuf serves the per-op (oversized/FLUSH) path; batchbuf holds one slot
     * per in-flight batched request. Both pinned, allocated once per worker. */
    _cuMemAllocHost(&iobuf, IO_BUF_SIZE);
    if (g_batch_enabled)
        _cuMemAllocHost(&batchbuf, (size_t)g_batch_depth * BATCH_SLOT);
    t_cstage = NULL;
    t_cstage_cuda = 0;
    if (g_compress) {
        if (_cuMemAllocHost((void **)&t_cstage, (size_t)COMP_BATCH * COMP_PAGE) == CUDA_SUCCESS) {
            t_cstage_cuda = 1;
        } else {
            t_cstage = calloc(COMP_BATCH, COMP_PAGE);
            if (!t_cstage)
                fprintf(stderr, "[nbd-vram] compress staging alloc failed\n");
        }
    }

    while (g_running) {
        /* Draining (SIGTERM arrived with a client attached): take no new
         * connections, just wait for the remaining clients to detach. The
         * thread that sees the last one go flips g_running for everyone. */
        if (g_term_requested) {
            if (!clients_connected()) {
                g_running = 0;
                printf("[nbd-vram] drain complete - last client gone, exiting\n");
                break;
            }
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        /* Wait for a connection with a 1s timeout so the loop re-checks
         * g_running and exits promptly on SIGTERM (a bare blocking accept()
         * cannot be woken by the signal handler, which hung shutdown for 90s). */
        struct pollfd pfd = { .fd = g_listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0) continue;            /* timeout or EINTR -> re-check g_running */
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        g_client_fds[idx] = cfd;
        printf("[nbd-vram] client connected\n");
        handle_client(cfd, stream, batchbuf, iobuf);
        g_client_fds[idx] = -1;
        close(cfd);
        printf("[nbd-vram] client disconnected\n");
    }

    if (t_cstage) {
        if (t_cstage_cuda) _cuMemFreeHost(t_cstage);
        else               free(t_cstage);
        t_cstage = NULL;
    }
    if (batchbuf) _cuMemFreeHost(batchbuf);
    if (iobuf)    _cuMemFreeHost(iobuf);
    _cuStreamDestroy(stream);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void)
{
    CUdevice  cu_dev;
    int       ret    = 1;

    /* Socket path is fixed in production; VRAM_SOCK_PATH overrides it for
     * non-root testing. Resolved early so every cleanup path sees it. */
    const char *sock_path = getenv("VRAM_SOCK_PATH");
    if (!sock_path || !*sock_path) sock_path = SOCK_PATH;

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Prevent kernel from paging out our own pages - doing so would route
     * the page fault back through this daemon, deadlocking under swap pressure.
     * Requires LimitMEMLOCK=infinity in the systemd service. VRAM_NO_MLOCK=1
     * skips it for unprivileged dev runs (MCL_FUTURE otherwise makes CUDA's huge
     * VA reservation exceed a normal user's memlock limit and cuInit OOMs). */
    if (getenv("VRAM_NO_MLOCK")) {
        fprintf(stderr, "[nbd-vram] VRAM_NO_MLOCK set - skipping mlockall (dev/test only)\n");
    } else if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "[nbd-vram] mlockall failed (%s) - daemon pages may be swapped, risking deadlock\n",
                strerror(errno));
    }

    /* Mark the daemon as part of the I/O flush path. Sets PF_MEMALLOC_NOIO +
     * PF_LOCAL_THROTTLE so allocations made while servicing a swap write do not
     * recurse back into reclaim/writeback - that recursion is the swap-over-NBD
     * deadlock at zero free RAM. Per-process; set before threads spawn. */
    if (prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0) != 0)
        fprintf(stderr, "[nbd-vram] PR_SET_IO_FLUSHER failed (%s) - deadlock risk under swap pressure\n",
                strerror(errno));

    if (load_libcuda() != 0) goto out;

    for (int i = 0; i < 10; i++) {
        CUresult r = _cuInit(0);
        if (r == CUDA_SUCCESS) break;
        if (i == 9) {
            fprintf(stderr, "[nbd-vram] cuInit failed: %s\n", cuda_err(r));
            goto out;
        }
        fprintf(stderr, "[nbd-vram] cuInit attempt %d failed, retrying\n", i + 1);
        sleep(2);
    }

    if (_cuDeviceGet(&cu_dev, 0) != CUDA_SUCCESS) goto out;
    if (_cuCtxCreate(&g_cu_ctx, CU_CTX_SCHED_AUTO, cu_dev) != CUDA_SUCCESS) goto out;

    const char *env = getenv("VRAM_SETUP_SIZE_MB");
    size_t mb = env ? (size_t)atol(env) : DEFAULT_SIZE_MB;

    {
        const char *tenv = getenv("VRAM_NBD_THREADS");
        if (tenv) {
            g_nbd_threads = atoi(tenv);
            if (g_nbd_threads < 1) g_nbd_threads = 1;
            if (g_nbd_threads > NBD_THREADS_MAX) g_nbd_threads = NBD_THREADS_MAX;
        }
        const char *benv = getenv("VRAM_BATCH");
        if (benv) g_batch_enabled = atoi(benv) != 0;
        const char *bdenv = getenv("VRAM_BATCH_DEPTH");
        if (bdenv) {
            g_batch_depth = atoi(bdenv);
            if (g_batch_depth < 1) g_batch_depth = 1;
            if (g_batch_depth > BATCH_DEPTH_MAX) g_batch_depth = BATCH_DEPTH_MAX;
        }
        const char *bgenv = getenv("VRAM_BATCH_DEBUG");
        if (bgenv) g_batch_debug = atoi(bgenv) != 0;
        const char *cenv = getenv("VRAM_COMPRESS");
        if (cenv) g_compress = atoi(cenv) != 0;
        const char *renv = getenv("VRAM_COMPRESS_RATIO");
        if (renv && parse_ratio_tenths(renv, &g_compress_ratio_tenths) != 0) {
            fprintf(stderr, "[nbd-vram] invalid VRAM_COMPRESS_RATIO='%s' (expected 1.0..8.0, one decimal)\n", renv);
            goto out_cuda;
        }
        /* Compressed store is page-mapped, not a 1:1 offset copy; the batch path
         * would write through to the wrong VRAM addresses. */
        if (g_compress) g_batch_enabled = 0;
    }

    /* Back off 512 MiB at a time if the GPU is short on memory (e.g. display compositor loaded) */
    g_vram_ptr = 0;
    while (mb >= 1024) {
        g_vram_size = (mb * 1024ULL * 1024ULL / SIZE_ALIGN) * SIZE_ALIGN;
        printf("[nbd-vram] allocating %llu MiB of VRAM\n",
               (unsigned long long)(g_vram_size >> 20));
        CUresult alloc_r = _cuMemAlloc(&g_vram_ptr, g_vram_size);
        if (alloc_r == CUDA_SUCCESS) break;
        fprintf(stderr, "[nbd-vram] %llu MiB failed (%s), backing off 512 MiB\n",
                (unsigned long long)mb, cuda_err(alloc_r));
        g_vram_ptr = 0;
        mb -= 512;
    }
    if (!g_vram_ptr) {
        fprintf(stderr, "[nbd-vram] all allocation attempts failed\n");
        goto out_cuda;
    }
    printf("[nbd-vram] VRAM at CUDA VA 0x%llx\n", (unsigned long long)g_vram_ptr);

    g_export_size = g_vram_size;
    if (g_compress) {
        g_export_size = (g_vram_size * (uint64_t)g_compress_ratio_tenths) / 10ULL;
        if (compress_init() != 0) goto out_cuda;
        printf("[nbd-vram] compression: lz4 on (ratio %.1fx, %llu MiB VRAM advertised as %llu MiB swap, %llu pages)\n",
               (double)g_compress_ratio_tenths / 10.0,
               (unsigned long long)(g_vram_size >> 20),
               (unsigned long long)(g_export_size >> 20),
               (unsigned long long)g_npages);
    } else {
        printf("[nbd-vram] compression: off (1:1 VRAM mapping, %llu MiB)\n",
               (unsigned long long)(g_vram_size >> 20));
    }

    /* Create Unix socket (path resolved at top of main; VRAM_SOCK_PATH may
     * override it for non-root testing). */
    unlink(sock_path);
    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("socket"); goto out_cuda; }

    {
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
        if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
            { perror("bind"); goto out_cuda; }
    }
    chmod(sock_path, 0600);
    if (listen(g_listen_fd, g_nbd_threads + 1) < 0) { perror("listen"); goto out_cuda; }
    /* non-blocking so the poll()+accept() in each worker never blocks if another
     * worker grabbed the pending connection first */
    fcntl(g_listen_fd, F_SETFL, fcntl(g_listen_fd, F_GETFL, 0) | O_NONBLOCK);

    printf("[nbd-vram] listening on %s (%d threads)\n", sock_path, g_nbd_threads);
    printf("[nbd-vram] request batching %s (depth %d, slot %d KiB)\n",
           g_batch_enabled ? "on" : "off", g_batch_depth, BATCH_SLOT / 1024);

    /* sd_notify READY=1 */
    {
        const char *ns = getenv("NOTIFY_SOCKET");
        if (ns) {
            int nfd = socket(AF_UNIX, SOCK_DGRAM, 0);
            if (nfd >= 0) {
                struct sockaddr_un na = { .sun_family = AF_UNIX };
                const char *p = (ns[0] == '@') ? ns + 1 : ns;
                strncpy(na.sun_path, p, sizeof(na.sun_path) - 1);
                const char *msg = "READY=1\n";
                sendto(nfd, msg, strlen(msg), 0, (struct sockaddr *)&na, sizeof(na));
                close(nfd);
            }
        }
    }

    {
        pthread_t threads[NBD_THREADS_MAX];
        pthread_t st;
        int have_st = 0;
        for (int i = 0; i < g_nbd_threads; i++) g_client_fds[i] = -1;
        if (g_compress) {
            pthread_create(&st, NULL, status_worker, NULL);
            have_st = 1;
        }
        for (int i = 0; i < g_nbd_threads; i++)
            pthread_create(&threads[i], NULL, thread_worker, (void *)(intptr_t)i);
        for (int i = 0; i < g_nbd_threads; i++)
            pthread_join(threads[i], NULL);
        g_running = 0;
        if (have_st) pthread_join(st, NULL);
    }
    ret = 0;

out_cuda:
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    unlink(sock_path);
    if (g_compress) compress_shutdown();
    if (g_vram_ptr) _cuMemFree(g_vram_ptr);
    if (g_cu_ctx)   _cuCtxDestroy(g_cu_ctx);
    if (g_libcuda)  dlclose(g_libcuda);
out:
    if (g_batch_debug) {
        unsigned long batched_path = g_flush_ops;
        unsigned long total = batched_path + g_legacy_ops;
        printf("[nbd-vram] batched-path: %lu flushes, %lu ops, true avg depth %.2f\n",
               g_flush_count, g_flush_ops,
               g_flush_count ? (double)g_flush_ops / (double)g_flush_count : 0.0);
        printf("[nbd-vram]   of those, %lu coalesced (n>1) carrying %lu ops (avg %.1f)\n",
               g_batch_count, g_batch_ops,
               g_batch_count ? (double)g_batch_ops / (double)g_batch_count : 0.0);
        printf("[nbd-vram]   legacy/oversized ops: %lu  (%.1f%% of %lu total R/W requests)\n",
               g_legacy_ops, total ? 100.0 * (double)g_legacy_ops / (double)total : 0.0, total);
    }
    printf("[nbd-vram] exiting (ret=%d)\n", ret);
    return ret;
}
