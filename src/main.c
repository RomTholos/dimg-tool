/*
 * dimg-tool — disc image preservation and conversion tool
 *
 * Usage:
 *   dimg-tool info    <image>
 *   dimg-tool convert -i <input> -o <output> [-c lzma|none] [-f aaru|gdi|cue|iso]
 *   dimg-tool verify  <image>
 */

#include <stdio.h>
#include <string.h>

#include "dimg.h"

static void print_usage(void)
{
    fprintf(stderr,
            "dimg-tool — disc image preservation and conversion\n"
            "\n"
            "Usage:\n"
            "  dimg-tool info    <image>           Show image metadata\n"
            "  dimg-tool convert -i <in> -o <out>  Convert between formats\n"
            "  dimg-tool verify  <image>           Verify image integrity\n"
            "\n"
            "Supported formats: .aaru (via libaaruformat)\n");
}

int main(int argc, char **argv)
{
    if(argc < 2)
    {
        print_usage();
        return DIMG_ERR_ARGS;
    }

    const char *cmd = argv[1];

    if(strcmp(cmd, "info") == 0)
        return cmd_info(argc - 1, argv + 1);
    else if(strcmp(cmd, "convert") == 0)
        return cmd_convert(argc - 1, argv + 1);
    else if(strcmp(cmd, "verify") == 0)
        return cmd_verify(argc - 1, argv + 1);
    else if(strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0)
    {
        print_usage();
        return DIMG_OK;
    }
    else
    {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage();
        return DIMG_ERR_ARGS;
    }
}
