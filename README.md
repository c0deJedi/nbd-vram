# nbd-vram

## Use your NVIDIA GPU's VRAM as swap space on Linux.

Built for hybrid graphics laptops with soldered memory and no upgrade path. The display runs off the integrated AMD/ATI GPU. The NVIDIA card sits idle most of the time, its VRAM completely unused. This puts that VRAM to work as high-priority swap.

Tested on: AMD/ATI + RTX 3070 Laptop (GA104M, 16 GB RAM, 8 GB VRAM), driver 580.159.03, kernel 6.17, Pop!_OS. Allocated 7 GB for swap. End result including zram and SSD swap: ~46 GB total addressable memory, tripled from stock. Overflow order: RAM fills, then VRAM absorbs the spill (PCIe), then zram compresses the rest (CPU), then SSD only if everything else is exhausted.

![demo](demo.gif)

---

## How it works

A small daemon allocates VRAM via the CUDA driver API, then serves it as a block device using the NBD (Network Block Device) protocol over a Unix socket. The kernel's built-in `nbd` driver connects to it and exposes `/dev/nbdX`. From there it's a normal swap device.

Data path: kernel swap subsystem - /dev/nbdX - nbd kernel driver - Unix socket - nbd-vram daemon - cuMemcpyHtoD/DtoH - GPU VRAM.

No kernel module to write or maintain. No NVIDIA kernel symbols. Survives kernel and driver updates without rebuilding anything.

---

## What it's for

NBD-VRAM is a free, low-wear tier of swap that makes a machine feel better under memory pressure. The benchmarks below don't fully capture why, because what you actually feel day to day is latency. When a process touches a paged-out page it stalls until swap answers: on an NVMe waking from a power-saving sleep that stall is milliseconds, felt as a stutter; on VRAM it is microseconds, with no stutter at all. Same idea as any swap, smoother machine, out of memory that was otherwise sitting idle.

Best of all, there's nothing you need to tune to get it to work as thread count auto-scales to your CPU.

Three main situations where you can benefit from NBD-VRAM:

**Sporadic pressure** a background app paging in, or switching back to something you left open hours ago. Single faults answered in ~250 us instead of milliseconds, so no stutter.

**Concurrent pressure** parallel compiles (`make -j`), dozens of browser tabs, VMs, data jobs spilling RAM. Many CPUs faulting at once, where the daemon's threads fan the I/O across all its connections and keep up.

**Spare your SSD** swap is write-heavy, and sending that churn to VRAM instead of finite NAND saves write cycles, which matters most on the soldered-storage laptops this is built for.

---

## Why not the NVIDIA P2P API?

The "obvious" approach is `nvidia_p2p_get_pages_persistent`, which pins VRAM pages in BAR1 so the CPU can access them directly via `ioremap_wc`. Every existing project that tried this route hits the same wall: the NVIDIA driver returns `EINVAL` on consumer GeForce GPUs. Both the persistent and non-persistent variants, both flag values. It's gated at the RM level for Quadro/datacenter SKUs only, regardless of driver version.

The other approach - directly `ioremap_wc` the BAR1 physical address without going through the P2P API - also doesn't work. The GPU's internal page tables only have ~16 MiB of BAR1 mapped (just the display framebuffer). Reads from the rest return zeros. `mkswap` appears to succeed, then `swapon` fails because the swap header isn't actually there.

The NBD approach sidesteps all of this. `cuMemcpyHtoD` and `cuMemcpyDtoH` work on any CUDA GPU without any special permissions.

---

## Requirements

- NVIDIA GPU with CUDA support (any consumer RTX/GTX card)
- NVIDIA driver with `libcuda.so.1` (no CUDA toolkit needed)
- Linux kernel 5.6+ recommended, since that is where `PR_SET_IO_FLUSHER` landed and the swap-deadlock protection leans on it, though older kernels still run with that one safeguard disabled (the `nbd` module itself is built into most distros)
- `nbd-client` package
- `gcc`, `make`
- `liblz4.so.1` only for `VRAM_COMPRESS=lz4` (`liblz4-1` / `lz4-libs`)
- `libzstd.so.1` only for `VRAM_COMPRESS=zstd` or `zstd:LEVEL` (`libzstd1` / `libzstd`)

---

## Install

```sh
git clone https://github.com/c0dejedi/nbd-vram
cd nbd-vram
sudo ./install.sh
sudo systemctl start vram-swap-nbd
```

Verify:

```sh
swapon --show
# NAME       TYPE      SIZE USED PRIO
# /dev/nbd0  partition   7G   0B 1500
```

The installer asks whether to enable the service at boot, defaulting to its current setting (no on a fresh install). Non-interactive installs preserve that setting. The installer also starts or restarts the service immediately, regardless of the boot-start choice.

---

## Configuration

Edit `/etc/systemd/system/vram-swap-nbd.service`:

```ini
Environment=VRAM_SETUP_SIZE_MB=7168    # how much VRAM to use
Environment=VRAM_SWAP_PRIORITY=1500    # swap priority (higher = used first)
Environment=VRAM_NBD_THREADS=2         # worker threads; default favors desktop responsiveness
Environment=VRAM_NBD_CONNECTIONS=2     # nbd connections; keep equal to threads
Environment=VRAM_CUDA_SCHED=blocking   # blocking (default), or auto for comparison
Environment=VRAM_COMPRESS=off          # off, lz4, zstd, or zstd:LEVEL (1-22)
Environment=VRAM_DEDUP=0              # 1 = share identical pages (optional)
Environment=VRAM_COMPRESS_RATIO=2.0    # logical swap size / VRAM when compression or dedup is enabled (1.0-8.0)
```

The daemon tries the requested size first and backs off in 512 MiB steps if the GPU is short on memory - so it will grab as much as it can even if the display compositor is already loaded. `VRAM_SETUP_SIZE_MB` is the ceiling, not a hard requirement.

`VRAM_NBD_THREADS` / `VRAM_NBD_CONNECTIONS` default to **2** and should match each other (range 1–64). More connections allow more simultaneous swap I/O, but can compete with the compositor for CPU time, GPU memory bandwidth, and device queues. Fresh installs use 2; interactive and non-interactive reinstalls preserve valid counts from the existing service file. Missing or invalid counts use the template default. Settings in systemd overrides are also retained; use `systemctl edit vram-swap-nbd.service` to keep custom settings separate from the installed unit.

### Desktop responsiveness

The defaults favor small bursts and fewer concurrent transfers. CUDA synchronization uses `VRAM_CUDA_SCHED=blocking` so waiting workers sleep; CUDA's `auto` policy can spin on the CPU. See [NVIDIA's synchronization documentation](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__STREAM.html). `VRAM_CUDA_SCHED=auto` restores the previous wait policy for comparison. This setting controls CPU waiting, not graphics priority or copy-engine preemption.

With batching enabled, a worker collects at most `VRAM_BATCH_DEPTH` complete requests (default **8**, maximum 256) and **64 KiB total payload**, whichever comes first. It never waits for an incomplete additional header or write payload. Direction changes finish the current batch and send its replies first, so a read is answered before later writes are submitted. Large requests also transfer in 64 KiB chunks, completing each chunk before submitting the next. The daemon reserves 64 KiB for the batch buffer and 64 KiB for the per-request buffer per worker, plus codec staging when enabled.

These limits bound submitted work, not wall-clock latency; GPU scheduling and system memory pressure still matter. They may reduce maximum throughput. Compare pointer/compositor smoothness and high-percentile swap latency under the same workload before increasing concurrency. For an existing installation, apply the two connection settings above as well as installing the new binary. On a GPU driving the display, leave enough VRAM free for the compositor and applications: successfully allocating swap VRAM alone does not establish that the remaining headroom is sufficient.

### Compression

Off by default (`VRAM_COMPRESS=off`). Set `VRAM_COMPRESS=lz4` or `VRAM_COMPRESS=zstd:3` to pack compressed 4K pages into the CUDA allocation, and advertise a larger NBD device (`VRAM_COMPRESS_RATIO` times the VRAM size, default 2.0x). `VRAM_COMPRESS_RATIO` accepts `X` or `X.Y` with one decimal place (range `1.0` to `8.0`, for example `2.5`). Same-filled pages (zeros) take no VRAM. Typical anonymous memory is around 2–3× with lz4, so 7 GiB of VRAM can back on the order of 14–21 GiB of swap if the data compresses; if it does not, writes return `ENOSPC` and the kernel can fall through to lower-priority swap (zram/SSD).

This is not zswap (which caches compressed pages in system RAM) and not zram (which is RAM-backed). The compressed bytes live in VRAM. Compression adds CPU work per page fault. LZ4 requires `liblz4.so.1` (`liblz4-1` on Debian/Pop!_OS, `lz4-libs` on Fedora); Zstandard requires `libzstd.so.1` (`libzstd1` on Debian/Pop!_OS, `libzstd` on Fedora). Only the selected library is loaded; no codec development headers are needed to build. The installer prompts for this; after a manual edit, `sudo systemctl daemon-reload && sudo systemctl restart vram-swap-nbd`.

Use `VRAM_COMPRESS=zstd:LEVEL` for a Zstandard level from **1 to 22**, for example:

```ini
Environment=VRAM_COMPRESS=zstd:3
Environment=VRAM_COMPRESS_RATIO=2.0
```

`zstd` alone defaults to level 3. Legacy `0` (off) and `1` (lz4) remain accepted; the installer saves the named values. Higher levels spend more CPU on compression and can increase swap-write latency; the level does not change the advertised device size. `VRAM_COMPRESS_RATIO` controls that size separately. Invalid codec names or levels fail startup. Pages that do not shrink are stored uncompressed. The codec and level are fixed until the service restarts; the installer preserves them on reinstall.

When compression or deduplication is on, `swapon --discard=pages` is used so freed swap slots TRIM and return VRAM to the pool. Without discard the pool would leak until those offsets are overwritten.

The packed store reserves and touches its page table, slab records, and slot bitmaps at startup. Slab metadata uses about 4.3 MiB of system RAM per GiB of VRAM; creating or recycling slabs during I/O does not allocate host memory. Each size class keeps a list of slabs with free slots. If its own slots and all unused slabs are exhausted, a page can use a free slot from a larger class before returning `ENOSPC`. Accounting and TRIM use the actual slot size. This fallback helps with fragmentation but cannot combine smaller holes into a larger slot.

Request batching also works with compression and deduplication. `VRAM_BATCH=1` (default) uses the request and byte limits above. Aligned requests of the same direction share device synchronization; overlapping pages, colliding lock stripes, and partial requests split storage groups. FLUSH, TRIM, and oversized requests finish earlier batches first. `VRAM_BATCH=0` processes requests individually while retaining the 64 KiB transfer chunks.

Readers share page locks; writes and TRIM take exclusive locks. Batches acquire lock stripes in a fixed order to avoid deadlocks across connections, and full-page compression happens before taking those locks. A successful write is visible to other connections before its reply is sent. Failed transfers are drained before reservations are recycled, and failed write groups preserve their previous mappings.

With dedup off, full-page writes also reserve slots and finish GPU uploads before acquiring page locks. Other connections can read the existing data during that upload, even when their pages share a lock stripe. At commit, the writer replaces the current mapping under the lock; an intervening overwrite or TRIM is accounted for at that point. Partial writes retain the lock throughout read/modify/write, and dedup writes retain their existing locking. Readers release page locks as soon as downloads finish and decompress from private staging buffers, including for partial reads.

Check the live ratio with `nbd-vram-compression-status.sh` (reads `/run/nbd-vram.status`, updated about once a second). It shows the codec and level, configured vs effective ratio and whether raising `VRAM_COMPRESS_RATIO` is likely to help. Run it after the machine has actually swapped — empty or all-zero pages inflate the number.

After changing, run `sudo systemctl daemon-reload && sudo systemctl restart vram-swap-nbd`.

### Page deduplication

Set `VRAM_DEDUP=1` to store identical 4 KiB pages once, with reference counting:

```ini
Environment=VRAM_COMPRESS=zstd:3
Environment=VRAM_DEDUP=1
Environment=VRAM_COMPRESS_RATIO=2.0
```

Off by default. The installer offers `Page deduplication (off, on)` and preserves the setting on reinstall. Deduplication works with LZ4, Zstandard, or `VRAM_COMPRESS=off`; raw pages that do not compress can also share storage. With deduplication alone, `VRAM_COMPRESS_RATIO` still controls logical device size. Changes take effect after a service restart.

A hash finds candidates, then the daemon reads the candidate from VRAM and compares all 4096 original bytes. Pages already prepared in the same batch can be compared directly in host memory. Hash collisions cannot merge different pages. New unique payloads upload together with one synchronization before their mappings are published; matching existing VRAM objects still requires downloads and synchronization. Shared payloads are immutable: overwriting or partially modifying a page changes only its own reference. TRIM or overwrite frees a payload when its final reference is removed. Same-filled pages already take no VRAM and are excluded from deduplication counts.

`nbd-vram-compression-status.sh` adds:

- **deduped pages**: current extra copies avoided. Three pages sharing one payload count as two deduped pages. This is a subset of the existing page totals, not an additional page type.
- **dedup savings**: object-slot bytes avoided after compression, including size-class rounding. This does not imply the same number of whole slabs became free.
- **dedup matches**: successful page writes that reused an existing payload since startup, including rewriting a page with identical contents. Failed batches do not increase this counter.
- **dedup index RAM**: reserved memory for the lookup table, per-page references, and the entire payload-record pool, including unused records (excluding allocator overhead).
- **dedup net savings**: VRAM object storage saved minus system RAM used by the index, displayed in MiB. Can be negative when the index costs more than deduplication saves.

Deduplication adds CPU hashing, VRAM reads to verify candidates, and host RAM metadata. Dedup write commits are serialized while verifying and updating references; ordinary reads still run in parallel. These costs can reduce throughput, so enable it when repeated pages save enough space.

On 64-bit systems, the index reserves 8 bytes per logical page, a hash table of up to 8 MiB, and 48 bytes per payload object in a pool allocated at startup. The pool holds at most one object per logical page plus 32 pending replacements, capped by the number of payloads that can physically fit in VRAM (4096-byte slots without compression, 32-byte slots with compression). Objects return to this pool on TRIM, overwrite, or rollback; writes do not allocate or free dedup metadata from the host heap. The startup log and `dedup index RAM` status report the full reservation, including unused objects. This moves the RAM cost to startup, where allocation failure prevents the daemon from starting, and the existing memory locking keeps the metadata resident.

---

## Power management

The installer asks whether to enable power-aware management on first install. If enabled, the service automatically stops when you unplug from AC (or when battery drops below a threshold), and restarts when power is restored. Manual `systemctl stop` is always respected and won't be overridden.

To change settings after install, edit `/etc/nbd-vram.conf`. Changes take effect on the next poll (within 60 seconds) or immediately on the next AC plug/unplug event.

---

## Suspend / resume

The installer enables `vram-swap-nbd-suspend.service`, which tears the swap down before the machine sleeps and brings it back when it wakes. This is required for correctness: when the system suspends, the NVIDIA driver powers down the GPU and destroys the CUDA context backing the swap device, so any swap I/O still in flight blocks forever and the machine can hang on resume. The unit is ordered ahead of both systemd's sleep services and nvidia's power-down hooks, so the swapoff runs while the GPU is still alive, the VRAM is freed before it powers off, and a fresh context is built on resume. If swap was already stopped (manually or by power management) it is left stopped. On a machine that never actually suspends, the hook simply never fires.

Please note: if swap is heavily used and the paged-out data can't fit back into RAM, the pre-sleep `swapoff` fails (the same ENOMEM case that's deliberately not forced, to avoid a panic), and the machine may still hang on resume. There's no way around evacuating VRAM into a RAM that can't hold it - free some memory before suspending.

---

## Using the GPU at the same time

NBD-VRAM uses your VRAM, so while it is active that memory is not available to anything else on the card. The installer asks how much to allocate and suggests an amount based on your setup: if this card does not drive a display (a hybrid laptop, or a workstation with a separate display GPU) it recommends nearly all of the VRAM, since the card is otherwise idle; if it does drive your display, it leaves headroom for the desktop and games. Whatever you choose, a GPU app started afterwards only gets the slice that is left, and if memory is being swapped while the GPU is busy the card does both jobs at once - rendering and serving swap copies - and neither is happy.

If you want to use the GPU heavily at the same time, pick one:

**Leave headroom** choose a smaller allocation when the installer asks, or change `VRAM_SETUP_SIZE_MB` in `/etc/systemd/system/vram-swap-nbd.service` later (e.g. `4096` keeps 4 GB for the GPU).

The installer lets you exceed its recommendation while leaving 1024 MiB for the GPU. For example, a 4096 MiB GPU can allocate 3072 MiB to swap, including when it drives a display.

**Stop it while you need the card** `sudo systemctl stop vram-swap-nbd`, then start it again afterwards. Swapped pages migrate back to RAM and other swap first.

This is the natural trade-off of swapping to VRAM: it is free memory, right up until you want the GPU for something else.

---

## Smoke test (without installing)

CPU-only allocator, compression, and deduplication regression tests (requires both runtime codec libraries and Python 3):

```sh
make test
```

These use simulated VRAM and do not activate swap. They cover larger-slot fallback, allocator churn, metadata allocation failures at startup, rollback, and storage operations with metadata heap calls forbidden. Batching tests defer device copies until synchronization, inject transfer failures, exercise concurrent readers/writers/TRIM, and drive the NBD handshake and request loop over local socket pairs. Both direct and packed storage are covered, including byte limits, large-write buffer lifetime, replies before later writes, fragmented headers/payloads, request ordering, and visibility across connections. Tests pause uploads and decompression to check reader progress, mapping changes during an upload, and safe slot reuse after a download. The socket tests require permission to send and receive over local sockets.

### GPU contention benchmark

```sh
make bench-contention BENCH_ARGS='lz4 10000 alias'
make bench-contention BENCH_ARGS='lz4 10000 disjoint'
```

This standalone benchmark owns 16 MiB of VRAM on GPU 0 and runs a 4 KiB reader alongside a writer issuing 64 KiB batches, with dedup off and blocking CUDA waits. It does not attach an NBD device or change the running swap service. It requires GPU access and the selected codec library; `zstd:1` and other supported Zstd levels also work. The `alias` case uses different pages sharing lock stripes; `disjoint` provides a control using different stripes. Each run stops after the requested read count or 10 seconds (with a 30-second watchdog).

It reports p50/p99/p99.9/max read latency, counts above 1/5/10 ms, and reader/writer throughput. Results use synthetic compressible pages and measure the storage functions directly: they exclude NBD queuing, kernel reclaim, and desktop frame timing. To compare revisions, compile this same benchmark with `NBD_VRAM_SOURCE` defined as a quoted path to the other revision's `nbd-vram.c`.

### GPU/NBD smoke test

For a test that attaches an NBD device and activates swap:

```sh
sudo bash test-nbd.sh
```

Allocates VRAM, connects the NBD device, does a 1 MiB write/readback check, activates swap, then prints teardown instructions. `install.sh` handles teardown automatically if a test instance is running.

To stress the full partition after the smoke test passes:

```sh
sudo bash test-fill.sh
```

Writes the entire VRAM partition with zeros, verifies a sample read back, then auto-restores swap on exit.

---

## Memory safety

The daemon backs a swap device, which creates two subtle deadlock risks under heavy memory pressure. Both froze early builds; both are now handled. Together they are why the daemon stays responsive while the entire VRAM swap fills at zero free RAM.

1. The kernel must never page out the daemon's own memory, or a fault on it would route back through the busy daemon and hang. The daemon pins all its pages in RAM with `mlockall(MCL_CURRENT | MCL_FUTURE)`.

2. While serving a swap write the daemon still needs to allocate memory, and at zero free RAM that allocation would normally trigger reclaim - which is itself waiting on the very write the daemon is doing. The daemon marks itself with `prctl(PR_SET_IO_FLUSHER)` (the same mechanism the kernel uses for the nbd socket, and what NFS-Ganesha and libfuse use) so its allocations never fall into that trap. It also runs with `OOMScoreAdjust=-1000`, so the kernel never kills it under pressure.

The daemon is multi-threaded - one worker and one connection per CPU - so it keeps up with concurrent swap traffic instead of saturating and stalling the system.

---

## Performance

Tested on RTX 3070 Laptop (8 GB VRAM), Ryzen 9 5900HX, kernel 6.17, Pop!_OS, against NVMe cryptswap (dm-crypt, PCIe 4.0). O_DIRECT. Each test was run 3 times; the numbers and the gif for each are from a representative (median) run.

NBD-VRAM turns otherwise-idle VRAM into a fast, zero-wear tier of swap. Its strengths are the ones that matter for everyday use: microsecond latency for the sporadic page faults that make a machine feel laggy, no SSD wear, and stable behaviour under heavy pressure. It sits above your SSD swap in priority, so it absorbs pressure first - for free.

Run any of these yourself (state is restored on exit; `fio`/`ioping` auto-install):

```sh
sudo bash benchmarks/bench-latency.sh         # per-operation latency
sudo bash benchmarks/bench-iops-parallel.sh   # 4K IOPS under concurrent load
sudo bash benchmarks/bench-iops.sh            # 4K IOPS, light/sporadic access
sudo bash benchmarks/bench-throughput.sh      # sequential dd
sudo bash benchmarks/bench-pressure.sh        # survival under heavy pressure
```

---

### Latency

![bench-latency](benchmarks/bench-latency.gif)

| Device | min | avg | max |
|--------|-----|-----|-----|
| NVMe | 115 us | 8.7 ms | 10.1 ms |
| NBD-VRAM | 90 us | **257 us** | 437 us |

**About 34x lower average latency.** An NVMe drive sleeps between sporadic requests (APST power saving) and wakes cold almost every time, paying a multi-millisecond penalty. VRAM has no power states - it answers in microseconds, every time. This is the case that dominates real desktop use: memory pressure is usually individual 4K page faults arriving seconds apart, each one stalling a process until swap responds. At ~9 ms per fault you feel it; at 257 us you don't.

---

### Concurrent 4K IOPS

![bench-iops-parallel](benchmarks/bench-iops-parallel.gif)

Under concurrent pressure - every CPU faulting at once, as in a parallel build or a wall of browser tabs - the daemon's worker threads spread requests across all its connections:

| Device | IOPS | bandwidth |
|--------|------|-----------|
| NVMe | 240k | 936 MiB/s |
| NBD-VRAM | **312k** | 1219 MiB/s |

Multi-threading is what gets NBD-VRAM here: single-threaded it manages ~77k on this workload, multi-threaded ~312k - a ~4x gain, and well past what swap demand needs. NVMe's concurrent IOPS swings hard with drive temperature: throttled under this sustained run it fell to 240k (below NBD-VRAM), while a cool drive measures far higher (~860k). NBD-VRAM holds steady around 310k either way.

---

### Single-stream 4K IOPS

![bench-iops](benchmarks/bench-iops.gif)

| Device | read IOPS | write IOPS | bandwidth |
|--------|-----------|------------|-----------|
| NVMe | 59k | 59k | 229 MiB/s |
| NBD-VRAM | 42k | 42k | 165 MiB/s |

One process faulting at a time - the light case. Only one request is ever in flight, so thread count makes no difference here, and both devices are far quicker than sporadic access needs.

---

### Sequential throughput

![bench-throughput](benchmarks/bench-throughput.gif)

| Device | write | read |
|--------|-------|------|
| NVMe | 2.8 GB/s | 3.1 GB/s |
| NBD-VRAM | 1.9 GB/s | 2.7 GB/s |

Big sequential streams are an NVMe's home turf, and they are not how swap behaves - the kernel moves random 4K pages, not multi-megabyte streams. Shown for completeness; ~2 GB/s is plenty for swapping a stream back in.

---

### Surviving heavy pressure

![bench-pressure](benchmarks/bench-pressure.gif)

Fill the entire 7 GB VRAM swap with RAM at zero free, and the machine stays responsive - no freeze, no deadlock. Earlier single-threaded builds hard-froze here; the multi-threaded daemon plus the two safeguards in [Memory safety](#memory-safety) keep it alive.

---

### GPU under load stress test

The nastier test I ran was using the GPU *while* it was swapping: a 3D render plus a CUDA compute load on the NVIDIA card, with RAM driven to zero so swap floods the same VRAM. It degrades gracefully rather than falling over - the GPU app keeps rendering (the card pegged near 100%), the daemon keeps serving swap, and the machine stays usable, just laggy. Nothing crashed.

The only hard limit is capacity, not stability: while the daemon holds its VRAM, a GPU app gets only what is left, so a large allocation simply fails to start. That is a tuning question, not a crash - see [Using the GPU at the same time](#using-the-gpu-at-the-same-time).

---

### SSD wear

Swap is write-heavy, and SSD NAND has a finite number of write cycles. Sending that churn to VRAM (DRAM-like, no wear) instead of your SSD spares its endurance - which matters most on exactly the soldered-everything laptops this is built for.

---

## Uninstall

```sh
sudo bash uninstall.sh
```

---

## Failure Recovery
**If** you run into trouble booting at any point, simply switch to a TTY session and run the uninstall script `sudo bash uninstall.sh` from the path you originally installed NBD-VRAM from.

That will get you back to a working state and allow you the opportunity to file an [issue](https://github.com/c0deJedi/nbd-vram/issues/new) for me to take a look at.

Please include as much information as possible in your bug report eg: system specifications, display adapter configurations (single GPU, hybrid, modes etc.) and concise logs where possible.

---

## License

MIT - Sean Lobjoit (c0dejedi)
