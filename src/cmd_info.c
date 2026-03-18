/*
 * dimg-tool info — display image metadata
 */

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
    int         res  = 0;

    ctx = aaruf_open(path, &res);
    if(ctx == NULL || res != AARUF_STATUS_OK)
    {
        fprintf(stderr, "Failed to open image: %s (error %d)\n", path, res);
        return DIMG_ERR_IO;
    }

    ImageInfo info;
    res = aaruf_get_image_info(ctx, &info);
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

    if(info.Creator[0] != '\0')
        printf("Creator:     %s\n", info.Creator);
    if(info.MediaTitle[0] != '\0')
        printf("Title:       %s\n", info.MediaTitle);

    aaruf_close(ctx);
    return DIMG_OK;
}
