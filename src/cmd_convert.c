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
#include <stdio.h>
#include <string.h>

#include "dimg.h"
#include "disc.h"

/* Format module prototypes */
int iso_parse(const char *iso_path, DiscSystem system, DiscLayout *layout);
int iso_write(const char *iso_path, const DiscLayout *layout, void *aaru_ctx);
int cue_parse(const char *cue_path, DiscSystem system, DiscLayout *layout);
int cue_write(const char *cue_path, const DiscLayout *layout, void *aaru_ctx);
int aaru_write(const char *aaru_path, const DiscLayout *layout, const char *options);
int aaru_read_layout(const char *aaru_path, DiscLayout *layout, void **ctx_out);

/* Forward declaration for close */
extern int aaruf_close(void *context);

static void print_convert_usage(void)
{
    fprintf(stderr,
            "Usage: dimg-tool convert -i <input> -o <output> [-s <system>] [-c <codec>]\n"
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
            "  none     No compression\n");
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

int cmd_convert(int argc, char **argv)
{
    const char *input  = NULL;
    const char *output = NULL;
    const char *system = NULL;
    const char *codec  = NULL;

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

        fprintf(stderr, "Converting: %s → %s\n", input, output);
        fprintf(stderr, "System:     %s\n", disc_system_name(disc_sys));
        fprintf(stderr, "Codec:      %s\n", codec != NULL ? codec : "lzma");

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

        return aaru_write(output, &layout, options);
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
    return rc;
}
