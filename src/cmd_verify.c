/*
 * dimg-tool verify — verify image integrity
 */

#include <stdbool.h>
#include <stdio.h>

#include "dimg.h"
#include "aaru.h"
#include "aaruformat.h"

int cmd_verify(int argc, char **argv)
{
    if(argc < 2)
    {
        fprintf(stderr, "Usage: dimg-tool verify <image>\n");
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

    int res = aaruf_verify_image(ctx);
    if(res == AARUF_STATUS_OK)
    {
        printf("PASS: %s\n", path);
        aaruf_close(ctx);
        return DIMG_OK;
    }
    else
    {
        fprintf(stderr, "FAIL: %s (error %d)\n", path, res);
        aaruf_close(ctx);
        return DIMG_ERR_VERIFY;
    }
}
