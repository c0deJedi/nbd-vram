all: nbd-vram

nbd-vram: nbd-vram.c
	gcc -O2 -Wall -o nbd-vram nbd-vram.c -ldl -lpthread

clean:
	rm -f nbd-vram

install: nbd-vram
	@echo "Use install.sh for full installation"

# Private 16 MiB GPU allocation; does not attach a device or change live swap.
# Example: make bench-contention BENCH_ARGS='lz4 10000 alias'
BENCH_ARGS ?= lz4 10000 alias
.PHONY: bench-contention
bench-contention:
	@set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	$(CC) -O2 -g -Wall -Wextra -Werror $(CFLAGS) -o "$$tmp/bench-contention" benchmarks/bench-contention.c -ldl -lpthread; \
	"$$tmp/bench-contention" $(BENCH_ARGS)

# Real codec libraries, simulated CUDA memory; no root, GPU, or active swap needed.
.PHONY: test
test:
	@set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	$(CC) -O2 -g -Wall -Wextra -Werror $(CFLAGS) -o "$$tmp/test-allocator" test-allocator.c -ldl -lpthread; \
	"$$tmp/test-allocator"; \
	$(CC) -O2 -g -Wall -Wextra -Werror $(CFLAGS) -o "$$tmp/test-compression" test-compression.c -ldl -lpthread; \
	for codec in 1 lz4 zstd zstd:1 zstd:9 zstd:22; do "$$tmp/test-compression" "$$codec"; done; \
	$(CC) -O2 -g -Wall -Wextra -Werror $(CFLAGS) -o "$$tmp/test-dedup" test-dedup.c -ldl -lpthread; \
	for codec in off 0 lz4 zstd:3 zstd:22; do "$$tmp/test-dedup" "$$codec"; done; \
	$(CC) -O2 -g -Wall -Wextra -Werror $(CFLAGS) -o "$$tmp/test-batching" test-batching.c -ldl -lpthread; \
	for codec in lz4 zstd:3; do for dedup in 0 1; do "$$tmp/test-batching" "$$codec" "$$dedup"; done; done; \
	"$$tmp/test-batching" off 1; \
	"$$tmp/test-batching" off 0; \
	python3 test-storage-scripts.py
