/*
 * dimg-tool info — display image metadata
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dimg.h"
#include "disc.h"
#include "aaru.h"
#include "aaruformat.h"

int cmd_info(int argc, char **argv)
{
    bool json_output = false;
    const char *path = NULL;

    /* Parse arguments: allow -j/--json before or after the path */
    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0)
            json_output = true;
        else if(argv[i][0] != '-' && path == NULL)
            path = argv[i];
    }

    if(path == NULL)
    {
        fprintf(stderr, "Usage: dimg-tool info [-j|--json] <image>\n");
        return DIMG_ERR_ARGS;
    }

    void *ctx = aaruf_open(path, false, NULL);
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

    DiscSystem system = (DiscSystem)info.MediaType;
    const char *codec = aaru_detect_codec(ctx);
    uint64_t media_size = (uint64_t)info.Sectors * info.SectorSize;

    /* Read track count for CD-based systems */
    int track_count = 1;
    if(disc_is_cd(system))
    {
        size_t track_buf_len = sizeof(TrackEntry) * DISC_MAX_TRACKS;
        uint8_t track_buf[sizeof(TrackEntry) * DISC_MAX_TRACKS];
        int32_t tres = aaruf_get_tracks(ctx, track_buf, &track_buf_len);
        if(tres == AARUF_STATUS_OK && track_buf_len > 0)
            track_count = (int)(track_buf_len / sizeof(TrackEntry));
    }

    if(json_output)
    {
        printf("{\n");
        printf("  \"format\": \"aaru\",\n");
        printf("  \"system\": \"%s\",\n", disc_system_cli_name(system));
        printf("  \"media_type\": %u,\n", info.MediaType);
        printf("  \"tracks\": %d,\n", track_count);
        printf("  \"sectors\": %" PRIu64 ",\n", info.Sectors);
        printf("  \"sector_size\": %u,\n", info.SectorSize);
        printf("  \"media_size\": %" PRIu64 ",\n", media_size);
        printf("  \"application\": \"%s\",\n", info.Application);
        printf("  \"application_version\": \"%s\",\n", info.ApplicationVersion);
        printf("  \"codec\": \"%s\"\n", codec);
        printf("}\n");
    }
    else
    {
        printf("Image:       %s\n", path);
        printf("System:      %s\n", disc_system_name(system));
        printf("Media type:  %u\n", info.MediaType);
        printf("Tracks:      %d\n", track_count);
        printf("Sectors:     %" PRIu64 "\n", info.Sectors);
        printf("Sector size: %u\n", info.SectorSize);
        printf("Media size:  %" PRIu64 " bytes\n", media_size);
        printf("Application: %s\n", info.Application);
        printf("Codec:       %s\n", codec);
    }

    aaruf_close(ctx);
    return DIMG_OK;
}
