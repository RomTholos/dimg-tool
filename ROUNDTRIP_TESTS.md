# dimg-tool Roundtrip Tests

## Test Date: 2026-03-18

## Setup

- libaaruformat: upstream git clone (alpha.33, 2025-03), patched with zstd (kCompressionZstd = 4)
- zstd: libzstd 1.5.7
- Build: static musl binary (x86_64-linux-musl-gcc), fully portable
- Method: `dimg-tool convert` for all ingest/render operations
- All tests: FLAC for audio tracks, zstd-19 for data tracks
- Deduplication enabled (DDT) for all tests
- Test media: redump disc images (CUE/BIN and ISO)

## Supported Conversions

| Direction | Formats | Systems |
|-----------|---------|---------|
| Ingest | CUE/BIN → .aaru | Dreamcast, Saturn, Mega CD, PC Engine CD, Neo Geo CD, PS1, PS2 CD |
| Ingest | ISO → .aaru | PS2 DVD |
| Render | .aaru → CUE/BIN | All CD-based systems |
| Render | .aaru → ISO | All DVD-based systems |

### Usage

```
dimg-tool convert -i <input> -o <output> [-s <system>] [-c <codec>]

Systems: dc, saturn, megacd, pce, neogeo, ps1, ps2cd, ps2dvd, cd, dvd
Codecs:  lzma (default), zstd, none
```

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

## SHA-256 Roundtrip Verification

Full cryptographic verification via `dimg-tool convert`: ingest to .aaru (zstd-19), render
back to CUE/BIN or ISO, SHA-256 of output matches original. Empty pregap sectors
(SECTOR_NOT_DUMPED) are included as zeros in both hashes.

| System | Tracks | Sectors | SHA-256 | Result |
|--------|--------|---------|---------|--------|
| Dreamcast A | 3 | 510,983 | `1107f6b2...a33063ba` | PASS |
| Dreamcast B | 3 | 519,777 | `c1ec41fc...02601785` | PASS |
| Saturn | 21 | 233,734 | `964a1640...1c1eb38a` | PASS |
| Mega CD | 35 | 255,826 | `643ab58a...eb3a652b` | PASS |
| PC Engine CD | 22 | 221,262 | `a8a2d223...76474b3b` | PASS |
| Neo Geo CD | 41 | 309,347 | `862e7038...deba4078` | PASS |
| PS1 (MODE2) | 1 | 183,775 | `1ae17e78...899a852a` | PASS |
| PS2 CD (MODE2) | 1 | 144,121 | `ee143e43...6eddf1bc` | PASS |
| PS2 DVD (ISO) | 1 | 869,680 | `8b2ffcc5...7e5052d6` | PASS |

All 9 disc images verified: **ingest → .aaru → render produces identical SHA-256.**

## Architecture

```
cmd_convert.c  — CLI parsing, format detection, dispatch
disc.h/disc.c  — shared DiscLayout struct, system/format enums
fmt_cue.c      — CUE/BIN parser (single + multi-file) and writer
fmt_iso.c      — ISO reader and writer (2048-byte DVD sectors)
fmt_aaru.c     — bridge to libaaruformat (ingest + render)
fmt_sbi.c      — SBI subchannel parser for PS1 LibCrypt
```

### Ingest flow (CUE/ISO → .aaru)

1. Format parser fills `DiscLayout` (tracks, sector ranges, source file paths)
2. `aaru_write()` creates .aaru image via `aaruf_create()`
3. For CD: `aaruf_set_tracks()` with track layout, `aaruf_write_sector_long()` per sector
4. For DVD: `aaruf_write_sector()` per sector (no track table needed)
5. If SBI file found alongside CUE: writes Q subchannel data via `aaruf_write_sector_tag()`

### Render flow (.aaru → CUE/ISO)

1. `aaru_read_layout()` opens .aaru, populates `DiscLayout` from image metadata
2. Format writer reads sectors via `aaruf_read_sector_long()` / `aaruf_read_sector()`
3. Writes output file(s) (CUE+BIN or ISO)

## Fixes Applied to libaaru-ext

### Mode 2 Form 2 NoCrc EDC zeroing

Upstream libaaruformat leaves the 4-byte EDC field (bytes 2348-2351) uninitialized for
Mode 2 Form 2 sectors with `SectorStatusMode2Form2NoCrc`. Our fix zeros these bytes
to match the original disc data (where the CRC was empty/zeroed).

2 lines changed in `src/read.c` — both DDT v1 and v2 code paths.

## SBI Subchannel Support (PS1 LibCrypt)

SBI files contain Q subchannel corrections for PS1 LibCrypt copy protection.
Auto-detected during CUE/BIN ingest when a `.sbi` file exists alongside the `.cue`.

### SBI format

- 4 bytes: `"SBI\0"` magic
- N × 14-byte records: 3 bytes BCD MSF + 1 byte type (0x01) + 10 bytes Q data
- LibCrypt v1: 16 records (8 key pairs), LibCrypt v2: 32 records (16 key pairs)
- Redump distributes 233 SBI files for European PS1 games

### Integration

- Q data (10 bytes) extended to 12 bytes with CRC16 (CRC-CCITT, inverted)
- 12-byte Q expanded to 96-byte interleaved raw subchannel (P=0xFF, Q from SBI, R-W=0x00)
- Written via `aaruf_write_sector_tag(ctx, lba, false, raw96, 96, kSectorTagCdSubchannel)`
- Stored in .aaru as CST+LZMA compressed subchannel block (sparse data compresses well)
- Sector data integrity unaffected — SHA-256 roundtrip still passes with SBI present

## What's NOT tested yet

- Multi-session disc handling
- Lead-in/lead-out sectors (negative sector addresses)
