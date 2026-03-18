# dimg-tool Roundtrip Tests

## Test Date: 2026-03-18

## Setup

- libaaruformat: upstream git clone (alpha.33, 2025-03), patched with zstd (kCompressionZstd = 4)
- zstd: libzstd 1.5.7
- Build: gcc, shared lib, x86_64 Linux
- Method: CUE/BIN → .aaru (write_sector_long, 2352-byte raw sectors) → read back → memcmp
- All tests: FLAC for audio tracks, configurable codec for data tracks
- Deduplication enabled (DDT) for all tests
- Test media: Sega Dreamcast GD-ROM disc images (CUE/BIN from redump)

## Size Comparison

### Disc A — 3 tracks (Mode1/Audio/Mode1), 510,983 sectors, dedup-heavy (mostly empty sectors)

| Format | Size | vs Original | Roundtrip |
|--------|------|-------------|-----------|
| CUE/BIN (original) | 1,146.2 MiB | — | — |
| 7z LZMA | 38.0 MiB | 3.3% | lossless |
| .aaru LZMA | 14.3 MiB | 1.2% | PASS |
| .aaru zstd-19 | 16.0 MiB | 1.4% | PASS |
| .aaru zstd-22 | 15.9 MiB | 1.4% | PASS |

### Disc B — 3 tracks (Mode1/Audio/Mode1), 519,777 sectors, dense multilingual data

| Format | Size | vs Original | Roundtrip |
|--------|------|-------------|-----------|
| CUE/BIN (original) | 1,165.9 MiB | — | — |
| 7z LZMA | 944.6 MiB | 81.0% | lossless |
| .aaru LZMA | 801.3 MiB | 68.7% | PASS |
| .aaru zstd-19 | 815.6 MiB | 70.0% | PASS |
| .aaru zstd-22 | 815.6 MiB | 70.0% | PASS |

### Observations

- All .aaru variants beat 7z thanks to DDT sector deduplication + FLAC for audio tracks
- Disc A: extreme dedup (mostly empty/zeroed sectors) — .aaru LZMA 2.7x smaller than 7z
- Disc B: dense data — .aaru LZMA still 15% smaller than 7z
- LZMA vs zstd: LZMA wins by ~1.7 MiB (dedup-heavy) or ~14 MiB (dense data)
- zstd-19 vs zstd-22: negligible size difference — level 19 is the sweet spot
- zstd decompression ~5.5x faster than LZMA (relevant for batch rendering and random access)

## Random Access & Emulator Viability

Block size is 4096 sectors (~8 MiB for CD). Reading 1 random sector requires decompressing the
entire block on a cache miss. The LRU cache (512 MiB) holds ~64 decompressed blocks.

### Random sector read latency (cold cache, 1000 random LBAs)

| Image | zstd-19 | LZMA |
|---|---|---|
| Disc A (dedup-heavy) | 0.07 ms/sector | 0.46 ms/sector |
| Disc B (dense data) | 0.94 ms/sector | 25.5 ms/sector |

### Sequential burst (100 consecutive sectors, warm block)

Both codecs: 0.008 ms/sector — pure memory copy from cached block.

### Full disc preload (sequential read of all sectors)

| Image | zstd-19 | LZMA |
|---|---|---|
| Disc A (1,146 MiB) | 4.0 s (283 MiB/s) | 4.4 s (258 MiB/s) |
| Disc B (1,166 MiB) | 5.0 s (232 MiB/s) | 30.0 s (39 MiB/s) |

### Assessment

- **zstd random access is viable for emulation**: worst case 0.94 ms fits within a 16 ms frame
- **LZMA is not viable**: 25 ms/sector on dense data exceeds frame budget
- **Full preload strategy**: with zstd an emulator can decompress an entire GD-ROM (~1 GiB)
  into RAM in ~5 seconds — practical as a loading screen. After preload, all reads are
  0.008 ms (pure memory). Dreamcast high-density area fits easily in modern RAM (16+ GB).
- **Block cache covers most access patterns**: games read sequentially within files,
  so most reads are cache hits. Cache misses only occur when seeking to a new disc region.

## Notes

- All tracks written as raw 2352-byte sectors via `aaruf_write_sector_long`
- Compression: FLAC for audio tracks (automatic), LZMA or zstd for data tracks
- No subchannel data preserved in this test (CUE/BIN doesn't carry it)
- Media type set to GDROM (152)
- zstd uses kCompressionZstd = 4 (pending upstream allocation)

## What's NOT tested yet

- Subchannel data preservation (would need .sub files or raw dumps)
- GDI format input (different track layout descriptor)
- CHD input/output and size comparison
- Multi-session disc handling
- Lead-in/lead-out sectors (negative sector addresses)
