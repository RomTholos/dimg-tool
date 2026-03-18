/*
 * dimg-tool convert — convert between disc image formats
 *
 * TODO: Implement format-specific ingest/render pipelines.
 *       This is the skeleton — actual conversion logic depends on
 *       understanding each source format's track layout.
 */

#include <stdio.h>
#include <string.h>

#include "dimg.h"

int cmd_convert(int argc, char **argv)
{
    const char *input  = NULL;
    const char *output = NULL;

    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            input = argv[++i];
        else if(strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
    }

    if(input == NULL || output == NULL)
    {
        fprintf(stderr, "Usage: dimg-tool convert -i <input> -o <output>\n");
        return DIMG_ERR_ARGS;
    }

    /* TODO: Detect input format from extension/magic, open with appropriate reader,
     *       detect output format from extension, write with appropriate writer.
     *
     *       Supported conversions (planned):
     *         .aaru  → .gdi, .cue+.bin, .iso   (render/export)
     *         .gdi   → .aaru                     (ingest)
     *         .cue   → .aaru                     (ingest)
     *         .iso   → .aaru                     (ingest)
     *         .chd   → .aaru                     (ingest, via libchdr)
     */

    fprintf(stderr, "Convert: %s → %s\n", input, output);
    fprintf(stderr, "Not yet implemented.\n");

    return DIMG_ERR_UNSUPPORTED;
}
