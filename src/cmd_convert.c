/*
 * dimg-tool convert — convert between disc image formats
 *
 * Supports:
 *   CUE/BIN → .aaru   (8 CD-based systems)
 *   ISO     → .aaru   (DVD systems)
 *   .aaru   → CUE/BIN (render)
 *   .aaru   → ISO     (render)
 */

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

/* Format module prototypes */
int iso_parse(const char *iso_path, DiscSystem system, DiscLayout *layout);
int iso_write(const char *iso_path, const DiscLayout *layout, void *aaru_ctx);
int cue_parse(const char *cue_path, DiscSystem system, DiscLayout *layout);
int cue_write(const char *cue_path, const DiscLayout *layout, void *aaru_ctx);
int aaru_write(const char *aaru_path, const DiscLayout *layout,
               const char *options, const char *sbi_path);
int aaru_read_layout(const char *aaru_path, DiscLayout *layout, void **ctx_out);
void sbi_find_for_cue(const char *cue_path, char *sbi_buf, size_t bufsize);

/* Forward declaration for close */
extern int aaruf_close(void *context);

/* SHA-256 from libaaruformat (aaruf_ prefix) */
extern void aaruf_aaruf_sha256_init(sha256_ctx *ctx);
extern void aaruf_aaruf_sha256_update(sha256_ctx *ctx, const void *data, unsigned long len);
extern void aaruf_aaruf_sha256_final(sha256_ctx *ctx, unsigned char *out);

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
            "  cd       Generic CD\n"
            "  dvd      Generic DVD\n"
            "\n"
            "Compression (-c, only for .aaru output):\n"
            "  lzma     LZMA (default, best ratio)\n"
            "  zstd     Zstandard level 19 (fast decompress)\n"
            "  none     No compression\n"
            "\n"
            "Options:\n"
            "  -j, --json     Print JSON summary to stdout on success\n"
            "  --verify       Roundtrip verify after ingest (CUE/ISO → .aaru only)\n");
}

/* Build libaaruformat options string from codec name */
static const char *codec_to_options(const char *codec)
{
    if(codec == NULL || strcmp(codec, "lzma") == 0)
        return "compress=true;deduplicate=true";
    if(strcmp(codec, "zstd") == 0)
        return "compress=true;deduplicate=true;zstd=true;zstd_level=19";
    if(strcmp(codec, "none") == 0)
        return "compress=false;deduplicate=true";
    return NULL;
}

/* Compute SHA-256 of a file. Returns 0 on success, -1 on failure. */
static int sha256_file(const char *path, uint8_t digest[SHA256_DIGEST_LENGTH])
{
    FILE *f = fopen(path, "rb");
    if(f == NULL)
        return -1;

    sha256_ctx sha;
    aaruf_sha256_init(&sha);

    uint8_t buf[65536];
    size_t n;
    while((n = fread(buf, 1, sizeof(buf), f)) > 0)
        aaruf_sha256_update(&sha, buf, (unsigned long)n);

    int err = ferror(f);
    fclose(f);
    if(err)
        return -1;

    aaruf_sha256_final(&sha, digest);
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
 * Roundtrip verification for ingest:
 * 1. Render .aaru → temp CUE/BIN or ISO
 * 2. SHA-256 compare each output file against original input
 * 3. Clean up temp files
 *
 * Returns DIMG_OK on match, DIMG_ERR_VERIFY on mismatch.
 */
static int verify_roundtrip(
    const char *aaru_path,
    const char *original_input,
    DiscFormat in_fmt,
    const DiscLayout *original_layout)
{
    /* Create temp directory */
    char tmpdir[] = "/tmp/dimg-verify-XXXXXX";
    if(mkdtemp(tmpdir) == NULL)
    {
        fprintf(stderr, "Failed to create temp directory for verification\n");
        return DIMG_ERR_IO;
    }

    /* Build temp output path */
    const char *ext = (in_fmt == DISC_FMT_CUE) ? ".cue" : ".iso";
    char tmp_out[512];
    snprintf(tmp_out, sizeof(tmp_out), "%s/verify%s", tmpdir, ext);

    /* Render .aaru → temp */
    DiscLayout render_layout;
    void *aaru_ctx = NULL;
    int rc = aaru_read_layout(aaru_path, &render_layout, &aaru_ctx);
    if(rc != DIMG_OK)
    {
        rmdir(tmpdir);
        return rc;
    }

    fprintf(stderr, "Verifying: roundtrip %s → %s\n", aaru_path, ext + 1);

    if(in_fmt == DISC_FMT_CUE)
        rc = cue_write(tmp_out, &render_layout, aaru_ctx);
    else
        rc = iso_write(tmp_out, &render_layout, aaru_ctx);

    aaruf_close(aaru_ctx);

    if(rc != DIMG_OK)
    {
        /* Cleanup */
        unlink(tmp_out);
        rmdir(tmpdir);
        return rc;
    }

    /* For CUE: compare the .bin file (the data).
     * The rendered .cue always uses a single .bin, so compare each
     * original track source against the corresponding portion.
     * For simplicity, compare the full rendered .bin against all
     * original source data concatenated.
     *
     * For ISO: compare the single file directly.
     */
    int verify_result = DIMG_OK;

    if(in_fmt == DISC_FMT_ISO)
    {
        /* Direct file comparison */
        uint8_t hash_orig[SHA256_DIGEST_LENGTH];
        uint8_t hash_render[SHA256_DIGEST_LENGTH];

        if(sha256_file(original_input, hash_orig) != 0)
        {
            fprintf(stderr, "Failed to hash original: %s\n", original_input);
            verify_result = DIMG_ERR_IO;
        }
        else if(sha256_file(tmp_out, hash_render) != 0)
        {
            fprintf(stderr, "Failed to hash rendered: %s\n", tmp_out);
            verify_result = DIMG_ERR_IO;
        }
        else if(memcmp(hash_orig, hash_render, SHA256_DIGEST_LENGTH) != 0)
        {
            fprintf(stderr, "VERIFY FAILED: ISO mismatch\n");
            verify_result = DIMG_ERR_VERIFY;
        }
    }
    else
    {
        /* CUE/BIN: rendered output is a single .bin file.
         * Hash each original track source and compare sector-by-sector
         * against the rendered .bin.
         *
         * The rendered .bin is the same extension with .bin instead of .cue.
         */
        char rendered_bin[512];
        snprintf(rendered_bin, sizeof(rendered_bin), "%s/verify.bin", tmpdir);

        /* Hash rendered bin */
        uint8_t hash_render[SHA256_DIGEST_LENGTH];
        if(sha256_file(rendered_bin, hash_render) != 0)
        {
            fprintf(stderr, "Failed to hash rendered .bin\n");
            verify_result = DIMG_ERR_IO;
            goto cleanup;
        }

        /* Concatenate and hash all original track sources in order */
        sha256_ctx sha_ctx;
        aaruf_sha256_init(&sha_ctx);

        uint8_t buf[65536];

        for(int t = 0; t < original_layout->track_count; t++)
        {
            const DiscTrack *dt = &original_layout->tracks[t];

            FILE *f = fopen(dt->bin_path, "rb");
            if(f == NULL)
            {
                fprintf(stderr, "Failed to open original track: %s\n", dt->bin_path);
                verify_result = DIMG_ERR_IO;
                goto cleanup;
            }

            if(dt->bin_offset > 0)
                fseeko(f, dt->bin_offset, SEEK_SET);

            int64_t track_bytes = (dt->end - dt->start + 1) * (int64_t)dt->sector_size;
            int64_t remaining = track_bytes;

            while(remaining > 0)
            {
                size_t chunk = (size_t)(remaining < (int64_t)sizeof(buf) ? remaining : (int64_t)sizeof(buf));
                size_t n = fread(buf, 1, chunk, f);
                if(n == 0)
                    break;
                aaruf_sha256_update(&sha_ctx, buf, (unsigned long)n);
                remaining -= (int64_t)n;
            }

            fclose(f);
        }

        uint8_t hash_orig[SHA256_DIGEST_LENGTH];
        aaruf_sha256_final(&sha_ctx, hash_orig);

        if(memcmp(hash_orig, hash_render, SHA256_DIGEST_LENGTH) != 0)
        {
            fprintf(stderr, "VERIFY FAILED: BIN data mismatch\n");
            verify_result = DIMG_ERR_VERIFY;
        }

cleanup:
        unlink(rendered_bin);
    }

    /* Clean up temp files */
    unlink(tmp_out);
    /* Remove any .bin file for CUE render */
    {
        char tmp_bin[512];
        snprintf(tmp_bin, sizeof(tmp_bin), "%s/verify.bin", tmpdir);
        unlink(tmp_bin);
    }
    rmdir(tmpdir);

    if(verify_result == DIMG_OK)
        fprintf(stderr, "Verify: PASS (SHA-256 match)\n");

    return verify_result;
}

int cmd_convert(int argc, char **argv)
{
    const char *input  = NULL;
    const char *output = NULL;
    const char *system = NULL;
    const char *codec  = NULL;
    bool json_output   = false;
    bool verify        = false;

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
        else if(strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0)
            json_output = true;
        else if(strcmp(argv[i], "--verify") == 0)
            verify = true;
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

    /* Validate compression codec */
    const char *options = codec_to_options(codec);
    if(codec != NULL && options == NULL)
    {
        fprintf(stderr, "Unknown compression codec: %s\n", codec);
        return DIMG_ERR_ARGS;
    }

    /* --verify only valid for ingest (to .aaru) */
    if(verify && out_fmt != DISC_FMT_AARU)
    {
        fprintf(stderr, "--verify is only valid for ingest (output must be .aaru)\n");
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

        rc = aaru_write(output, &layout, options,
                         sbi_path[0] ? sbi_path : NULL);
        if(rc != DIMG_OK)
            return rc;

        /* Roundtrip verification */
        bool verified = false;
        if(verify)
        {
            rc = verify_roundtrip(output, input, in_fmt, &layout);
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
            printf("  \"tracks\": %d,\n", layout.track_count);
            printf("  \"sectors\": %" PRId64 ",\n", layout.total_sectors);
            printf("  \"input_size\": %" PRId64 ",\n", in_size);
            printf("  \"output_size\": %" PRId64 ",\n", out_size < 0 ? 0 : out_size);
            printf("  \"sbi_embedded\": %s", sbi_embedded ? "true" : "false");

            if(verify)
            {
                printf(",\n");
                printf("  \"verified\": %s,\n", verified ? "true" : "false");
                printf("  \"verify_hash\": \"sha256\"\n");
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

    fprintf(stderr, "Converting: %s → %s\n", input, output);
    fprintf(stderr, "System:     %s\n", disc_system_name(layout.system));
    fprintf(stderr, "Tracks:     %d\n", layout.track_count);
    fprintf(stderr, "Sectors:    %" PRId64 "\n", layout.total_sectors);

    switch(out_fmt)
    {
        case DISC_FMT_ISO:
            rc = iso_write(output, &layout, aaru_ctx);
            break;
        case DISC_FMT_CUE:
            rc = cue_write(output, &layout, aaru_ctx);
            break;
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
        int64_t out_size = file_size(output);

        printf("{\n");
        printf("  \"input\": \"%s\",\n", input);
        printf("  \"output\": \"%s\",\n", output);
        printf("  \"system\": \"%s\",\n", disc_system_cli_name(layout.system));
        printf("  \"tracks\": %d,\n", layout.track_count);
        printf("  \"sectors\": %" PRId64 ",\n", layout.total_sectors);
        printf("  \"input_size\": %" PRId64 ",\n", in_size < 0 ? 0 : in_size);
        printf("  \"output_size\": %" PRId64 "\n", out_size < 0 ? 0 : out_size);
        printf("}\n");
    }

    return rc;
}
