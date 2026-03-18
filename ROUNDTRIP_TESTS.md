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

## Multi-System Results

Tested with one disc per system, CUE/BIN source format (redump).

| System | Tracks (data/audio) | Original | .aaru LZMA | .aaru zstd-19 | Roundtrip |
|--------|---------------------|----------|------------|---------------|-----------|
| Dreamcast (dedup-heavy) | 3 (2D/1A) | 1,146 MiB | 14.3 MiB (1.2%) | 16.0 MiB (1.4%) | PASS |
| Dreamcast (dense) | 3 (2D/1A) | 1,166 MiB | 801.3 MiB (68.7%) | 815.6 MiB (70.0%) | PASS |
| Sega Saturn | 21 (2D/19A) | 524 MiB | 315.7 MiB (60.2%) | 324.8 MiB (62.0%) | PASS |
| Mega CD | 35 (1D/34A) | 574 MiB | 296.8 MiB (51.7%) | 298.6 MiB (52.0%) | PASS |
| PC Engine CD | 22 (2D/20A) | 496 MiB | 255.1 MiB (51.4%) | 255.3 MiB (51.4%) | PASS |
| Neo Geo CD | 41 (1D/40A) | 694 MiB | 335.5 MiB (48.4%) | 336.6 MiB (48.5%) | PASS |

## API Notes

### Sector status handling

The library marks all-zero sectors as `SectorStatusNotDumped` internally. This is correct
behavior — the data IS preserved (zeros in, zeros out), the status is metadata indicating
the sector had no content on the original disc.

When reading back via `aaruf_read_sector_long`:
- `return 0` (AARUF_STATUS_OK): sector has data, buffer filled
- `return 1` (AARUF_STATUS_SECTOR_NOT_DUMPED): sector was empty, buffer is zeros
- `return <0`: actual error

For roundtrip verification, both status 0 and 1 are valid — check the buffer contents, not
just the return code. For rendering back to CUE/BIN, write the buffer regardless of status.

### Track setup for CUE/BIN import

- `TrackEntry.start` = first LBA of the BIN file (includes pregap if present)
- `TrackEntry.end` = last LBA of the BIN file
- `TrackEntry.pregap` = 0 (set to actual value only if you need INDEX 01 metadata)
- Write all sectors with `aaruf_write_sector_long(ctx, lba, false, data, SectorStatusDumped, 2352)`
- `SectorStatusDumped = 0x1` (not 0 — zero means NotDumped in the DDT enum)

### Mode1/2352 prefix/suffix handling

The library automatically splits raw 2352-byte Mode1 sectors into:
- 16-byte prefix (sync pattern + MSF header) — validated and stored/reconstructed
- 2048-byte user data — stored in data blocks with compression
- 288-byte suffix (ECC/EDC) — validated and stored/reconstructed

All-zero sectors (common in pregap regions) are detected and marked as not-dumped.
Sectors with correct ECC/EDC are stored with `SectorStatusMode1Correct` and the
prefix/suffix is regenerated on read — no storage overhead.

## Notes

- All tracks written as raw 2352-byte sectors via `aaruf_write_sector_long`
- Compression: FLAC for audio tracks (automatic), LZMA or zstd for data tracks
- No subchannel data preserved in this test (CUE/BIN doesn't carry it)
- zstd uses kCompressionZstd = 4 (pending upstream allocation)

## SHA-256 Roundtrip Verification

Full cryptographic verification: SHA-256 of all raw BIN sectors vs SHA-256 of readback
from .aaru (zstd-19). Empty pregap sectors (SECTOR_NOT_DUMPED) included as zeros in both hashes.

| System | Tracks | Sectors | SHA-256 | Result |
|--------|--------|---------|---------|--------|
| Dreamcast A | 3 | 510,983 | `1107f6b2...a33063ba` | PASS |
| Dreamcast B | 3 | 519,777 | `c1ec41fc...02601785` | PASS |
| Saturn | 21 | 233,734 | `964a1640...1c1eb38a` | PASS |
| Mega CD | 35 | 255,826 | `643ab58a...eb3a652b` | PASS |
| PC Engine CD | 22 | 221,262 | `a8a2d223...76474b3b` | PASS |
| Neo Geo CD | 41 | 309,347 | `862e7038...deba4078` | PASS |
| PS1 (MODE2) | 1 | 183,775 | `1ae17e78...899a852a` | PASS |

All 7 disc images verified: **CUE/BIN → .aaru → readback produces identical SHA-256.**

## Fixes Applied to libaaru-ext

### Mode 2 Form 2 NoCrc EDC zeroing

Upstream libaaruformat leaves the 4-byte EDC field (bytes 2348-2351) uninitialized for
Mode 2 Form 2 sectors with `SectorStatusMode2Form2NoCrc`. Our fix zeros these bytes
to match the original disc data (where the CRC was empty/zeroed).

2 lines changed in `src/read.c` — both DDT v1 and v2 code paths.

## What's NOT tested yet
- Subchannel data preservation (would need .sub files or raw dumps)
- GDI format input (different track layout descriptor)
- CHD input/output and size comparison
- Multi-session disc handling
- Lead-in/lead-out sectors (negative sector addresses)
