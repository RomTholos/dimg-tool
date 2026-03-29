# dimg-tool

Disc image conversion tool for game preservation. Converts between
common disc image formats and the [Aaru](https://github.com/aaru-dps/Aaru)
compressed container format (.aaru) with lossless, bit-perfect roundtrips.

Similar to [dolphin-tool](https://dolphin-emu.org/) (ISO↔RVZ for Wii/GC),
but for CD and DVD-based console systems.

## Supported Systems

| System | Format | Sectors |
|--------|--------|---------|
| Sega Dreamcast | CUE/BIN | Multi-track (data + audio) |
| Sega Saturn | CUE/BIN | Multi-track (data + audio) |
| Sega Mega CD | CUE/BIN | Multi-track (data + audio) |
| PC Engine CD | CUE/BIN | Multi-track (data + audio) |
| Neo Geo CD | CUE/BIN | Multi-track (data + audio) |
| PlayStation | CUE/BIN | MODE2/2352, SBI subchannel |
| PlayStation 2 (CD) | CUE/BIN | MODE2/2352 |
| PlayStation 2 (DVD) | ISO | 2048-byte sectors |
| PlayStation Portable | ISO | 2048-byte UMD sectors |

All 10 systems verified with BLAKE3 lossless roundtrips.

## Usage

```
dimg-tool convert -i <input> -o <output> [-s <system>] [-c <codec>] [options]
dimg-tool info    [-j|--json] <image>
dimg-tool verify  <image>
```

### Convert examples

```sh
# CUE/BIN → .aaru (zstd compression)
dimg-tool convert -i game.cue -o game.aaru -s ps1 -c zstd

# ISO → .aaru with multi-threaded compression and verification
dimg-tool convert -i game.iso -o game.aaru -s ps2dvd -T 0 --verify

# .aaru → CUE/BIN
dimg-tool convert -i game.aaru -o game.cue

# .aaru → ISO
dimg-tool convert -i game.aaru -o game.iso

# Extract specific tracks as multi-BIN
dimg-tool convert -i game.aaru -o game.cue -t 1,3-5 --multi-bin

# Extract a single track as raw .bin
dimg-tool convert -i game.aaru -o track02.bin -t 2
```

### Systems (-s)

`dc` `saturn` `megacd` `pce` `neogeo` `ps1` `ps2cd` `ps2dvd` `psp` `cd` `dvd`

### Compression (-c)

| Codec | Description |
|-------|-------------|
| `lzma` | Best ratio (default) |
| `zstd` | ~5.5x faster decompression, comparable ratio |
| `none` | No compression |

Audio tracks always use FLAC. Deduplication (DDT) is always enabled.

### Options

| Flag | Description |
|------|-------------|
| `-T`, `--threads N` | Compression threads (`0`=auto, default `1`). zstd scales to N workers; LZMA max 2. |
| `--verify` | Roundtrip verification after ingest (BLAKE3, no temp files) |
| `-j`, `--json` | JSON summary to stdout on success |
| `--multi-bin` | Render per-track BIN files (Redump multi-BIN format) |
| `-t <tracks>` | Track selection for render (e.g. `1,3-5,8`) |
| `-V`, `--version` | Print version |

## Verification

`--verify` performs a streaming roundtrip check: the original sector data
is BLAKE3-hashed during ingest (zero extra I/O), then the .aaru file is
decompressed and hashed directly in memory. No temp files or disk space
needed. With `--json`, the digest is included in the output for
reproducibility.

## SBI Subchannel Support

PS1 LibCrypt subchannel data (.sbi files) is automatically detected
alongside CUE/BIN inputs and embedded in the .aaru image.

## Building

Requires [libaaruformat](https://github.com/RomTholos/libaaruformat)
in the sibling directory (`../libaaruformat`).

### Development build

```sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```

### Release builds (static musl binaries)

Requires [musl-cross-make](https://github.com/richfelker/musl-cross-make) toolchains.

```sh
# All architectures (x86_64, ARM, AArch64, RISC-V)
./build-release.sh v0.3.4

# Single architecture
./build-release.sh v0.3.4 x86_64

# Force rebuild of libaaruformat
./build-release.sh --clean v0.3.4
```

Output in `dist/`: static stripped binary + `.tar.gz` + `.sha256`.
Architectures without a toolchain installed are skipped automatically.

### Cross-compilation

The build script handles cross-compilation automatically via `CMAKE_CROSSCOMPILING=ON`
and architecture-specific C flags. libaaruformat's CMakeLists.txt skips its native arch
flags when cross-compiling — the build script provides the correct flags instead.

| Architecture | Toolchain triple | Key flags |
|-------------|-----------------|-----------|
| x86_64 | `x86_64-linux-musl` | (native defaults) |
| ARM (NEON) | `arm-linux-musleabihf` | `-march=armv7-a+fp -mfpu=neon -mfloat-abi=hard` |
| AArch64 | `aarch64-linux-musl` | (native defaults) |
| RISC-V 64 | `riscv64-linux-musl` | `-march=rv64gc -mabi=lp64d` |

For ARM without NEON (e.g. Cortex-A8), change `-mfpu=neon` to `-mfpu=vfpv3-d16` in the build script.

## License

MIT
