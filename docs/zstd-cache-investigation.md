# Zstd compression investigation for preloaded file contents

The initial investigation mistakenly focused on compressing the persisted state
file. The question was really about whether the **files that preload pulls into
memory** can be compressed. This follow-up looks at that path.

## How preload warms the cache today
- `preload_readahead()` walks the `preload_map_t` entries chosen by the
  predictor, sorts them, and then opens each path with `open()` before issuing
  the `readahead()` system call. This pushes file pages into the kernel page
  cache exactly as they exist on disk; no transformation happens in userspace.
  【F:src/readahead.c†L109-L148】【F:src/readahead.c†L200-L235】

## Can we zstd-compress those in-memory pages?

- The kernel page cache owns the memory that `readahead()` populates. Userland
  consumers like preload do not get a hook to swap that memory for compressed
  buffers; only the kernel decides the caching policy. Adding zstd here would
  therefore require a kernel feature, not an application change.
- Filesystem-level compression (e.g., Btrfs or F2FS with zstd) **already stores
  the on-disk blocks in compressed form**, and the kernel transparently
  decompresses into the page cache when `readahead()` runs. Preload cannot
  influence this beyond respecting whatever compression the filesystem uses.
- Swap/page-cache compression stacks such as zswap or zram can reduce memory
  pressure for cached pages, but they are configured globally at the kernel
  level and are independent of preload. Preload cannot opt specific files into
  a compressed cache on its own.

## Practical options
- If the goal is to reduce disk I/O bandwidth: rely on filesystem compression
  (Btrfs/F2FS zstd, bcachefs, etc.). Preload will simply warm already-compressed
  blocks and benefit from reduced read sizes.
- If the goal is to reduce memory footprint of warmed pages: configure a kernel
  cache-compression mechanism (zswap/zram-backed swap) or accept that cache
  residency is uncompressed. No userspace-only change to preload can make the
  cache itself zstd-compressed.
