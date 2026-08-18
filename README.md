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
- `liblz4.so.1` only if you enable `VRAM_COMPRESS=1` (`liblz4-1` / `lz4-libs`)

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

The service is enabled on install, so it comes up automatically on every boot.

---

## Configuration

Edit `/etc/systemd/system/vram-swap-nbd.service`:

```ini
Environment=VRAM_SETUP_SIZE_MB=7168    # how much VRAM to use
Environment=VRAM_SWAP_PRIORITY=1500    # swap priority (higher = used first)
Environment=VRAM_NBD_THREADS=8         # worker threads; install.sh sets this to nproc
Environment=VRAM_NBD_CONNECTIONS=8     # nbd connections; keep equal to threads
Environment=VRAM_COMPRESS=0            # 1 = lz4-compress pages in VRAM (needs liblz4)
Environment=VRAM_COMPRESS_RATIO=2.0    # logical swap size / VRAM when compress=1 (1.0-8.0)
```

The daemon tries the requested size first and backs off in 512 MiB steps if the GPU is short on memory - so it will grab as much as it can even if the display compositor is already loaded. `VRAM_SETUP_SIZE_MB` is the ceiling, not a hard requirement.

`VRAM_NBD_THREADS` / `VRAM_NBD_CONNECTIONS` are auto-set to `nproc` at install and should match each other. More connections let the daemon drain concurrent swap I/O in parallel; the benefit saturates around your physical core count, and single-stream workloads do not use it at all (see Performance).

### Compression

Off by default. Set `VRAM_COMPRESS=1` to pack lz4-compressed 4K pages into the CUDA allocation, and advertise a larger NBD device (`VRAM_COMPRESS_RATIO` times the VRAM size, default 2.0x). `VRAM_COMPRESS_RATIO` accepts `X` or `X.Y` with one decimal place (range `1.0` to `8.0`, for example `2.5`). Same-filled pages (zeros) take no VRAM. Typical anonymous memory is around 2–3× with lz4, so 7 GiB of VRAM can back on the order of 14–21 GiB of swap if the data compresses; if it does not, writes return `ENOSPC` and the kernel can fall through to lower-priority swap (zram/SSD).

This is not zswap (which caches compressed pages in system RAM) and not zram (which is RAM-backed). The compressed bytes live in VRAM. Cost is extra CPU per page fault and the loss of the 1:1 copy batching path. Requires `liblz4.so.1` (`liblz4-1` on Debian/Pop!_OS, `lz4-libs` on Fedora). The installer prompts for this; after a manual edit, `sudo systemctl daemon-reload && sudo systemctl restart vram-swap-nbd`.

When compression is on, `swapon --discard=pages` is used so freed swap slots TRIM and return VRAM to the pool. Without discard the pool would leak until those offsets are overwritten.

Check the live ratio with `nbd-vram-compression-status.sh` (reads `/run/nbd-vram.status`, updated about once a second). It shows configured vs effective ratio and whether raising `VRAM_COMPRESS_RATIO` is likely to help. Run it after the machine has actually swapped — empty or all-zero pages inflate the number.

After changing, run `sudo systemctl daemon-reload && sudo systemctl restart vram-swap-nbd`.

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

**Stop it while you need the card** `sudo systemctl stop vram-swap-nbd`, then start it again afterwards. Swapped pages migrate back to RAM and other swap first.

This is the natural trade-off of swapping to VRAM: it is free memory, right up until you want the GPU for something else.

---

## Smoke test (without installing)

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
