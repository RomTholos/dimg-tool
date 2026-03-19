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

All 9 systems verified with SHA-256 lossless roundtrips.

## Usage

```
dimg-tool convert -i <input> -o <output> [-s <system>] [-c <codec>]
dimg-tool info    <image>
dimg-tool verify  <image>
```

### Convert examples

```sh
# CUE/BIN → .aaru (zstd compression)
dimg-tool convert -i game.cue -o game.aaru -s ps1 -c zstd

# ISO → .aaru
dimg-tool convert -i game.iso -o game.aaru -s ps2dvd

# .aaru → CUE/BIN
dimg-tool convert -i game.aaru -o game.cue

# .aaru → ISO
dimg-tool convert -i game.aaru -o game.iso
```

### Systems (-s)

`dc` `saturn` `megacd` `pce` `neogeo` `ps1` `ps2cd` `ps2dvd` `cd` `dvd`

### Compression (-c)

| Codec | Description |
|-------|-------------|
| `lzma` | Best ratio (default) |
| `zstd` | ~5.5x faster decompression, comparable ratio |
| `none` | No compression |

Audio tracks always use FLAC. Deduplication (DDT) is always enabled.

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
# All architectures (x86_64, ARM, RISC-V)
./build-release.sh v0.2.0

# Single architecture
./build-release.sh v0.2.0 x86_64

# Multiple specific architectures
./build-release.sh v0.2.0 arm riscv64
```

Output in `dist/`: static stripped binary + `.tar.gz` + `.sha256`.

### Cross-compilation

The build script handles cross-compilation automatically via `CMAKE_CROSSCOMPILING=ON`
and architecture-specific C flags. libaaruformat's CMakeLists.txt skips its native arch
flags when cross-compiling — the build script provides the correct flags instead.

| Architecture | Toolchain triple | Key flags |
|-------------|-----------------|-----------|
| x86_64 | `x86_64-linux-musl` | (native defaults) |
| ARM (NEON) | `arm-linux-musleabihf` | `-march=armv7-a+fp -mfpu=neon -mfloat-abi=hard` |
| RISC-V 64 | `riscv64-linux-musl` | `-march=rv64gc -mabi=lp64d` |

For ARM without NEON (e.g. Cortex-A8), change `-mfpu=neon` to `-mfpu=vfpv3-d16` in the build script.

## License

MIT
