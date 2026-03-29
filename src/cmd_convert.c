/*
 * dimg-tool convert — convert between disc image formats
 *
 * Supports:
 *   CUE/BIN → .aaru   (8 CD-based systems)
 *   ISO     → .aaru   (DVD systems)
 *   .aaru   → CUE/BIN (render)
 *   .aaru   → ISO     (render)
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dimg.h"
#include "disc.h"
#include "aaru.h"
#include "aaruformat.h"
#include "blake3.h"

/* Format module prototypes */
int iso_parse(const char *iso_path, DiscSystem system, DiscLayout *layout);
int iso_write(const char *iso_path, const DiscLayout *layout, void *aaru_ctx);
int cue_parse(const char *cue_path, DiscSystem system, DiscLayout *layout);
int cue_write(const char *cue_path, const DiscLayout *layout, void *aaru_ctx,
              bool multi_bin);
int aaru_write(const char *aaru_path, const DiscLayout *layout,
               const char *options, const char *sbi_path,
               uint8_t *ingest_digest);
int aaru_read_layout(const char *aaru_path, DiscLayout *layout, void **ctx_out);
void sbi_find_for_cue(const char *cue_path, char *sbi_buf, size_t bufsize);

/* Forward declaration for close */
extern int aaruf_close(void *context);

static void print_convert_usage(void)
{
    fprintf(stderr,
            "Usage: dimg-tool convert -i <input> -o <output> [-s <system>] [-c <codec>]\n"
            "                         [--json] [--verify]\n"
            "\n"
            "Formats (detected from extension):\n"
            "  .cue   CUE/BIN disc image (CD systems)\n"
            "  .iso   ISO disc image (DVD systems)\n"
            "  .aaru  Aaru compressed disc image\n"
            "\n"
            "Systems (-s, required for ingest to .aaru):\n"
            "  dc       Sega Dreamcast (GD-ROM)\n"
            "  saturn   Sega Saturn\n"
            "  megacd   Sega Mega CD\n"
            "  pce      PC Engine / TurboGrafx CD\n"
            "  neogeo   Neo Geo CD\n"
            "  ps1      Sony PlayStation\n"
            "  ps2cd    Sony PlayStation 2 (CD)\n"
            "  ps2dvd   Sony PlayStation 2 (DVD)\n"
            "  psp      Sony PlayStation Portable (UMD)\n"
            "  cd       Generic CD\n"
            "  dvd      Generic DVD\n"
            "\n"
            "Compression (-c, only for .aaru output):\n"
            "  lzma     LZMA (default, best ratio)\n"
            "  zstd     Zstandard level 19 (fast decompress)\n"
            "  none     No compression\n"
            "\n"
            "Options:\n"
            "  -j, --json       Print JSON summary to stdout on success\n"
            "  --verify         Roundtrip verify after ingest (CUE/ISO → .aaru only)\n"
            "  --multi-bin      Render per-track BIN files (Redump multi-BIN format)\n"
            "  -T, --threads N  Compression threads (0=auto, default 1)\n"
            "                   zstd: N worker threads; LZMA: max 2\n"
            "  -t <tracks>      Extract specific tracks (render only, e.g. 1,3-5,8)\n"
            "                   .bin output requires exactly 1 track\n"
            "                   .cue output with -t forces multi-BIN\n");
}

/* Build libaaruformat options string from codec name.
 * Writes into caller-provided buffer. Returns 0 on success, -1 if codec unknown.
 * threads > 1 appends ;threads=N (omitted otherwise, lib defaults to 1). */
static int codec_to_options(const char *codec, int threads, char *buf, size_t bufsize)
{
    const char *base;
    if(codec == NULL || strcmp(codec, "lzma") == 0)
        base = "compress=true;deduplicate=true";
    else if(strcmp(codec, "zstd") == 0)
        base = "compress=true;deduplicate=true;zstd=true;zstd_level=19";
    else if(strcmp(codec, "none") == 0)
        base = "compress=false;deduplicate=true";
    else
        return -1;

    if(threads > 1)
        snprintf(buf, bufsize, "%s;threads=%d", base, threads);
    else
        snprintf(buf, bufsize, "%s", base);

    return 0;
}

/* Get file size. Returns -1 on error. */
static int64_t file_size(const char *path)
{
    struct stat st;
    if(stat(path, &st) != 0)
        return -1;
    return (int64_t)st.st_size;
}

/*
 * Direct roundtrip verification:
 * 1. Open .aaru, decompress each sector into BLAKE3 hasher (no temp files)
 * 2. Compare digest against ingest_digest computed during aaru_write()
 *
 * Returns DIMG_OK on match, DIMG_ERR_VERIFY on mismatch.
 */
static int verify_direct(const char *aaru_path, const DiscLayout *original_layout,
                         const uint8_t *ingest_digest)
{
    DiscLayout layout;
    void *aaru_ctx = NULL;
    int rc = aaru_read_layout(aaru_path, &layout, &aaru_ctx);
    if(rc != DIMG_OK)
        return rc;

    int is_cd = disc_is_cd(layout.system);

    blake3_hasher b3;
    blake3_hasher_init(&b3);

    uint8_t buf[SECTOR_RAW]; /* large enough for both 2352 and 2048 */
    int64_t total_sectors = 0;
    int64_t sectors_done  = 0;

    for(int i = 0; i < layout.track_count; i++)
        total_sectors += layout.tracks[i].end - layout.tracks[i].start + 1;

    fprintf(stderr, "Verifying: decompress + BLAKE3 %" PRId64 " sectors\n",
            total_sectors);

    for(int i = 0; i < layout.track_count; i++)
    {
        const DiscTrack *dt = &layout.tracks[i];
        int64_t count = dt->end - dt->start + 1;

        for(int64_t s = 0; s < count; s++)
        {
            uint32_t rlen   = dt->sector_size;
            uint8_t  status = 0;
            int32_t  res;

            if(is_cd)
                res = aaruf_read_sector_long(aaru_ctx, (uint64_t)(dt->start + s),
                                              false, buf, &rlen, &status);
            else
                res = aaruf_read_sector(aaru_ctx, (uint64_t)(dt->start + s),
                                         false, buf, &rlen, &status);

            if(res < 0)
            {
                fprintf(stderr, "Verify read error at sector %" PRId64 ": %d\n",
                        dt->start + s, res);
                aaruf_close(aaru_ctx);
                return DIMG_ERR_IO;
            }

            if(res == 1)
                memset(buf, 0, dt->sector_size);

            blake3_hasher_update(&b3, buf, dt->sector_size);

            sectors_done++;
            if(sectors_done % 10000 == 0)
                fprintf(stderr, "\r  %" PRId64 "/%" PRId64 " sectors",
                        sectors_done, total_sectors);
        }
    }

    if(total_sectors > 10000)
        fprintf(stderr, "\r  %" PRId64 "/%" PRId64 " sectors\n",
                total_sectors, total_sectors);

    aaruf_close(aaru_ctx);

    uint8_t verify_digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&b3, verify_digest, BLAKE3_OUT_LEN);

    if(memcmp(ingest_digest, verify_digest, BLAKE3_OUT_LEN) != 0)
    {
        fprintf(stderr, "VERIFY FAILED: BLAKE3 digest mismatch\n");
        return DIMG_ERR_VERIFY;
    }

    fprintf(stderr, "Verify: PASS (BLAKE3 match)\n");
    return DIMG_OK;
}

/*
 * Parse track specification string into a selection array.
 * Supports: single (3), list (1,3,5), range (1-5), mixed (1,3-5,8).
 * selected[] is indexed by track number (1-based), size DISC_MAX_TRACKS+1.
 * Returns 0 on success, -1 on parse error.
 */
static int parse_track_spec(const char *spec, bool selected[DISC_MAX_TRACKS + 1])
{
    memset(selected, 0, sizeof(bool) * (DISC_MAX_TRACKS + 1));

    const char *p = spec;
    while(*p)
    {
        /* Skip whitespace */
        while(*p == ' ') p++;
        if(*p == '\0') break;

        /* Parse first number */
        char *end;
        long a = strtol(p, &end, 10);
        if(end == p || a < 1 || a > DISC_MAX_TRACKS)
            return -1;
        p = end;

        if(*p == '-')
        {
            /* Range: N-M */
            p++;
            long b = strtol(p, &end, 10);
            if(end == p || b < a || b > DISC_MAX_TRACKS)
                return -1;
            p = end;
            for(long t = a; t <= b; t++)
                selected[t] = true;
        }
        else
        {
            /* Single track */
            selected[a] = true;
        }

        /* Expect comma or end */
        if(*p == ',')
            p++;
        else if(*p != '\0')
            return -1;
    }

    return 0;
}

int cmd_convert(int argc, char **argv)
{
    const char *input  = NULL;
    const char *output = NULL;
    const char *system = NULL;
    const char *codec  = NULL;
    const char *track_spec = NULL;
    const char *threads_str = NULL;
    int threads        = 0;   /* 0 = not specified (default single-threaded) */
    bool json_output   = false;
    bool verify        = false;
    bool multi_bin     = false;

    /* Parse arguments */
    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            input = argv[++i];
        else if(strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if(strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            system = argv[++i];
        else if(strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            codec = argv[++i];
        else if(strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            track_spec = argv[++i];
        else if((strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--threads") == 0) && i + 1 < argc)
            threads_str = argv[++i];
        else if(strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0)
            json_output = true;
        else if(strcmp(argv[i], "--verify") == 0)
            verify = true;
        else if(strcmp(argv[i], "--multi-bin") == 0)
            multi_bin = true;
        else if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_convert_usage();
            return DIMG_OK;
        }
    }

    if(input == NULL || output == NULL)
    {
        print_convert_usage();
        return DIMG_ERR_ARGS;
    }

    /* Parse thread count */
    if(threads_str != NULL)
    {
        errno = 0;
        char *endptr = NULL;
        long val = strtol(threads_str, &endptr, 10);
        if(endptr == threads_str || *endptr != '\0' || errno == ERANGE)
        {
            fprintf(stderr, "Invalid thread count: %s\n", threads_str);
            return DIMG_ERR_ARGS;
        }
        if(val < 0)
        {
            fprintf(stderr, "Thread count must be non-negative\n");
            return DIMG_ERR_ARGS;
        }
        if(val == 0)
        {
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            threads = (ncpu > 0) ? (int)ncpu : 1;
        }
        else
        {
            threads = (int)val;
        }
    }

    /* Detect formats */
    DiscFormat in_fmt  = disc_detect_format(input);
    DiscFormat out_fmt = disc_detect_format(output);

    if(in_fmt == DISC_FMT_UNKNOWN)
    {
        fprintf(stderr, "Unknown input format: %s\n", input);
        return DIMG_ERR_FORMAT;
    }
    if(out_fmt == DISC_FMT_UNKNOWN)
    {
        fprintf(stderr, "Unknown output format: %s\n", output);
        return DIMG_ERR_FORMAT;
    }
    if(in_fmt == out_fmt)
    {
        fprintf(stderr, "Input and output formats are the same\n");
        return DIMG_ERR_ARGS;
    }

    /* One side must be .aaru */
    if(in_fmt != DISC_FMT_AARU && out_fmt != DISC_FMT_AARU)
    {
        fprintf(stderr, "One of input/output must be .aaru\n");
        return DIMG_ERR_ARGS;
    }

    /* Validate compression codec and build options string */
    char options_buf[128];
    if(codec_to_options(codec, threads, options_buf, sizeof(options_buf)) != 0)
    {
        fprintf(stderr, "Unknown compression codec: %s\n", codec);
        return DIMG_ERR_ARGS;
    }
    const char *options = options_buf;

    /* --verify only valid for ingest (to .aaru) */
    if(verify && out_fmt != DISC_FMT_AARU)
    {
        fprintf(stderr, "--verify is only valid for ingest (output must be .aaru)\n");
        return DIMG_ERR_ARGS;
    }

    /* --multi-bin only valid for CUE render */
    if(multi_bin && out_fmt != DISC_FMT_CUE)
    {
        fprintf(stderr, "--multi-bin is only valid for CUE output\n");
        return DIMG_ERR_ARGS;
    }

    /* -t only valid for render (.aaru input) */
    if(track_spec != NULL && in_fmt != DISC_FMT_AARU)
    {
        fprintf(stderr, "-t is only valid for render (input must be .aaru)\n");
        return DIMG_ERR_ARGS;
    }

    /* .bin output requires -t */
    if(out_fmt == DISC_FMT_BIN && track_spec == NULL)
    {
        fprintf(stderr, ".bin output requires -t to select a track\n");
        return DIMG_ERR_ARGS;
    }

    /* .bin output not valid as ingest target */
    if(out_fmt == DISC_FMT_BIN && in_fmt != DISC_FMT_AARU)
    {
        fprintf(stderr, ".bin output only valid for render from .aaru\n");
        return DIMG_ERR_ARGS;
    }

    /* === INGEST: source → .aaru === */
    if(out_fmt == DISC_FMT_AARU)
    {
        /* System flag required for ingest */
        if(system == NULL)
        {
            fprintf(stderr, "System type (-s) required for ingest to .aaru\n");
            print_convert_usage();
            return DIMG_ERR_ARGS;
        }

        int sys_val = disc_parse_system(system);
        if(sys_val < 0)
        {
            fprintf(stderr, "Unknown system: %s\n", system);
            return DIMG_ERR_ARGS;
        }
        DiscSystem disc_sys = (DiscSystem)sys_val;
        const char *effective_codec = codec != NULL ? codec : "lzma";

        fprintf(stderr, "Converting: %s → %s\n", input, output);
        fprintf(stderr, "System:     %s\n", disc_system_name(disc_sys));
        fprintf(stderr, "Codec:      %s\n", effective_codec);
        if(threads > 1)
            fprintf(stderr, "Threads:    %d%s\n", threads,
                    strcmp(effective_codec, "lzma") == 0 ? " (LZMA max 2)" : "");

        DiscLayout layout;
        int rc;

        switch(in_fmt)
        {
            case DISC_FMT_ISO:
                rc = iso_parse(input, disc_sys, &layout);
                break;
            case DISC_FMT_CUE:
                rc = cue_parse(input, disc_sys, &layout);
                break;
            default:
                fprintf(stderr, "Unsupported input format for ingest\n");
                return DIMG_ERR_UNSUPPORTED;
        }

        if(rc != DIMG_OK)
            return rc;

        fprintf(stderr, "Tracks:     %d\n", layout.track_count);
        fprintf(stderr, "Sectors:    %" PRId64 "\n", layout.total_sectors);

        /* Auto-detect SBI subchannel file for CUE/BIN input */
        char sbi_path[512] = {0};
        bool sbi_embedded = false;
        if(in_fmt == DISC_FMT_CUE)
        {
            sbi_find_for_cue(input, sbi_path, sizeof(sbi_path));
            sbi_embedded = sbi_path[0] != '\0';
        }

        uint8_t ingest_digest[BLAKE3_OUT_LEN];
        rc = aaru_write(output, &layout, options,
                         sbi_path[0] ? sbi_path : NULL,
                         verify ? ingest_digest : NULL);
        if(rc != DIMG_OK)
            return rc;

        /* Roundtrip verification */
        bool verified = false;
        if(verify)
        {
            rc = verify_direct(output, &layout, ingest_digest);
            if(rc != DIMG_OK)
                return rc;
            verified = true;
        }

        /* JSON output */
        if(json_output)
        {
            int64_t in_size = 0;
            /* Sum input sizes: for CUE, sum all track source files */
            if(in_fmt == DISC_FMT_CUE)
            {
                for(int t = 0; t < layout.track_count; t++)
                {
                    int64_t s = file_size(layout.tracks[t].bin_path);
                    if(s > 0)
                        in_size += s;
                }
            }
            else
            {
                in_size = file_size(input);
            }

            int64_t out_size = file_size(output);

            printf("{\n");
            printf("  \"input\": \"%s\",\n", input);
            printf("  \"output\": \"%s\",\n", output);
            printf("  \"system\": \"%s\",\n", disc_system_cli_name(disc_sys));
            printf("  \"codec\": \"%s\",\n", effective_codec);
            printf("  \"threads\": %d,\n", threads > 0 ? threads : 1);
            printf("  \"tracks\": %d,\n", layout.track_count);
            printf("  \"sectors\": %" PRId64 ",\n", layout.total_sectors);
            printf("  \"input_size\": %" PRId64 ",\n", in_size);
            printf("  \"output_size\": %" PRId64 ",\n", out_size < 0 ? 0 : out_size);
            printf("  \"sbi_embedded\": %s", sbi_embedded ? "true" : "false");

            if(verify)
            {
                printf(",\n");
                printf("  \"verified\": %s,\n", verified ? "true" : "false");
                printf("  \"verify_hash\": \"blake3\",\n");
                printf("  \"verify_digest\": \"");
                for(int d = 0; d < BLAKE3_OUT_LEN; d++)
                    printf("%02x", ingest_digest[d]);
                printf("\"\n");
            }
            else
            {
                printf("\n");
            }
            printf("}\n");
        }

        return DIMG_OK;
    }

    /* === RENDER: .aaru → target === */
    DiscLayout layout;
    void *aaru_ctx = NULL;
    int rc = aaru_read_layout(input, &layout, &aaru_ctx);
    if(rc != DIMG_OK)
        return rc;

    /* Apply track filter if -t specified */
    DiscLayout render_layout = layout;
    bool track_selected[DISC_MAX_TRACKS + 1];
    int filtered_count = 0;

    if(track_spec != NULL)
    {
        if(parse_track_spec(track_spec, track_selected) != 0)
        {
            fprintf(stderr, "Invalid track specification: %s\n", track_spec);
            aaruf_close(aaru_ctx);
            return DIMG_ERR_ARGS;
        }

        /* Build filtered layout with only selected tracks */
        render_layout.track_count = 0;
        for(int i = 0; i < layout.track_count; i++)
        {
            if(track_selected[layout.tracks[i].number])
            {
                render_layout.tracks[render_layout.track_count++] =
                    layout.tracks[i];
                filtered_count++;
            }
        }

        /* Validate all requested tracks were found */
        int requested = 0;
        for(int t = 1; t <= DISC_MAX_TRACKS; t++)
            if(track_selected[t]) requested++;

        if(filtered_count != requested)
        {
            fprintf(stderr, "Some requested tracks not found in image\n");
            aaruf_close(aaru_ctx);
            return DIMG_ERR_ARGS;
        }

        if(filtered_count == 0)
        {
            fprintf(stderr, "No tracks selected\n");
            aaruf_close(aaru_ctx);
            return DIMG_ERR_ARGS;
        }

        /* .bin requires exactly 1 track */
        if(out_fmt == DISC_FMT_BIN && filtered_count != 1)
        {
            fprintf(stderr, ".bin output requires exactly 1 track (-t), got %d\n",
                    filtered_count);
            aaruf_close(aaru_ctx);
            return DIMG_ERR_ARGS;
        }

        /* .cue with -t forces multi-BIN */
        if(out_fmt == DISC_FMT_CUE)
            multi_bin = true;
    }

    fprintf(stderr, "Converting: %s → %s\n", input, output);
    fprintf(stderr, "System:     %s\n", disc_system_name(layout.system));
    fprintf(stderr, "Tracks:     %d%s\n", render_layout.track_count,
            track_spec ? " (filtered)" : "");
    fprintf(stderr, "Sectors:    %" PRId64 "\n", layout.total_sectors);

    switch(out_fmt)
    {
        case DISC_FMT_ISO:
            rc = iso_write(output, &render_layout, aaru_ctx);
            break;
        case DISC_FMT_CUE:
            rc = cue_write(output, &render_layout, aaru_ctx, multi_bin);
            break;
        case DISC_FMT_BIN:
        {
            const DiscTrack *t = &render_layout.tracks[0];
            fprintf(stderr, "  Track %d [%s] sectors %" PRId64 "-%" PRId64 "\n",
                    t->number,
                    t->type == DISC_TRACK_AUDIO ? "AUDIO" :
                    t->type == DISC_TRACK_MODE1 ? "MODE1" :
                    t->type == DISC_TRACK_MODE2 ? "MODE2" : "DATA",
                    t->start, t->end);

            FILE *bf = fopen(output, "wb");
            if(bf == NULL)
            {
                fprintf(stderr, "Cannot create output: %s\n", output);
                aaruf_close(aaru_ctx);
                return DIMG_ERR_IO;
            }
            rc = write_sectors_to_bin(bf, aaru_ctx, t->start, t->end,
                                      t->end - t->start + 1);
            fclose(bf);
            break;
        }
        default:
            fprintf(stderr, "Unsupported output format for render\n");
            rc = DIMG_ERR_UNSUPPORTED;
            break;
    }

    aaruf_close(aaru_ctx);

    if(rc != DIMG_OK)
        return rc;

    /* JSON output for render */
    if(json_output)
    {
        int64_t in_size = file_size(input);
        int64_t out_size = 0;

        if(multi_bin || out_fmt == DISC_FMT_BIN)
        {
            if(out_fmt == DISC_FMT_BIN)
            {
                out_size = file_size(output);
            }
            else
            {
                /* Sum per-track BIN file sizes */
                char stem[512];
                size_t olen = strlen(output);
                if(olen >= 4 && olen < sizeof(stem))
                {
                    memcpy(stem, output, olen - 4);
                    stem[olen - 4] = '\0';
                }
                for(int t = 0; t < render_layout.track_count; t++)
                {
                    char tpath[512];
                    snprintf(tpath, sizeof(tpath), "%s (Track %02d).bin",
                             stem, render_layout.tracks[t].number);
                    int64_t s = file_size(tpath);
                    if(s > 0) out_size += s;
                }
            }
        }
        else
        {
            out_size = file_size(output);
        }

        printf("{\n");
        printf("  \"input\": \"%s\",\n", input);
        printf("  \"output\": \"%s\",\n", output);
        printf("  \"system\": \"%s\",\n", disc_system_cli_name(layout.system));
        printf("  \"tracks\": %d,\n", render_layout.track_count);
        printf("  \"sectors\": %" PRId64 ",\n", layout.total_sectors);
        printf("  \"input_size\": %" PRId64 ",\n", in_size < 0 ? 0 : in_size);
        printf("  \"output_size\": %" PRId64 ",\n", out_size < 0 ? 0 : out_size);
        printf("  \"multi_bin\": %s\n", multi_bin ? "true" : "false");
        printf("}\n");
    }

    return rc;
}
