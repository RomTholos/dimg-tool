/*
 * dimg-tool info — display image metadata
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "dimg.h"
#include "aaru.h"
#include "aaruformat.h"

int cmd_info(int argc, char **argv)
{
    if(argc < 2)
    {
        fprintf(stderr, "Usage: dimg-tool info <image>\n");
        return DIMG_ERR_ARGS;
    }

    const char *path = argv[1];
    void       *ctx  = NULL;

    ctx = aaruf_open(path, false, NULL);
    if(ctx == NULL)
    {
        fprintf(stderr, "Failed to open image: %s\n", path);
        return DIMG_ERR_IO;
    }

    ImageInfo info;
    int res = aaruf_get_image_info(ctx, &info);
    if(res != AARUF_STATUS_OK)
    {
        fprintf(stderr, "Failed to read image info (error %d)\n", res);
        aaruf_close(ctx);
        return DIMG_ERR_FORMAT;
    }

    printf("Image:       %s\n", path);
    printf("Media type:  %u\n", info.MediaType);
    printf("Sectors:     %" PRIu64 "\n", info.Sectors);
    printf("Sector size: %u\n", info.SectorSize);
    printf("Media size:  %" PRIu64 " bytes\n", (uint64_t)info.Sectors * info.SectorSize);
    printf("Application: %s\n", info.Application);

    aaruf_close(ctx);
    return DIMG_OK;
}
